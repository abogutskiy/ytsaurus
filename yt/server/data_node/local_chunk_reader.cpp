#include "local_chunk_reader.h"
#include "chunk_store.h"

#include <yt/server/cell_node/bootstrap.h>

#include "chunk_block_manager.h"
#include <yt/server/data_node/chunk.h>

#include <yt/ytlib/chunk_client/block_cache.h>
#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/chunk_reader.h>
#include <yt/ytlib/chunk_client/config.h>

#include <yt/ytlib/table_client/chunk_meta_extensions.h>

namespace NYT {
namespace NDataNode {

using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NTableClient;
using namespace NDataNode;
using namespace NCellNode;

////////////////////////////////////////////////////////////////////////////////

class TLocalChunkReader
    : public IChunkReader
{
public:
    TLocalChunkReader(
        TReplicationReaderConfigPtr config,
        IChunkPtr chunk,
        TChunkBlockManagerPtr chunkBlockManager,
        IBlockCachePtr blockCache,
        TClosure failureHandler)
        : Config_(std::move(config))
        , Chunk_(std::move(chunk))
        , ChunkBlockManager_(std::move(chunkBlockManager))
        , BlockCache_(std::move(blockCache))
        , FailureHandler_(std::move(failureHandler))
    { }

    virtual TFuture<std::vector<TBlock>> ReadBlocks(
        const TWorkloadDescriptor& workloadDescriptor,
        NChunkClient::TChunkReaderStatisticsPtr chunkDiskReadStatistis,
        const TReadSessionId& /*readSessionId*/,
        const std::vector<int>& blockIndexes) override
    {
        auto session = New<TReadBlockSetSession>();
        session->WorkloadDescriptor = workloadDescriptor;
        session->BlockIndexes = blockIndexes;
        session->Blocks.resize(blockIndexes.size());
        RequestBlockSet(session);
        return session->Promise.ToFuture();
    }

    virtual TFuture<std::vector<TBlock>> ReadBlocks(
        const TWorkloadDescriptor& workloadDescriptor,
        NChunkClient::TChunkReaderStatisticsPtr chunkDiskReadStatistis,
        const TReadSessionId& /*readSessionId*/,
        int firstBlockIndex,
        int blockCount) override
    {
        TBlockReadOptions options;
        options.WorkloadDescriptor = workloadDescriptor;
        options.BlockCache = BlockCache_;
        options.PopulateCache = Config_->PopulateCache;

        auto asyncResult = ChunkBlockManager_->ReadBlockRange(
            Chunk_->GetId(),
            firstBlockIndex,
            blockCount,
            options);

        return asyncResult.Apply(BIND([=] (const TErrorOr<std::vector<TBlock>>& blocksOrError) {
            if (!blocksOrError.IsOK()) {
                ThrowError(blocksOrError);
            }
            return blocksOrError.Value();
        }));
    }

    virtual TFuture<TChunkMeta> GetMeta(
        const TWorkloadDescriptor& workloadDescriptor,
        NChunkClient::TChunkReaderStatisticsPtr chunkDiskReadStatistis,
        const TReadSessionId& /*readSessionId*/,
        const TNullable<int>& partitionTag,
        const TNullable<std::vector<int>>& extensionTags) override
    {
        auto asyncResult = Chunk_->ReadMeta(workloadDescriptor, chunkDiskReadStatistis, extensionTags);
        return asyncResult.Apply(BIND([=] (const TErrorOr<TRefCountedChunkMetaPtr>& metaOrError) {
            if (!metaOrError.IsOK()) {
                ThrowError(metaOrError);
            }
            const auto& meta = metaOrError.Value();
            return partitionTag
                ? FilterChunkMetaByPartitionTag(*meta, *partitionTag)
                : TChunkMeta(*meta);
        }));
    }

    virtual TChunkId GetChunkId() const override
    {
        return Chunk_->GetId();
    }

    virtual bool IsValid() const override
    {
        return true;
    }

private:
    const TReplicationReaderConfigPtr Config_;
    const IChunkPtr Chunk_;
    const TChunkBlockManagerPtr ChunkBlockManager_;
    const IBlockCachePtr BlockCache_;
    const TClosure FailureHandler_;

    struct TReadBlockSetSession
        : public TIntrinsicRefCounted
    {
        TWorkloadDescriptor WorkloadDescriptor;
        std::vector<int> BlockIndexes;
        std::vector<TBlock> Blocks;
        TPromise<std::vector<TBlock>> Promise = NewPromise<std::vector<TBlock>>();
    };

    using TReadBlockSetSessionPtr = TIntrusivePtr<TReadBlockSetSession>;

    void RequestBlockSet(TReadBlockSetSessionPtr session)
    {
        try {
            if (!Chunk_->IsAlive()) {
                ThrowError(TError("Local chunk %v is no longer available",
                    Chunk_->GetId()));
            }

            std::vector<int> localIndexes;
            std::vector<int> blockIndexes;
            for (int index = 0; index < session->Blocks.size(); ++index) {
                if (!session->Blocks[index]) {
                    localIndexes.push_back(index);
                    blockIndexes.push_back(session->BlockIndexes[index]);
                }
            }

            if (localIndexes.empty()) {
                session->Promise.Set(std::move(session->Blocks));
                return;
            }

            TBlockReadOptions options;
            options.WorkloadDescriptor = session->WorkloadDescriptor;
            options.BlockCache = BlockCache_;
            options.PopulateCache = Config_->PopulateCache;

            auto asyncResult = ChunkBlockManager_->ReadBlockSet(
                Chunk_->GetId(),
                blockIndexes,
                options);

            asyncResult.Subscribe(
                BIND(&TLocalChunkReader::OnBlockSetRead, MakeStrong(this), session, localIndexes));
        } catch (const std::exception& ex) {
            session->Promise.Set(TError(ex));
        }
    }

    void OnBlockSetRead(
        TReadBlockSetSessionPtr session,
        const std::vector<int>& localIndexes,
        const TErrorOr<std::vector<TBlock>>& blocksOrError)
    {
        try {
            if (!blocksOrError.IsOK()) {
                ThrowError(blocksOrError);
            }

            const auto& blocks = blocksOrError.Value();
            for (int index = 0; index < localIndexes.size(); ++index) {
                session->Blocks[localIndexes[index]] = blocks[index];
            }

            RequestBlockSet(session);
        } catch (const std::exception& ex) {
            session->Promise.Set(TError(ex));
        }
    }

    void ThrowError(const TError& error)
    {
        if (FailureHandler_) {
            FailureHandler_.Run();
        }

        THROW_ERROR_EXCEPTION(
            NDataNode::EErrorCode::LocalChunkReaderFailed,
            "Error accessing local chunk %v",
            Chunk_->GetId())
            << error;
    }

};

IChunkReaderPtr CreateLocalChunkReader(
    TReplicationReaderConfigPtr config,
    IChunkPtr chunk,
    TChunkBlockManagerPtr chunkBlockManager,
    IBlockCachePtr blockCache,
    TClosure failureHandler)
{
    return New<TLocalChunkReader>(
        std::move(config),
        std::move(chunk),
        std::move(chunkBlockManager),
        std::move(blockCache),
        std::move(failureHandler));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT
