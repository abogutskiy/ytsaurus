#pragma once

#include <yt/yt/ytlib/chunk_client/parallel_reader_memory_manager.h>

namespace NYT::NChunkClient {

// TODO(max42): move to .cpp

////////////////////////////////////////////////////////////////////////////////

class TMultiReaderMemoryManagerMock
    : public IMultiReaderMemoryManager
    , public IReaderMemoryManagerHost
{
public:
    virtual TChunkReaderMemoryManagerPtr CreateChunkReaderMemoryManager(
        std::optional<i64> /*reservedMemorySize*/,
        NProfiling::TTagIdList /*profilingTagList*/) override
    {
        YT_UNIMPLEMENTED();
    }

    virtual IMultiReaderMemoryManagerPtr CreateMultiReaderMemoryManager(
        std::optional<i64> /*requiredMemorySize*/,
        NProfiling::TTagIdList /*profilingTagList*/) override
    {
        YT_UNIMPLEMENTED();
    }

    virtual void Unregister(IReaderMemoryManagerPtr /*readerMemoryManager*/) override
    {
        YT_UNIMPLEMENTED();
    }

    virtual void UpdateMemoryRequirements(IReaderMemoryManagerPtr /*readerMemoryManager*/) override
    {
        YT_UNIMPLEMENTED();
    }

    virtual i64 GetRequiredMemorySize() const override
    {
        YT_UNIMPLEMENTED();
    }

    virtual i64 GetDesiredMemorySize() const override
    {
        YT_UNIMPLEMENTED();
    }

    virtual i64 GetReservedMemorySize() const override
    {
        YT_UNIMPLEMENTED();
    }

    virtual void SetReservedMemorySize(i64 /*size*/) override
    {
        YT_UNIMPLEMENTED();
    }

    virtual i64 GetFreeMemorySize() override
    {
        YT_UNIMPLEMENTED();
    }

    virtual void Finalize() override
    { }

    virtual const NProfiling::TTagIdList& GetProfilingTagList() const override
    {
        YT_UNIMPLEMENTED();
    }

    virtual void AddChunkReaderInfo(TGuid /*chunkReaderId*/) override
    { }

    virtual void AddReadSessionInfo(TGuid /*readSessionId*/) override
    {
        YT_UNIMPLEMENTED();
    }

    virtual TGuid GetId() const override
    {
        YT_UNIMPLEMENTED();
    }
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
