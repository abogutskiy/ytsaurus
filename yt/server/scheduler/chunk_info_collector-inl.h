#ifndef CHUNK_INFO_COLLECTOR_INL_H_
#error "Direct inclusion of this file is not allowed, include chunk_info_collector.h"
#endif
#undef CHUNK_INFO_COLLECTOR_INL_H_

#include <ytlib/actions/parallel_awaiter.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

template <class TChunkInfoFetcher>
TChunkInfoCollector<TChunkInfoFetcher>::TChunkInfoCollector(
    const TChunkInfoFetcherPtr& fetcher,
    const IInvokerPtr& invoker)
    : ChunkInfoFetcher(fetcher)
    , Invoker(invoker)
    , Promise(NewPromise< TValueOrError<void> >())
{ }

template <class TChunkInfoFetcher>
void TChunkInfoCollector<TChunkInfoFetcher>::AddChunk(
    const NTableClient::NProto::TInputChunk& chunk)
{
    YCHECK(UnfetchedChunkIndexes.insert(static_cast<int>(Chunks.size())).second);
    Chunks.push_back(chunk);
}

template <class TChunkInfoFetcher>
TFuture< TValueOrError<void> > TChunkInfoCollector<TChunkInfoFetcher>::Run()
{
    ChunkInfoFetcher->Prepare(Chunks);
    SendRequests();
    return Promise;
}

template <class TChunkInfoFetcher>
void TChunkInfoCollector<TChunkInfoFetcher>::SendRequests()
{
    auto& Logger = ChunkInfoFetcher->GetLogger();

    // Construct address -> chunk* map.
    typedef yhash_map<Stroka, std::vector<int> > TAddressToChunkIndexes;
    TAddressToChunkIndexes addressToChunkIndexes;

    FOREACH (auto chunkIndex, UnfetchedChunkIndexes) {
        const auto& chunk = Chunks[chunkIndex];
        auto chunkId = NChunkServer::TChunkId::FromProto(chunk.slice().chunk_id());
        bool chunkAvailable = false;
        FOREACH (const auto& address, chunk.node_addresses()) {
            if (DeadNodes.find(address) == DeadNodes.end() &&
                DeadChunks.find(std::make_pair(address, chunkId)) == DeadChunks.end())
            {
                addressToChunkIndexes[address].push_back(chunkIndex);
                chunkAvailable = true;
            }
        }
        if (!chunkAvailable) {
            Promise.Set(TError("Unable to fetch chunk info for chunk %s from any of nodes [%s]",
                ~chunkId.ToString(),
                ~JoinToString(chunk.node_addresses())));
            return;
        }
    }

    // Sort nodes by number of chunks (in decreasing order).
    std::vector<TAddressToChunkIndexes::iterator> addressIts;
    for (auto it = addressToChunkIndexes.begin(); it != addressToChunkIndexes.end(); ++it) {
        addressIts.push_back(it);
    }
    std::sort(
        addressIts.begin(),
        addressIts.end(),
        [=] (const TAddressToChunkIndexes::iterator& lhs, const TAddressToChunkIndexes::iterator& rhs) {
            return lhs->second.size() > rhs->second.size();
        });

    // Pick nodes greedily.
    auto awaiter = New<TParallelAwaiter>(Invoker);
    yhash_set<int> requestedChunkIndexes;
    FOREACH (const auto& it, addressIts) {
        auto address = it->first;

        ChunkInfoFetcher->CreateNewRequest(address);

        auto& addressChunkIndexes = it->second;
        std::vector<int> requestChunkIndexes;
        FOREACH (auto chunkIndex, addressChunkIndexes) {
            if (requestedChunkIndexes.find(chunkIndex) == requestedChunkIndexes.end()) {
                YCHECK(requestedChunkIndexes.insert(chunkIndex).second);

                const auto& chunk = Chunks[chunkIndex];
                if (ChunkInfoFetcher->AddChunkToRequest(chunk)) {
                    requestChunkIndexes.push_back(chunkIndex);
                }
            }
        }

        // Send the request, if not empty.
        if (!requestChunkIndexes.empty()) {
            LOG_DEBUG("Requesting chunk info for %d chunks from %s",
                static_cast<int>(requestChunkIndexes.size()),
                ~address);

            awaiter->Await(
                ChunkInfoFetcher->InvokeRequest(),
                BIND(
                    &TChunkInfoCollector<TChunkInfoFetcher>::OnResponse,
                    MakeStrong(this),
                    address,
                    Passed(MoveRV(requestChunkIndexes))));
        }
    }
    awaiter->Complete(BIND(
        &TChunkInfoCollector<TChunkInfoFetcher>::OnEndRound, 
        MakeStrong(this)));

    LOG_INFO("%d requests sent", awaiter->GetRequestCount());
}

template <class TChunkInfoFetcher>
void TChunkInfoCollector<TChunkInfoFetcher>::OnResponse(
    const Stroka& address,
    std::vector<int> chunkIndexes,
    typename TChunkInfoFetcher::TResponsePtr rsp)
{
    auto& Logger = ChunkInfoFetcher->GetLogger();

    if (rsp->IsOK()) {
        YCHECK(chunkIndexes.size() == rsp->samples_size());
        int samplesAdded = 0;
        for (int index = 0; index < static_cast<int>(chunkIndexes.size()); ++index) {
            int chunkIndex = chunkIndexes[index];
            const auto& chunk = Chunks[chunkIndex];
            auto chunkId = NChunkServer::TChunkId::FromProto(chunk.slice().chunk_id());

            auto result = ChunkInfoFetcher->ProcessResponseItem(rsp, index);
            if (result.IsOK()) {
                YCHECK(UnfetchedChunkIndexes.erase(chunkIndex) == 1);
            } else {
                auto error = TError(
                    "Unable to fetch chunk info for chunk %s from %s",
                    ~chunkId.ToString(),
                    ~address) << result;
                LOG_WARNING("%s", ~ToString(error));
                YCHECK(DeadChunks.insert(std::make_pair(address, chunkId)).second);
            }
        }
        LOG_DEBUG("Received chunk info from %s", ~address);
    } else {
        auto error = TError("Error requesting chunk info from %s",
            ~address) << rsp->GetError();
        LOG_WARNING("%s", ~ToString(error));
        YCHECK(DeadNodes.insert(address).second);
    }
}

template <class TChunkInfoFetcher>
void TChunkInfoCollector<TChunkInfoFetcher>::OnEndRound()
{
    auto& Logger = ChunkInfoFetcher->GetLogger();

    if (UnfetchedChunkIndexes.empty()) {
        LOG_INFO("All info is fetched");
        Promise.Set(TError());
    } else {
        SendRequests();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
