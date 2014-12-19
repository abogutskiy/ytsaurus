﻿#pragma once

#include "public.h"

#include <ytlib/table_client/public.h>

#include <ytlib/scheduler/job.pb.h>

namespace NYT {
namespace NJobProxy {

////////////////////////////////////////////////////////////////////////////////

struct IUserJobIO
    : private TNonCopyable
{
    virtual void Init() = 0;

    virtual const std::vector<NTableClient::ISyncWriterUnsafePtr>& GetWriters() const = 0;
    virtual const std::vector<NTableClient::ISyncReaderPtr>& GetReaders() const = 0;

    virtual void PopulateResult(NScheduler::NProto::TSchedulerJobResultExt* schedulerJobResultExt) = 0;

    virtual ~IUserJobIO()
    { }
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT


