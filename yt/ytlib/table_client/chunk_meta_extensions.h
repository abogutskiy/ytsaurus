#pragma once

#include "public.h"
#include "unversioned_row.h"

#include <yt/ytlib/table_client/chunk_meta.pb.h>
#include <yt/ytlib/table_client/legacy_chunk_meta.pb.h>

#include <yt/core/misc/phoenix.h>
#include <yt/core/misc/protobuf_helpers.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

DECLARE_PROTO_EXTENSION(NTableClient::NProto::TTableSchemaExt, 50)
DECLARE_PROTO_EXTENSION(NTableClient::NProto::TBlockMetaExt, 51)
DECLARE_PROTO_EXTENSION(NTableClient::NProto::TNameTableExt, 53)
DECLARE_PROTO_EXTENSION(NTableClient::NProto::TBoundaryKeysExt, 55)
DECLARE_PROTO_EXTENSION(NTableClient::NProto::TSamplesExt, 56)
DECLARE_PROTO_EXTENSION(NTableClient::NProto::TPartitionsExt, 57)
DECLARE_PROTO_EXTENSION(NTableClient::NProto::TColumnMetaExt, 58)

// Moved from old table client.
DECLARE_PROTO_EXTENSION(NTableClient::NProto::TKeyColumnsExt, 14)

// Legacy.
DECLARE_PROTO_EXTENSION(NTableClient::NProto::TChannelsExt, 10)
DECLARE_PROTO_EXTENSION(NTableClient::NProto::TOldSamplesExt, 11)
DECLARE_PROTO_EXTENSION(NTableClient::NProto::TIndexExt, 12)
DECLARE_PROTO_EXTENSION(NTableClient::NProto::TOldBoundaryKeysExt, 13)

////////////////////////////////////////////////////////////////////////////////

namespace NTableClient {

struct TBoundaryKeys
    : public NPhoenix::TFactoryTag<NPhoenix::TSimpleFactory>
{
    TOwningKey MinKey;
    TOwningKey MaxKey;

    void Persist(NPhoenix::TPersistenceContext& context);

    size_t SpaceUsed() const;
};

Stroka ToString(const TBoundaryKeys& keys);

////////////////////////////////////////////////////////////////////////////////

bool TryGetBoundaryKeys(
    const NChunkClient::NProto::TChunkMeta& chunkMeta,
    TOwningKey* minKey,
    TOwningKey* maxKey);

std::unique_ptr<TBoundaryKeys> GetBoundaryKeys(
    const NChunkClient::NProto::TChunkMeta& chunkMeta);

NChunkClient::NProto::TChunkMeta FilterChunkMetaByPartitionTag(
    const NChunkClient::NProto::TChunkMeta& chunkMeta,
    int partitionTag);

const NProto::TBoundaryKeysExt& EmptyBoundaryKeys();

} // namespace NTableClient

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
