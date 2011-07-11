#include "block_cache.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

TCachedBlock::TCachedBlock(TBlockId blockId, const TSharedRef& data)
    : TCacheValueBase<TBlockId, TCachedBlock, TBlockIdHash>(blockId)
    , Data(data)
{ }

////////////////////////////////////////////////////////////////////////////////

void TBlockCache::TConfig::Read(const TJsonObject* jsonConfig)
{
    if (jsonConfig == NULL)
        return;
    NYT::TryRead(jsonConfig, L"Capacity", &Capacity);
}

////////////////////////////////////////////////////////////////////////////////

TBlockCache::TBlockCache(const TConfig& config)
    : TCapacityLimitedCache<TBlockId, TCachedBlock, TBlockIdHash>(config.Capacity)
{}

TCachedBlock::TPtr TBlockCache::Put(TBlockId blockId, TSharedRef data)
{
    TCachedBlock::TPtr value = new TCachedBlock(blockId, data);
    Put(value);
    return Get(blockId);
}

void TBlockCache::Put(TCachedBlock::TPtr cachedBlock)
{
    TInsertCookie cookie(cachedBlock->GetKey());
    if (BeginInsert(&cookie))
        EndInsert(cachedBlock, &cookie);
}

TCachedBlock::TPtr TBlockCache::Get(TBlockId blockId)
{
    TAsyncResultPtr result = Lookup(blockId);
    TCachedBlock::TPtr block;
    if (~result != NULL && result->TryGet(&block))
        return block;
    else
        return NULL;

}

////////////////////////////////////////////////////////////////////////////////

}
