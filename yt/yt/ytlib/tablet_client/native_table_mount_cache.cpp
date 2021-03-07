#include "native_table_mount_cache.h"
#include "private.h"
#include "config.h"

#include <yt/yt/ytlib/api/native/connection.h>
#include <yt/yt/ytlib/api/native/config.h>
#include <yt/yt/ytlib/api/native/rpc_helpers.h>

#include <yt/yt/ytlib/cypress_client/cypress_ypath_proxy.h>

#include <yt/yt/ytlib/election/config.h>

#include <yt/yt/ytlib/hive/cell_directory.h>

#include <yt/yt/ytlib/object_client/object_service_proxy.h>
#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/ytlib/table_client/table_ypath_proxy.h>
#include <yt/yt/client/table_client/unversioned_row.h>
#include <yt/yt/client/table_client/versioned_row.h>
#include <yt/yt/client/table_client/helpers.h>

#include <yt/yt/ytlib/tablet_client/public.h>

#include <yt/yt/client/tablet_client/table_mount_cache.h>
#include <yt/yt/client/tablet_client/table_mount_cache_detail.h>

#include <yt/yt/client/security_client/public.h>

#include <yt/yt/core/concurrency/delayed_executor.h>

#include <yt/yt/core/misc/farm_hash.h>
#include <yt/yt/core/misc/string.h>

#include <yt/yt/core/rpc/proto/rpc.pb.h>

#include <yt/yt/core/ytree/proto/ypath.pb.h>

#include <yt/yt/core/yson/string.h>

#include <util/datetime/base.h>

namespace NYT::NTabletClient {

using namespace NApi;
using namespace NConcurrency;
using namespace NCypressClient;
using namespace NElection;
using namespace NHiveClient;
using namespace NNodeTrackerClient;
using namespace NObjectClient;
using namespace NRpc;
using namespace NTableClient;
using namespace NYPath;
using namespace NYson;
using namespace NYTree;
using namespace NApi::NNative;

using NNative::IConnection;
using NNative::IConnectionPtr;

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TTableMountCache)

