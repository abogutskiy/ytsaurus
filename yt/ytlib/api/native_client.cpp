#include "client.h"
#include "private.h"
#include "box.h"
#include "config.h"
#include "native_connection.h"
#include "native_transaction.h"
#include "file_reader.h"
#include "file_writer.h"
#include "journal_reader.h"
#include "journal_writer.h"
#include "rowset.h"
#include "table_reader.h"

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/chunk_replica.h>
#include <yt/ytlib/chunk_client/read_limit.h>
#include <yt/ytlib/chunk_client/chunk_teleporter.h>
#include <yt/ytlib/chunk_client/chunk_service_proxy.h>
#include <yt/ytlib/chunk_client/helpers.h>

#include <yt/ytlib/cypress_client/cypress_ypath_proxy.h>
#include <yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/ytlib/hive/cell_directory.h>
#include <yt/ytlib/hive/config.h>

#include <yt/ytlib/job_prober_client/job_prober_service_proxy.h>

#include <yt/ytlib/node_tracker_client/channel.h>

#include <yt/ytlib/object_client/helpers.h>
#include <yt/ytlib/object_client/master_ypath_proxy.h>
#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/ytlib/query_client/column_evaluator.h>
#include <yt/ytlib/query_client/coordinator.h>
#include <yt/ytlib/query_client/evaluator.h>
#include <yt/ytlib/query_client/helpers.h>
#include <yt/ytlib/query_client/query.h>
#include <yt/ytlib/query_client/query_helpers.h>
#include <yt/ytlib/query_client/query_preparer.h>
#include <yt/ytlib/query_client/query_service_proxy.h>
#include <yt/ytlib/query_client/query_statistics.h>
#include <yt/ytlib/query_client/functions_cache.h>

#include <yt/ytlib/scheduler/helpers.h>
#include <yt/ytlib/scheduler/job_prober_service_proxy.h>
#include <yt/ytlib/scheduler/scheduler_service_proxy.h>

#include <yt/ytlib/security_client/group_ypath_proxy.h>
#include <yt/ytlib/security_client/helpers.h>

#include <yt/ytlib/table_client/name_table.h>
#include <yt/ytlib/table_client/schema.h>
#include <yt/ytlib/table_client/schemaful_writer.h>

#include <yt/ytlib/tablet_client/table_mount_cache.h>
#include <yt/ytlib/tablet_client/tablet_service_proxy.h>
#include <yt/ytlib/tablet_client/wire_protocol.h>
#include <yt/ytlib/tablet_client/wire_protocol.pb.h>
#include <yt/ytlib/tablet_client/table_replica_ypath.h>

#include <yt/ytlib/transaction_client/timestamp_provider.h>
#include <yt/ytlib/transaction_client/transaction_manager.h>
#include <yt/ytlib/transaction_client/action.h>

#include <yt/core/compression/helpers.h>

#include <yt/core/concurrency/scheduler.h>
#include <yt/core/concurrency/async_semaphore.h>
#include <yt/core/concurrency/action_queue.h>

#include <yt/core/profiling/scoped_timer.h>

#include <yt/core/rpc/helpers.h>
#include <yt/core/rpc/latency_taming_channel.h>

#include <yt/core/ytree/helpers.h>
#include <yt/core/ytree/fluent.h>
#include <yt/core/ytree/ypath_proxy.h>

#include <yt/core/misc/collection_helpers.h>

// TODO(babenko): refactor this
#include <yt/ytlib/table_client/chunk_meta_extensions.h>
#include <yt/ytlib/table_client/schemaful_reader.h>
#include <yt/ytlib/table_client/table_ypath_proxy.h>
#include <yt/ytlib/table_client/row_merger.h>
#include <yt/ytlib/table_client/row_base.h>

