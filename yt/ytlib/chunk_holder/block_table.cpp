#include "stdafx.h"
#include "block_table.h"

namespace NYT {
namespace NChunkHolder {

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = ChunkHolderLogger;

////////////////////////////////////////////////////////////////////////////////

TBlockTable::TBlockTable(TBlockTableConfig* config)
    : Config(config)
{ }

const yvector<TPeerInfo>& TBlockTable::GetPeers(const TBlockId& blockId)
{
    SweepAllExpiredPeers();

    auto it = Table.find(blockId);
    if (it == Table.end()) {
        static yvector<TPeerInfo> empty;
        return empty;
    } else {
        SweepExpiredPeers(it->Second());
        return it->Second();
    }
}

void TBlockTable::UpdatePeer(const TBlockId& blockId, const TPeerInfo& peer)
{
    LOG_DEBUG("Updating peer (BlockId: %s, Address: %s, ExpirationTime: %s)",
        ~blockId.ToString(),
        ~peer.Address,
        ~peer.ExpirationTime.ToString());

    SweepAllExpiredPeers();

    auto& peers = GetMutablePeers(blockId);
    SweepExpiredPeers(peers); // In case when all expired peers were not swept

    for (auto it = peers.begin(); it != peers.end(); ++it) {
        if (it->Address == peer.Address) {
            peers.erase(it);
            break;
        }
    }
    
    {
        auto it = peers.begin();
        while (it != peers.end() && it->ExpirationTime > peer.ExpirationTime) {
            ++it;
        }

        peers.insert(it, peer);
    }

    if (peers.ysize() > Config->MaxPeersPerBlock) {
        peers.erase(peers.begin() + Config->MaxPeersPerBlock, peers.end());
    }
}

void TBlockTable::SweepAllExpiredPeers()
{
    if (TInstant::Now() < LastSwept + Config->SweepPeriod) {
        return;
    }

    // TODO: implement FilterMap/FilterSet
    auto it = Table.begin();
    while (it != Table.end()) {
        auto jt = it;
        ++jt;
        SweepExpiredPeers(it->Second());
        if (it->Second().empty()) {
            Table.erase(it);
        }
        it = jt;
    }
    
    LastSwept = TInstant::Now();

    LOG_DEBUG("All expired peers were swept");
}

void TBlockTable::SweepExpiredPeers(yvector<TPeerInfo>& peers)
{
    auto now = TInstant::Now();

    auto it = peers.end();
    while (it != peers.begin() && (it - 1)->ExpirationTime < now) {
        --it;
    }

    peers.erase(it, peers.end());
}

yvector<TPeerInfo>& TBlockTable::GetMutablePeers(const TBlockId& blockId)
{
    auto it = Table.find(blockId);
    if (it != Table.end())
        return it->Second();
    auto pair = Table.insert(MakePair(blockId, yvector<TPeerInfo>()));
    YASSERT(pair.Second());
    return pair.First()->Second();
}

//////////////////////////////////////////////////////////////////////////////// 

} // namespace NChunkHolder
} // namespace NYT
