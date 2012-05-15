#include "stdafx.h"
#include "chunk_reader.h"
#include "channel_reader.h"
#include "private.h"
#include "chunk_meta_extensions.h"

#include <ytlib/table_client/table_chunk_meta.pb.h>
#include <ytlib/chunk_client/async_reader.h>
#include <ytlib/chunk_client/sequential_reader.h>
#include <ytlib/chunk_client/config.h>
#include <ytlib/chunk_client/private.h>
#include <ytlib/chunk_holder/chunk_meta_extensions.h>
#include <ytlib/ytree/tokenizer.h>
#include <ytlib/misc/foreach.h>
#include <ytlib/misc/sync.h>
#include <ytlib/misc/protobuf_helpers.h>
#include <ytlib/actions/invoker.h>
#include <ytlib/logging/tagged_logger.h>

#include <algorithm>
#include <limits>

namespace NYT {
namespace NTableClient {

using namespace NChunkClient;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = TableClientLogger;

////////////////////////////////////////////////////////////////////////////////

class TChunkReader::TKeyValidator
{
public:
    TKeyValidator(const NProto::TKey& pivot, bool leftBoundary)
        : LeftBoundary(leftBoundary)
    {
        Pivot.FromProto(pivot);
    }

    template<class TBuffer>
    bool IsValid(const TKey<TBuffer>& key)
    {
        int result = CompareKeys(key, Pivot);
        return LeftBoundary ? result >= 0 : result < 0;
    }

private:
    bool LeftBoundary;
    TKey<TBlobOutput> Pivot;

};

////////////////////////////////////////////////////////////////////////////////

//! Represents element of the heap used to determine 
//! block reading order. (see TInitializer::GetBlockReadingOrder).
struct TBlockInfo
{
    int ChunkBlockIndex;
    int ChannelBlockIndex;
    int ChannelIndex;
    i64 LastRow;

    bool operator< (const TBlockInfo& rhs)
    {
        return
            LastRow > rhs.LastRow || 
            (LastRow == rhs.LastRow && ChannelIndex > rhs.ChannelIndex);
    }

    TBlockInfo(
        int chunkBlockIndex, 
        int channelBlockIndex, 
        int channelIdx, 
        i64 lastRow)
        : ChunkBlockIndex(chunkBlockIndex)
        , ChannelBlockIndex(channelBlockIndex)
        , ChannelIndex(channelIdx)
        , LastRow(lastRow)
    { }
};

////////////////////////////////////////////////////////////////////////////////

// TODO(babenko): eliminate
template <template <typename T> class TComparator>
struct TChunkReader::TIndexComparator
{
    bool operator()(const NProto::TKey& key, const NProto::TIndexRow& row)
    {
        return Comparator(CompareKeys(key, row.key()), 0);
    }