class TTableMountCache
    : public TTableMountCacheBase
{
public:
    TTableMountCache(
        TTableMountCacheConfigPtr config,
        IConnectionPtr connection,
        TCellDirectoryPtr cellDirectory,
        const NLogging::TLogger& logger)
        : TTableMountCacheBase(std::move(config), logger)
        , Connection_(std::move(connection))
        , CellDirectory_(std::move(cellDirectory))
        , Invoker_(connection->GetInvoker())
    { }

private:
    virtual TFuture<TTableMountInfoPtr> DoGet(
        const TTableMountCacheKey& key,
        bool isPeriodicUpdate) noexcept override
    {
        auto session = New<TGetSession>(this, Connection_, key, Logger);
        return BIND(&TGetSession::Run, std::move(session))
            .AsyncVia(Invoker_)
            .Run();
    }

private:
    const TWeakPtr<IConnection> Connection_;
    const TCellDirectoryPtr CellDirectory_;
    const IInvokerPtr Invoker_;

    class TGetSession
        : public TRefCounted
    {
    public:
        TGetSession(
            TTableMountCachePtr owner,
            TWeakPtr<IConnection> connection,
            const TTableMountCacheKey& key,
            const NLogging::TLogger& logger)
            : Owner_(std::move(owner))
            , Connection_(std::move(connection))
            , Key_(key)
            , Logger(logger.WithTag("Path: %v, CacheSessionId: %v",
                Key_.Path,
                TGuid::Create()))
        { }

        TTableMountInfoPtr Run()
        {
            try {
                WaitFor(RequestTableAttributes(Key_.RefreshPrimaryRevision))
                    .ThrowOnError();

                auto mountInfoOrError = WaitFor(RequestMountInfo(Key_.RefreshSecondaryRevision));
                if (!mountInfoOrError.IsOK() && PrimaryRevision_) {
                    WaitFor(RequestTableAttributes(PrimaryRevision_))
                        .ThrowOnError();
                    mountInfoOrError = WaitFor(RequestMountInfo(NHydra::NullRevision));
                }

                if (!mountInfoOrError.IsOK() && SecondaryRevision_) {
                    mountInfoOrError = WaitFor(RequestMountInfo(SecondaryRevision_));
                }

                return mountInfoOrError.ValueOrThrow();
            } catch (const std::exception& ex) {
                YT_LOG_DEBUG(ex, "Error getting table mount info");
                THROW_ERROR_EXCEPTION("Error getting mount info for %v",
                    Key_.Path)
                    << ex;
            }
        }

    private:
        const TTableMountCachePtr Owner_;
        const TWeakPtr<IConnection> Connection_;
        const TTableMountCacheKey Key_;
        const NLogging::TLogger Logger;

        TTableId TableId_;
        TCellTag CellTag_;
        NHydra::TRevision PrimaryRevision_ = NHydra::NullRevision;
        NHydra::TRevision SecondaryRevision_ = NHydra::NullRevision;

        TMasterReadOptions GetMasterReadOptions()
        {
            return {
                .ReadFrom = EMasterChannelKind::Cache,
                .ExpireAfterSuccessfulUpdateTime = Owner_->Config_->ExpireAfterSuccessfulUpdateTime,
                .ExpireAfterFailedUpdateTime = Owner_->Config_->ExpireAfterFailedUpdateTime,
                .EnableClientCacheStickiness = true
            };
        }

        TFuture<void> RequestTableAttributes(NHydra::TRevision refreshPrimaryRevision)
        {
            auto connection = Connection_.Lock();
            if (!connection) {
                return MakeFuture<void>(TError(NYT::EErrorCode::Canceled, "Connection destroyed"));
            }

            YT_LOG_DEBUG("Requesting table mount info from primary master (RefreshPrimaryRevision: %llx)",
                refreshPrimaryRevision);

            auto options = GetMasterReadOptions();

            auto channel = CreateAuthenticatedChannel(
                connection->GetMasterChannelOrThrow(options.ReadFrom),
                NRpc::TAuthenticationIdentity(NSecurityClient::TableMountInformerUserName));

            auto primaryProxy = TObjectServiceProxy(channel, connection->GetStickyGroupSizeCache());
            auto batchReq = primaryProxy.ExecuteBatch();
            SetBalancingHeader(batchReq, connection->GetConfig(), options);

            {
                auto req = TTableYPathProxy::Get(Key_.Path + "/@");
                ToProto(req->mutable_attributes()->mutable_keys(), std::vector<TString>{
                    "id",
                    "dynamic",
                    "external_cell_tag"
                });

                SetCachingHeader(req, connection->GetConfig(), options, refreshPrimaryRevision);

                size_t hash = 0;
                HashCombine(hash, FarmHash(Key_.Path.begin(), Key_.Path.size()));
                batchReq->AddRequest(req, "get_attributes", hash);
            }

            return batchReq->Invoke()
                .Apply(BIND(&TGetSession::OnTableAttributesReceived, MakeWeak(this)));
        }

        void OnTableAttributesReceived(const TObjectServiceProxy::TErrorOrRspExecuteBatchPtr& batchRspOrError)
        {
            THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error getting attributes of table %v",
                Key_.Path);
            
            const auto& batchRsp = batchRspOrError.Value();
            auto getAttributesRspOrError = batchRsp->GetResponse<TYPathProxy::TRspGet>("get_attributes");
            auto& rsp = getAttributesRspOrError.Value();

            PrimaryRevision_ = batchRsp->GetRevision(0);

            auto attributes = ConvertToAttributes(TYsonString(rsp->value()));
            CellTag_ = attributes->Get<TCellTag>("external_cell_tag", PrimaryMasterCellTag);
            TableId_ = attributes->Get<TObjectId>("id");
            auto dynamic = attributes->Get<bool>("dynamic", false);

            if (!dynamic) {
                THROW_ERROR_EXCEPTION("Table %v is not dynamic",
                    Key_.Path);
            }
        }

        TFuture<TTableMountInfoPtr> RequestMountInfo(NHydra::TRevision refreshSecondaryRevision)
        {
            auto connection = Connection_.Lock();
            if (!connection) {
                return MakeFuture<TTableMountInfoPtr>(TError(NYT::EErrorCode::Canceled, "Connection destroyed"));
            }

            YT_LOG_DEBUG("Requesting table mount info from secondary master (TableId: %v, CellTag: %v, RefreshSecondaryRevision: %llx)",
                TableId_,
                CellTag_,
                refreshSecondaryRevision);

            auto options = GetMasterReadOptions();

            auto channel = CreateAuthenticatedChannel(
                connection->GetMasterChannelOrThrow(options.ReadFrom, CellTag_),
                NRpc::TAuthenticationIdentity(NSecurityClient::TableMountInformerUserName));

            auto secondaryProxy = TObjectServiceProxy(channel, connection->GetStickyGroupSizeCache());
            auto batchReq = secondaryProxy.ExecuteBatch();
            SetBalancingHeader(batchReq, connection->GetConfig(), options);

            auto req = TTableYPathProxy::GetMountInfo(FromObjectId(TableId_));
            SetCachingHeader(req, connection->GetConfig(), options, refreshSecondaryRevision);

            size_t hash = 0;
            HashCombine(hash, FarmHash(TableId_.Parts64[0]));
            HashCombine(hash, FarmHash(TableId_.Parts64[1]));
            batchReq->AddRequest(req, std::nullopt, hash);

            return batchReq->Invoke()
                .Apply(BIND(&TGetSession::OnTableMountInfoReceived, MakeStrong(this)));
        }

        TTableMountInfoPtr OnTableMountInfoReceived(const TObjectServiceProxy::TErrorOrRspExecuteBatchPtr& batchRspOrError)
        {
            THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error getting mount info for table %v",
                Key_.Path);

            const auto& batchRsp = batchRspOrError.Value();
            const auto& rspOrError = batchRsp->GetResponse<TTableYPathProxy::TRspGetMountInfo>(0);
            const auto& rsp = rspOrError.Value();

            SecondaryRevision_ = batchRsp->GetRevision(0);

            auto tableInfo = New<TTableMountInfo>();
            tableInfo->Path = Key_.Path;
            tableInfo->TableId = FromProto<TObjectId>(rsp->table_id());
            tableInfo->SecondaryRevision = SecondaryRevision_;
            tableInfo->PrimaryRevision = PrimaryRevision_;

            auto primarySchema = FromProto<TTableSchemaPtr>(rsp->schema());
            tableInfo->Schemas[ETableSchemaKind::Primary] = primarySchema;
            tableInfo->Schemas[ETableSchemaKind::Write] = primarySchema->ToWrite();
            tableInfo->Schemas[ETableSchemaKind::VersionedWrite] = primarySchema->ToVersionedWrite();
            tableInfo->Schemas[ETableSchemaKind::Delete] = primarySchema->ToDelete();
            tableInfo->Schemas[ETableSchemaKind::Query] = primarySchema->ToQuery();
            tableInfo->Schemas[ETableSchemaKind::Lookup] = primarySchema->ToLookup();
            tableInfo->Schemas[ETableSchemaKind::PrimaryWithTabletIndex] = primarySchema->WithTabletIndex();

            tableInfo->UpstreamReplicaId = FromProto<TTableReplicaId>(rsp->upstream_replica_id());
            tableInfo->Dynamic = rsp->dynamic();
            tableInfo->NeedKeyEvaluation = primarySchema->HasComputedColumns();

            for (const auto& protoTabletInfo : rsp->tablets()) {
                auto tabletInfo = New<TTabletInfo>();
                tabletInfo->CellId = FromProto<TCellId>(protoTabletInfo.cell_id());
                tabletInfo->TabletId = FromProto<TObjectId>(protoTabletInfo.tablet_id());
                tabletInfo->MountRevision = protoTabletInfo.mount_revision();
                tabletInfo->State = FromProto<ETabletState>(protoTabletInfo.state());
                tabletInfo->UpdateTime = Now();
                tabletInfo->InMemoryMode = FromProto<EInMemoryMode>(protoTabletInfo.in_memory_mode());

                if (tableInfo->IsSorted()) {
                    // Take the actual pivot from master response.
                    tabletInfo->PivotKey = FromProto<TLegacyOwningKey>(protoTabletInfo.pivot_key());
                } else {
                    // Synthesize a fake pivot key.
                    TUnversionedOwningRowBuilder builder(1);
                    int tabletIndex = static_cast<int>(tableInfo->Tablets.size());
                    builder.AddValue(MakeUnversionedInt64Value(tabletIndex));
                    tabletInfo->PivotKey = builder.FinishRow();
                }

                if (protoTabletInfo.has_cell_id()) {
                    tabletInfo->CellId = FromProto<TCellId>(protoTabletInfo.cell_id());
                }

                tabletInfo->Owners.push_back(MakeWeak(tableInfo));

                tabletInfo = Owner_->TabletInfoCache_.Insert(std::move(tabletInfo));
                tableInfo->Tablets.push_back(tabletInfo);
                if (tabletInfo->State == ETabletState::Mounted) {
                    tableInfo->MountedTablets.push_back(tabletInfo);
                }
            }

            for (const auto& protoDescriptor : rsp->tablet_cells()) {
                auto descriptor = FromProto<TCellDescriptor>(protoDescriptor);
                Owner_->CellDirectory_->ReconfigureCell(descriptor);
            }

            for (const auto& protoReplicaInfo : rsp->replicas()) {
                auto replicaInfo = New<TTableReplicaInfo>();
                replicaInfo->ReplicaId = FromProto<TTableReplicaId>(protoReplicaInfo.replica_id());
                replicaInfo->ClusterName = protoReplicaInfo.cluster_name();
                replicaInfo->ReplicaPath = protoReplicaInfo.replica_path();
                replicaInfo->Mode = FromProto<ETableReplicaMode>(protoReplicaInfo.mode());
                tableInfo->Replicas.push_back(replicaInfo);
            }

            if (tableInfo->IsSorted()) {
                tableInfo->LowerCapBound = MinKey();
                tableInfo->UpperCapBound = MaxKey();
            } else {
                tableInfo->LowerCapBound = MakeUnversionedOwningRow(static_cast<int>(0));
                tableInfo->UpperCapBound = MakeUnversionedOwningRow(static_cast<int>(tableInfo->Tablets.size()));
            }

            YT_LOG_DEBUG("Table mount info received (TableId: %v, TabletCount: %v, Dynamic: %v, PrimaryRevision: %llx, SecondaryRevision: %llx)",
                tableInfo->TableId,
                tableInfo->Tablets.size(),
                tableInfo->Dynamic,
                tableInfo->PrimaryRevision,
                tableInfo->SecondaryRevision);

            return tableInfo;
        }
    };

    virtual void InvalidateTable(const TTableMountInfoPtr& tableInfo) override
    {
        Invalidate(tableInfo->Path);

        TAsyncExpiringCache::Get(TTableMountCacheKey{
            tableInfo->Path,
            tableInfo->PrimaryRevision,
            tableInfo->SecondaryRevision});
    }

    virtual void OnAdded(const TTableMountCacheKey& key) noexcept override
    {
        YT_LOG_DEBUG("Table mount info added to cache (Path: %v, PrimaryRevision: %llx, SecondaryRevision: %llx)",
            key.Path,
            key.RefreshPrimaryRevision,
            key.RefreshSecondaryRevision);
    }

    virtual void OnRemoved(const TTableMountCacheKey& key) noexcept override
    {
        YT_LOG_DEBUG("Table mount info removed from cache (Path: %v, PrimaryRevision: %llx, SecondaryRevision: %llx)",
            key.Path,
            key.RefreshPrimaryRevision,
            key.RefreshSecondaryRevision);
    }
};

DEFINE_REFCOUNTED_TYPE(TTableMountCache)

ITableMountCachePtr CreateNativeTableMountCache(
    TTableMountCacheConfigPtr config,
    IConnectionPtr connection,
    TCellDirectoryPtr cellDirectory,
    const NLogging::TLogger& logger)
{
    return New<TTableMountCache>(
        std::move(config),
        std::move(connection),
        std::move(cellDirectory),
        logger);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletClient

