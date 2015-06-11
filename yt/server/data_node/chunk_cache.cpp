#include "stdafx.h"
#include "chunk_cache.h"
#include "private.h"
#include "blob_reader_cache.h"
#include "location.h"
#include "blob_chunk.h"
#include "block_store.h"
#include "config.h"
#include "master_connector.h"
#include "artifact.h"

#include <core/concurrency/thread_affinity.h>
#include <core/concurrency/scheduler.h>

#include <core/misc/serialize.h>
#include <core/misc/string.h>
#include <core/misc/fs.h>
#include <core/misc/random.h>

#include <core/logging/log.h>

#include <ytlib/hydra/peer_channel.h>

#include <ytlib/chunk_client/block_cache.h>
#include <ytlib/chunk_client/file_writer.h>
#include <ytlib/chunk_client/replication_reader.h>
#include <ytlib/chunk_client/sequential_reader.h>
#include <ytlib/chunk_client/chunk_meta_extensions.h>
#include <ytlib/chunk_client/client_block_cache.h>

#include <ytlib/file_client/file_chunk_reader.h>

#include <ytlib/node_tracker_client/node_directory.h>

#include <ytlib/new_table_client/name_table.h>
#include <ytlib/new_table_client/schemaless_chunk_reader.h>
#include <ytlib/new_table_client/helpers.h>

#include <ytlib/formats/format.h>

#include <ytlib/api/client.h>
#include <ytlib/api/config.h>

#include <server/cell_node/bootstrap.h>

namespace NYT {
namespace NDataNode {

using namespace NYTree;
using namespace NChunkClient;
using namespace NObjectClient;
using namespace NFileClient;
using namespace NNodeTrackerClient;
using namespace NVersionedTableClient;
using namespace NCellNode;
using namespace NRpc;
using namespace NChunkClient::NProto;
using namespace NConcurrency;
using namespace NApi;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = DataNodeLogger;

////////////////////////////////////////////////////////////////////////////////

class TChunkCache::TImpl
    : public TAsyncSlruCacheBase<TArtifactKey, TCachedBlobChunk>
{
public:
    TImpl(TDataNodeConfigPtr config, TBootstrap* bootstrap)
        : TAsyncSlruCacheBase(
            New<TSlruCacheConfig>(config->CacheLocation->Quota.Get(std::numeric_limits<i64>::max())),
            NProfiling::TProfiler(DataNodeProfiler.GetPathPrefix() + "/chunk_cache"))
        , Config_(config)
        , Bootstrap_(bootstrap)
        , RandomGenerator_(TInstant::Now().MicroSeconds())
    {
        VERIFY_INVOKER_THREAD_AFFINITY(Bootstrap_->GetControlInvoker(), ControlThread);
    }

    void Initialize()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        Location_ = New<TLocation>(
            ELocationType::Cache,
            "cache",
            Config_->CacheLocation,
            Bootstrap_);

        Location_->SubscribeDisabled(
            BIND(&TImpl::OnLocationDisabled, Unretained(this))
                .Via(Bootstrap_->GetControlInvoker()));

        auto descriptors = Location_->Scan();
        for (const auto& descriptor : descriptors) {
            TArtifactKey key;
            if (IsArtifactChunkId(descriptor.Id)) {
                try {
                    LoadArtifactMeta(descriptor.Id, &key);
                } catch (const std::exception& ex) {
                    auto error = TError("Error loading artifact meta %v",
                        descriptor.Id)
                        << ex;
                    LOG_WARNING(error);
                    continue;
                }
            } else {
                key = TArtifactKey(descriptor.Id);
            }

            auto cookie = BeginInsert(key);
            YCHECK(cookie.IsActive());
            auto chunk = CreateChunk(Location_, key, descriptor);
            cookie.EndInsert(chunk);
        }

        Location_->Start();

        LOG_INFO("Chunk cache initialized, %v chunks total",
            GetSize());
    }

    bool IsEnabled() const
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return Location_->IsEnabled();
    }

    IChunkPtr FindChunk(const TChunkId& chunkId)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Find(TArtifactKey(chunkId));
    }

    std::vector<IChunkPtr> GetChunks()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto chunks = GetAll();
        return std::vector<IChunkPtr>(chunks.begin(), chunks.end());
    }

    TFuture<IChunkPtr> PrepareArtifact(
        const TArtifactKey& key,
        TNodeDirectoryPtr nodeDirectory)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        LOG_INFO("Getting artifact from cache: %v",
            key.DebugString());

        auto cookie = BeginInsert(key);
        auto cookieValue = cookie.GetValue();
        if (cookie.IsActive()) {
            LOG_INFO("Loading artifact into cache");
            auto invoker = CreateSerializedInvoker(Location_->GetWritePoolInvoker());
            invoker->Invoke(BIND(
                &TImpl::DoPrepareArtifact,
                MakeStrong(this),
                key,
                nodeDirectory ? std::move(nodeDirectory) : New<TNodeDirectory>(),
                Passed(std::move(cookie))));
        } else {
            LOG_INFO("Artifact is already cached");
        }
        return cookieValue.As<IChunkPtr>();
    }

private:
    const TDataNodeConfigPtr Config_;
    TBootstrap* const Bootstrap_;

    TLocationPtr Location_;

    TSpinLock RandomSpinLock_;
    TRandomGenerator RandomGenerator_;

    DEFINE_SIGNAL(void(IChunkPtr), ChunkAdded);
    DEFINE_SIGNAL(void(IChunkPtr), ChunkRemoved);

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);


    void OnChunkCreated(
        TLocationPtr location,
        const TChunkDescriptor& descriptor)
    {
        Bootstrap_->GetControlInvoker()->Invoke(BIND(([=] () {
            location->UpdateChunkCount(+1);
            location->UpdateUsedSpace(+descriptor.DiskSpace);
        })));
    }

    void OnChunkDestroyed(
        TLocationPtr location,
        const TChunkDescriptor& descriptor)
    {
        location->GetWritePoolInvoker()->Invoke(BIND(
            &TLocation::RemoveChunkFiles,
            Location_,
            descriptor.Id));

        Bootstrap_->GetControlInvoker()->Invoke(BIND([=] () {
            location->UpdateChunkCount(-1);
            location->UpdateUsedSpace(-descriptor.DiskSpace);
        }));
    }

    TCachedBlobChunkPtr CreateChunk(
        TLocationPtr location,
        const TArtifactKey& key,
        const TChunkDescriptor& descriptor,
        const NChunkClient::NProto::TChunkMeta* meta = nullptr)
    {
        auto chunk = New<TCachedBlobChunk>(
            Bootstrap_,
            Location_,
            descriptor,
            meta,
            key,
            BIND(&TImpl::OnChunkDestroyed, MakeStrong(this), Location_, descriptor));

        OnChunkCreated(location, descriptor);
        return chunk;
    }


    virtual i64 GetWeight(TCachedBlobChunk* chunk) const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return chunk->GetInfo().disk_space();
    }

    virtual void OnAdded(TCachedBlobChunk* value) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TAsyncSlruCacheBase::OnAdded(value);
        ChunkAdded_.Fire(value);
    }

    virtual void OnRemoved(TCachedBlobChunk* value) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TAsyncSlruCacheBase::OnRemoved(value);
        ChunkRemoved_.Fire(value);
    }

    void OnLocationDisabled(const TError& reason)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        LOG_WARNING("Chunk cache disabled");
        Clear();

        // Register an alert and
        // schedule an out-of-order heartbeat to notify the master about the disaster.
        auto masterConnector = Bootstrap_->GetMasterConnector();
        masterConnector->RegisterAlert(TError("Chunk cache at %v is disabled",
            Location_->GetPath())
            << reason);
        masterConnector->ForceRegister();
    }

    TChunkId GenerateArtifactId()
    {
        TGuard<TSpinLock> guard(RandomSpinLock_);

        auto random = RandomGenerator_.Generate<i64>();
        return TChunkId(
            static_cast<int>(TInstant::Now().MicroSeconds()),
            static_cast<int>(EObjectType::Artifact),
            static_cast<int>(random >> 32),
            static_cast<int>(random & 0xffffffffll));
    }

    bool CanPrepareSingleChunk(const TArtifactKey& key)
    {
        if (EObjectType(key.type()) != EObjectType::File) {
            return false;
        }
        if (key.chunks_size() != 1) {
            return false;
        }

        const auto& chunk = key.chunks(0);
        if (chunk.has_lower_limit() && !IsTrivial(chunk.lower_limit())) {
            return false;
        }
        if (chunk.has_upper_limit() && !IsTrivial(chunk.upper_limit())) {
            return false;
        }

        auto miscExt = GetProtoExtension<TMiscExt>(chunk.chunk_meta().extensions());
        auto compressionCodecId = NCompression::ECodec(miscExt.compression_codec());
        if (compressionCodecId != NCompression::ECodec::None) {
            return false;
        }

        auto chunkId = FromProto<TChunkId>(chunk.chunk_id());
        if (IsErasureChunkId(chunkId)) {
            return false;
        }

        return true;
    }

    void DoPrepareArtifact(
        const TArtifactKey& key,
        TNodeDirectoryPtr nodeDirectory,
        TInsertCookie cookie)
    {
        if (CanPrepareSingleChunk(key)) {
            DownloadChunk(key, nodeDirectory, std::move(cookie));
        } else {
            switch (EObjectType(key.type())) {
                case EObjectType::File:
                    DownloadFile(key, nodeDirectory, std::move(cookie));
                    break;
                case EObjectType::Table:
                    DownloadTable(key, nodeDirectory, std::move(cookie));
                    break;
                default:
                    YUNREACHABLE();
            }
        }
    }

    void DownloadChunk(
        const TArtifactKey& key,
        TNodeDirectoryPtr nodeDirectory,
        TInsertCookie cookie)
    {
        const auto& chunkSpec = key.chunks(0);
        auto chunkId = FromProto<TChunkId>(chunkSpec.chunk_id());
        auto seedReplicas = FromProto<TChunkReplica, TChunkReplicaList>(chunkSpec.replicas());

        auto Logger = DataNodeLogger;
        Logger.AddTag("ChunkId: %v", chunkId);

        try {
            auto options = New<TRemoteReaderOptions>();
            auto chunkReader = CreateReplicationReader(
                Config_->CacheRemoteReader,
                options,
                Bootstrap_->GetMasterClient()->GetMasterChannel(NApi::EMasterChannelKind::LeaderOrFollower),
                nodeDirectory,
                Bootstrap_->GetMasterConnector()->GetLocalDescriptor(),
                chunkId,
                seedReplicas,
                Bootstrap_->GetBlockCache());

            auto fileName = Location_->GetChunkPath(chunkId);
            auto chunkWriter = New<TFileWriter>(chunkId, fileName);

            try {
                NFS::ForcePath(NFS::GetDirectoryName(fileName));
                WaitFor(chunkWriter->Open())
                    .ThrowOnError();
            } catch (const std::exception& ex) {
                LOG_FATAL(ex, "Error opening cached chunk for writing");
            }

            LOG_INFO("Getting chunk meta");
            auto chunkMetaOrError = WaitFor(chunkReader->GetMeta());
            THROW_ERROR_EXCEPTION_IF_FAILED(chunkMetaOrError);
            LOG_INFO("Chunk meta received");
            const auto& chunkMeta = chunkMetaOrError.Value();

            // Download all blocks.
            auto blocksExt = GetProtoExtension<TBlocksExt>(chunkMeta.extensions());
            int blockCount = blocksExt.blocks_size();
            std::vector<TSequentialReader::TBlockInfo> blocks;
            blocks.reserve(blockCount);
            for (int index = 0; index < blockCount; ++index) {
                blocks.push_back(TSequentialReader::TBlockInfo(
                    index,
                    blocksExt.blocks(index).size()));
            }

            auto sequentialReader = New<TSequentialReader>(
                Config_->CacheSequentialReader,
                std::move(blocks),
                chunkReader,
                GetNullBlockCache(),
                NCompression::ECodec::None);

            for (int blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
                LOG_INFO("Downloading block (BlockIndex: %v)",
                    blockIndex);

                auto blockResult = WaitFor(sequentialReader->FetchNextBlock());
                THROW_ERROR_EXCEPTION_IF_FAILED(blockResult);

                LOG_INFO("Writing block (BlockIndex: %v)",
                    blockIndex);
                // NB: This is always done synchronously.
                auto block = sequentialReader->GetCurrentBlock();
                if (!chunkWriter->WriteBlock(block)) {
                    THROW_ERROR_EXCEPTION(chunkWriter->GetReadyEvent().Get());
                }
                LOG_INFO("Block written");
            }


            LOG_INFO("Closing chunk");
            auto closeResult = WaitFor(chunkWriter->Close(chunkMeta));
            THROW_ERROR_EXCEPTION_IF_FAILED(closeResult);
            LOG_INFO("Chunk is closed");

            LOG_INFO("Chunk is downloaded into cache");

            TChunkDescriptor descriptor(chunkId);
            descriptor.DiskSpace = chunkWriter->GetChunkInfo().disk_space();
            auto chunk = CreateChunk(Location_, key, descriptor, &chunkMeta);
            cookie.EndInsert(chunk);

        } catch (const std::exception& ex) {
            auto error = TError("Error downloading chunk %v into cache",
                chunkId)
                << ex;
            cookie.Cancel(error);
            LOG_WARNING(error);
        }
    }

    void DownloadFile(
        const TArtifactKey& key,
        TNodeDirectoryPtr nodeDirectory,
        TInsertCookie cookie)
    {
        std::vector<TChunkSpec> chunkSpecs;
        chunkSpecs.insert(chunkSpecs.end(), key.chunks().begin(), key.chunks().end());

        auto reader = CreateFileMultiChunkReader(
            New<TFileReaderConfig>(),
            New<TMultiChunkReaderOptions>(),
            Bootstrap_->GetMasterClient()->GetMasterChannel(EMasterChannelKind::Leader),
            Bootstrap_->GetBlockCache(),
            nodeDirectory,
            chunkSpecs);

        try {
            WaitFor(reader->Open()).ThrowOnError();

            auto producer = [&] (TOutputStream* output) {
                TSharedRef block;
                while (reader->ReadBlock(&block)) {
                    if (block.Empty()) {
                        WaitFor(reader->GetReadyEvent())
                            .ThrowOnError();
                    } else {
                        output->Write(block.Begin(), block.Size());
                    }
                }
            };

            auto chunk = ProduceArtifactFile(key, producer);
            cookie.EndInsert(chunk);

        } catch (const std::exception& ex) {
            auto error = TError("Error downloading file artifact into cache: %v",
                key.DebugString())
                << ex;
            cookie.Cancel(error);
            LOG_WARNING(error);
        }
    }

    void DownloadTable(
        const TArtifactKey& key,
        TNodeDirectoryPtr nodeDirectory,
        TInsertCookie cookie)
    {
        auto nameTable = New<TNameTable>();

        std::vector<TChunkSpec> chunkSpecs;
        chunkSpecs.insert(chunkSpecs.end(), key.chunks().begin(), key.chunks().end());

        auto reader = CreateSchemalessSequentialMultiChunkReader(
            New<TTableReaderConfig>(),
            New<TMultiChunkReaderOptions>(),
            Bootstrap_->GetMasterClient()->GetMasterChannel(EMasterChannelKind::Leader),
            Bootstrap_->GetBlockCache(),
            nodeDirectory,
            chunkSpecs,
            nameTable);

        auto format = ConvertTo<NFormats::TFormat>(TYsonString(key.format()));

        try {
            WaitFor(reader->Open()).ThrowOnError();

            auto producer = [&] (TOutputStream* output) {
                auto bufferedOutput = std::make_unique<TBufferedOutput>(output);
                auto controlAttributesConfig = New<TControlAttributesConfig>();
                auto writer = CreateSchemalessWriterForFormat(
                    format,
                    nameTable,
                    std::move(bufferedOutput),
                    false,
                    false,
                    0);
                PipeReaderToWriter(reader, writer, controlAttributesConfig, 10000);
            };

            auto chunk = ProduceArtifactFile(key, producer);
            cookie.EndInsert(chunk);

        } catch (const std::exception& ex) {
            auto error = TError("Error downloading table artifact into cache: %v",
                key.DebugString())
                << ex;
            cookie.Cancel(error);
            LOG_WARNING(error);
        }
    }

    TCachedBlobChunkPtr ProduceArtifactFile(
        const TArtifactKey& key,
        std::function<void(TOutputStream*)> producer)
    {
        auto chunkId = GenerateArtifactId();

        LOG_INFO("Producing artifact file (ChunkId: %v)",
            chunkId);

        auto fileName = Location_->GetChunkPath(chunkId);
        auto metaFileName = fileName + ArtifactMetaSuffix;
        auto tempFileName = fileName + NFS::TempFileSuffix;
        auto tempMetaFileName = metaFileName + NFS::TempFileSuffix;

        TFile file(
            tempFileName,
            CreateAlways | WrOnly | Seq | CloseOnExec);
        file.Flock(LOCK_EX);
        TFileOutput fileOutput(file);
        producer(&fileOutput);
        file.Close();

        auto chunkSize = NFS::GetFileStatistics(tempFileName).Size;

        TFile metaFile(
            tempMetaFileName,
            CreateAlways | WrOnly | Seq | CloseOnExec);
        metaFile.Flock(LOCK_EX);
        auto metaData = SerializeToProto(key);
        metaFile.Write(metaData.Begin(), metaData.Size());
        metaFile.Close();

        NFS::Rename(tempMetaFileName, metaFileName);
        NFS::Rename(tempFileName, fileName);

        TChunkDescriptor descriptor(chunkId);
        descriptor.DiskSpace = chunkSize + metaData.Size();
        return CreateChunk(Location_, key, descriptor);
    }

    void LoadArtifactMeta(const TChunkId& chunkId, TArtifactKey* key)
    {
        auto fileName = Location_->GetChunkPath(chunkId);
        auto metaFileName = fileName + ArtifactMetaSuffix;

        TFile metaFile(
            metaFileName,
            OpenExisting | RdOnly | Seq | CloseOnExec);
        TBufferedFileInput metaInput(metaFile);

        auto metaBlob = metaInput.ReadAll();
        auto metaBlobRef = TRef::FromString(metaBlob);

        if (!TryDeserializeFromProto(key, metaBlobRef)) {
            THROW_ERROR_EXCEPTION("Failed to parse artifact meta file %v",
                metaFileName);
        }

        LOG_INFO("Artifact meta file %v loaded",
            metaFileName);
    }

};