    TComparator<int> Comparator;
};

////////////////////////////////////////////////////////////////////////////////

//! Helper class aimed to asynchronously initialize the internals of TChunkReader.
class TChunkReader::TInitializer
    : public TRefCounted
{
public:
    typedef TIntrusivePtr<TInitializer> TPtr;

    TInitializer(
        TSequentialReaderConfigPtr config,
        TChunkReaderPtr chunkReader, 
        NChunkClient::IAsyncReaderPtr asyncReader,
        const NProto::TReadLimit& startLimit,
        const NProto::TReadLimit& endLimit,
        int partitionTag)
        : SequentialConfig(config)
        , AsyncReader(asyncReader)
        , ChunkReader(chunkReader)
        , Channel(chunkReader->Channel)
        , StartLimit(startLimit)
        , EndLimit(endLimit)
        , HasRangeRequest(false)
        , PartitionTag(partitionTag)
        , Logger(TableClientLogger)
    { }

    void Initialize()
    {
        auto chunkReader = ChunkReader.Lock();
        YASSERT(chunkReader);

        Logger.AddTag(Sprintf("ChunkId: %s", ~AsyncReader->GetChunkId().ToString()));

        std::vector<int> tags;
        tags.reserve(10);
        tags.push_back(GetProtoExtensionTag<NChunkHolder::NProto::TBlocksExt>());
        tags.push_back(GetProtoExtensionTag<NChunkHolder::NProto::TMiscExt>());
        tags.push_back(GetProtoExtensionTag<NProto::TChannelsExt>());

        HasRangeRequest =
            (StartLimit.has_key() && (StartLimit.key().parts_size() > 0)) ||
            (EndLimit.has_key() && (EndLimit.key().parts_size() > 0));

        YASSERT(PartitionTag == DefaultPartitionTag || !HasRangeRequest);

        if (HasRangeRequest) {
            tags.push_back(GetProtoExtensionTag<NProto::TIndexExt>());
        }

        if (HasRangeRequest || chunkReader->Options.ReadKey) {
            tags.push_back(GetProtoExtensionTag<NProto::TKeyColumnsExt>());
        }

        AsyncReader->AsyncGetChunkMeta(&tags).Subscribe(BIND(
            &TInitializer::OnGotMeta, 
            MakeStrong(this)).Via(NChunkClient::ReaderThread->GetInvoker()));
    }

private:
    void OnFail(const TError& error, TChunkReaderPtr chunkReader) 
    {
        chunkReader->Initializer.Reset();
        chunkReader->State.Fail(error);
    }

    void OnGotMeta(NChunkClient::IAsyncReader::TGetMetaResult result)
    {
        auto chunkReader = ChunkReader.Lock();
        if (!chunkReader)
            return;

        if (!result.IsOK()) {
            LOG_WARNING("Failed to download chunk meta\n%s", ~result.GetMessage());
            OnFail(result, chunkReader);
            return;
        }

        LOG_DEBUG("Chunk meta received");

        FOREACH (const auto& column, Channel.GetColumns()) {
            auto& columnInfo = chunkReader->FixedColumns[TStringBuf(column)];
            columnInfo.InChannel = true;
        }

        auto miscExt = GetProtoExtension<NChunkHolder::NProto::TMiscExt>(
            result.Value().extensions());

        StartRowIndex = 0;
        chunkReader->EndRowIndex = miscExt->row_count();

        if (StartLimit.has_row_index()) {
            StartRowIndex = std::max(StartRowIndex, StartLimit.row_index());
        }

        if (EndLimit.has_row_index()) {
            chunkReader->EndRowIndex = std::min(chunkReader->EndRowIndex, EndLimit.row_index());
        }

        if (HasRangeRequest || chunkReader->Options.ReadKey) {
            if (!miscExt->sorted()) {
                LOG_WARNING("Received key range read request for an unsorted chunk");
                OnFail(
                    TError("Received key range read request for an unsorted chunk"), 
                    chunkReader);
                return;
            }

            chunkReader->KeyColumnsExt = GetProtoExtension<NProto::TKeyColumnsExt>(
                result.Value().extensions());

            YASSERT(chunkReader->KeyColumnsExt->values_size() > 0);
            for (int i = 0; i < chunkReader->KeyColumnsExt->values_size(); ++i) {
                const auto& column = chunkReader->KeyColumnsExt->values(i);
                Channel.AddColumn(column);
                auto& columnInfo = chunkReader->FixedColumns[TStringBuf(column)];
                columnInfo.KeyIndex = i;
                if (chunkReader->Channel.Contains(column))
                    columnInfo.InChannel = true;
            }

            chunkReader->CurrentKey.Reset(chunkReader->KeyColumnsExt->values_size());
        }

        if (HasRangeRequest) {
            auto indexExt = GetProtoExtension<NProto::TIndexExt>(
                result.Value().extensions());

            if (StartLimit.has_key() && StartLimit.key().parts_size() > 0) {
                StartValidator.Reset(new TKeyValidator(StartLimit.key(), true));

                typedef decltype(indexExt->items().begin()) TSampleIter;
                std::reverse_iterator<TSampleIter> rbegin(indexExt->items().end());
                std::reverse_iterator<TSampleIter> rend(indexExt->items().begin());
                auto it = std::upper_bound(
                    rbegin, 
                    rend, 
                    StartLimit.key(), 
                    TIndexComparator<std::greater>());

                if (it != rend) {
                    StartRowIndex = std::max(it->row_index() + 1, StartRowIndex);
                }
            }

            if (EndLimit.has_key() && EndLimit.key().parts_size() > 0) {
                chunkReader->EndValidator.Reset(new TKeyValidator(EndLimit.key(), false));

                auto it = std::upper_bound(
                    indexExt->items().begin(), 
                    indexExt->items().end(), 
                    EndLimit.key(), 
                    TIndexComparator<std::less>());

                if (it != indexExt->items().end()) {
                    chunkReader->EndRowIndex = std::min(
                        it->row_index(), 
                        chunkReader->EndRowIndex);
                }
            }
        }

        LOG_DEBUG("Reading rows %" PRId64 "-%" PRId64,
            StartRowIndex,
            chunkReader->EndRowIndex);

        chunkReader->CurrentRowIndex = StartRowIndex;
        if (chunkReader->CurrentRowIndex >= chunkReader->EndRowIndex) {
            LOG_WARNING("Nothing to read from the current chunk");
            chunkReader->Initializer.Reset();
            chunkReader->State.FinishOperation();
            return;
        }

        chunkReader->Codec = GetCodec(ECodecId(miscExt->codec_id()));

        ChannelsExt = GetProtoExtension<NProto::TChannelsExt>(
            result.Value().extensions());

        YASSERT(PartitionTag == DefaultPartitionTag || ChannelsExt->items_size() == 1);

        SelectChannels(chunkReader);
        YASSERT(SelectedChannels.size() > 0);

        LOG_DEBUG("Selected channels [%s]", ~JoinToString(SelectedChannels));

        auto blockIndexSequence = GetBlockReadSequence(chunkReader);

        chunkReader->SequentialReader = New<TSequentialReader>(
            SequentialConfig,
            blockIndexSequence,
            AsyncReader,
            GetProtoExtension<NChunkHolder::NProto::TBlocksExt>(
                result.Value().extensions()));

        LOG_DEBUG("Reading blocks [%s]", ~JoinToString(blockIndexSequence));

        chunkReader->ChannelReaders.reserve(SelectedChannels.size());

        chunkReader->SequentialReader->AsyncNextBlock().Subscribe(
            BIND(&TInitializer::OnStartingBlockReceived, MakeWeak(this), 0)
            .Via(ReaderThread->GetInvoker()));
    }

    void SelectChannels(TChunkReaderPtr chunkReader)
    {
        ChunkChannels.reserve(ChannelsExt->items_size());
        for(int i = 0; i < ChannelsExt->items_size(); ++i) {
            ChunkChannels.push_back(TChannel::FromProto(ChannelsExt->items(i).channel()));
        }

        // Heuristic: first try to find a channel that contain the whole read channel.
        // If several exists, choose the one with the minimum number of blocks.
        if (SelectSingleChannel(chunkReader))
            return;

        auto remainder = Channel;
        for (int channelIdx = 0; channelIdx < ChunkChannels.size(); ++channelIdx) {
            auto& currentChannel = ChunkChannels[channelIdx];
            if (currentChannel.Overlaps(remainder)) {
                remainder -= currentChannel;
                SelectedChannels.push_back(channelIdx);
                if (remainder.IsEmpty()) {
                    break;
                }
            }
        }
    }

    bool SelectSingleChannel(TChunkReaderPtr chunkReader)
    {
        int resultIdx = -1;
        size_t minBlockCount = std::numeric_limits<size_t>::max();

        for (int i = 0; i < ChunkChannels.size(); ++i) {
            auto& channel = ChunkChannels[i];
            if (channel.Contains(Channel)) {
                size_t blockCount = ChannelsExt->items(i).blocks_size();
                if (minBlockCount > blockCount) {
                    resultIdx = i;
                    minBlockCount = blockCount;
                }
            }
        }

        if (resultIdx < 0)
            return false;

        SelectedChannels.push_back(resultIdx); 
        return true;
    }

    void SelectOpeningBlocks(
        TChunkReaderPtr chunkReader,
        std::vector<int>& result, 
        std::vector<TBlockInfo>& blockHeap) 
    {
        FOREACH (auto channelIdx, SelectedChannels) {
            const auto& protoChannel = ChannelsExt->items(channelIdx);
            int blockIndex = -1;
            i64 startRow = 0;
            i64 lastRow = 0;
            while (true) {
                ++blockIndex;
                YASSERT(blockIndex < static_cast<int>(protoChannel.blocks_size()));
                const auto& protoBlock = protoChannel.blocks(blockIndex);

                YASSERT((protoBlock.partition_tag() == DefaultPartitionTag) == (PartitionTag == DefaultPartitionTag));


                startRow = lastRow;
                lastRow += protoBlock.row_count();

                if (lastRow > StartRowIndex && protoBlock.partition_tag() == PartitionTag) {
                    blockHeap.push_back(TBlockInfo(
                        protoBlock.block_index(),
                        blockIndex,
                        channelIdx,
                        lastRow));

                    result.push_back(protoBlock.block_index());
                    StartRows.push_back(startRow);
                    break;
                }
            }
        }
    }

    std::vector<int> GetBlockReadSequence(TChunkReaderPtr chunkReader)
    {
        std::vector<int> result;
        std::vector<TBlockInfo> blockHeap;

        SelectOpeningBlocks(chunkReader, result, blockHeap);

        std::make_heap(blockHeap.begin(), blockHeap.end());

        while (true) {
            auto currentBlock = blockHeap.front();
            int nextBlockIndex = currentBlock.ChannelBlockIndex + 1;
            const auto& protoChannel = ChannelsExt->items(currentBlock.ChannelIndex);
            int lastRow = currentBlock.LastRow;

            std::pop_heap(blockHeap.begin(), blockHeap.end());
            blockHeap.pop_back();

            YASSERT(nextBlockIndex <= protoChannel.blocks_size());

            if (currentBlock.LastRow >= chunkReader->EndRowIndex) {
                FOREACH (auto& block, blockHeap) {
                    YASSERT(block.LastRow >= chunkReader->EndRowIndex);
                }
                break;
            }

            while (nextBlockIndex < protoChannel.blocks_size()) {
                const auto& protoBlock = protoChannel.blocks(nextBlockIndex);
                YASSERT((protoBlock.partition_tag() == DefaultPartitionTag && PartitionTag == DefaultPartitionTag) ||
                    (protoBlock.partition_tag() != DefaultPartitionTag && PartitionTag != DefaultPartitionTag));

                lastRow += protoBlock.row_count();

                if (protoBlock.partition_tag() != PartitionTag) {
                    ++nextBlockIndex;
                } else {
                    blockHeap.push_back(TBlockInfo(
                        protoBlock.block_index(),
                        nextBlockIndex,
                        currentBlock.ChannelIndex,
                        lastRow));

                    std::push_heap(blockHeap.begin(), blockHeap.end());
                    result.push_back(protoBlock.block_index());
                    break;
                }
            }
        }

        return result;
    }

    void OnStartingBlockReceived(int selectedChannelIndex, TError error)
    {
        auto chunkReader = ChunkReader.Lock();
        if (!chunkReader) {
            LOG_DEBUG("Chunk reader canceled during initialization");
            return;
        }

        auto& channelIdx = SelectedChannels[selectedChannelIndex];

        LOG_DEBUG("Fetched starting block for channel %d", channelIdx);

        if (!error.IsOK()) {
            LOG_WARNING("Failed to download starting block for channel %d\n%s", 
                channelIdx,
                ~error.GetMessage());
            OnFail(error, chunkReader);
            return;
        }

        chunkReader->ChannelReaders.push_back(New<TChannelReader>(ChunkChannels[channelIdx]));

        auto& channelReader = chunkReader->ChannelReaders.back();
        auto compressedBlock = chunkReader->SequentialReader->GetBlock();
        auto decompressedBlock = chunkReader->Codec->Decompress(compressedBlock);
        if (chunkReader->Options.KeepBlocks)
            chunkReader->FetchedBlocks.push_back(decompressedBlock);
        channelReader->SetBlock(decompressedBlock);

        for (i64 rowIndex = StartRows[selectedChannelIndex]; 
            rowIndex < StartRowIndex; 
            ++rowIndex) 
        {
            YVERIFY(channelReader->NextRow());
        }

        LOG_DEBUG("Skipped initial rows for channel %d", channelIdx);

        ++selectedChannelIndex;
        if (selectedChannelIndex < SelectedChannels.size()) {
            chunkReader->SequentialReader->AsyncNextBlock()
                .Subscribe(BIND(
                    &TInitializer::OnStartingBlockReceived, 
                    MakeWeak(this), 
                    selectedChannelIndex)
                .Via(ReaderThread->GetInvoker()));
        } else {
            // Create current row.
            LOG_DEBUG("All starting blocks received");

            chunkReader->MakeCurrentRow();
            ValidateRow(TError());
        }
    }

    void ValidateRow(TError error)
    {
        auto chunkReader = ChunkReader.Lock();
        if (!chunkReader)
            return;

        while (true) {
            LOG_TRACE("Validating row %" PRId64, chunkReader->CurrentRowIndex);

            YASSERT(chunkReader->CurrentRowIndex < chunkReader->EndRowIndex);
            if (~StartValidator && !StartValidator->IsValid(chunkReader->CurrentKey)) {
                auto result = chunkReader->DoNextRow();

                // This quick check is aimed to improve potential performance issue and
                // eliminate unnecessary calls to Subscribe and BIND.
                if (!result.IsSet()) {
                    result.Subscribe(
                        BIND(&TInitializer::ValidateRow, MakeWeak(this))
                        .Via(ReaderThread->GetInvoker()));
                    return;
                }
            } else {
                break;
            }
        }

        LOG_DEBUG("Reader initialized");

        // Initialization complete.
        chunkReader->Initializer.Reset();
        chunkReader->State.FinishOperation();
    }

    TSequentialReaderConfigPtr SequentialConfig;
    NChunkClient::IAsyncReaderPtr AsyncReader;
    TWeakPtr<TChunkReader> ChunkReader;

    NLog::TTaggedLogger Logger;

    TChannel Channel;

    NProto::TReadLimit StartLimit;
    NProto::TReadLimit EndLimit;

    i64 StartRowIndex;

    THolder<TKeyValidator> StartValidator;

    TAutoPtr<NProto::TChannelsExt> ChannelsExt;
    std::vector<TChannel> ChunkChannels;
    std::vector<int> SelectedChannels;

    //! First row of the first block in each selected channel.
    /*!
     *  Is used to set channel readers to ChunkReader's StartRow during initialization.
     */
    std::vector<i64> StartRows;
    bool HasRangeRequest;

    int PartitionTag;
};

////////////////////////////////////////////////////////////////////////////////

TChunkReader::TChunkReader(TSequentialReaderConfigPtr config,
    const TChannel& channel,
    NChunkClient::IAsyncReaderPtr chunkReader,
    const NProto::TReadLimit& startLimit,
    const NProto::TReadLimit& endLimit,
    const NYTree::TYson& rowAttributes,
    int partitionTag,
    TReaderOptions options)
    : Codec(NULL)
    , SequentialReader(NULL)
    , Channel(channel)
    , CurrentRowIndex(-1)
    , EndRowIndex(0)
    , Options(options)
    , RowAttributes(rowAttributes)
    , SuccessResult(MakePromise(TError()))
{
    VERIFY_THREAD_AFFINITY_ANY();
    YASSERT(chunkReader);

    Initializer = New<TInitializer>(
        config, 
        this, 
        chunkReader, 
        startLimit, 
        endLimit,
        partitionTag);
}

TAsyncError TChunkReader::AsyncOpen()
{
    State.StartOperation();

    Initializer->Initialize();

    return State.GetOperationError();
}

TAsyncError TChunkReader::AsyncNextRow()
{
    // No thread affinity, called from SetCurrentChunk of TChunkSequenceReader.
    YASSERT(!State.HasRunningOperation());
    YASSERT(!Initializer);

    State.StartOperation();

    // This is a performance-critical spot. Try to avoid using callbacks for synchronously fetched rows.
    auto asyncResult = DoNextRow();
    auto error = asyncResult.TryGet();
    if (error) {
        OnRowFetched(error.Get());
    } else {
        asyncResult.Subscribe(BIND(&TChunkReader::OnRowFetched, MakeWeak(this)));
    }
    
    return State.GetOperationError();
}

void TChunkReader::OnRowFetched(TError error)
{
    if (error.IsOK()) {
        State.FinishOperation();
    } else {
        State.Fail(error);
    }
}

TAsyncError TChunkReader::DoNextRow()
{
    CurrentRowIndex = std::min(CurrentRowIndex + 1, EndRowIndex);

    if (CurrentRowIndex == EndRowIndex)
        return SuccessResult;

    UsedRangeColumns.clear();
    FOREACH (auto& it, FixedColumns) {
        it.second.Used = false;
    }
    CurrentRow.clear();
    CurrentKey.Reset();

    return ContinueNextRow(-1, SuccessResult, TError());
}

TAsyncError TChunkReader::ContinueNextRow(
    int channelIndex,
    TAsyncErrorPromise result,
    TError error)
{
    if (!error.IsOK()) {
        YASSERT(!result.IsSet());
        result.Set(error);
        return result;
    }

    if (channelIndex >= 0) {
        auto& channel = ChannelReaders[channelIndex];
        auto decompressedBlock = Codec->Decompress(SequentialReader->GetBlock());
        if (Options.KeepBlocks)
            FetchedBlocks.push_back(decompressedBlock);
        channel->SetBlock(decompressedBlock);
    }

    ++channelIndex;

    while (channelIndex < ChannelReaders.size()) {
        auto& channel = ChannelReaders[channelIndex];
        if (!channel->NextRow()) {
            YASSERT(SequentialReader->HasNext());

            if (result.IsSet()) {
                // Possible when called directly from DoNextRow
                result = NewPromise<TError>();
            }

            SequentialReader->AsyncNextBlock().Subscribe(BIND(
                IgnoreResult(&TChunkReader::ContinueNextRow),
                MakeWeak(this),
                channelIndex,
                result));
            return result;
        }
        ++channelIndex;
    }

    MakeCurrentRow();

    if (!result.IsSet()) {
        result.Set(TError());
    }
    return result;
}

void TChunkReader::MakeCurrentRow()
{
    TLexer lexer;

    FOREACH (const auto& reader, ChannelReaders) {
        while (reader->NextColumn()) {
            auto column = reader->GetColumn();
            auto fixedColumnsIt = FixedColumns.find(column);
            if (fixedColumnsIt != FixedColumns.end()) {
                auto& columnInfo = fixedColumnsIt->second;
                if (!columnInfo.Used) {
                    columnInfo.Used = true;

                    if (columnInfo.KeyIndex >= 0) {
                        // Use first token to create key part.
                        CurrentKey.SetKeyPart(
                            columnInfo.KeyIndex,
                            reader->GetValue(),
                            lexer);
                    }

                    if (columnInfo.InChannel) {
                        CurrentRow.push_back(std::make_pair(column, reader->GetValue()));
                    }
                }
            } else if (UsedRangeColumns.insert(column).second && Channel.ContainsInRanges(column)) {
                CurrentRow.push_back(std::make_pair(column, reader->GetValue()));
            }
        }
    }
}

TRow& TChunkReader::GetRow()
{
    VERIFY_THREAD_AFFINITY(ClientThread);
    YASSERT(!State.HasRunningOperation());
    YASSERT(!Initializer);

    return CurrentRow;
}

const TNonOwningKey& TChunkReader::GetKey() const
{
    VERIFY_THREAD_AFFINITY(ClientThread);
    YASSERT(!State.HasRunningOperation());
    YASSERT(!Initializer);

    YASSERT(Options.ReadKey);

    return CurrentKey;
}

bool TChunkReader::IsValid() const
{
    if (CurrentRowIndex >= EndRowIndex)
        return false;
    if (!EndValidator)
        return true;
    return EndValidator->IsValid(CurrentKey);
}

const TYson& TChunkReader::GetRowAttributes() const
{
    return RowAttributes;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