namespace NYT {
namespace NApi {

using namespace NConcurrency;
using namespace NYPath;
using namespace NYTree;
using namespace NYson;
using namespace NObjectClient;
using namespace NObjectClient::NProto;
using namespace NCypressClient;
using namespace NTransactionClient;
using namespace NRpc;
using namespace NTableClient;
using namespace NTableClient::NProto;
using namespace NTabletClient;
using namespace NTabletClient::NProto;
using namespace NSecurityClient;
using namespace NQueryClient;
using namespace NChunkClient;
using namespace NScheduler;
using namespace NHiveClient;
using namespace NHydra;

using NChunkClient::TReadLimit;
using NChunkClient::TReadRange;
using NTableClient::TColumnSchema;
using NNodeTrackerClient::INodeChannelFactoryPtr;
using NNodeTrackerClient::CreateNodeChannelFactory;
using NNodeTrackerClient::TNetworkPreferenceList;

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TQueryHelper)
DECLARE_REFCOUNTED_CLASS(TNativeClient)
DECLARE_REFCOUNTED_CLASS(TNativeTransaction)

////////////////////////////////////////////////////////////////////////////////

TUserWorkloadDescriptor::operator TWorkloadDescriptor() const
{
    TWorkloadDescriptor result;
    switch (Category) {
        case EUserWorkloadCategory::Realtime:
            result.Category = EWorkloadCategory::UserRealtime;
            break;
        case EUserWorkloadCategory::Batch:
            result.Category = EWorkloadCategory::UserBatch;
            break;
        default:
            Y_UNREACHABLE();
    }
    result.Band = Band;
    return result;
}

struct TSerializableUserWorkloadDescriptor
    : public TYsonSerializableLite
{
    TUserWorkloadDescriptor Underlying;

    TSerializableUserWorkloadDescriptor()
    {
        RegisterParameter("category", Underlying.Category);
        RegisterParameter("band", Underlying.Band)
            .Optional();
    }
};

void Serialize(const TUserWorkloadDescriptor& workloadDescriptor, NYson::IYsonConsumer* consumer)
{
    TSerializableUserWorkloadDescriptor serializableWorkloadDescriptor;
    serializableWorkloadDescriptor.Underlying = workloadDescriptor;
    Serialize(serializableWorkloadDescriptor, consumer);
}

void Deserialize(TUserWorkloadDescriptor& workloadDescriptor, INodePtr node)
{
    TSerializableUserWorkloadDescriptor serializableWorkloadDescriptor;
    Deserialize(serializableWorkloadDescriptor, node);
    workloadDescriptor = serializableWorkloadDescriptor.Underlying;
}

////////////////////////////////////////////////////////////////////////////////

NRpc::TMutationId TMutatingOptions::GetOrGenerateMutationId() const
{
    if (Retry && !MutationId) {
        THROW_ERROR_EXCEPTION("Cannot execute retry without mutation id");
    }
    return MutationId ? MutationId : NRpc::GenerateMutationId();
}

////////////////////////////////////////////////////////////////////////////////

namespace {

TNameTableToSchemaIdMapping BuildColumnIdMapping(
    const TTableSchema& schema,
    const TNameTablePtr& nameTable)
{
    for (const auto& name : schema.GetKeyColumns()) {
        // We shouldn't consider computed columns below because client doesn't send them.
        if (!nameTable->FindId(name) && !schema.GetColumnOrThrow(name).Expression) {
            THROW_ERROR_EXCEPTION("Missing key column %Qv",
                name);
        }
    }

    TNameTableToSchemaIdMapping mapping;
    mapping.resize(nameTable->GetSize());
    for (int nameTableId = 0; nameTableId < nameTable->GetSize(); ++nameTableId) {
        const auto& name = nameTable->GetName(nameTableId);
        const auto* columnSchema = schema.FindColumn(name);
        mapping[nameTableId] = columnSchema ? schema.GetColumnIndex(*columnSchema) : -1;
    }
    return mapping;
}

SmallVector<const TCellPeerDescriptor*, 3> GetValidPeers(const TCellDescriptor& cellDescriptor)
{
    SmallVector<const TCellPeerDescriptor*, 3> peers;
    for (const auto& peer : cellDescriptor.Peers) {
        if (!peer.IsNull()) {
            peers.push_back(&peer);
        }
    }
    return peers;
}

const TCellPeerDescriptor& GetPrimaryTabletPeerDescriptor(
    const TCellDescriptor& cellDescriptor,
    EPeerKind peerKind = EPeerKind::Leader)
{
    auto peers = GetValidPeers(cellDescriptor);

    if (peers.empty()) {
        THROW_ERROR_EXCEPTION("No alive replicas for tablet cell %v",
            cellDescriptor.CellId);
    }

    int leadingPeerIndex = -1;
    for (int index = 0; index < peers.size(); ++index) {
        if (peers[index]->GetVoting()) {
            leadingPeerIndex = index;
            break;
        }
    }

    switch (peerKind) {
        case EPeerKind::Leader: {
            if (leadingPeerIndex < 0) {
                THROW_ERROR_EXCEPTION("No leading peer is known for tablet cell %v",
                    cellDescriptor.CellId);
            }
            return *peers[leadingPeerIndex];
        }

        case EPeerKind::LeaderOrFollower: {
            int randomIndex = RandomNumber(peers.size());
            return *peers[randomIndex];
        }

        case EPeerKind::Follower: {
            if (leadingPeerIndex < 0 || peers.size() == 1) {
                int randomIndex = RandomNumber(peers.size());
                return *peers[randomIndex];
            } else {
                int randomIndex = RandomNumber(peers.size() - 1);
                if (randomIndex >= leadingPeerIndex) {
                    ++randomIndex;
                }
                return *peers[randomIndex];
            }
        }

        default:
            Y_UNREACHABLE();
    }
}

const TCellPeerDescriptor& GetBackupTabletPeerDescriptor(
    const TCellDescriptor& cellDescriptor,
    const TCellPeerDescriptor& primaryPeerDescriptor)
{
    auto peers = GetValidPeers(cellDescriptor);

    Y_ASSERT(peers.size() > 1);

    int primaryPeerIndex = -1;
    for (int index = 0; index < peers.size(); ++index) {
        if (peers[index] == &primaryPeerDescriptor) {
            primaryPeerIndex = index;
            break;
        }
    }

    Y_ASSERT(primaryPeerIndex >= 0 && primaryPeerIndex < peers.size());

    int randomIndex = RandomNumber(peers.size() - 1);
    if (randomIndex >= primaryPeerIndex) {
        ++randomIndex;
    }

    return *peers[randomIndex];
}

IChannelPtr CreateTabletReadChannel(
    const IChannelFactoryPtr& channelFactory,
    const TCellDescriptor& cellDescriptor,
    const TTabletReadOptions& options,
    const TNetworkPreferenceList& networks)
{
    const auto& primaryPeerDescriptor = GetPrimaryTabletPeerDescriptor(cellDescriptor, options.ReadFrom);
    auto primaryChannel = channelFactory->CreateChannel(primaryPeerDescriptor.GetAddress(networks));
    if (cellDescriptor.Peers.size() == 1 || !options.BackupRequestDelay) {
        return primaryChannel;
    }

    const auto& backupPeerDescriptor = GetBackupTabletPeerDescriptor(cellDescriptor, primaryPeerDescriptor);
    auto backupChannel = channelFactory->CreateChannel(backupPeerDescriptor.GetAddress(networks));

    return CreateLatencyTamingChannel(
        std::move(primaryChannel),
        std::move(backupChannel),
        *options.BackupRequestDelay);
}

void ValidateTabletMountedOrFrozen(const TTableMountInfoPtr& tableInfo, const TTabletInfoPtr& tabletInfo)
{
    auto state = tabletInfo->State;
    if (state != ETabletState::Mounted &&
        state != ETabletState::Freezing &&
        state != ETabletState::Frozen)
    {
        THROW_ERROR_EXCEPTION(
            NTabletClient::EErrorCode::TabletNotMounted,
            "Cannot read from tablet %v while it is in %Qlv state",
            tabletInfo->TabletId,
            state)
            << TErrorAttribute("tablet_id", tabletInfo->TabletId);
    }
}

void ValidateTabletMounted(const TTableMountInfoPtr& tableInfo, const TTabletInfoPtr& tabletInfo)
{
    if (tabletInfo->State != ETabletState::Mounted) {
        THROW_ERROR_EXCEPTION(
            NTabletClient::EErrorCode::TabletNotMounted,
            "Tablet %v of table %v is in %Qlv state",
            tabletInfo->TabletId,
            tableInfo->Path,
            tabletInfo->State)
            << TErrorAttribute("tablet_id", tabletInfo->TabletId);
    }
}

TTabletInfoPtr GetSortedTabletForRow(
    const TTableMountInfoPtr& tableInfo,
    NTableClient::TKey key)
{
    Y_ASSERT(tableInfo->IsSorted());

    auto tabletInfo = tableInfo->GetTabletForRow(key);
    ValidateTabletMounted(tableInfo, tabletInfo);
    return tabletInfo;
}

TTabletInfoPtr GetOrderedTabletForRow(
    const TTableMountInfoPtr& tableInfo,
    const TTabletInfoPtr& randomTabletInfo,
    TNullable<int> tabletIndexColumnId,
    NTableClient::TKey key)
{
    Y_ASSERT(!tableInfo->IsSorted());

    int tabletIndex = -1;
    for (const auto& value : key) {
        if (tabletIndexColumnId && value.Id == *tabletIndexColumnId) {
            Y_ASSERT(value.Type == EValueType::Null || value.Type == EValueType::Int64);
            if (value.Type == EValueType::Int64) {
                tabletIndex = value.Data.Int64;
                if (tabletIndex < 0 || tabletIndex >= tableInfo->Tablets.size()) {
                    THROW_ERROR_EXCEPTION("Invalid tablet index: actual %v, expected in range [0, %v]",
                        tabletIndex,
                        tableInfo->Tablets.size() - 1);
                }
            }
        }
    }

    if (tabletIndex < 0) {
        return randomTabletInfo;
    }

    auto tabletInfo = tableInfo->Tablets[tabletIndex];
    ValidateTabletMounted(tableInfo, tabletInfo);
    return tabletInfo;
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

TError TCheckPermissionResult::ToError(const Stroka& user, EPermission permission) const
{
    switch (Action) {
        case NSecurityClient::ESecurityAction::Allow:
            return TError();

        case NSecurityClient::ESecurityAction::Deny: {
            TError error;
            if (ObjectName && SubjectName) {
                error = TError(
                    NSecurityClient::EErrorCode::AuthorizationError,
                    "Access denied: %Qlv permission is denied for %Qv by ACE at %v",
                    permission,
                    *SubjectName,
                    *ObjectName);
            } else {
                error = TError(
                    NSecurityClient::EErrorCode::AuthorizationError,
                    "Access denied: %Qlv permission is not allowed by any matching ACE",
                    permission);
            }
            error.Attributes().Set("user", user);
            error.Attributes().Set("permission", permission);
            if (ObjectId) {
                error.Attributes().Set("denied_by", ObjectId);
            }
            if (SubjectId) {
                error.Attributes().Set("denied_for", SubjectId);
            }
            return error;
        }

        default:
            Y_UNREACHABLE();
    }
}

////////////////////////////////////////////////////////////////////////////////

class TQueryResponseReader
    : public ISchemafulReader
{
public:
    TQueryResponseReader(
        TFuture<TQueryServiceProxy::TRspExecutePtr> asyncResponse,
        const TTableSchema& schema,
        const NLogging::TLogger& logger)
        : Schema_(schema)
        , Logger(logger)
    {
        QueryResult_ = asyncResponse.Apply(BIND(
            &TQueryResponseReader::OnResponse,
            MakeStrong(this)));
    }

    virtual bool Read(std::vector<TUnversionedRow>* rows) override
    {
        return !RowsetReader_ || RowsetReader_->Read(rows);
    }

    virtual TFuture<void> GetReadyEvent() override
    {
        if (!RowsetReader_) {
            return QueryResult_.As<void>();
        } else {
            return RowsetReader_->GetReadyEvent();
        }
    }

    TFuture<TQueryStatistics> GetQueryResult() const
    {
        return QueryResult_;
    }

private:

    TTableSchema Schema_;
    ISchemafulReaderPtr RowsetReader_;

    TFuture<TQueryStatistics> QueryResult_;

    NLogging::TLogger Logger;

    TQueryStatistics OnResponse(const TQueryServiceProxy::TRspExecutePtr& response)
    {
        TSharedRef data;

        TDuration deserializationTime;
        {
            NProfiling::TAggregatingTimingGuard timingGuard(&deserializationTime);
            data = NCompression::DecompressWithEnvelope(response->Attachments());
        }

        LOG_DEBUG("Received subquery result (DeserializationTime: %v, DataSize: %v)",
            deserializationTime,
            data.Size());

        YCHECK(!RowsetReader_);
        RowsetReader_ = TWireProtocolReader(data).CreateSchemafulRowsetReader(Schema_);

        return FromProto(response->query_statistics());
    }
};

DEFINE_REFCOUNTED_TYPE(TQueryResponseReader)

////////////////////////////////////////////////////////////////////////////////

struct TQueryHelperRowBufferTag
{ };

class TQueryHelper
    : public IExecutor
    , public IPrepareCallbacks
{
public:
    TQueryHelper(
        INativeConnectionPtr connection,
        IChannelPtr masterChannel,
        IChannelFactoryPtr nodeChannelFactory,
        const TFunctionImplCachePtr& functionImplCache)
        : Connection_(std::move(connection))
        , MasterChannel_(std::move(masterChannel))
        , NodeChannelFactory_(std::move(nodeChannelFactory))
        , FunctionImplCache_(functionImplCache)
    { }

    // IPrepareCallbacks implementation.

    virtual TFuture<TDataSplit> GetInitialSplit(
        const TRichYPath& path,
        TTimestamp timestamp) override
    {
        return BIND(&TQueryHelper::DoGetInitialSplit, MakeStrong(this))
            .AsyncVia(Connection_->GetLightInvoker())
            .Run(path, timestamp);
    }

    virtual TFuture<TQueryStatistics> Execute(
        TConstQueryPtr query,
        TConstExternalCGInfoPtr externalCGInfo,
        TDataRanges dataSource,
        ISchemafulWriterPtr writer,
        const TQueryOptions& options) override
    {
        TRACE_CHILD("QueryClient", "Execute") {

            auto execute = query->IsOrdered()
                ? &TQueryHelper::DoExecuteOrdered
                : &TQueryHelper::DoExecute;

            return BIND(execute, MakeStrong(this))
                .AsyncVia(Connection_->GetHeavyInvoker())
                .Run(
                    std::move(query),
                    std::move(externalCGInfo),
                    std::move(dataSource),
                    options,
                    std::move(writer));
        }
    }

private:
    const INativeConnectionPtr Connection_;
    const IChannelPtr MasterChannel_;
    const IChannelFactoryPtr NodeChannelFactory_;
    const TFunctionImplCachePtr FunctionImplCache_;


    TDataSplit DoGetInitialSplit(
        const TRichYPath& path,
        TTimestamp timestamp)
    {
        auto tableMountCache = Connection_->GetTableMountCache();
        auto tableInfo = WaitFor(tableMountCache->GetTableInfo(path.GetPath()))
            .ValueOrThrow();

        tableInfo->ValidateNotReplicated();

        TDataSplit result;
        SetObjectId(&result, tableInfo->TableId);
        SetTableSchema(&result, GetTableSchema(path, tableInfo));
        SetTimestamp(&result, timestamp);
        return result;
    }

    TTableSchema GetTableSchema(
        const TRichYPath& path,
        const TTableMountInfoPtr& tableInfo)
    {
        if (auto maybePathSchema = path.GetSchema()) {
            if (tableInfo->Dynamic) {
                THROW_ERROR_EXCEPTION("Explicit YPath \"schema\" specification is only allowed for static tables");
            }
            return *maybePathSchema;
        }

        return tableInfo->Schemas[ETableSchemaKind::Query];
    }

    std::vector<std::pair<TDataRanges, Stroka>> SplitTable(
        const TObjectId& tableId,
        const TSharedRange<TRowRange>& ranges,
        const TRowBufferPtr& rowBuffer,
        const TQueryOptions& options,
        const NLogging::TLogger& Logger)
    {
        auto tableMountCache = Connection_->GetTableMountCache();
        auto tableInfo = WaitFor(tableMountCache->GetTableInfo(FromObjectId(tableId)))
            .ValueOrThrow();

        auto result = tableInfo->Dynamic
            ? SplitDynamicTable(tableId, ranges, rowBuffer, tableInfo)
            : SplitStaticTable(tableId, ranges, rowBuffer, tableInfo);

        LOG_DEBUG_IF(options.VerboseLogging, "Got %v sources for input %v",
            result.size(),
            tableId);

        return result;
    }

    std::vector<std::pair<TDataRanges, Stroka>> SplitStaticTable(
        const TObjectId& tableId,
        const TSharedRange<TRowRange>& ranges,
        const TRowBufferPtr& rowBuffer,
        const TTableMountInfoPtr& tableInfo)
    {
        if (!tableInfo->IsSorted()) {
            THROW_ERROR_EXCEPTION("Table %v is not sorted",
                tableInfo->Path);
        }

        std::vector<TReadRange> readRanges;
        for (const auto& range : ranges) {
            readRanges.emplace_back(TReadLimit(TOwningKey(range.first)), TReadLimit(TOwningKey(range.second)));
        }

        // TODO(babenko): refactor and optimize
        TObjectServiceProxy proxy(MasterChannel_);

        // XXX(babenko): multicell
        auto req = TTableYPathProxy::Fetch(FromObjectId(tableId));
        ToProto(req->mutable_ranges(), readRanges);
        req->set_fetch_all_meta_extensions(true);

        auto rsp = WaitFor(proxy.Execute(req))
            .ValueOrThrow();

        auto nodeDirectory = New<NNodeTrackerClient::TNodeDirectory>();
        nodeDirectory->MergeFrom(rsp->node_directory());

        auto chunkSpecs = FromProto<std::vector<NChunkClient::NProto::TChunkSpec>>(rsp->chunks());

        // Remove duplicate chunks.
        std::sort(chunkSpecs.begin(), chunkSpecs.end(), [] (const TDataSplit& lhs, const TDataSplit& rhs) {
            return GetObjectIdFromDataSplit(lhs) < GetObjectIdFromDataSplit(rhs);
        });
        chunkSpecs.erase(
            std::unique(
                chunkSpecs.begin(),
                chunkSpecs.end(),
                [] (const TDataSplit& lhs, const TDataSplit& rhs) {
                    return GetObjectIdFromDataSplit(lhs) == GetObjectIdFromDataSplit(rhs);
                }),
            chunkSpecs.end());

        // Sort chunks by lower bound.
        std::sort(chunkSpecs.begin(), chunkSpecs.end(), [] (const TDataSplit& lhs, const TDataSplit& rhs) {
            return GetLowerBoundFromDataSplit(lhs) < GetLowerBoundFromDataSplit(rhs);
        });

        const auto& networks = Connection_->GetNetworks();

        for (auto& chunkSpec : chunkSpecs) {
            auto chunkSchema = FindProtoExtension<TTableSchemaExt>(chunkSpec.chunk_meta().extensions());

            // TODO(sandello): One day we should validate consistency.
            // Now we just check we do _not_ have any of these.
            YCHECK(!chunkSchema);

            TOwningKey chunkLowerBound, chunkUpperBound;
            if (FindBoundaryKeys(chunkSpec.chunk_meta(), &chunkLowerBound, &chunkUpperBound)) {
                chunkUpperBound = GetKeySuccessor(chunkUpperBound);
                SetLowerBound(&chunkSpec, chunkLowerBound);
                SetUpperBound(&chunkSpec, chunkUpperBound);
            }
        }

        std::vector<std::pair<TDataRanges, Stroka>> subsources;
        for (const auto& range : ranges) {
            auto lowerBound = range.first;
            auto upperBound = range.second;

            // Run binary search to find the relevant chunks.
            auto startIt = std::lower_bound(
                chunkSpecs.begin(),
                chunkSpecs.end(),
                lowerBound,
                [] (const TDataSplit& chunkSpec, TRow key) {
                    return GetUpperBoundFromDataSplit(chunkSpec) <= key;
                });

            for (auto it = startIt; it != chunkSpecs.end(); ++it) {
                const auto& chunkSpec = *it;
                auto keyRange = GetBothBoundsFromDataSplit(chunkSpec);

                if (upperBound <= keyRange.first) {
                    break;
                }

                auto replicas = FromProto<TChunkReplicaList>(chunkSpec.replicas());
                if (replicas.empty()) {
                    auto objectId = GetObjectIdFromDataSplit(chunkSpec);
                    THROW_ERROR_EXCEPTION("No alive replicas for chunk %v",
                        objectId);
                }

                const TChunkReplica& selectedReplica = replicas[RandomNumber(replicas.size())];
                const auto& descriptor = nodeDirectory->GetDescriptor(selectedReplica);
                const auto& address = descriptor.GetAddress(networks);

                TRowRange subrange;
                subrange.first = rowBuffer->Capture(std::max(lowerBound, keyRange.first.Get()));
                subrange.second = rowBuffer->Capture(std::min(upperBound, keyRange.second.Get()));

                TDataRanges dataSource;
                dataSource.Id = GetObjectIdFromDataSplit(chunkSpec);
                dataSource.Ranges = MakeSharedRange(
                    SmallVector<TRowRange, 1>{subrange},
                    rowBuffer,
                    ranges.GetHolder());

                subsources.emplace_back(std::move(dataSource), address);
            }
        }

        return subsources;
    }

    std::vector<std::pair<TDataRanges, Stroka>> SplitDynamicTable(
        const TObjectId& tableId,
        const TSharedRange<TRowRange>& ranges,
        const TRowBufferPtr& rowBuffer,
        const TTableMountInfoPtr& tableInfo)
    {
        const auto& cellDirectory = Connection_->GetCellDirectory();
        const auto& networks = Connection_->GetNetworks();

        yhash_map<NTabletClient::TTabletCellId, TCellDescriptor> tabletCellReplicas;

        auto getAddress = [&] (const TTabletInfoPtr& tabletInfo) mutable {
            // TODO(babenko): learn to work with unmounted tablets
            ValidateTabletMountedOrFrozen(tableInfo, tabletInfo);

            auto insertResult = tabletCellReplicas.insert(std::make_pair(tabletInfo->CellId, TCellDescriptor()));
            auto& descriptor = insertResult.first->second;

            if (insertResult.second) {
                descriptor = cellDirectory->GetDescriptorOrThrow(tabletInfo->CellId);
            }

            // TODO(babenko): pass proper read options
            const auto& peerDescriptor = GetPrimaryTabletPeerDescriptor(descriptor);
            return peerDescriptor.GetAddress(networks);
        };

        std::vector<std::pair<TDataRanges, Stroka>> subsources;
        for (auto rangesIt = begin(ranges); rangesIt != end(ranges);) {
            auto lowerBound = rangesIt->first;
            auto upperBound = rangesIt->second;

            if (lowerBound < tableInfo->LowerCapBound) {
                lowerBound = rowBuffer->Capture(tableInfo->LowerCapBound.Get());
            }
            if (upperBound > tableInfo->UpperCapBound) {
                upperBound = rowBuffer->Capture(tableInfo->UpperCapBound.Get());
            }

            if (lowerBound >= upperBound) {
                ++rangesIt;
                continue;
            }

            // Run binary search to find the relevant tablets.
            auto startIt = std::upper_bound(
                tableInfo->Tablets.begin(),
                tableInfo->Tablets.end(),
                lowerBound,
                [] (TKey key, const TTabletInfoPtr& tabletInfo) {
                    return key < tabletInfo->PivotKey;
                }) - 1;

            auto tabletInfo = *startIt;
            auto nextPivotKey = (startIt + 1 == tableInfo->Tablets.end())
                ? tableInfo->UpperCapBound
                : (*(startIt + 1))->PivotKey;

            if (upperBound < nextPivotKey) {
                auto rangesItEnd = std::upper_bound(
                    rangesIt,
                    end(ranges),
                    nextPivotKey.Get(),
                    [] (TKey key, const TRowRange& rowRange) {
                        return key < rowRange.second;
                    });

                const auto& address = getAddress(tabletInfo);

                TDataRanges dataSource;
                dataSource.Id = tabletInfo->TabletId;
                dataSource.MountRevision = tabletInfo->MountRevision;
                dataSource.Ranges = MakeSharedRange(
                    MakeRange<TRowRange>(rangesIt, rangesItEnd),
                    rowBuffer,
                    ranges.GetHolder());
                dataSource.LookupSupported = tableInfo->IsSorted();

                subsources.emplace_back(std::move(dataSource), address);
                rangesIt = rangesItEnd;
            } else {
                for (auto it = startIt; it != tableInfo->Tablets.end(); ++it) {
                    const auto& tabletInfo = *it;
                    Y_ASSERT(upperBound > tabletInfo->PivotKey);

                    const auto& address = getAddress(tabletInfo);

                    auto pivotKey = tabletInfo->PivotKey;
                    auto nextPivotKey = (it + 1 == tableInfo->Tablets.end())
                        ? tableInfo->UpperCapBound
                        : (*(it + 1))->PivotKey;

                    bool isLast = (upperBound <= nextPivotKey);

                    TRowRange subrange;
                    subrange.first = it == startIt ? lowerBound : rowBuffer->Capture(pivotKey.Get());
                    subrange.second = isLast ? upperBound : rowBuffer->Capture(nextPivotKey.Get());

                    TDataRanges dataSource;
                    dataSource.Id = tabletInfo->TabletId;
                    dataSource.MountRevision = tabletInfo->MountRevision;
                    dataSource.Ranges = MakeSharedRange(
                        SmallVector<TRowRange, 1>{subrange},
                        rowBuffer,
                        ranges.GetHolder());
                    dataSource.LookupSupported = tableInfo->IsSorted();

                    subsources.emplace_back(std::move(dataSource), address);

                    if (isLast) {
                        break;
                    }
                }
                ++rangesIt;
            }
        }

        return subsources;
    }

    std::vector<std::pair<TDataRanges, Stroka>> InferRanges(
        TConstQueryPtr query,
        const TDataRanges& dataSource,
        const TQueryOptions& options,
        TRowBufferPtr rowBuffer,
        const NLogging::TLogger& Logger)
    {
        const auto& tableId = dataSource.Id;
        auto ranges = dataSource.Ranges;

        auto prunedRanges = GetPrunedRanges(
            query,
            tableId,
            ranges,
            rowBuffer,
            Connection_->GetColumnEvaluatorCache(),
            BuiltinRangeExtractorMap,
            options);

        LOG_DEBUG("Splitting %v pruned splits", prunedRanges.size());

        return SplitTable(
            tableId,
            MakeSharedRange(std::move(prunedRanges), rowBuffer),
            std::move(rowBuffer),
            options,
            Logger);
    }

    TQueryStatistics DoCoordinateAndExecute(
        TConstQueryPtr query,
        const TConstExternalCGInfoPtr& externalCGInfo,
        const TQueryOptions& options,
        ISchemafulWriterPtr writer,
        int subrangesCount,
        std::function<std::pair<std::vector<TDataRanges>, Stroka>(int)> getSubsources)
    {
        auto Logger = BuildQueryLogger(query);

        std::vector<TRefiner> refiners(subrangesCount, [] (
            TConstExpressionPtr expr,
            const TKeyColumns& keyColumns) {
                return expr;
            });

        auto functionGenerators = New<TFunctionProfilerMap>();
        auto aggregateGenerators = New<TAggregateProfilerMap>();
        MergeFrom(functionGenerators.Get(), *BuiltinFunctionCG);
        MergeFrom(aggregateGenerators.Get(), *BuiltinAggregateCG);
        FetchImplementations(
            functionGenerators,
            aggregateGenerators,
            externalCGInfo,
            FunctionImplCache_);

        return CoordinateAndExecute(
            query,
            writer,
            refiners,
            [&] (TConstQueryPtr subquery, int index) {
                std::vector<TDataRanges> dataSources;
                Stroka address;
                std::tie(dataSources, address) = getSubsources(index);

                LOG_DEBUG("Delegating subquery (SubQueryId: %v, Address: %v, MaxSubqueries: %v)",
                    subquery->Id,
                    address,
                    options.MaxSubqueries);

                return Delegate(std::move(subquery), externalCGInfo, options, std::move(dataSources), address);
            },
            [&] (TConstQueryPtr topQuery, ISchemafulReaderPtr reader, ISchemafulWriterPtr writer) {
                LOG_DEBUG("Evaluating top query (TopQueryId: %v)", topQuery->Id);
                auto evaluator = Connection_->GetQueryEvaluator();
                return evaluator->Run(
                    std::move(topQuery),
                    std::move(reader),
                    std::move(writer),
                    functionGenerators,
                    aggregateGenerators,
                    options.EnableCodeCache);
            });
    }

    TQueryStatistics DoExecute(
        TConstQueryPtr query,
        TConstExternalCGInfoPtr externalCGInfo,
        TDataRanges dataSource,
        const TQueryOptions& options,
        ISchemafulWriterPtr writer)
    {
        auto Logger = BuildQueryLogger(query);

        auto rowBuffer = New<TRowBuffer>(TQueryHelperRowBufferTag{});
        auto allSplits = InferRanges(
            query,
            dataSource,
            options,
            rowBuffer,
            Logger);

        LOG_DEBUG("Regrouping %v splits into groups",
            allSplits.size());

        yhash_map<Stroka, std::vector<TDataRanges>> groupsByAddress;
        for (const auto& split : allSplits) {
            const auto& address = split.second;
            groupsByAddress[address].push_back(split.first);
        }

        std::vector<std::pair<std::vector<TDataRanges>, Stroka>> groupedSplits;
        for (const auto& group : groupsByAddress) {
            groupedSplits.emplace_back(group.second, group.first);
        }

        LOG_DEBUG("Regrouped %v splits into %v groups",
            allSplits.size(),
            groupsByAddress.size());

        return DoCoordinateAndExecute(
            query,
            externalCGInfo,
            options,
            writer,
            groupedSplits.size(),
            [&] (int index) {
                return groupedSplits[index];
            });
    }

    TQueryStatistics DoExecuteOrdered(
        TConstQueryPtr query,
        TConstExternalCGInfoPtr externalCGInfo,
        TDataRanges dataSource,
        const TQueryOptions& options,
        ISchemafulWriterPtr writer)
    {
        auto Logger = BuildQueryLogger(query);

        auto rowBuffer = New<TRowBuffer>(TQueryHelperRowBufferTag());
        auto allSplits = InferRanges(
            query,
            dataSource,
            options,
            rowBuffer,
            Logger);

        // Should be already sorted
        LOG_DEBUG("Sorting %v splits", allSplits.size());

        std::sort(
            allSplits.begin(),
            allSplits.end(),
            [] (const std::pair<TDataRanges, Stroka>& lhs, const std::pair<TDataRanges, Stroka>& rhs) {
                return lhs.first.Ranges.Begin()->first < rhs.first.Ranges.Begin()->first;
            });

        return DoCoordinateAndExecute(
            query,
            externalCGInfo,
            options,
            writer,
            allSplits.size(),
            [&] (int index) {
                const auto& split = allSplits[index];

                LOG_DEBUG("Delegating to tablet %v at %v",
                    split.first.Id,
                    split.second);

                return std::make_pair(std::vector<TDataRanges>{split.first}, split.second);
            });
    }

    std::pair<ISchemafulReaderPtr, TFuture<TQueryStatistics>> Delegate(
        TConstQueryPtr query,
        const TConstExternalCGInfoPtr& externalCGInfo,
        const TQueryOptions& options,
        std::vector<TDataRanges> dataSources,
        const Stroka& address)
    {
        auto Logger = BuildQueryLogger(query);

        TRACE_CHILD("QueryClient", "Delegate") {
            auto channel = NodeChannelFactory_->CreateChannel(address);
            auto config = Connection_->GetConfig();

            TQueryServiceProxy proxy(channel);
            proxy.SetDefaultTimeout(config->QueryTimeout);

            auto req = proxy.Execute();

            TDuration serializationTime;
            {
                NProfiling::TAggregatingTimingGuard timingGuard(&serializationTime);
                ToProto(req->mutable_query(), query);
                ToProto(req->mutable_external_functions(), externalCGInfo->Functions);
                externalCGInfo->NodeDirectory->DumpTo(req->mutable_node_directory());
                ToProto(req->mutable_options(), options);
                ToProto(req->mutable_data_sources(), dataSources);

                req->set_response_codec(static_cast<int>(config->QueryResponseCodec));
            }

            auto queryFingerprint = InferName(query, true);
            LOG_DEBUG("Sending subquery (Fingerprint: %v, InputSchema: %v, ResultSchema: %v, SerializationTime: "
                          "%v, RequestSize: %v)",
                queryFingerprint,
                NYTree::ConvertToYsonString(query->OriginalSchema, NYson::EYsonFormat::Text).Data(),
                NYTree::ConvertToYsonString(query->GetTableSchema(), NYson::EYsonFormat::Text).Data(),
                serializationTime,
                req->ByteSize());

            TRACE_ANNOTATION("serialization_time", serializationTime);
            TRACE_ANNOTATION("request_size", req->ByteSize());

            auto resultReader = New<TQueryResponseReader>(
                req->Invoke(),
                query->GetTableSchema(),
                Logger);
            return std::make_pair(resultReader, resultReader->GetQueryResult());
        }
    }

};

DEFINE_REFCOUNTED_TYPE(TQueryHelper)

////////////////////////////////////////////////////////////////////////////////

struct TLookupRowsBufferTag
{ };

struct TWriteRowsBufferTag
{ };

struct TDeleteRowsBufferTag
{ };

class TNativeClient
    : public INativeClient
{
public:
    TNativeClient(
        INativeConnectionPtr connection,
        const TClientOptions& options)
        : Connection_(std::move(connection))
        , Options_(options)
        , ConcurrentRequestsSemaphore_(New<TAsyncSemaphore>(Connection_->GetConfig()->MaxConcurrentRequests))
    {
        auto wrapChannel = [&] (IChannelPtr channel) {
            channel = CreateAuthenticatedChannel(channel, options.User);
            return channel;
        };
        auto wrapChannelFactory = [&] (IChannelFactoryPtr factory) {
            factory = CreateAuthenticatedChannelFactory(factory, options.User);
            return factory;
        };

        auto initMasterChannel = [&] (EMasterChannelKind kind, TCellTag cellTag) {
            // NB: Caching is only possible for the primary master.
            if (kind == EMasterChannelKind::Cache && cellTag != Connection_->GetPrimaryMasterCellTag()) {
                return;
            }
            MasterChannels_[kind][cellTag] = wrapChannel(Connection_->GetMasterChannelOrThrow(kind, cellTag));
        };
        for (auto kind : TEnumTraits<EMasterChannelKind>::GetDomainValues()) {
            initMasterChannel(kind, Connection_->GetPrimaryMasterCellTag());
            for (auto cellTag : Connection_->GetSecondaryMasterCellTags()) {
                initMasterChannel(kind, cellTag);
            }
        }

        SchedulerChannel_ = wrapChannel(Connection_->GetSchedulerChannel());

        LightChannelFactory_ = CreateNodeChannelFactory(
            wrapChannelFactory(Connection_->GetLightChannelFactory()),
            Connection_->GetNetworks());
        HeavyChannelFactory_ = CreateNodeChannelFactory(
            wrapChannelFactory(Connection_->GetHeavyChannelFactory()),
            Connection_->GetNetworks());

        SchedulerProxy_.reset(new TSchedulerServiceProxy(GetSchedulerChannel()));
        JobProberProxy_.reset(new TJobProberServiceProxy(GetSchedulerChannel()));

        TransactionManager_ = New<TTransactionManager>(
            Connection_->GetConfig()->TransactionManager,
            Connection_->GetConfig()->PrimaryMaster->CellId,
            GetMasterChannelOrThrow(EMasterChannelKind::Leader),
            Connection_->GetTimestampProvider(),
            Connection_->GetCellDirectory());

        QueryHelper_ = New<TQueryHelper>(
            Connection_,
            GetMasterChannelOrThrow(EMasterChannelKind::Follower),
            HeavyChannelFactory_,
            CreateFunctionImplCache(
                Connection_->GetConfig()->FunctionImplCache,
                MakeWeak(this)));

        FunctionRegistry_ = CreateFunctionRegistryCache(
            Connection_->GetConfig()->UdfRegistryPath,
            Connection_->GetConfig()->FunctionRegistryCache,
            MakeWeak(this),
            Connection_->GetLightInvoker());

        Logger.AddTag("Client: %p", this);
    }


    virtual IConnectionPtr GetConnection() override
    {
        return Connection_;
    }

    virtual INativeConnectionPtr GetNativeConnection() override
    {
        return Connection_;
    }

    virtual IChannelPtr GetMasterChannelOrThrow(
        EMasterChannelKind kind,
        TCellTag cellTag = PrimaryMasterCellTag) override
    {
        const auto& channels = MasterChannels_[kind];
        auto it = channels.find(cellTag == PrimaryMasterCellTag ? Connection_->GetPrimaryMasterCellTag() : cellTag);
        if (it == channels.end()) {
            THROW_ERROR_EXCEPTION("Unknown master cell tag %v",
                cellTag);
        }
        return it->second;
    }

    virtual IChannelPtr GetSchedulerChannel() override
    {
        return SchedulerChannel_;
    }

    virtual INodeChannelFactoryPtr GetLightChannelFactory() override
    {
        return LightChannelFactory_;
    }

    virtual INodeChannelFactoryPtr GetHeavyChannelFactory() override
    {
        return HeavyChannelFactory_;
    }

    virtual NQueryClient::IExecutorPtr GetQueryExecutor() override
    {
        return QueryHelper_;
    }


    virtual TFuture<void> Terminate() override
    {
        TransactionManager_->AbortAll();

        auto error = TError("Client terminated");
        std::vector<TFuture<void>> asyncResults;

        for (auto kind : TEnumTraits<EMasterChannelKind>::GetDomainValues()) {
            for (const auto& pair : MasterChannels_[kind]) {
                auto channel = pair.second;
                asyncResults.push_back(channel->Terminate(error));
            }
        }
        asyncResults.push_back(SchedulerChannel_->Terminate(error));

        return Combine(asyncResults);
    }


    virtual TFuture<INativeTransactionPtr> StartNativeTransaction(
        ETransactionType type,
        const TTransactionStartOptions& options) override;
    virtual INativeTransactionPtr AttachNativeTransaction(
        const TTransactionId& transactionId,
        const TTransactionAttachOptions& options) override;

    virtual TFuture<ITransactionPtr> StartTransaction(
        ETransactionType type,
        const TTransactionStartOptions& options) override
    {
        return StartNativeTransaction(type, options).As<ITransactionPtr>();
    }

    virtual ITransactionPtr AttachTransaction(
        const TTransactionId& transactionId,
        const TTransactionAttachOptions& options) override
    {
        return AttachNativeTransaction(transactionId, options);
    }

#define DROP_BRACES(...) __VA_ARGS__
#define IMPLEMENT_OVERLOADED_METHOD(returnType, method, doMethod, signature, args) \
    virtual TFuture<returnType> method signature override \
    { \
        return Execute( \
            #method, \
            options, \
            BIND( \
                &TNativeClient::doMethod, \
                Unretained(this), \
                DROP_BRACES args)); \
    }

#define IMPLEMENT_METHOD(returnType, method, signature, args) \
    IMPLEMENT_OVERLOADED_METHOD(returnType, method, Do##method, signature, args)

    IMPLEMENT_METHOD(IRowsetPtr, LookupRows, (
        const TYPath& path,
        TNameTablePtr nameTable,
        const TSharedRange<NTableClient::TKey>& keys,
        const TLookupRowsOptions& options),
        (path, std::move(nameTable), std::move(keys), options))
    IMPLEMENT_METHOD(TSelectRowsResult, SelectRows, (
        const Stroka& query,
        const TSelectRowsOptions& options),
        (query, options))
    IMPLEMENT_METHOD(void, MountTable, (
        const TYPath& path,
        const TMountTableOptions& options),
        (path, options))
    IMPLEMENT_METHOD(void, UnmountTable, (
        const TYPath& path,
        const TUnmountTableOptions& options),
        (path, options))
    IMPLEMENT_METHOD(void, RemountTable, (
        const TYPath& path,
        const TRemountTableOptions& options),
        (path, options))
    IMPLEMENT_METHOD(void, FreezeTable, (
        const TYPath& path,
        const TFreezeTableOptions& options),
        (path, options))
    IMPLEMENT_METHOD(void, UnfreezeTable, (
        const TYPath& path,
        const TUnfreezeTableOptions& options),
        (path, options))
    IMPLEMENT_OVERLOADED_METHOD(void, ReshardTable, DoReshardTableWithPivotKeys, (
        const TYPath& path,
        const std::vector<NTableClient::TOwningKey>& pivotKeys,
        const TReshardTableOptions& options),
        (path, pivotKeys, options))
    IMPLEMENT_OVERLOADED_METHOD(void, ReshardTable, DoReshardTableWithTabletCount, (
        const TYPath& path,
        int tabletCount,
        const TReshardTableOptions& options),
        (path, tabletCount, options))
    IMPLEMENT_METHOD(void, AlterTable, (
        const TYPath& path,
        const TAlterTableOptions& options),
        (path, options))
    IMPLEMENT_METHOD(void, TrimTable, (
        const TYPath& path,
        int tabletIndex,
        i64 trimmedRowCount,
        const TTrimTableOptions& options),
        (path, tabletIndex, trimmedRowCount, options))
    IMPLEMENT_METHOD(void, EnableTableReplica, (
        const TTableReplicaId& replicaId,
        const TEnableTableReplicaOptions& options),
        (replicaId, options))
    IMPLEMENT_METHOD(void, DisableTableReplica, (
        const TTableReplicaId& replicaId,
        const TDisableTableReplicaOptions& options),
        (replicaId, options))


    IMPLEMENT_METHOD(TYsonString, GetNode, (
        const TYPath& path,
        const TGetNodeOptions& options),
        (path, options))
    IMPLEMENT_METHOD(void, SetNode, (
        const TYPath& path,
        const TYsonString& value,
        const TSetNodeOptions& options),
        (path, value, options))
    IMPLEMENT_METHOD(void, RemoveNode, (
        const TYPath& path,
        const TRemoveNodeOptions& options),
        (path, options))
    IMPLEMENT_METHOD(TYsonString, ListNode, (
        const TYPath& path,
        const TListNodeOptions& options),
        (path, options))
    IMPLEMENT_METHOD(TNodeId, CreateNode, (
        const TYPath& path,
        EObjectType type,
        const TCreateNodeOptions& options),
        (path, type, options))
    IMPLEMENT_METHOD(TLockId, LockNode, (
        const TYPath& path,
        ELockMode mode,
        const TLockNodeOptions& options),
        (path, mode, options))
    IMPLEMENT_METHOD(TNodeId, CopyNode, (
        const TYPath& srcPath,
        const TYPath& dstPath,
        const TCopyNodeOptions& options),
        (srcPath, dstPath, options))
    IMPLEMENT_METHOD(TNodeId, MoveNode, (
        const TYPath& srcPath,
        const TYPath& dstPath,
        const TMoveNodeOptions& options),
        (srcPath, dstPath, options))
    IMPLEMENT_METHOD(TNodeId, LinkNode, (
        const TYPath& srcPath,
        const TYPath& dstPath,
        const TLinkNodeOptions& options),
        (srcPath, dstPath, options))
    IMPLEMENT_METHOD(void, ConcatenateNodes, (
        const std::vector<TYPath>& srcPaths,
        const TYPath& dstPath,
        TConcatenateNodesOptions options),
        (srcPaths, dstPath, options))
    IMPLEMENT_METHOD(bool, NodeExists, (
        const TYPath& path,
        const TNodeExistsOptions& options),
        (path, options))


    IMPLEMENT_METHOD(TObjectId, CreateObject, (
        EObjectType type,
        const TCreateObjectOptions& options),
        (type, options))


    virtual IFileReaderPtr CreateFileReader(
        const TYPath& path,
        const TFileReaderOptions& options) override
    {
        return NApi::CreateFileReader(this, path, options);
    }

    virtual IFileWriterPtr CreateFileWriter(
        const TYPath& path,
        const TFileWriterOptions& options) override
    {
        return NApi::CreateFileWriter(this, path, options);
    }


    virtual IJournalReaderPtr CreateJournalReader(
        const TYPath& path,
        const TJournalReaderOptions& options) override
    {
        return NApi::CreateJournalReader(this, path, options);
    }

    virtual IJournalWriterPtr CreateJournalWriter(
        const TYPath& path,
        const TJournalWriterOptions& options) override
    {
        return NApi::CreateJournalWriter(this, path, options);
    }

    virtual TFuture<ISchemalessMultiChunkReaderPtr> CreateTableReader(
        const NYPath::TRichYPath& path,
        const TTableReaderOptions& options) override
    {
        return NApi::CreateTableReader(this, path, options);
    }

    IMPLEMENT_METHOD(void, AddMember, (
        const Stroka& group,
        const Stroka& member,
        const TAddMemberOptions& options),
        (group, member, options))
    IMPLEMENT_METHOD(void, RemoveMember, (
        const Stroka& group,
        const Stroka& member,
        const TRemoveMemberOptions& options),
        (group, member, options))
    IMPLEMENT_METHOD(TCheckPermissionResult, CheckPermission, (
        const Stroka& user,
        const TYPath& path,
        EPermission permission,
        const TCheckPermissionOptions& options),
        (user, path, permission, options))

    IMPLEMENT_METHOD(TOperationId, StartOperation, (
        EOperationType type,
        const TYsonString& spec,
        const TStartOperationOptions& options),
        (type, spec, options))
    IMPLEMENT_METHOD(void, AbortOperation, (
        const TOperationId& operationId,
        const TAbortOperationOptions& options),
        (operationId, options))
    IMPLEMENT_METHOD(void, SuspendOperation, (
        const TOperationId& operationId,
        const TSuspendOperationOptions& options),
        (operationId, options))
    IMPLEMENT_METHOD(void, ResumeOperation, (
        const TOperationId& operationId,
        const TResumeOperationOptions& options),
        (operationId, options))
    IMPLEMENT_METHOD(void, CompleteOperation, (
        const TOperationId& operationId,
        const TCompleteOperationOptions& options),
        (operationId, options))

    IMPLEMENT_METHOD(void, DumpJobContext, (
        const TJobId& jobId,
        const TYPath& path,
        const TDumpJobContextOptions& options),
        (jobId, path, options))
    IMPLEMENT_METHOD(TSharedRef, GetJobStderr, (
        const TOperationId& operationId,
        const TJobId& jobId,
        const TGetJobStderrOptions& options),
        (operationId, jobId, options))
    IMPLEMENT_METHOD(TYsonString, StraceJob, (
        const TJobId& jobId,
        const TStraceJobOptions& options),
        (jobId, options))
    IMPLEMENT_METHOD(void, SignalJob, (
        const TJobId& jobId,
        const Stroka& signalName,
        const TSignalJobOptions& options),
        (jobId, signalName, options))
    IMPLEMENT_METHOD(void, AbandonJob, (
        const TJobId& jobId,
        const TAbandonJobOptions& options),
        (jobId, options))
    IMPLEMENT_METHOD(TYsonString, PollJobShell, (
        const TJobId& jobId,
        const TYsonString& parameters,
        const TPollJobShellOptions& options),
        (jobId, parameters, options))
    IMPLEMENT_METHOD(void, AbortJob, (
        const TJobId& jobId,
        const TAbortJobOptions& options),
        (jobId, options))

#undef DROP_BRACES
#undef IMPLEMENT_METHOD

private:
    friend class TNativeTransaction;

    const INativeConnectionPtr Connection_;
    const TClientOptions Options_;

    TEnumIndexedVector<yhash_map<TCellTag, IChannelPtr>, EMasterChannelKind> MasterChannels_;
    IChannelPtr SchedulerChannel_;
    INodeChannelFactoryPtr LightChannelFactory_;
    INodeChannelFactoryPtr HeavyChannelFactory_;
    TTransactionManagerPtr TransactionManager_;
    TQueryHelperPtr QueryHelper_;
    IFunctionRegistryPtr FunctionRegistry_;
    std::unique_ptr<TSchedulerServiceProxy> SchedulerProxy_;
    std::unique_ptr<TJobProberServiceProxy> JobProberProxy_;

    TAsyncSemaphorePtr ConcurrentRequestsSemaphore_;

    NLogging::TLogger Logger = ApiLogger;


    template <class T>
    TFuture<T> Execute(
        const Stroka& commandName,
        const TTimeoutOptions& options,
        TCallback<T()> callback)
    {
        auto guard = TAsyncSemaphoreGuard::TryAcquire(ConcurrentRequestsSemaphore_);
        if (!guard) {
            return MakeFuture<T>(TError(EErrorCode::TooManyConcurrentRequests, "Too many concurrent requests"));
        }

        return
            BIND([commandName, callback = std::move(callback), this_ = MakeWeak(this), guard = std::move(guard)] () {
                auto client = this_.Lock();
                if (!client) {
                    THROW_ERROR_EXCEPTION("Client was abandoned");
                }
                auto& Logger = client->Logger;
                try {
                    LOG_DEBUG("Command started (Command: %v)", commandName);
                    TBox<T> result(callback);
                    LOG_DEBUG("Command completed (Command: %v)", commandName);
                    return result.Unwrap();
                } catch (const std::exception& ex) {
                    LOG_DEBUG(ex, "Command failed (Command: %v)", commandName);
                    throw;
                }
            })
            .AsyncVia(Connection_->GetLightInvoker())
            .Run()
            .WithTimeout(options.Timeout);
    }

    template <class T>
    auto CallAndRetryIfMetadataCacheIsInconsistent(T callback) -> decltype(callback())
    {
        int retryCount = 0;
        while (true) {
            TError error;

            try {
                return callback();
            } catch (const NYT::TErrorException& ex) {
                error = ex.Error();
            }

            auto config = Connection_->GetConfig();
            if (++retryCount <= config->TableMountInfoUpdateRetryCount) {
                bool retry = false;
                std::vector<NTabletClient::EErrorCode> retriableCodes = {
                    NTabletClient::EErrorCode::NoSuchTablet,
                    NTabletClient::EErrorCode::TabletNotMounted,
                    NTabletClient::EErrorCode::InvalidMountRevision};

                for (const auto& errCode : retriableCodes) {
                    if (auto err = error.FindMatching(errCode)) {
                        error = err.Get();
                        retry = true;
                        break;
                    }
                }

                if (retry) {
                    LOG_DEBUG(error, "Got error, will clear table mount cache and retry");
                    auto tabletId = error.Attributes().Get<TTabletId>("tablet_id");
                    auto tableMountCache = Connection_->GetTableMountCache();
                    auto tabletInfo = tableMountCache->FindTablet(tabletId);
                    if (tabletInfo) {
                        tableMountCache->InvalidateTablet(tabletInfo);
                        auto now = Now();
                        auto retryTime = tabletInfo->UpdateTime + config->TableMountInfoUpdateRetryPeriod;
                        if (retryTime > now) {
                            WaitFor(TDelayedExecutor::MakeDelayed(retryTime - now))
                                .ThrowOnError();
                        }
                    }
                    continue;
                }
            }

            THROW_ERROR error;
        }
    }

    TTableMountInfoPtr SyncGetTableInfo(const TYPath& path)
    {
        const auto& tableMountCache = Connection_->GetTableMountCache();
        return WaitFor(tableMountCache->GetTableInfo(path))
            .ValueOrThrow();
    }


    static void SetMutationId(IClientRequestPtr request, const TMutatingOptions& options)
    {
        NRpc::SetMutationId(request, options.GetOrGenerateMutationId(), options.Retry);
    }


    TTransactionId GetTransactionId(const TTransactionalOptions& options, bool allowNullTransaction)
    {
        auto transaction = GetTransaction(options, allowNullTransaction, true);
        return transaction ? transaction->GetId() : NullTransactionId;
    }

    NTransactionClient::TTransactionPtr GetTransaction(
        const TTransactionalOptions& options,
        bool allowNullTransaction,
        bool pingTransaction)
    {
        if (!options.TransactionId) {
            if (!allowNullTransaction) {
                THROW_ERROR_EXCEPTION("A valid master transaction is required");
            }
            return nullptr;
        }

        TTransactionAttachOptions attachOptions;
        attachOptions.Ping = pingTransaction;
        attachOptions.PingAncestors = options.PingAncestors;
        return TransactionManager_->Attach(options.TransactionId, attachOptions);
    }

    void SetTransactionId(
        IClientRequestPtr request,
        const TTransactionalOptions& options,
        bool allowNullTransaction)
    {
        NCypressClient::SetTransactionId(request, GetTransactionId(options, allowNullTransaction));
    }


    void SetPrerequisites(
        IClientRequestPtr request,
        const TPrerequisiteOptions& options)
    {
        if (options.PrerequisiteTransactionIds.empty() && options.PrerequisiteRevisions.empty()) {
            return;
        }

        auto* prerequisitesExt = request->Header().MutableExtension(TPrerequisitesExt::prerequisites_ext);
        for (const auto& id : options.PrerequisiteTransactionIds) {
            auto* prerequisiteTransaction = prerequisitesExt->add_transactions();
            ToProto(prerequisiteTransaction->mutable_transaction_id(), id);
        }
        for (const auto& revision : options.PrerequisiteRevisions) {
            auto* prerequisiteRevision = prerequisitesExt->add_revisions();
            prerequisiteRevision->set_path(revision->Path);
            ToProto(prerequisiteRevision->mutable_transaction_id(), revision->TransactionId);
            prerequisiteRevision->set_revision(revision->Revision);
        }
    }


    static void SetSuppressAccessTracking(
        IClientRequestPtr request,
        const TSuppressableAccessTrackingOptions& commandOptions)
    {
        if (commandOptions.SuppressAccessTracking) {
            NCypressClient::SetSuppressAccessTracking(request, true);
        }
        if (commandOptions.SuppressModificationTracking) {
            NCypressClient::SetSuppressModificationTracking(request, true);
        }
    }

    static void SetCachingHeader(
        IClientRequestPtr request,
        const TMasterReadOptions& options)
    {
        if (options.ReadFrom == EMasterChannelKind::Cache) {
            auto* cachingHeaderExt = request->Header().MutableExtension(NYTree::NProto::TCachingHeaderExt::caching_header_ext);
            cachingHeaderExt->set_success_expiration_time(ToProto(options.ExpireAfterSuccessfulUpdateTime));
            cachingHeaderExt->set_failure_expiration_time(ToProto(options.ExpireAfterFailedUpdateTime));
        }
    }

    template <class TProxy>
    std::unique_ptr<TProxy> CreateReadProxy(
        const TMasterReadOptions& options,
        TCellTag cellTag = PrimaryMasterCellTag)
    {
        auto channel = GetMasterChannelOrThrow(options.ReadFrom, cellTag);
        return std::make_unique<TProxy>(channel);
    }

    template <class TProxy>
    std::unique_ptr<TProxy> CreateWriteProxy(
        TCellTag cellTag = PrimaryMasterCellTag)
    {
        auto channel = GetMasterChannelOrThrow(EMasterChannelKind::Leader, cellTag);
        return std::make_unique<TProxy>(channel);
    }

    IChannelPtr GetTabletChannelOrThrow(const TTabletCellId& cellId) const
    {
        const auto& cellDirectory = Connection_->GetCellDirectory();
        auto channel = cellDirectory->GetChannelOrThrow(cellId);
        return CreateAuthenticatedChannel(std::move(channel), Options_.User);
    }


    class TTabletCellLookupSession
        : public TIntrinsicRefCounted
    {
    public:
        TTabletCellLookupSession(
            TNativeConnectionConfigPtr config,
            const TNetworkPreferenceList& networks,
            const TCellId& cellId,
            const TLookupRowsOptions& options,
            TTableMountInfoPtr tableInfo)
            : Config_(std::move(config))
            , Networks_(networks)
            , CellId_(cellId)
            , Options_(options)
            , TableInfo_(std::move(tableInfo))
        { }

        void AddKey(int index, TTabletInfoPtr tabletInfo, NTableClient::TKey key)
        {
            if (Batches_.empty() ||
                Batches_.back()->TabletInfo->TabletId != tabletInfo->TabletId ||
                Batches_.back()->Indexes.size() >= Config_->MaxRowsPerReadRequest)
            {
                Batches_.emplace_back(new TBatch(std::move(tabletInfo)));
            }

            auto& batch = Batches_.back();
            batch->Indexes.push_back(index);
            batch->Keys.push_back(key);
        }

        TFuture<void> Invoke(IChannelFactoryPtr channelFactory, TCellDirectoryPtr cellDirectory)
        {
            // Do all the heavy lifting here.
            for (auto& batch : Batches_) {
                TReqLookupRows req;
                if (!Options_.ColumnFilter.All) {
                    ToProto(req.mutable_column_filter()->mutable_indexes(), Options_.ColumnFilter.Indexes);
                }

                TWireProtocolWriter writer;
                writer.WriteCommand(EWireProtocolCommand::LookupRows);
                writer.WriteMessage(req);
                writer.WriteSchemafulRowset(batch->Keys, nullptr);

                auto chunkedData = writer.Flush();

                batch->RequestData = NCompression::CompressWithEnvelope(
                    chunkedData,
                    Config_->LookupRequestCodec);
            }

            const auto& cellDescriptor = cellDirectory->GetDescriptorOrThrow(CellId_);
            auto channel = CreateTabletReadChannel(
                channelFactory,
                cellDescriptor,
                Options_,
                Networks_);

            InvokeProxy_ = std::make_unique<TQueryServiceProxy>(std::move(channel));
            InvokeProxy_->SetDefaultTimeout(Config_->LookupTimeout);
            InvokeProxy_->SetDefaultRequestAck(false);

            InvokeNextBatch();
            return InvokePromise_;
        }

        void ParseResponse(
            std::vector<TUnversionedRow>* resultRows,
            std::vector<std::unique_ptr<TWireProtocolReader>>* readers)
        {
            auto schemaData = TWireProtocolReader::GetSchemaData(TableInfo_->Schemas[ETableSchemaKind::Primary], Options_.ColumnFilter);
            for (const auto& batch : Batches_) {
                auto data = NCompression::DecompressWithEnvelope(batch->Response->Attachments());
                auto reader = std::make_unique<TWireProtocolReader>(data);
                for (int index = 0; index < batch->Keys.size(); ++index) {
                    auto row = reader->ReadSchemafulRow(schemaData);
                    (*resultRows)[batch->Indexes[index]] = row;
                }
                readers->push_back(std::move(reader));
            }
        }

    private:
        const TNativeClientPtr Client_;
        const TNativeConnectionConfigPtr Config_;
        const TNetworkPreferenceList Networks_;
        const TCellId CellId_;
        const TLookupRowsOptions Options_;
        const TTableMountInfoPtr TableInfo_;

        struct TBatch
        {
            explicit TBatch(TTabletInfoPtr tabletInfo)
                : TabletInfo(std::move(tabletInfo))
            { }

            TTabletInfoPtr TabletInfo;
            std::vector<int> Indexes;
            std::vector<NTableClient::TKey> Keys;
            std::vector<TSharedRef> RequestData;
            TQueryServiceProxy::TRspReadPtr Response;
        };

        std::vector<std::unique_ptr<TBatch>> Batches_;
        std::unique_ptr<TQueryServiceProxy> InvokeProxy_;
        int InvokeBatchIndex_ = 0;
        TPromise<void> InvokePromise_ = NewPromise<void>();


        void InvokeNextBatch()
        {
            if (InvokeBatchIndex_ >= Batches_.size()) {
                InvokePromise_.Set(TError());
                return;
            }

            const auto& batch = Batches_[InvokeBatchIndex_];

            auto req = InvokeProxy_->Read();
            ToProto(req->mutable_tablet_id(), batch->TabletInfo->TabletId);
            req->set_mount_revision(batch->TabletInfo->MountRevision);
            req->set_timestamp(Options_.Timestamp);
            req->set_response_codec(static_cast<int>(Config_->LookupResponseCodec));
            req->Attachments() = std::move(batch->RequestData);

            req->Invoke().Subscribe(
                BIND(&TTabletCellLookupSession::OnResponse, MakeStrong(this)));
        }

        void OnResponse(const TQueryServiceProxy::TErrorOrRspReadPtr& rspOrError)
        {
            if (rspOrError.IsOK()) {
                Batches_[InvokeBatchIndex_]->Response = rspOrError.Value();
                ++InvokeBatchIndex_;
                InvokeNextBatch();
            } else {
                InvokePromise_.Set(rspOrError);
            }
        }

    };

    using TTabletCellLookupSessionPtr = TIntrusivePtr<TTabletCellLookupSession>;

    IRowsetPtr DoLookupRows(
        const TYPath& path,
        TNameTablePtr nameTable,
        const TSharedRange<NTableClient::TKey>& keys,
        const TLookupRowsOptions& options)
    {
        return CallAndRetryIfMetadataCacheIsInconsistent([&] () {
            return DoLookupRowsOnce(path, nameTable, keys, options);
        });
    }

    IRowsetPtr DoLookupRowsOnce(
        const TYPath& path,
        TNameTablePtr nameTable,
        const TSharedRange<NTableClient::TKey>& keys,
        TLookupRowsOptions options)
    {
        auto tableInfo = SyncGetTableInfo(path);
        tableInfo->ValidateDynamic();
        tableInfo->ValidateSorted();
        tableInfo->ValidateNotReplicated();

        const auto& schema = tableInfo->Schemas[ETableSchemaKind::Primary];
        auto idMapping = BuildColumnIdMapping(schema, nameTable);

        for (auto& index : options.ColumnFilter.Indexes) {
            if (index < 0 || index >= idMapping.size()) {
                THROW_ERROR_EXCEPTION("Column filter contains invalid index: actual %v, expected in range [0, %v]",
                    index,
                    idMapping.size() - 1);
            }
            if (idMapping[index] == -1) {
                THROW_ERROR_EXCEPTION("Invalid column %Qv in column filter",
                    nameTable->GetName(index));
            }

            index = idMapping[index];
        }

        auto resultSchema = tableInfo->Schemas[ETableSchemaKind::Primary].Filter(options.ColumnFilter);

        // NB: The server-side requires the keys to be sorted.
        std::vector<std::pair<NTableClient::TKey, int>> sortedKeys;
        sortedKeys.reserve(keys.Size());

        auto rowBuffer = New<TRowBuffer>(TLookupRowsBufferTag());
        auto evaluatorCache = Connection_->GetColumnEvaluatorCache();
        auto evaluator = tableInfo->NeedKeyEvaluation ? evaluatorCache->Find(schema) : nullptr;

        for (int index = 0; index < keys.Size(); ++index) {
            ValidateClientKey(keys[index], schema, idMapping, nameTable);
            auto capturedKey = rowBuffer->CaptureAndPermuteRow(keys[index], schema, idMapping);

            if (evaluator) {
                evaluator->EvaluateKeys(capturedKey, rowBuffer);
            }

            sortedKeys.push_back(std::make_pair(capturedKey, index));
        }

        std::sort(sortedKeys.begin(), sortedKeys.end());
        std::vector<int> keyIndexToResultIndex(keys.Size());
        int currentResultIndex = -1;

        yhash_map<TCellId, TTabletCellLookupSessionPtr> cellIdToSession;

        for (int index = 0; index < sortedKeys.size(); ++index) {
            if (index == 0 || sortedKeys[index].first != sortedKeys[index - 1].first) {
                auto key = sortedKeys[index].first;
                auto tabletInfo = GetSortedTabletForRow(tableInfo, key);
                const auto& cellId = tabletInfo->CellId;
                auto it = cellIdToSession.find(cellId);
                if (it == cellIdToSession.end()) {
                    it = cellIdToSession.insert(std::make_pair(
                        cellId,
                        New<TTabletCellLookupSession>(
                            Connection_->GetConfig(),
                            Connection_->GetNetworks(),
                            cellId,
                            options,
                            tableInfo)))
                        .first;
                }
                const auto& session = it->second;
                session->AddKey(++currentResultIndex, std::move(tabletInfo), key);
            }

            keyIndexToResultIndex[sortedKeys[index].second] = currentResultIndex;
        }

        std::vector<TFuture<void>> asyncResults;
        for (const auto& pair : cellIdToSession) {
            const auto& session = pair.second;
            asyncResults.push_back(session->Invoke(
                GetHeavyChannelFactory(),
                Connection_->GetCellDirectory()));
        }

        WaitFor(Combine(asyncResults))
            .ThrowOnError();

        std::vector<TUnversionedRow> uniqueResultRows;
        uniqueResultRows.resize(currentResultIndex + 1);

        std::vector<std::unique_ptr<TWireProtocolReader>> readers;

        for (const auto& pair : cellIdToSession) {
            const auto& session = pair.second;
            session->ParseResponse(&uniqueResultRows, &readers);
        }

        std::vector<TUnversionedRow> resultRows;
        resultRows.resize(keys.Size());

        for (int index = 0; index < keys.Size(); ++index) {
            resultRows[index] = uniqueResultRows[keyIndexToResultIndex[index]];
        }

        if (!options.KeepMissingRows) {
            resultRows.erase(
                std::remove_if(
                    resultRows.begin(),
                    resultRows.end(),
                    [] (TUnversionedRow row) {
                        return !static_cast<bool>(row);
                    }),
                resultRows.end());
        }

        return CreateRowset(
            std::move(readers),
            resultSchema,
            std::move(resultRows));
    }

    TSelectRowsResult DoSelectRows(
        const Stroka& queryString,
        const TSelectRowsOptions& options)
    {
        return CallAndRetryIfMetadataCacheIsInconsistent([&] () {
            return DoSelectRowsOnce(queryString, options);
        });
    }

    TSelectRowsResult DoSelectRowsOnce(
        const Stroka& queryString,
        const TSelectRowsOptions& options)
    {
        auto inputRowLimit = options.InputRowLimit.Get(Connection_->GetConfig()->DefaultInputRowLimit);
        auto outputRowLimit = options.OutputRowLimit.Get(Connection_->GetConfig()->DefaultOutputRowLimit);

        auto externalCGInfo = New<TExternalCGInfo>();
        auto fetchFunctions = [&] (const std::vector<Stroka>& names, const TTypeInferrerMapPtr& typeInferrers) {
            MergeFrom(typeInferrers.Get(), *BuiltinTypeInferrersMap);

            std::vector<Stroka> externalNames;
            for (const auto& name : names) {
                auto found = typeInferrers->find(name);
                if (found == typeInferrers->end()) {
                    externalNames.push_back(name);
                }
            }

            auto descriptors = WaitFor(FunctionRegistry_->FetchFunctions(externalNames))
                .ValueOrThrow();

            AppendUdfDescriptors(typeInferrers, externalCGInfo, externalNames, descriptors);
        };

        TQueryPtr query;
        TDataRanges dataSource;
        std::tie(query, dataSource) = PreparePlanFragment(
            QueryHelper_.Get(),
            queryString,
            fetchFunctions,
            inputRowLimit,
            outputRowLimit,
            options.Timestamp);

        TQueryOptions queryOptions;
        queryOptions.Timestamp = options.Timestamp;
        queryOptions.RangeExpansionLimit = options.RangeExpansionLimit;
        queryOptions.VerboseLogging = options.VerboseLogging;
        queryOptions.EnableCodeCache = options.EnableCodeCache;
        queryOptions.MaxSubqueries = options.MaxSubqueries;
        queryOptions.WorkloadDescriptor = options.WorkloadDescriptor;

        ISchemafulWriterPtr writer;
        TFuture<IRowsetPtr> asyncRowset;
        std::tie(writer, asyncRowset) = CreateSchemafulRowsetWriter(query->GetTableSchema());

        auto statistics = WaitFor(QueryHelper_->Execute(
            query,
            externalCGInfo,
            dataSource,
            writer,
            queryOptions))
            .ValueOrThrow();

        auto rowset = WaitFor(asyncRowset)
            .ValueOrThrow();

        if (options.FailOnIncompleteResult) {
            if (statistics.IncompleteInput) {
                THROW_ERROR_EXCEPTION("Query terminated prematurely due to excessive input; consider rewriting your query or changing input limit")
                    << TErrorAttribute("input_row_limit", inputRowLimit);
            }
            if (statistics.IncompleteOutput) {
                THROW_ERROR_EXCEPTION("Query terminated prematurely due to excessive output; consider rewriting your query or changing output limit")
                    << TErrorAttribute("output_row_limit", outputRowLimit);
            }
        }

        return TSelectRowsResult{rowset, statistics};
    }

    void DoMountTable(
        const TYPath& path,
        const TMountTableOptions& options)
    {
        auto req = TTableYPathProxy::Mount(path);
        if (options.FirstTabletIndex) {
            req->set_first_tablet_index(*options.FirstTabletIndex);
        }
        if (options.LastTabletIndex) {
            req->set_last_tablet_index(*options.LastTabletIndex);
        }
        if (options.CellId) {
            ToProto(req->mutable_cell_id(), options.CellId);
        }
        req->set_freeze(options.Freeze);

        auto proxy = CreateWriteProxy<TObjectServiceProxy>();
        WaitFor(proxy->Execute(req))
            .ThrowOnError();
    }

    void DoUnmountTable(
        const TYPath& path,
        const TUnmountTableOptions& options)
    {
        auto req = TTableYPathProxy::Unmount(path);
        if (options.FirstTabletIndex) {
            req->set_first_tablet_index(*options.FirstTabletIndex);
        }
        if (options.LastTabletIndex) {
            req->set_last_tablet_index(*options.LastTabletIndex);
        }
        req->set_force(options.Force);

        auto proxy = CreateWriteProxy<TObjectServiceProxy>();
        WaitFor(proxy->Execute(req))
            .ThrowOnError();
    }

    void DoRemountTable(
        const TYPath& path,
        const TRemountTableOptions& options)
    {
        auto req = TTableYPathProxy::Remount(path);
        if (options.FirstTabletIndex) {
            req->set_first_tablet_index(*options.FirstTabletIndex);
        }
        if (options.LastTabletIndex) {
            req->set_first_tablet_index(*options.LastTabletIndex);
        }

        auto proxy = CreateWriteProxy<TObjectServiceProxy>();
        WaitFor(proxy->Execute(req))
            .ThrowOnError();
    }

    void DoFreezeTable(
        const TYPath& path,
        const TFreezeTableOptions& options)
    {
        auto req = TTableYPathProxy::Freeze(path);
        if (options.FirstTabletIndex) {
            req->set_first_tablet_index(*options.FirstTabletIndex);
        }
        if (options.LastTabletIndex) {
            req->set_last_tablet_index(*options.LastTabletIndex);
        }

        auto proxy = CreateWriteProxy<TObjectServiceProxy>();
        WaitFor(proxy->Execute(req))
            .ThrowOnError();
    }

    void DoUnfreezeTable(
        const TYPath& path,
        const TUnfreezeTableOptions& options)
    {
        auto req = TTableYPathProxy::Unfreeze(path);
        if (options.FirstTabletIndex) {
            req->set_first_tablet_index(*options.FirstTabletIndex);
        }
        if (options.LastTabletIndex) {
            req->set_last_tablet_index(*options.LastTabletIndex);
        }

        auto proxy = CreateWriteProxy<TObjectServiceProxy>();
        WaitFor(proxy->Execute(req))
            .ThrowOnError();
    }

    TTableYPathProxy::TReqReshardPtr MakeReshardRequest(
        const TYPath& path,
        const TReshardTableOptions& options)
    {
        auto req = TTableYPathProxy::Reshard(path);
        if (options.FirstTabletIndex) {
            req->set_first_tablet_index(*options.FirstTabletIndex);
        }
        if (options.LastTabletIndex) {
            req->set_last_tablet_index(*options.LastTabletIndex);
        }
        return req;
    }

    void DoReshardTableWithPivotKeys(
        const TYPath& path,
        const std::vector<NTableClient::TOwningKey>& pivotKeys,
        const TReshardTableOptions& options)
    {
        auto req = MakeReshardRequest(path, options);
        ToProto(req->mutable_pivot_keys(), pivotKeys);
        req->set_tablet_count(pivotKeys.size());

        auto proxy = CreateWriteProxy<TObjectServiceProxy>();
        WaitFor(proxy->Execute(req))
            .ThrowOnError();
    }

    void DoReshardTableWithTabletCount(
        const TYPath& path,
        int tabletCount,
        const TReshardTableOptions& options)
    {
        auto req = MakeReshardRequest(path, options);
        req->set_tablet_count(tabletCount);

        auto proxy = CreateWriteProxy<TObjectServiceProxy>();
        WaitFor(proxy->Execute(req))
            .ThrowOnError();
    }

    void DoAlterTable(
        const TYPath& path,
        const TAlterTableOptions& options)
    {
        auto req = TTableYPathProxy::Alter(path);
        SetTransactionId(req, options, true);
        SetMutationId(req, options);

        if (options.Schema) {
            ToProto(req->mutable_schema(), *options.Schema);
        }
        if (options.Dynamic) {
            req->set_dynamic(*options.Dynamic);
        }

        auto proxy = CreateWriteProxy<TObjectServiceProxy>();
        WaitFor(proxy->Execute(req))
            .ThrowOnError();
    }

    void DoTrimTable(
        const TYPath& path,
        int tabletIndex,
        i64 trimmedRowCount,
        const TTrimTableOptions& options)
    {
        auto tableMountCache = Connection_->GetTableMountCache();
        auto tableInfo = WaitFor(tableMountCache->GetTableInfo(path))
            .ValueOrThrow();

        tableInfo->ValidateDynamic();
        tableInfo->ValidateOrdered();

        if (tabletIndex < 0 || tabletIndex >= tableInfo->Tablets.size()) {
            THROW_ERROR_EXCEPTION("Invalid tablet index: expected in range [0,%v], got %v",
                tableInfo->Tablets.size(),
                tabletIndex);
        }

        const auto& tabletInfo = tableInfo->Tablets[tabletIndex];

        auto channel = GetTabletChannelOrThrow(tabletInfo->CellId);

        TTabletServiceProxy proxy(channel);
        proxy.SetDefaultTimeout(Connection_->GetConfig()->WriteTimeout);

        auto req = proxy.Trim();
        ToProto(req->mutable_tablet_id(), tabletInfo->TabletId);
        req->set_mount_revision(tabletInfo->MountRevision);
        req->set_trimmed_row_count(trimmedRowCount);

        WaitFor(req->Invoke())
            .ValueOrThrow();
    }

    void DoEnableTableReplica(
        const TTableReplicaId& replicaId,
        const TEnableTableReplicaOptions& options)
    {
        auto req = TTableReplicaYPathProxy::Enable(FromObjectId(replicaId));
        auto proxy = CreateWriteProxy<TObjectServiceProxy>();
        WaitFor(proxy->Execute(req))
            .ThrowOnError();
    }

    void DoDisableTableReplica(
        const TTableReplicaId& replicaId,
        const TDisableTableReplicaOptions& options)
    {
        auto req = TTableReplicaYPathProxy::Disable(FromObjectId(replicaId));
        auto proxy = CreateWriteProxy<TObjectServiceProxy>();
        WaitFor(proxy->Execute(req))
            .ThrowOnError();
    }

    TYsonString DoGetNode(
        const TYPath& path,
        const TGetNodeOptions& options)
    {
        auto req = TYPathProxy::Get(path);
        SetTransactionId(req, options, true);
        SetSuppressAccessTracking(req, options);
        SetCachingHeader(req, options);

        if (options.Attributes) {
            ToProto(req->mutable_attributes()->mutable_keys(), *options.Attributes);
        }
        if (options.MaxSize) {
            req->set_limit(*options.MaxSize);
        }
        if (options.Options) {
            ToProto(req->mutable_options(), *options.Options);
        }

        auto proxy = CreateReadProxy<TObjectServiceProxy>(options);
        auto rsp = WaitFor(proxy->Execute(req))
            .ValueOrThrow();

        return TYsonString(rsp->value());
    }

    void DoSetNode(
        const TYPath& path,
        const TYsonString& value,
        const TSetNodeOptions& options)
    {
        auto proxy = CreateWriteProxy<TObjectServiceProxy>();
        auto batchReq = proxy->ExecuteBatch();
        SetPrerequisites(batchReq, options);

        auto req = TYPathProxy::Set(path);
        SetTransactionId(req, options, true);
        SetMutationId(req, options);

        // Binarize the value.
        TStringStream stream;
        TBufferedBinaryYsonWriter writer(&stream, EYsonType::Node, false);
        YCHECK(value.GetType() == EYsonType::Node);
        writer.OnRaw(value.Data(), EYsonType::Node);
        writer.Flush();
        req->set_value(stream.Str());

        batchReq->AddRequest(req);

        auto batchRsp = WaitFor(batchReq->Invoke())
            .ValueOrThrow();
        batchRsp->GetResponse<TYPathProxy::TRspSet>(0)
            .ThrowOnError();
    }

    void DoRemoveNode(
        const TYPath& path,
        const TRemoveNodeOptions& options)
    {
        auto proxy = CreateWriteProxy<TObjectServiceProxy>();
        auto batchReq = proxy->ExecuteBatch();
        SetPrerequisites(batchReq, options);

        auto req = TYPathProxy::Remove(path);
        SetTransactionId(req, options, true);
        SetMutationId(req, options);
        req->set_recursive(options.Recursive);
        req->set_force(options.Force);
        batchReq->AddRequest(req);

        auto batchRsp = WaitFor(batchReq->Invoke())
            .ValueOrThrow();
        batchRsp->GetResponse<TYPathProxy::TRspRemove>(0)
            .ThrowOnError();
    }

    TYsonString DoListNode(
        const TYPath& path,
        const TListNodeOptions& options)
    {
        auto req = TYPathProxy::List(path);
        SetTransactionId(req, options, true);
        SetSuppressAccessTracking(req, options);
        SetCachingHeader(req, options);

        if (options.Attributes) {
            ToProto(req->mutable_attributes()->mutable_keys(), *options.Attributes);
        }
        if (options.MaxSize) {
            req->set_limit(*options.MaxSize);
        }

        auto proxy = CreateReadProxy<TObjectServiceProxy>(options);
        auto rsp = WaitFor(proxy->Execute(req))
            .ValueOrThrow();
        return TYsonString(rsp->value());
    }

    TNodeId DoCreateNode(
        const TYPath& path,
        EObjectType type,
        const TCreateNodeOptions& options)
    {
        auto proxy = CreateWriteProxy<TObjectServiceProxy>();
        auto batchReq = proxy->ExecuteBatch();
        SetPrerequisites(batchReq, options);

        auto req = TCypressYPathProxy::Create(path);
        SetTransactionId(req, options, true);
        SetMutationId(req, options);
        req->set_type(static_cast<int>(type));
        req->set_recursive(options.Recursive);
        req->set_ignore_existing(options.IgnoreExisting);
        req->set_force(options.Force);
        if (options.Attributes) {
            ToProto(req->mutable_node_attributes(), *options.Attributes);
        }
        batchReq->AddRequest(req);

        auto batchRsp = WaitFor(batchReq->Invoke())
            .ValueOrThrow();
        auto rsp = batchRsp->GetResponse<TCypressYPathProxy::TRspCreate>(0)
            .ValueOrThrow();
        return FromProto<TNodeId>(rsp->node_id());
    }

    TLockId DoLockNode(
        const TYPath& path,
        ELockMode mode,
        const TLockNodeOptions& options)
    {
        auto proxy = CreateWriteProxy<TObjectServiceProxy>();
        auto batchReq = proxy->ExecuteBatch();
        SetPrerequisites(batchReq, options);

        auto req = TCypressYPathProxy::Lock(path);
        SetTransactionId(req, options, false);
        SetMutationId(req, options);
        req->set_mode(static_cast<int>(mode));
        req->set_waitable(options.Waitable);
        if (options.ChildKey) {
            req->set_child_key(*options.ChildKey);
        }
        if (options.AttributeKey) {
            req->set_attribute_key(*options.AttributeKey);
        }
        batchReq->AddRequest(req);

        auto batchRsp = WaitFor(batchReq->Invoke())
            .ValueOrThrow();
        auto rsp = batchRsp->GetResponse<TCypressYPathProxy::TRspLock>(0)
            .ValueOrThrow();
        return FromProto<TLockId>(rsp->lock_id());
    }

    TNodeId DoCopyNode(
        const TYPath& srcPath,
        const TYPath& dstPath,
        const TCopyNodeOptions& options)
    {
        auto proxy = CreateWriteProxy<TObjectServiceProxy>();
        auto batchReq = proxy->ExecuteBatch();
        SetPrerequisites(batchReq, options);

        auto req = TCypressYPathProxy::Copy(dstPath);
        SetTransactionId(req, options, true);
        SetMutationId(req, options);
        req->set_source_path(srcPath);
        req->set_preserve_account(options.PreserveAccount);
        req->set_recursive(options.Recursive);
        req->set_force(options.Force);
        batchReq->AddRequest(req);

        auto batchRsp = WaitFor(batchReq->Invoke())
            .ValueOrThrow();
        auto rsp = batchRsp->GetResponse<TCypressYPathProxy::TRspCopy>(0)
            .ValueOrThrow();
        return FromProto<TNodeId>(rsp->node_id());
    }

    TNodeId DoMoveNode(
        const TYPath& srcPath,
        const TYPath& dstPath,
        const TMoveNodeOptions& options)
    {
        auto proxy = CreateWriteProxy<TObjectServiceProxy>();
        auto batchReq = proxy->ExecuteBatch();
        SetPrerequisites(batchReq, options);

        auto req = TCypressYPathProxy::Copy(dstPath);
        SetTransactionId(req, options, true);
        SetMutationId(req, options);
        req->set_source_path(srcPath);
        req->set_preserve_account(options.PreserveAccount);
        req->set_remove_source(true);
        req->set_recursive(options.Recursive);
        req->set_force(options.Force);
        batchReq->AddRequest(req);

        auto batchRsp = WaitFor(batchReq->Invoke())
            .ValueOrThrow();
        auto rsp = batchRsp->GetResponse<TCypressYPathProxy::TRspCopy>(0)
            .ValueOrThrow();
        return FromProto<TNodeId>(rsp->node_id());
    }

    TNodeId DoLinkNode(
        const TYPath& srcPath,
        const TYPath& dstPath,
        const TLinkNodeOptions& options)
    {
        auto proxy = CreateWriteProxy<TObjectServiceProxy>();
        auto batchReq = proxy->ExecuteBatch();
        SetPrerequisites(batchReq, options);

        auto req = TCypressYPathProxy::Create(dstPath);
        req->set_type(static_cast<int>(EObjectType::Link));
        req->set_recursive(options.Recursive);
        req->set_ignore_existing(options.IgnoreExisting);
        req->set_force(options.Force);
        SetTransactionId(req, options, true);
        SetMutationId(req, options);
        auto attributes = options.Attributes ? ConvertToAttributes(options.Attributes.get()) : CreateEphemeralAttributes();
        attributes->Set("target_path", srcPath);
        ToProto(req->mutable_node_attributes(), *attributes);
        batchReq->AddRequest(req);

        auto batchRsp = WaitFor(batchReq->Invoke())
            .ValueOrThrow();
        auto rsp = batchRsp->GetResponse<TCypressYPathProxy::TRspCreate>(0)
            .ValueOrThrow();
        return FromProto<TNodeId>(rsp->node_id());
    }

    void DoConcatenateNodes(
        const std::vector<TYPath>& srcPaths,
        const TYPath& dstPath,
        TConcatenateNodesOptions options)
    {
        if (options.Retry) {
            THROW_ERROR_EXCEPTION("\"concatenate\" command is not retriable");
        }

        using NChunkClient::NProto::TDataStatistics;

        try {
            // Get objects ids.
            std::vector<TObjectId> srcIds;
            TCellTagList srcCellTags;
            TObjectId dstId;
            TCellTag dstCellTag;
            {
                auto proxy = CreateWriteProxy<TObjectServiceProxy>();
                auto batchReq = proxy->ExecuteBatch();

                for (const auto& path : srcPaths) {
                    auto req = TObjectYPathProxy::GetBasicAttributes(path);
                    SetTransactionId(req, options, true);
                    batchReq->AddRequest(req, "get_src_attributes");
                }
                {
                    auto req = TObjectYPathProxy::GetBasicAttributes(dstPath);
                    SetTransactionId(req, options, true);
                    batchReq->AddRequest(req, "get_dst_attributes");
                }

                auto batchRspOrError = WaitFor(batchReq->Invoke());
                THROW_ERROR_EXCEPTION_IF_FAILED(batchRspOrError, "Error getting basic attributes of inputs and outputs");
                const auto& batchRsp = batchRspOrError.Value();

                TNullable<EObjectType> commonType;
                TNullable<Stroka> pathWithCommonType;
                auto checkType = [&] (EObjectType type, const TYPath& path) {
                    if (type != EObjectType::Table && type != EObjectType::File) {
                        THROW_ERROR_EXCEPTION("Type of %v must be either %Qlv or %Qlv",
                            path,
                            EObjectType::Table,
                            EObjectType::File);
                    }
                    if (commonType && *commonType != type) {
                        THROW_ERROR_EXCEPTION("Type of %v (%Qlv) must be the same as type of %v (%Qlv)",
                            path,
                            type,
                            *pathWithCommonType,
                            *commonType);
                    }
                    commonType = type;
                    pathWithCommonType = path;
                };

                {
                    auto rspsOrError = batchRsp->GetResponses<TObjectYPathProxy::TRspGetBasicAttributes>("get_src_attributes");
                    for (int srcIndex = 0; srcIndex < srcPaths.size(); ++srcIndex) {
                        const auto& srcPath = srcPaths[srcIndex];
                        THROW_ERROR_EXCEPTION_IF_FAILED(rspsOrError[srcIndex], "Error getting attributes of %v", srcPath);
                        const auto& rsp = rspsOrError[srcIndex].Value();

                        auto id = FromProto<TObjectId>(rsp->object_id());
                        srcIds.push_back(id);
                        srcCellTags.push_back(rsp->cell_tag());
                        checkType(TypeFromId(id), srcPath);
                    }
                }

                {
                    auto rspsOrError = batchRsp->GetResponses<TObjectYPathProxy::TRspGetBasicAttributes>("get_dst_attributes");
                    THROW_ERROR_EXCEPTION_IF_FAILED(rspsOrError[0], "Error getting attributes of %v", dstPath);
                    const auto& rsp = rspsOrError[0].Value();

                    dstId = FromProto<TObjectId>(rsp->object_id());
                    dstCellTag = rsp->cell_tag();
                    checkType(TypeFromId(dstId), dstPath);
                }
            }

            auto dstIdPath = FromObjectId(dstId);

            // Get source chunk ids.
            // Maps src index -> list of chunk ids for this src.
            std::vector<std::vector<TChunkId>> groupedChunkIds(srcPaths.size());
            {
                yhash_map<TCellTag, std::vector<int>> cellTagToIndexes;
                for (int srcIndex = 0; srcIndex < srcCellTags.size(); ++srcIndex) {
                    cellTagToIndexes[srcCellTags[srcIndex]].push_back(srcIndex);
                }

                for (const auto& pair : cellTagToIndexes) {
                    auto srcCellTag = pair.first;
                    const auto& srcIndexes = pair.second;

                    auto proxy = CreateWriteProxy<TObjectServiceProxy>(srcCellTag);
                    auto batchReq = proxy->ExecuteBatch();

                    for (int localIndex = 0; localIndex < srcIndexes.size(); ++localIndex) {
                        int srcIndex = srcIndexes[localIndex];
                        auto req = TChunkOwnerYPathProxy::Fetch(FromObjectId(srcIds[srcIndex]));
                        SetTransactionId(req, options, true);
                        ToProto(req->mutable_ranges(), std::vector<TReadRange>{TReadRange()});
                        batchReq->AddRequest(req, "fetch");
                    }

                    auto batchRspOrError = WaitFor(batchReq->Invoke());
                    THROW_ERROR_EXCEPTION_IF_FAILED(batchRspOrError, "Error fetching inputs");

                    const auto& batchRsp = batchRspOrError.Value();
                    auto rspsOrError = batchRsp->GetResponses<TChunkOwnerYPathProxy::TRspFetch>("fetch");
                    for (int localIndex = 0; localIndex < srcIndexes.size(); ++localIndex) {
                        int srcIndex = srcIndexes[localIndex];
                        const auto& rspOrError = rspsOrError[localIndex];
                        const auto& path = srcPaths[srcIndex];
                        THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error fetching %v", path);
                        const auto& rsp = rspOrError.Value();

                        for (const auto& chunk : rsp->chunks()) {
                            groupedChunkIds[srcIndex].push_back(FromProto<TChunkId>(chunk.chunk_id()));
                        }
                    }
                }
            }

            // Begin upload.
            TTransactionId uploadTransactionId;
            {
                auto proxy = CreateWriteProxy<TObjectServiceProxy>();

                auto req = TChunkOwnerYPathProxy::BeginUpload(dstIdPath);
                req->set_update_mode(static_cast<int>(options.Append ? EUpdateMode::Append : EUpdateMode::Overwrite));
                req->set_lock_mode(static_cast<int>(options.Append ? ELockMode::Shared : ELockMode::Exclusive));
                req->set_upload_transaction_title(Format("Concatenating %v to %v",
                    srcPaths,
                    dstPath));
                // NB: Replicate upload transaction to each secondary cell since we have
                // no idea as of where the chunks we're about to attach may come from.
                ToProto(req->mutable_upload_transaction_secondary_cell_tags(), Connection_->GetSecondaryMasterCellTags());
                req->set_upload_transaction_timeout(ToProto(Connection_->GetConfig()->TransactionManager->DefaultTransactionTimeout));
                NRpc::GenerateMutationId(req);
                SetTransactionId(req, options, true);

                auto rspOrError = WaitFor(proxy->Execute(req));
                THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error starting upload to %v", dstPath);
                const auto& rsp = rspOrError.Value();

                uploadTransactionId = FromProto<TTransactionId>(rsp->upload_transaction_id());
            }

            NTransactionClient::TTransactionAttachOptions attachOptions;
            attachOptions.PingAncestors = options.PingAncestors;
            attachOptions.AutoAbort = true;
            auto uploadTransaction = TransactionManager_->Attach(uploadTransactionId, attachOptions);

            // Flatten chunk ids.
            std::vector<TChunkId> flatChunkIds;
            for (const auto& ids : groupedChunkIds) {
                flatChunkIds.insert(flatChunkIds.end(), ids.begin(), ids.end());
            }

            // Teleport chunks.
            {
                auto teleporter = New<TChunkTeleporter>(
                    Connection_->GetConfig(),
                    this,
                    Connection_->GetLightInvoker(),
                    uploadTransactionId,
                    Logger);

                for (const auto& chunkId : flatChunkIds) {
                    teleporter->RegisterChunk(chunkId, dstCellTag);
                }

                WaitFor(teleporter->Run())
                    .ThrowOnError();
            }

            // Get upload params.
            TChunkListId chunkListId;
            {
                auto proxy = CreateWriteProxy<TObjectServiceProxy>(dstCellTag);

                auto req = TChunkOwnerYPathProxy::GetUploadParams(dstIdPath);
                NCypressClient::SetTransactionId(req, uploadTransactionId);

                auto rspOrError = WaitFor(proxy->Execute(req));
                THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error requesting upload parameters for %v", dstPath);
                const auto& rsp = rspOrError.Value();

                chunkListId = FromProto<TChunkListId>(rsp->chunk_list_id());
            }

            // Attach chunks to chunk list.
            TDataStatistics dataStatistics;
            {
                auto proxy = CreateWriteProxy<TChunkServiceProxy>(dstCellTag);

                auto batchReq = proxy->ExecuteBatch();
                NRpc::GenerateMutationId(batchReq);
                batchReq->set_suppress_upstream_sync(true);

                auto req = batchReq->add_attach_chunk_trees_subrequests();
                ToProto(req->mutable_parent_id(), chunkListId);
                ToProto(req->mutable_child_ids(), flatChunkIds);
                req->set_request_statistics(true);

                auto batchRspOrError = WaitFor(batchReq->Invoke());
                THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error attaching chunks to %v", dstPath);
                const auto& batchRsp = batchRspOrError.Value();

                const auto& rsp = batchRsp->attach_chunk_trees_subresponses(0);
                dataStatistics = rsp.statistics();
            }

            // End upload.
            {
                auto proxy = CreateWriteProxy<TObjectServiceProxy>();

                auto req = TChunkOwnerYPathProxy::EndUpload(dstIdPath);
                *req->mutable_statistics() = dataStatistics;
                NCypressClient::SetTransactionId(req, uploadTransactionId);
                NRpc::GenerateMutationId(req);

                auto rspOrError = WaitFor(proxy->Execute(req));
                THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error finishing upload to %v", dstPath);
            }

            uploadTransaction->Detach();
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error concatenating %v to %v",
                srcPaths,
                dstPath)
                << ex;
        }
    }

    bool DoNodeExists(
        const TYPath& path,
        const TNodeExistsOptions& options)
    {
        auto req = TYPathProxy::Exists(path);
        SetTransactionId(req, options, true);
        SetCachingHeader(req, options);

        auto proxy = CreateReadProxy<TObjectServiceProxy>(options);
        auto rsp = WaitFor(proxy->Execute(req))
            .ValueOrThrow();
        return rsp->value();
    }


    TObjectId DoCreateObject(
        EObjectType type,
        const TCreateObjectOptions& options)
    {
        auto proxy = CreateWriteProxy<TObjectServiceProxy>();
        auto batchReq = proxy->ExecuteBatch();
        SetPrerequisites(batchReq, options);

        auto req = TMasterYPathProxy::CreateObject();
        SetMutationId(req, options);
        req->set_type(static_cast<int>(type));
        if (options.Attributes) {
            ToProto(req->mutable_object_attributes(), *options.Attributes);
        }
        batchReq->AddRequest(req);

        auto batchRsp = WaitFor(batchReq->Invoke())
            .ValueOrThrow();
        auto rsp = batchRsp->GetResponse<TMasterYPathProxy::TRspCreateObject>(0)
            .ValueOrThrow();
        return FromProto<TObjectId>(rsp->object_id());
    }


    void DoAddMember(
        const Stroka& group,
        const Stroka& member,
        const TAddMemberOptions& options)
    {
        auto req = TGroupYPathProxy::AddMember(GetGroupPath(group));
        req->set_name(member);
        SetMutationId(req, options);

        auto proxy = CreateWriteProxy<TObjectServiceProxy>();
        WaitFor(proxy->Execute(req))
            .ThrowOnError();
    }

    void DoRemoveMember(
        const Stroka& group,
        const Stroka& member,
        const TRemoveMemberOptions& options)
    {
        auto req = TGroupYPathProxy::RemoveMember(GetGroupPath(group));
        req->set_name(member);
        SetMutationId(req, options);

        auto proxy = CreateWriteProxy<TObjectServiceProxy>();
        WaitFor(proxy->Execute(req))
            .ThrowOnError();
    }

    TCheckPermissionResult DoCheckPermission(
        const Stroka& user,
        const TYPath& path,
        EPermission permission,
        const TCheckPermissionOptions& options)
    {
        auto req = TObjectYPathProxy::CheckPermission(path);
        req->set_user(user);
        req->set_permission(static_cast<int>(permission));
        SetTransactionId(req, options, true);
        SetCachingHeader(req, options);

        auto proxy = CreateReadProxy<TObjectServiceProxy>(options);
        auto rsp = WaitFor(proxy->Execute(req))
            .ValueOrThrow();

        TCheckPermissionResult result;
        result.Action = ESecurityAction(rsp->action());
        result.ObjectId = FromProto<TObjectId>(rsp->object_id());
        result.ObjectName = rsp->has_object_name() ? MakeNullable(rsp->object_name()) : Null;
        result.SubjectId = FromProto<TSubjectId>(rsp->subject_id());
        result.SubjectName = rsp->has_subject_name() ? MakeNullable(rsp->subject_name()) : Null;
        return result;
    }


    TOperationId DoStartOperation(
        EOperationType type,
        const TYsonString& spec,
        const TStartOperationOptions& options)
    {
        auto req = SchedulerProxy_->StartOperation();
        SetTransactionId(req, options, true);
        SetMutationId(req, options);
        req->set_type(static_cast<int>(type));
        req->set_spec(spec.Data());

        auto rsp = WaitFor(req->Invoke())
            .ValueOrThrow();

        return FromProto<TOperationId>(rsp->operation_id());
    }

    void DoAbortOperation(
        const TOperationId& operationId,
        const TAbortOperationOptions& options)
    {
        auto req = SchedulerProxy_->AbortOperation();
        ToProto(req->mutable_operation_id(), operationId);
        if (options.AbortMessage) {
            req->set_abort_message(*options.AbortMessage);
        }

        WaitFor(req->Invoke())
            .ThrowOnError();
    }

    void DoSuspendOperation(
        const TOperationId& operationId,
        const TSuspendOperationOptions& options)
    {
        auto req = SchedulerProxy_->SuspendOperation();
        ToProto(req->mutable_operation_id(), operationId);
        req->set_abort_running_jobs(options.AbortRunningJobs);

        WaitFor(req->Invoke())
            .ThrowOnError();
    }

    void DoResumeOperation(
        const TOperationId& operationId,
        const TResumeOperationOptions& /*options*/)
    {
        auto req = SchedulerProxy_->ResumeOperation();
        ToProto(req->mutable_operation_id(), operationId);

        WaitFor(req->Invoke())
            .ThrowOnError();
    }

    void DoCompleteOperation(
        const TOperationId& operationId,
        const TCompleteOperationOptions& /*options*/)
    {
        auto req = SchedulerProxy_->CompleteOperation();
        ToProto(req->mutable_operation_id(), operationId);

        WaitFor(req->Invoke())
            .ThrowOnError();
    }


    void DoDumpJobContext(
        const TJobId& jobId,
        const TYPath& path,
        const TDumpJobContextOptions& /*options*/)
    {
        auto req = JobProberProxy_->DumpInputContext();
        ToProto(req->mutable_job_id(), jobId);
        ToProto(req->mutable_path(), path);

        WaitFor(req->Invoke())
            .ThrowOnError();
    }

    TSharedRef DoGetJobStderr(
        const TOperationId& operationId,
        const TJobId& jobId,
        const TGetJobStderrOptions& /*options*/)
    {
        try {
            NNodeTrackerClient::TNodeDescriptor jobNodeDescriptor;
            {
                auto req = JobProberProxy_->GetJobNode();
                ToProto(req->mutable_job_id(), jobId);
                auto rsp = WaitFor(req->Invoke())
                    .ValueOrThrow();
                FromProto(&jobNodeDescriptor, rsp->node_descriptor());
            }

            auto nodeChannel = GetHeavyChannelFactory()->CreateChannel(jobNodeDescriptor);
            NJobProberClient::TJobProberServiceProxy jobProberServiceProxy(nodeChannel);

            auto req = jobProberServiceProxy.GetStderr();
            ToProto(req->mutable_job_id(), jobId);
            auto rsp = WaitFor(req->Invoke())
                .ValueOrThrow();
            return TSharedRef::FromString(rsp->stderr_data());
        } catch (const TErrorException& exception) {
            auto matchedError = exception.Error().FindMatching(NScheduler::EErrorCode::NoSuchJob);

            if (!matchedError) {
                THROW_ERROR_EXCEPTION("Failed to get job stderr from job proxy")
                    << TErrorAttribute("operation_id", operationId)
                    << TErrorAttribute("job_id", jobId)
                    << exception.Error();
            }
        }

        try {
            auto path = NScheduler::GetStderrPath(operationId, jobId);

            std::vector<TSharedRef> blocks;
            auto fileReader = static_cast<IClientBase*>(this)->CreateFileReader(path);
            WaitFor(fileReader->Open())
                .ThrowOnError();

            while (true) {
                auto block = WaitFor(fileReader->Read())
                    .ValueOrThrow();

                if (!block) {
                    break;
                }

                blocks.push_back(std::move(block));
            }

            i64 size = GetByteSize(blocks);
            YCHECK(size);
            auto stderrFile = TSharedMutableRef::Allocate(size);
            auto memoryOutput = TMemoryOutput(stderrFile.Begin(), size);

            for (const auto& block : blocks) {
                memoryOutput.Write(block.Begin(), block.Size());
            }

            return stderrFile;
        } catch (const TErrorException& exception) {
            auto matchedError = exception.Error().FindMatching(NYTree::EErrorCode::ResolveError);

            if (!matchedError) {
                THROW_ERROR_EXCEPTION("Failed to get job stderr from Cypress")
                    << TErrorAttribute("operation_id", operationId)
                    << TErrorAttribute("job_id", jobId)
                    << exception.Error();
            }
        }

        try {
            auto nameTable = New<TNameTable>();

            struct TStderrArchiveIds
            {
                explicit TStderrArchiveIds(const TNameTablePtr& nameTable)
                    : OperationIdHi(nameTable->RegisterName("operation_id_hi"))
                    , OperationIdLo(nameTable->RegisterName("operation_id_lo"))
                    , JobIdHi(nameTable->RegisterName("job_id_hi"))
                    , JobIdLo(nameTable->RegisterName("job_id_lo"))
                    , Stderr(nameTable->RegisterName("stderr"))
                { }

                const int OperationIdHi;
                const int OperationIdLo;
                const int JobIdHi;
                const int JobIdLo;
                const int Stderr;
            };

            TStderrArchiveIds ids(nameTable);

            auto rowBuffer = New<TRowBuffer>();

            std::vector<TUnversionedRow> keys;
            auto key = rowBuffer->Allocate(4);
            key[0] = MakeUnversionedUint64Value(operationId.Parts64[0], ids.OperationIdHi);
            key[1] = MakeUnversionedUint64Value(operationId.Parts64[1], ids.OperationIdLo);
            key[2] = MakeUnversionedUint64Value(jobId.Parts64[0], ids.JobIdHi);
            key[3] = MakeUnversionedUint64Value(jobId.Parts64[1], ids.JobIdLo);
            keys.push_back(key);

            TLookupRowsOptions lookupOptions;
            lookupOptions.ColumnFilter = NTableClient::TColumnFilter({ids.Stderr});
            lookupOptions.KeepMissingRows = true;

            auto rowset = WaitFor(LookupRows(
                "//sys/operations_archive/stderrs",
                nameTable,
                MakeSharedRange(keys, rowBuffer),
                lookupOptions))
                .ValueOrThrow();

            const auto& rows = rowset->Rows();

            YCHECK(!rows.empty());

            if (rows[0]) {
                auto value = rows[0][0];

                YCHECK(value.Type == EValueType::String);
                return TSharedRef::MakeCopy<char>(TRef(value.Data.String, value.Length));
            }
        } catch (const TErrorException& exception) {
            auto matchedError = exception.Error().FindMatching(NYTree::EErrorCode::ResolveError);

            if (!matchedError) {
                THROW_ERROR_EXCEPTION("Failed to get job stderr from archive")
                    << TErrorAttribute("operation_id", operationId)
                    << TErrorAttribute("job_id", jobId)
                    << exception.Error();
            }
        }

        THROW_ERROR_EXCEPTION(NScheduler::EErrorCode::NoSuchJob, "Job stderr is not found")
            << TErrorAttribute("operation_id", operationId)
            << TErrorAttribute("job_id", jobId);
    }

    TYsonString DoStraceJob(
        const TJobId& jobId,
        const TStraceJobOptions& /*options*/)
    {
        auto req = JobProberProxy_->Strace();
        ToProto(req->mutable_job_id(), jobId);

        auto rsp = WaitFor(req->Invoke())
            .ValueOrThrow();

        return TYsonString(rsp->trace());
    }

    void DoSignalJob(
        const TJobId& jobId,
        const Stroka& signalName,
        const TSignalJobOptions& /*options*/)
    {
        auto req = JobProberProxy_->SignalJob();
        ToProto(req->mutable_job_id(), jobId);
        ToProto(req->mutable_signal_name(), signalName);

        WaitFor(req->Invoke())
            .ThrowOnError();
    }

    void DoAbandonJob(
        const TJobId& jobId,
        const TAbandonJobOptions& /*options*/)
    {
        auto req = JobProberProxy_->AbandonJob();
        ToProto(req->mutable_job_id(), jobId);

        WaitFor(req->Invoke())
            .ThrowOnError();
    }

    TYsonString DoPollJobShell(
        const TJobId& jobId,
        const TYsonString& parameters,
        const TPollJobShellOptions& options)
    {
        auto req = JobProberProxy_->PollJobShell();
        ToProto(req->mutable_job_id(), jobId);
        ToProto(req->mutable_parameters(), parameters.Data());

        auto rsp = WaitFor(req->Invoke())
            .ValueOrThrow();

        return TYsonString(rsp->result());
    }

    void DoAbortJob(
        const TJobId& jobId,
        const TAbortJobOptions& /*options*/)
    {
        auto req = JobProberProxy_->AbortJob();
        ToProto(req->mutable_job_id(), jobId);

        WaitFor(req->Invoke())
            .ThrowOnError();
    }
};

DEFINE_REFCOUNTED_TYPE(TNativeClient)

INativeClientPtr CreateNativeClient(
    INativeConnectionPtr connection,
    const TClientOptions& options)
{
    YCHECK(connection);

    return New<TNativeClient>(std::move(connection), options);
}

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ETransactionState,
    (Active)
    (Commit)
    (Abort)
    (Flush)
    (Detach)
);

class TNativeTransaction
    : public INativeTransaction
{
public:
    TNativeTransaction(
        TNativeClientPtr client,
        NTransactionClient::TTransactionPtr transaction)
        : Client_(std::move(client))
        , Transaction_(std::move(transaction))
        , CommitInvoker_(CreateSerializedInvoker(Client_->GetConnection()->GetHeavyInvoker()))
        , Logger(NLogging::TLogger(Client_->Logger)
            .AddTag("TransactionId: %v", GetId()))
    { }


    virtual IConnectionPtr GetConnection() override
    {
        return Client_->GetConnection();
    }

    virtual IClientPtr GetClient() const override
    {
        return Client_;
    }

    virtual NTransactionClient::ETransactionType GetType() const override
    {
        return Transaction_->GetType();
    }

    virtual const TTransactionId& GetId() const override
    {
        return Transaction_->GetId();
    }

    virtual TTimestamp GetStartTimestamp() const override
    {
        return Transaction_->GetStartTimestamp();
    }

    virtual EAtomicity GetAtomicity() const override
    {
        return Transaction_->GetAtomicity();
    }

    virtual EDurability GetDurability() const override
    {
        return Transaction_->GetDurability();
    }

    virtual TDuration GetTimeout() const override
    {
        return Transaction_->GetTimeout();
    }

    virtual TTimestamp GetCommitTimestamp() const override
    {
        return Transaction_->GetCommitTimestamp();
    }


    virtual TFuture<void> Ping() override
    {
        return Transaction_->Ping();
    }

    virtual TFuture<void> Commit(const TTransactionCommitOptions& options) override
    {
    	auto guard = Guard(SpinLock_);

        auto result = ValidateActiveAsync<void>();
        if (result) {
            return result;
        }

        State_ = ETransactionState::Commit;
        return BIND(&TNativeTransaction::DoCommit, MakeStrong(this))
            .AsyncVia(CommitInvoker_)
            .Run(options);
    }

    virtual TFuture<void> Abort(const TTransactionAbortOptions& options) override
    {
    	auto guard = Guard(SpinLock_);

        if (State_ == ETransactionState::Abort) {
            return AbortResult_;
        }

        auto result = ValidateActiveAsync<void>();
        if (result) {
            return result;
        }

        State_ = ETransactionState::Abort;
        AbortResult_ = Transaction_->Abort(options);
        return AbortResult_;
    }

    virtual void Detach() override
    {
    	auto guard = Guard(SpinLock_);
        State_ = ETransactionState::Detach;
        Transaction_->Detach();
    }

    virtual TFuture<TTransactionFlushResult> Flush() override
    {
    	auto guard = Guard(SpinLock_);

        auto result = ValidateActiveAsync<TTransactionFlushResult>();
        if (result) {
            return result;
        }

        State_ = ETransactionState::Flush;
        return BIND(&TNativeTransaction::DoFlush, MakeStrong(this))
            .AsyncVia(CommitInvoker_)
            .Run();
    }

    virtual void AddAction(const TCellId& cellId, const TTransactionActionData& data) override
    {
        YCHECK(TypeFromId(cellId) == EObjectType::TabletCell);

        ValidateActiveSync();

        if (GetAtomicity() != EAtomicity::Full) {
            THROW_ERROR_EXCEPTION("Atomicity must be %Qlv for custom actions",
                EAtomicity::Full);
        }

        Transaction_->AddParticipant(cellId);

        auto session = GetOrCreateCellCommitSession(cellId);
        session->RegisterAction(data);

        LOG_DEBUG("Transaction action added (TransactionId: %v, CellId: %v, ActionType: %v)",
            GetId(),
            cellId,
            data.Type);
    }


    virtual TFuture<ITransactionPtr> StartSlaveTransaction(
        IClientPtr client,
        const TTransactionStartOptions& options_) override
    {
        if (client->GetConnection()->GetCellTag() == GetConnection()->GetCellTag()) {
            return MakeFuture<ITransactionPtr>(this);
        }

        auto options = options_;
        options.Id = GetId();

        return client->StartTransaction(GetType(), options)
            .Apply(BIND([this, this_ = MakeStrong(this)] (const ITransactionPtr& transaction) {
                RegisterSlaveTransaction(transaction);
                return transaction;
            }));
    }


    virtual void SubscribeCommitted(const TClosure& callback) override
    {
        Transaction_->SubscribeCommitted(callback);
    }

    virtual void UnsubscribeCommitted(const TClosure& callback) override
    {
        Transaction_->UnsubscribeCommitted(callback);
    }


    virtual void SubscribeAborted(const TClosure& callback) override
    {
        Transaction_->SubscribeAborted(callback);
    }

    virtual void UnsubscribeAborted(const TClosure& callback) override
    {
        Transaction_->UnsubscribeAborted(callback);
    }


    virtual TFuture<INativeTransactionPtr> StartNativeTransaction(
        ETransactionType type,
        const TTransactionStartOptions& options) override
    {
        auto adjustedOptions = options;
        adjustedOptions.ParentId = GetId();
        return Client_->StartNativeTransaction(
            type,
            adjustedOptions);
    }

    virtual TFuture<ITransactionPtr> StartTransaction(
        ETransactionType type,
        const TTransactionStartOptions& options) override
    {
        return StartNativeTransaction(type, options).As<ITransactionPtr>();
    }


    virtual void WriteRows(
        const TYPath& path,
        TNameTablePtr nameTable,
        TSharedRange<TUnversionedRow> rows,
        const TWriteRowsOptions& options) override
    {
        std::vector<TRowModification> modifications;
        modifications.reserve(rows.Size());

        for (auto row : rows) {
            TRowModification modification;
            modification.Type = ERowModificationType::Write;
            modification.Row = row;
            modifications.push_back(modification);
        }

        ModifyRows(
            path,
            std::move(nameTable),
            MakeSharedRange(std::move(modifications), std::move(rows)));
    }

    virtual void DeleteRows(
        const TYPath& path,
        TNameTablePtr nameTable,
        TSharedRange<TUnversionedRow> keys,
        const TDeleteRowsOptions& options) override
    {
        std::vector<TRowModification> modifications;
        modifications.reserve(keys.Size());

        for (auto key : keys) {
            TRowModification modification;
            modification.Type = ERowModificationType::Delete;
            modification.Row = key;
            modifications.push_back(modification);
        }

        ModifyRows(
            path,
            std::move(nameTable),
            MakeSharedRange(std::move(modifications), std::move(keys)));
    }

    virtual void ModifyRows(
        const TYPath& path,
        TNameTablePtr nameTable,
        TSharedRange<TRowModification> modifications,
        const TModifyRowsOptions& options = TModifyRowsOptions()) override
    {
    	auto guard = Guard(SpinLock_);

        ValidateTabletTransaction();
    	ValidateActiveSync();

        Requests_.push_back(std::make_unique<TModificationRequest>(
            this,
            Client_->GetNativeConnection(),
            path,
            std::move(nameTable),
            std::move(modifications),
            options));

        LOG_DEBUG("Row modifications buffered (Count: %v)", modifications.Size());
    }


#define DELEGATE_METHOD(returnType, method, signature, args) \
    virtual returnType method signature override \
    { \
        return Client_->method args; \
    }

#define DELEGATE_TRANSACTIONAL_METHOD(returnType, method, signature, args) \
    virtual returnType method signature override \
    { \
        auto& originalOptions = options; \
        { \
            auto options = originalOptions; \
            options.TransactionId = GetId(); \
            return Client_->method args; \
        } \
    }

#define DELEGATE_TIMESTAMPED_METHOD(returnType, method, signature, args) \
    virtual returnType method signature override \
    { \
        auto& originalOptions = options; \
        { \
            auto options = originalOptions; \
            options.Timestamp = GetReadTimestamp(); \
            return Client_->method args; \
        } \
    }

    DELEGATE_TIMESTAMPED_METHOD(TFuture<IRowsetPtr>, LookupRows, (
        const TYPath& path,
        TNameTablePtr nameTable,
        const TSharedRange<NTableClient::TKey>& keys,
        const TLookupRowsOptions& options),
        (path, nameTable, keys, options))


    DELEGATE_TIMESTAMPED_METHOD(TFuture<TSelectRowsResult>, SelectRows, (
        const Stroka& query,
        const TSelectRowsOptions& options),
        (query, options))


    DELEGATE_TRANSACTIONAL_METHOD(TFuture<TYsonString>, GetNode, (
        const TYPath& path,
        const TGetNodeOptions& options),
        (path, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<void>, SetNode, (
        const TYPath& path,
        const TYsonString& value,
        const TSetNodeOptions& options),
        (path, value, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<void>, RemoveNode, (
        const TYPath& path,
        const TRemoveNodeOptions& options),
        (path, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<TYsonString>, ListNode, (
        const TYPath& path,
        const TListNodeOptions& options),
        (path, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<TNodeId>, CreateNode, (
        const TYPath& path,
        EObjectType type,
        const TCreateNodeOptions& options),
        (path, type, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<TLockId>, LockNode, (
        const TYPath& path,
        NCypressClient::ELockMode mode,
        const TLockNodeOptions& options),
        (path, mode, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<TNodeId>, CopyNode, (
        const TYPath& srcPath,
        const TYPath& dstPath,
        const TCopyNodeOptions& options),
        (srcPath, dstPath, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<TNodeId>, MoveNode, (
        const TYPath& srcPath,
        const TYPath& dstPath,
        const TMoveNodeOptions& options),
        (srcPath, dstPath, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<TNodeId>, LinkNode, (
        const TYPath& srcPath,
        const TYPath& dstPath,
        const TLinkNodeOptions& options),
        (srcPath, dstPath, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<void>, ConcatenateNodes, (
        const std::vector<TYPath>& srcPaths,
        const TYPath& dstPath,
        TConcatenateNodesOptions options),
        (srcPaths, dstPath, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<bool>, NodeExists, (
        const TYPath& path,
        const TNodeExistsOptions& options),
        (path, options))


    DELEGATE_METHOD(TFuture<TObjectId>, CreateObject, (
        EObjectType type,
        const TCreateObjectOptions& options),
        (type, options))


    DELEGATE_TRANSACTIONAL_METHOD(IFileReaderPtr, CreateFileReader, (
        const TYPath& path,
        const TFileReaderOptions& options),
        (path, options))
    DELEGATE_TRANSACTIONAL_METHOD(IFileWriterPtr, CreateFileWriter, (
        const TYPath& path,
        const TFileWriterOptions& options),
        (path, options))


    DELEGATE_TRANSACTIONAL_METHOD(IJournalReaderPtr, CreateJournalReader, (
        const TYPath& path,
        const TJournalReaderOptions& options),
        (path, options))
    DELEGATE_TRANSACTIONAL_METHOD(IJournalWriterPtr, CreateJournalWriter, (
        const TYPath& path,
        const TJournalWriterOptions& options),
        (path, options))

    DELEGATE_TRANSACTIONAL_METHOD(TFuture<ISchemalessMultiChunkReaderPtr>, CreateTableReader, (
        const TRichYPath& path,
        const TTableReaderOptions& options),
        (path, options))

#undef DELEGATE_TRANSACTIONAL_METHOD
#undef DELEGATE_TIMESTAMPED_METHOD

private:
    const TNativeClientPtr Client_;
    const NTransactionClient::TTransactionPtr Transaction_;
    const IInvokerPtr CommitInvoker_;
    const NLogging::TLogger Logger;

    struct TNativeTransactionBufferTag
    { };

    const TRowBufferPtr RowBuffer_ = New<TRowBuffer>(TNativeTransactionBufferTag());

    TSpinLock SpinLock_;
    ETransactionState State_ = ETransactionState::Active;
    TFuture<void> AbortResult_;

    TSpinLock SlaveTransactionsLock_;
    std::vector<ITransactionPtr> SlaveTransactions_;


    class TModificationRequest
    {
    public:
        TModificationRequest(
            TNativeTransaction* transaction,
            INativeConnectionPtr connection,
            const TYPath& path,
            TNameTablePtr nameTable,
            TSharedRange<TRowModification> modifications,
            const TModifyRowsOptions& options)
            : Transaction_(transaction)
            , Connection_(std::move(connection))
            , Path_(path)
            , NameTable_(std::move(nameTable))
            , TabletIndexColumnId_(NameTable_->FindId(TabletIndexColumnName))
            , Modifications_(std::move(modifications))
            , Options_(options)
        { }

        void Run()
        {
            auto tableInfo = Transaction_->Client_->SyncGetTableInfo(Path_);
            const auto& primarySchema = tableInfo->Schemas[ETableSchemaKind::Primary];
            const auto& primaryIdMapping = Transaction_->GetColumnIdMapping(tableInfo, NameTable_, ETableSchemaKind::Primary);
            const auto& writeSchema = tableInfo->Schemas[ETableSchemaKind::Write];
            const auto& writeIdMapping = Transaction_->GetColumnIdMapping(tableInfo, NameTable_, ETableSchemaKind::Write);
            const auto& rowBuffer = Transaction_->RowBuffer_;
            auto evaluatorCache = Connection_->GetColumnEvaluatorCache();
            auto evaluator = tableInfo->NeedKeyEvaluation ? evaluatorCache->Find(primarySchema) : nullptr;
            auto randomTabletInfo = tableInfo->GetRandomMountedTablet();

            for (const auto& modification : Modifications_) {
                ValidateModification(modification, tableInfo, writeSchema, writeIdMapping);

                auto capturedRow = rowBuffer->CaptureAndPermuteRow(modification.Row, primarySchema, primaryIdMapping);

                TTabletInfoPtr tabletInfo;
                if (tableInfo->IsSorted()) {
                    if (evaluator) {
                        evaluator->EvaluateKeys(capturedRow, rowBuffer);
                    }

                    tabletInfo = GetSortedTabletForRow(tableInfo, capturedRow);
                } else {
                    tabletInfo = GetOrderedTabletForRow(tableInfo, randomTabletInfo, TabletIndexColumnId_, modification.Row);
                }

                auto* session = Transaction_->GetTabletSession(tabletInfo, tableInfo);
                auto command = GetCommand(modification);
                session->SubmitRow(command, capturedRow);
            }
        }

    protected:
        TNativeTransaction* const Transaction_;
        const INativeConnectionPtr Connection_;
		const TYPath Path_;
        const TNameTablePtr NameTable_;
        const TNullable<int> TabletIndexColumnId_;
        const TSharedRange<TRowModification> Modifications_;
        const TModifyRowsOptions Options_;


        void ValidateModification(
            const TRowModification& modification,
            const TTableMountInfoPtr& tableInfo,
            const TTableSchema& schema,
            const TNameTableToSchemaIdMapping& idMapping)
        {
            switch (modification.Type) {
                case ERowModificationType::Write:
                    ValidateClientDataRow(modification.Row, schema, idMapping, NameTable_);
                    break;

                case ERowModificationType::Delete:
                    if (!tableInfo->IsSorted()) {
                        THROW_ERROR_EXCEPTION("Cannot delete rows from a non-sorted table %v",
                            tableInfo->Path);
                    }
                    ValidateClientKey(modification.Row, schema, idMapping, NameTable_);
                    break;

                default:
                    Y_UNREACHABLE();
            }
        }

        static EWireProtocolCommand GetCommand(const TRowModification& modification)
        {
            switch (modification.Type) {
                case ERowModificationType::Write:
                    return EWireProtocolCommand::WriteRow;

                case ERowModificationType::Delete:
                    return EWireProtocolCommand::DeleteRow;

                default:
                    Y_UNREACHABLE();
            }
        }
    };

    std::vector<std::unique_ptr<TModificationRequest>> Requests_;

    class TTabletCommitSession
        : public TIntrinsicRefCounted
    {
    public:
        TTabletCommitSession(
            TNativeTransactionPtr owner,
            TTabletInfoPtr tabletInfo,
            TTableMountInfoPtr tableInfo,
            TColumnEvaluatorPtr columnEvauator)
            : Owner_(owner)
            , TableInfo_(std::move(tableInfo))
            , TabletInfo_(std::move(tabletInfo))
            , Config_(owner->Client_->Connection_->GetConfig())
            , ColumnCount_(TableInfo_->Schemas[ETableSchemaKind::Primary].Columns().size())
            , KeyColumnCount_(TableInfo_->Schemas[ETableSchemaKind::Primary].GetKeyColumnCount())
            , ColumnEvaluator_(std::move(columnEvauator))
            , Logger(owner->Logger)
        {
            Logger.AddTag("TabletId: %v", TabletInfo_->TabletId);
        }

        TWireProtocolWriter* GetWriter()
        {
            if (Batches_.empty() || Batches_.back()->RowCount >= Config_->MaxRowsPerWriteRequest) {
                Batches_.emplace_back(new TBatch());
            }
            auto& batch = Batches_.back();
            ++batch->RowCount;
            return &batch->Writer;
        }

        void SubmitRow(
            EWireProtocolCommand command,
            TUnversionedRow row)
        {
            if (SubmittedRows_.size() >= Config_->MaxRowsPerTransaction) {
                THROW_ERROR_EXCEPTION("Transaction affects too many rows")
                    << TErrorAttribute("limit", Config_->MaxRowsPerTransaction);
            }
            SubmittedRows_.push_back(TSubmittedRow{
                command,
                row,
                static_cast<int>(SubmittedRows_.size())});
        }

        int Prepare()
        {
            if (TableInfo_->IsSorted()) {
                PrepareSortedBatches();
            } else {
                PrepareOrderedBatches();
            }

            return static_cast<int>(Batches_.size());
        }

        TFuture<void> Invoke(IChannelPtr channel)
        {
            // Do all the heavy lifting here.
            YCHECK(!Batches_.empty());
            for (auto& batch : Batches_) {
                batch->RequestData = NCompression::CompressWithEnvelope(
                    batch->Writer.Flush(),
                    Config_->WriteRequestCodec);;
            }

            InvokeChannel_ = channel;
            InvokeNextBatch();
            return InvokePromise_;
        }

        const TCellId& GetCellId() const
        {
            return TabletInfo_->CellId;
        }

    private:
        const TWeakPtr<TNativeTransaction> Owner_;
        const TTableMountInfoPtr TableInfo_;
        const TTabletInfoPtr TabletInfo_;
        const TNativeConnectionConfigPtr Config_;
        const int ColumnCount_;
        const int KeyColumnCount_;

        struct TCommitSessionBufferTag
        { };

        TColumnEvaluatorPtr ColumnEvaluator_;
        TRowBufferPtr RowBuffer_ = New<TRowBuffer>(TCommitSessionBufferTag());

        NLogging::TLogger Logger;

        struct TBatch
        {
            TWireProtocolWriter Writer;
            std::vector<TSharedRef> RequestData;
            int RowCount = 0;
        };

        std::vector<std::unique_ptr<TBatch>> Batches_;

        struct TSubmittedRow
        {
            EWireProtocolCommand Command;
            TUnversionedRow Row;
            int SequentialId;
        };

        std::vector<TSubmittedRow> SubmittedRows_;

        IChannelPtr InvokeChannel_;
        int InvokeBatchIndex_ = 0;
        TPromise<void> InvokePromise_ = NewPromise<void>();


        void PrepareSortedBatches()
        {
            std::sort(
                SubmittedRows_.begin(),
                SubmittedRows_.end(),
                [=] (const TSubmittedRow& lhs, const TSubmittedRow& rhs) {
                    // NB: CompareRows may throw on composite values.
                    int res = CompareRows(lhs.Row, rhs.Row, KeyColumnCount_);
                    return res != 0 ? res < 0 : lhs.SequentialId < rhs.SequentialId;
                });

            std::vector<TSubmittedRow> mergedRows;
            mergedRows.reserve(SubmittedRows_.size());

            auto merger = New<TUnversionedRowMerger>(
                RowBuffer_,
                ColumnCount_,
                KeyColumnCount_,
                ColumnEvaluator_);

            auto addPartialRow = [&] (const TSubmittedRow& submittedRow) {
                switch (submittedRow.Command) {
                    case EWireProtocolCommand::DeleteRow:
                        merger->DeletePartialRow(submittedRow.Row);
                        break;

                    case EWireProtocolCommand::WriteRow:
                        merger->AddPartialRow(submittedRow.Row);
                        break;

                    default:
                        Y_UNREACHABLE();
                }
            };

            int index = 0;
            while (index < SubmittedRows_.size()) {
                if (index < SubmittedRows_.size() - 1 &&
                    CompareRows(SubmittedRows_[index].Row, SubmittedRows_[index + 1].Row, KeyColumnCount_) == 0)
                {
                    addPartialRow(SubmittedRows_[index]);
                    while (index < SubmittedRows_.size() - 1 &&
                           CompareRows(SubmittedRows_[index].Row, SubmittedRows_[index + 1].Row, KeyColumnCount_) == 0)
                    {
                        ++index;
                        addPartialRow(SubmittedRows_[index]);
                    }
                    SubmittedRows_[index].Row = merger->BuildMergedRow();
                }
                mergedRows.push_back(SubmittedRows_[index]);
                ++index;
            }

            WriteRows(mergedRows);
        }

        void PrepareOrderedBatches()
        {
            WriteRows(SubmittedRows_);
        }

        void WriteRows(const std::vector<TSubmittedRow>& rows)
        {
            for (const auto& submittedRow : rows) {
                WriteRow(submittedRow);
            }
        }

        void WriteRow(const TSubmittedRow& submittedRow)
        {
            if (Batches_.empty() || Batches_.back()->RowCount >= Config_->MaxRowsPerWriteRequest) {
                Batches_.emplace_back(new TBatch());
            }
            auto& batch = Batches_.back();
            ++batch->RowCount;
            auto& writer = batch->Writer;
            writer.WriteCommand(submittedRow.Command);
            writer.WriteUnversionedRow(submittedRow.Row);
        }

        void InvokeNextBatch()
        {
            if (InvokeBatchIndex_ >= Batches_.size()) {
                InvokePromise_.Set(TError());
                return;
            }

            const auto& batch = Batches_[InvokeBatchIndex_];

            auto owner = Owner_.Lock();
            if (!owner) {
                return;
            }

            auto cellSession = owner->GetCommitSession(GetCellId());

            TTabletServiceProxy proxy(InvokeChannel_);
            proxy.SetDefaultTimeout(Config_->WriteTimeout);
            proxy.SetDefaultRequestAck(false);

            auto req = proxy.Write();
            ToProto(req->mutable_transaction_id(), owner->GetId());
            if (owner->GetAtomicity() == EAtomicity::Full) {
                req->set_transaction_start_timestamp(owner->GetStartTimestamp());
                req->set_transaction_timeout(ToProto(owner->GetTimeout()));
            }
            ToProto(req->mutable_tablet_id(), TabletInfo_->TabletId);
            req->set_mount_revision(TabletInfo_->MountRevision);
            req->set_durability(static_cast<int>(owner->GetDurability()));
            req->set_signature(cellSession->AllocateRequestSignature());
            req->Attachments() = std::move(batch->RequestData);

            LOG_DEBUG("Sending batch (BatchIndex: %v/%v, RowCount: %v, Signature: %x)",
                InvokeBatchIndex_,
                Batches_.size(),
                batch->RowCount,
                req->signature());

            req->Invoke().Subscribe(
                BIND(&TTabletCommitSession::OnResponse, MakeStrong(this))
                    .Via(owner->CommitInvoker_));
        }

        void OnResponse(const TTabletServiceProxy::TErrorOrRspWritePtr& rspOrError)
        {
            if (!rspOrError.IsOK()) {
                LOG_DEBUG(rspOrError, "Error sending batch");
                InvokePromise_.Set(rspOrError);
                return;
            }

            auto owner = Owner_.Lock();
            if (!owner) {
                return;
            }

            LOG_DEBUG("Batch sent successfully");
            owner->Transaction_->AddParticipant(TabletInfo_->CellId);
            ++InvokeBatchIndex_;
            InvokeNextBatch();
        }
    };

    using TTabletCommitSessionPtr = TIntrusivePtr<TTabletCommitSession>;

    //! Maintains per-tablet commit info.
    yhash_map<TTabletId, TTabletCommitSessionPtr> TabletIdToSession_;

    class TCellCommitSession
        : public TIntrinsicRefCounted
    {
    public:
        explicit TCellCommitSession(TNativeTransactionPtr owner)
            : Owner_(std::move(owner))
        { }

        void RegisterRequests(int count)
        {
            RequestsRemaining_ += count;
        }

        TTransactionSignature AllocateRequestSignature()
        {
            YCHECK(--RequestsRemaining_ >= 0);
            if (RequestsRemaining_ == 0) {
                return FinalTransactionSignature - CurrentSignature_;
            } else {
                ++CurrentSignature_;
                return 1;
            }
        }

        void RegisterAction(const TTransactionActionData& data)
        {
            if (Actions_.empty()) {
                RegisterRequests(1);
            }
            Actions_.push_back(data);
        }

        TFuture<void> Invoke(IChannelPtr channel)
        {
            if (Actions_.empty()) {
                return VoidFuture;
            }

            auto owner = Owner_.Lock();
            if (!owner) {
                return VoidFuture;
            }

            TTabletServiceProxy proxy(channel);
            auto req = proxy.RegisterTransactionActions();
            ToProto(req->mutable_transaction_id(), owner->GetId());
            req->set_transaction_start_timestamp(owner->GetStartTimestamp());
            req->set_transaction_timeout(ToProto(owner->GetTimeout()));
            req->set_signature(AllocateRequestSignature());
            ToProto(req->mutable_actions(), Actions_);
            return req->Invoke().As<void>();
        }

    private:
        const TWeakPtr<TNativeTransaction> Owner_;

        std::vector<TTransactionActionData> Actions_;
        TTransactionSignature CurrentSignature_ = InitialTransactionSignature;
        int RequestsRemaining_ = 0;

    };

    using TCellCommitSessionPtr = TIntrusivePtr<TCellCommitSession>;


    //! Maintains per-cell commit info.
    yhash_map<TCellId, TCellCommitSessionPtr> CellIdToSession_;

    //! Caches mappings from name table ids to schema ids.
    yhash_map<std::pair<TNameTablePtr, ETableSchemaKind>, TNameTableToSchemaIdMapping> IdMappingCache_;


    const TNameTableToSchemaIdMapping& GetColumnIdMapping(
        const TTableMountInfoPtr& tableInfo,
        const TNameTablePtr& nameTable,
        ETableSchemaKind kind)
    {
        auto key = std::make_pair(nameTable, kind);
        auto it = IdMappingCache_.find(key);
        if (it == IdMappingCache_.end()) {
            auto mapping = BuildColumnIdMapping(tableInfo->Schemas[kind], nameTable);
            it = IdMappingCache_.insert(std::make_pair(key, std::move(mapping))).first;
        }
        return it->second;
    }


    TTabletCommitSession* GetTabletSession(const TTabletInfoPtr& tabletInfo, const TTableMountInfoPtr& tableInfo)
    {
        const auto& tabletId = tabletInfo->TabletId;
        auto it = TabletIdToSession_.find(tabletId);
        if (it == TabletIdToSession_.end()) {
            auto evaluatorCache = Client_->GetNativeConnection()->GetColumnEvaluatorCache();
            auto evaluator = evaluatorCache->Find(tableInfo->Schemas[ETableSchemaKind::Primary]);
            it = TabletIdToSession_.insert(std::make_pair(
                tabletId,
                New<TTabletCommitSession>(
                    this,
                    tabletInfo,
                    tableInfo,
                    evaluator)
                )).first;
        }
        return it->second.Get();
    }

    TFuture<void> SendRequests()
    {
        for (const auto& request : Requests_) {
            request->Run();
        }

        for (const auto& pair : TabletIdToSession_) {
            const auto& tabletSession = pair.second;
            const auto& cellId = tabletSession->GetCellId();
            int requestCount = tabletSession->Prepare();
            auto cellSession = GetOrCreateCellCommitSession(cellId);
            cellSession->RegisterRequests(requestCount);
        }

        std::vector<TFuture<void>> asyncResults;

        for (const auto& pair : TabletIdToSession_) {
            const auto& session = pair.second;
            const auto& cellId = session->GetCellId();
            auto channel = Client_->GetTabletChannelOrThrow(cellId);
            asyncResults.push_back(session->Invoke(std::move(channel)));
        }

        for (auto& pair : CellIdToSession_) {
            const auto& cellId = pair.first;
            const auto& session = pair.second;
            auto channel = Client_->GetTabletChannelOrThrow(cellId);
            asyncResults.push_back(session->Invoke(std::move(channel)));
        }

        return Combine(asyncResults);
    }

    void DoCommit(const TTransactionCommitOptions& options)
    {
        try {
            std::vector<TFuture<void>> asyncRequestResults{
                SendRequests()
            };

            std::vector<TFuture<TTransactionFlushResult>> asyncFlushResults;
            for (const auto& slave : GetSlaveTransactions()) {
                asyncFlushResults.push_back(slave->Flush());
            }

            auto flushResults = WaitFor(Combine(asyncFlushResults))
                .ValueOrThrow();

            for (const auto& flushResult : flushResults) {
                asyncRequestResults.push_back(flushResult.AsyncResult);
                for (const auto& cellId : flushResult.ParticipantCellIds) {
                    Transaction_->AddParticipant(cellId);
                }
            }

            WaitFor(Combine(asyncRequestResults))
                .ThrowOnError();
        } catch (const std::exception& ex) {
            // Fire and forget.
            Transaction_->Abort();
            throw;
        }

        WaitFor(Transaction_->Commit(options))
            .ThrowOnError();
    }

    TTransactionFlushResult DoFlush()
    {
        TTransactionFlushResult result;
        result.AsyncResult = SendRequests();
        result.ParticipantCellIds = GetKeys(CellIdToSession_);
        return result;
    }


    TCellCommitSessionPtr GetOrCreateCellCommitSession(const TCellId& cellId)
    {
        auto it = CellIdToSession_.find(cellId);
        if (it == CellIdToSession_.end()) {
            it = CellIdToSession_.emplace(cellId, New<TCellCommitSession>(this)).first;
        }
        return it->second;
    }

    TCellCommitSessionPtr GetCommitSession(const TCellId& cellId)
    {
        auto it = CellIdToSession_.find(cellId);
        YCHECK(it != CellIdToSession_.end());
        return it->second;
    }


    TTimestamp GetReadTimestamp() const
    {
        switch (Transaction_->GetAtomicity()) {
            case EAtomicity::Full:
                return GetStartTimestamp();
            case EAtomicity::None:
                // NB: Start timestamp is approximate.
                return SyncLastCommittedTimestamp;
            default:
                Y_UNREACHABLE();
        }
    }

    void ValidateTabletTransaction()
    {
        if (TypeFromId(GetId()) == EObjectType::NestedTransaction) {
            THROW_ERROR_EXCEPTION("Nested master transactions cannot be used for updating dynamic tables");
        }
    }

    void ValidateActiveSync()
    {
        if (State_ != ETransactionState::Active) {
            THROW_ERROR_EXCEPTION("Transaction is already in %Qlv state",
                State_);
        }
    }

    template <class T>
    TFuture<T> ValidateActiveAsync()
    {
        if (State_ != ETransactionState::Active) {
            return MakeFuture<T>(TError("Transaction is already in %Qlv state",
                State_));
        }
        return TFuture<T>();
    }


    void RegisterSlaveTransaction(ITransactionPtr transaction)
    {
        auto guard = Guard(SlaveTransactionsLock_);
        SlaveTransactions_.emplace_back(std::move(transaction));
    }

    std::vector<ITransactionPtr> GetSlaveTransactions()
    {
        auto guard = Guard(SlaveTransactionsLock_);
        return SlaveTransactions_;
    }
};

DEFINE_REFCOUNTED_TYPE(TNativeTransaction)

TFuture<INativeTransactionPtr> TNativeClient::StartNativeTransaction(
    ETransactionType type,
    const TTransactionStartOptions& options)
{
    return TransactionManager_->Start(type, options).Apply(
        BIND([=, this_ = MakeStrong(this)] (const NTransactionClient::TTransactionPtr& nativeTranasction) -> INativeTransactionPtr {
            auto transaction = New<TNativeTransaction>(this_, nativeTranasction);
            if (options.Sticky) {
                Connection_->RegisterStickyTransaction(transaction);
            }
            return transaction;
        }));
}

INativeTransactionPtr TNativeClient::AttachNativeTransaction(
    const TTransactionId& transactionId,
    const TTransactionAttachOptions& options)
{
    if (options.Sticky) {
        return Connection_->GetStickyTransaction(transactionId);
    } else {
        auto nativeTransaction = TransactionManager_->Attach(transactionId, options);
        return New<TNativeTransaction>(this, std::move(nativeTransaction));
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NApi
} // namespace NYT