////////////////////////////////////////////////////////////////////////////////

TChunkCache::TChunkCache(TDataNodeConfigPtr config, TBootstrap* bootstrap)
    : Impl_(New<TImpl>(config, bootstrap))
{ }

TChunkCache::~TChunkCache()
{ }

void TChunkCache::Initialize()
{
    Impl_->Initialize();
}

bool TChunkCache::IsEnabled() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return Impl_->IsEnabled();
}

IChunkPtr TChunkCache::FindChunk(const TChunkId& chunkId)
{
    VERIFY_THREAD_AFFINITY_ANY();

    return Impl_->FindChunk(chunkId);
}

std::vector<IChunkPtr> TChunkCache::GetChunks()
{
    VERIFY_THREAD_AFFINITY_ANY();

    return Impl_->GetChunks();
}

int TChunkCache::GetChunkCount()
{
    VERIFY_THREAD_AFFINITY_ANY();

    return Impl_->GetSize();
}

TFuture<IChunkPtr> TChunkCache::PrepareArtifact(
    const TArtifactKey& key,
    TNodeDirectoryPtr nodeDirectory)
{
    VERIFY_THREAD_AFFINITY_ANY();

    return Impl_->PrepareArtifact(key, nodeDirectory);
}

DELEGATE_SIGNAL(TChunkCache, void(IChunkPtr), ChunkAdded, *Impl_);
DELEGATE_SIGNAL(TChunkCache, void(IChunkPtr), ChunkRemoved, *Impl_);

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT
