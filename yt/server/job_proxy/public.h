﻿#pragma once

#include <ytlib/scheduler/public.h>

#include <core/misc/enum.h>

namespace NYT {
namespace NJobProxy {

////////////////////////////////////////////////////////////////////////////////

class IUserJobIO;

class TErrorOutput;

DECLARE_REFCOUNTED_CLASS(TJobProxyConfig)

DECLARE_REFCOUNTED_STRUCT(IJob)

DECLARE_REFCOUNTED_STRUCT(IJobHost)

DECLARE_REFCOUNTED_CLASS(TContextPreservingInput);

DECLARE_ENUM(EJobProxyExitCode,
    ((HeartbeatFailed)        (20))
    ((ResultReportFailed)     (21))
    ((ResourcesUpdateFailed)  (22))
    ((SetRLimitFailed)        (23))
    ((ExecFailed)             (24))
    ((UncaughtException)      (25))
    ((RetreiveJobSpecFailed)  (26))
);

DECLARE_ENUM(EErrorCode,
    ((MemoryLimitExceeded)  (1200))
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
