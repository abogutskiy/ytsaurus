﻿#pragma once

#include <core/misc/common.h>
#include <core/misc/small_vector.h>

#include <ytlib/object_client/public.h>

namespace NYT {
namespace NChunkClient {

///////////////////////////////////////////////////////////////////////////////

namespace NProto {

class TChunkSpec;
class TChunkMeta;

class TReqFetch;

} // namespace NProto

///////////////////////////////////////////////////////////////////////////////

typedef NObjectClient::TObjectId TChunkId;
extern TChunkId NullChunkId;

typedef NObjectClient::TObjectId TChunkListId;
extern TChunkListId NullChunkListId;

typedef NObjectClient::TObjectId TChunkTreeId;
extern TChunkTreeId NullChunkTreeId;

//! Used as an expected upper bound in SmallVector.
/*
 *  Maximum regular number of replicas is 16 (for LRC codec).
 *  Additional +8 enables some flexibility during balancing.
 */
const int TypicalReplicaCount = 24;

class TChunkReplica;
typedef SmallVector<TChunkReplica, TypicalReplicaCount> TChunkReplicaList;

//! Represents an offset inside a chunk.
typedef i64 TBlockOffset;

//! A |(chunkId, blockIndex)| pair.
struct TBlockId;

DECLARE_ENUM(EChunkType,
    ((Unknown) (0))
    ((File)    (1))
    ((Table)   (2))
);

DECLARE_ENUM(EErrorCode,
    ((AllTargetNodesFailed)     (700))
    ((PipelineFailed)           (701))
    ((NoSuchSession)            (702))
    ((SessionAlreadyExists)     (703))
    ((ChunkAlreadyExists)       (704))
    ((WindowError)              (705))
    ((BlockContentMismatch)     (706))
    ((NoSuchBlock)              (707))
    ((NoSuchChunk)              (708))
    ((ChunkPrecachingFailed)    (709))
    ((OutOfSpace)               (710))
    ((IOError)                  (711))

    ((MasterCommunicationFailed)(712))
);

//! Values must be contiguous.
DECLARE_ENUM(EWriteSessionType,
    ((User)                     (0))
    ((Replication)              (1))
    ((Repair)                   (2))
);

DECLARE_ENUM(EReadSessionType,
    ((User)                     (0))
    ((Repair)                   (1))
);

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TReplicationReaderConfig)
DECLARE_REFCOUNTED_CLASS(TClientBlockCacheConfig)
DECLARE_REFCOUNTED_CLASS(TEncodingWriterOptions)
DECLARE_REFCOUNTED_CLASS(TDispatcherConfig)
DECLARE_REFCOUNTED_CLASS(TMultiChunkWriterConfig)
DECLARE_REFCOUNTED_CLASS(TMultiChunkWriterOptions)
DECLARE_REFCOUNTED_CLASS(TMultiChunkReaderConfig)
DECLARE_REFCOUNTED_CLASS(TSequentialReaderConfig)
DECLARE_REFCOUNTED_CLASS(TReplicationWriterConfig)
DECLARE_REFCOUNTED_CLASS(TErasureWriterConfig)
DECLARE_REFCOUNTED_CLASS(TEncodingWriterConfig)
DECLARE_REFCOUNTED_CLASS(TFetcherConfig)

DECLARE_REFCOUNTED_CLASS(TEncodingWriter)
DECLARE_REFCOUNTED_CLASS(TEncodingChunkWriter)
DECLARE_REFCOUNTED_CLASS(TSequentialReader)

DECLARE_REFCOUNTED_STRUCT(IAsyncReader)
DECLARE_REFCOUNTED_STRUCT(IAsyncWriter)

DECLARE_REFCOUNTED_STRUCT(IChunkWriterBase)

DECLARE_REFCOUNTED_STRUCT(IBlockCache)

DECLARE_REFCOUNTED_CLASS(TFileReader)
DECLARE_REFCOUNTED_CLASS(TFileWriter)

DECLARE_REFCOUNTED_CLASS(TMemoryReader)
DECLARE_REFCOUNTED_CLASS(TMemoryWriter)

template <class TChunkReader>
class TMultiChunkSequentialReader;

template <class TChunkWriter>
class TOldMultiChunkSequentialWriter;

template <class TChunkReader>
class TMultiChunkParallelReader;

DECLARE_REFCOUNTED_CLASS(TRefCountedChunkSpec)
DECLARE_REFCOUNTED_CLASS(TChunkSlice)

class TReadLimit;

class TChannel;
typedef std::vector<TChannel> TChannels;

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT

