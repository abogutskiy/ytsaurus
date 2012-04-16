#pragma once

#include "public.h"

namespace NYT {
namespace NChunkServer {

////////////////////////////////////////////////////////////////////////////////

//! Acts as a compact type-safe handle that either points nowhere,
//! to a chunk, or to a chunk list.
//! The actual type is stored in the lowest bits.
class TChunkTreeRef
{
public:
    TChunkTreeRef();
    explicit TChunkTreeRef(TChunk* chunk);
    explicit TChunkTreeRef(TChunkList* chunkList);

    bool operator == (const TChunkTreeRef& other) const;
    bool operator != (const TChunkTreeRef& other) const;

    NObjectServer::EObjectType GetType() const;

    TChunk* AsChunk() const;
    TChunkList* AsChunkList() const;

    TChunkTreeId GetId() const;

private:
    friend struct ::hash<TChunkTreeRef>;

    uintptr_t Cookie;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT

// Hashing helper.

template <>
struct hash<NYT::NChunkServer::TChunkTreeRef>
{
    inline size_t operator()(const NYT::NChunkServer::TChunkTreeRef& chunkRef) const
    {
        return hash<uintptr_t>()(chunkRef.Cookie);
    }
};

// GetObjectId specialization.

namespace NYT {
namespace NObjectServer {

inline NObjectServer::TObjectId GetObjectId(const NChunkServer::TChunkTreeRef& object)
{
    return object.GetId();
}

} // namespace NChunkServer
} // namespace NYT

////////////////////////////////////////////////////////////////////////////////
