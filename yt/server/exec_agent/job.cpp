#include "stdafx.h"
#include "job.h"
#include "environment_manager.h"
#include "slot.h"
#include "environment.h"
#include "private.h"

#include <ytlib/misc/fs.h>
#include <ytlib/misc/assert.h>

#include <ytlib/ytree/serialize.h>

#include <ytlib/transaction_client/transaction.h>

#include <ytlib/file_client/file_ypath_proxy.h>

#include <server/chunk_holder/chunk.h>
#include <server/chunk_holder/location.h>
#include <server/chunk_holder/chunk_cache.h>

#include <server/job_proxy/config.h>

#include <server/scheduler/job_resources.h>

namespace NYT {
namespace NExecAgent {

using namespace NScheduler;
using namespace NScheduler::NProto;
using namespace NRpc;
using namespace NJobProxy;
using namespace NYTree;
using NChunkClient::TChunkId;

////////////////////////////////////////////////////////////////////////////////

TJob::TJob(
    const TJobId& jobId,
    const TJobSpec& jobSpec,
    TJobProxyConfigPtr proxyConfig,
    NChunkHolder::TChunkCachePtr chunkCache,
    TSlotPtr slot)
    : JobId(jobId)
    , JobSpec(jobSpec)
    // Use initial utilization provided by the scheduler.
    , ResourceUtilization(JobSpec.resource_utilization())
    , Logger(ExecAgentLogger)
    , ChunkCache(chunkCache)
    , Slot(slot)
    , JobState(EJobState::Running)
    , JobProgress(EJobProgress::Created)
    , JobResult(Null)
    , JobFinished(NewPromise<void>())
    , ProxyConfig(proxyConfig)
{
    VERIFY_INVOKER_AFFINITY(Slot->GetInvoker(), JobThread);

    Logger.AddTag(Sprintf("JobId: %s", ~jobId.ToString()));
    Slot->Acquire();
}

TJob::~TJob()
{
    Slot->Release();
}

void TJob::Start(TEnvironmentManagerPtr environmentManager)
{
    YASSERT(JobProgress == EJobProgress::Created);

    Slot->GetInvoker()->Invoke(BIND(
        &TJob::DoStart,
        MakeWeak(this),
        environmentManager));
}

void TJob::DoStart(TEnvironmentManagerPtr environmentManager)
{
    VERIFY_THREAD_AFFINITY(JobThread);

    if (JobProgress > EJobProgress::Cleanup)
        return;

    YASSERT(JobProgress == EJobProgress::Created);

    JobProgress = EJobProgress::PreparingConfig;

    {
        INodePtr ioConfigNode;
        try {
            ioConfigNode = ConvertToNode(TYsonString(JobSpec.io_config()));
        } catch (const std::exception& ex) {
            Stroka message = Sprintf(
                "Error deserializing job IO configuration\n%s", 
                ex.what());
            LOG_ERROR("%s", ~message);
            DoAbort(TError(message), EJobState::Failed);
            return;
        }

        auto ioConfig = New<TJobIOConfig>();
        ioConfig->Load(ioConfigNode);

        auto proxyConfig = CloneYsonSerializable(ProxyConfig);
        proxyConfig->JobIO = ioConfig;

        auto proxyConfigPath = NFS::CombinePaths(
            Slot->GetWorkingDirectory(), 
            ProxyConfigFileName);
        
        TFileOutput output(proxyConfigPath);
        TYsonWriter writer(&output, EYsonFormat::Pretty);
        proxyConfig->Save(&writer);
    }

    JobProgress = EJobProgress::PreparingProxy;

    Stroka environmentType = "default";
    try {
        ProxyController = environmentManager->CreateProxyController(
            //XXX(psushin): execution environment type must not be directly
            // selectable by user -- it is more of the global cluster setting
            //jobSpec.operation_spec().environment(),
            environmentType,
            JobId,
            Slot->GetWorkingDirectory());
    } catch (const std::exception& ex) {
        Stroka message = Sprintf(
            "Failed to create proxy controller for environment %s\n%s", 
            ~environmentType.Quote(),
            ex.what());
        LOG_ERROR("%s", ~message);
        DoAbort(TError(message), EJobState::Failed);
        return;
    }

    JobProgress = EJobProgress::PreparingSandbox;
    Slot->InitSandbox();

    auto awaiter = New<TParallelAwaiter>(Slot->GetInvoker());

    if (JobSpec.HasExtension(TMapJobSpecExt::map_job_spec_ext)) {
        const auto& jobSpecExt = JobSpec.GetExtension(TMapJobSpecExt::map_job_spec_ext);
        PrepareUserJob(jobSpecExt.mapper_spec(), awaiter);
    }

    if (JobSpec.HasExtension(TReduceJobSpecExt::reduce_job_spec_ext)) {
        const auto& jobSpecExt = JobSpec.GetExtension(TReduceJobSpecExt::reduce_job_spec_ext);
        PrepareUserJob(jobSpecExt.reducer_spec(), awaiter);
    }

    if (JobSpec.HasExtension(TPartitionJobSpecExt::partition_job_spec_ext)) {
        const auto& jobSpecExt = JobSpec.GetExtension(TPartitionJobSpecExt::partition_job_spec_ext);
        if (jobSpecExt.has_mapper_spec()) {
            PrepareUserJob(jobSpecExt.mapper_spec(), awaiter);
        }
    }

    awaiter->Complete(BIND(&TJob::RunJobProxy, MakeWeak(this)));
}

void TJob::PrepareUserJob(
    const NScheduler::NProto::TUserJobSpec& userJobSpec,
    TParallelAwaiterPtr awaiter)
{
    FOREACH (const auto& fetchRsp, userJobSpec.files()) {
        auto chunkId = TChunkId::FromProto(fetchRsp.chunk_id());
        LOG_INFO("Downloading user file (FileName: %s, ChunkId: %s)", 
            ~fetchRsp.file_name(),
            ~chunkId.ToString());
        awaiter->Await(
            ChunkCache->DownloadChunk(chunkId), 
            BIND(&TJob::OnChunkDownloaded, MakeWeak(this), fetchRsp));
    }
}

void TJob::OnChunkDownloaded(
    const NFileClient::NProto::TRspFetch& fetchRsp,
    NChunkHolder::TChunkCache::TDownloadResult result)
{
    VERIFY_THREAD_AFFINITY(JobThread);

    if (JobProgress > EJobProgress::Cleanup)
        return;

    YASSERT(JobProgress == EJobProgress::PreparingSandbox);

    auto fileName = fetchRsp.file_name();

    if (!result.IsOK()) {
        Stroka message = Sprintf(
            "Failed to download user file (FileName: %s)\n%s", 
            ~fileName,
            ~result.ToString());
        LOG_WARNING("%s", ~message);
        DoAbort(TError(message), EJobState::Failed, false);
        return;
    }

    CachedChunks.push_back(result.Value());

    try {
        Slot->MakeLink(
            fileName, 
            CachedChunks.back()->GetFileName(), 
            fetchRsp.executable());
    } catch (const std::exception& ex) {
        Stroka message = Sprintf(
            "Failed to create a symlink for %s\n%s", 
            ~fileName.Quote(),
            ex.what());
        LOG_ERROR("%s", ~message);
        DoAbort(TError(message), EJobState::Failed, false);
        return;
    }

    LOG_INFO("User file downloaded successfully (FileName: %s)",
        ~fileName);
}

void TJob::RunJobProxy()
{
    VERIFY_THREAD_AFFINITY(JobThread);

    if (JobProgress > EJobProgress::Cleanup)
        return;

    YASSERT(JobProgress == EJobProgress::PreparingSandbox);

    try {
        JobProgress = EJobProgress::StartedProxy;
        ProxyController->Run();
    } catch (const std::exception& ex) {
        DoAbort(TError(ex.what()), EJobState::Failed);
        return;
    }

    ProxyController->SubscribeExited(BIND(
        &TJob::OnJobExit,
        MakeWeak(this)).Via(Slot->GetInvoker()));
}

bool TJob::IsResultSet() const
{
    TGuard<TSpinLock> guard(SpinLock);
    return JobResult.IsInitialized();
}

void TJob::OnJobExit(TError error)
{
    VERIFY_THREAD_AFFINITY(JobThread);

    // NB: we expect that
    //  1. job proxy process finished
    //  2. proxy controller already cleaned up possible child processes.

    if (JobProgress > EJobProgress::Cleanup)
        return;

    YASSERT(JobProgress < EJobProgress::Cleanup);

    if (!error.IsOK()) {
        DoAbort(error, EJobState::Failed);
        return;
    }

    if (!IsResultSet()) {
        DoAbort(TError(
            "Job proxy exited successfully but job result has not been set"),
            EJobState::Failed);
        return;
    }

    JobProgress = EJobProgress::Cleanup;
    Slot->Clean();

    JobProgress = EJobProgress::Completed;
        
    if (JobResult->error().code() == TError::OK) {
        JobState = EJobState::Completed;
    } else {
        JobState = EJobState::Failed;
    }

    JobFinished.Set();
}

const TJobId& TJob::GetId() const
{
    return JobId;
}

const TJobSpec& TJob::GetSpec()
{
    return JobSpec;
}

void TJob::SetResult(const TJobResult& jobResult)
{
    TGuard<TSpinLock> guard(SpinLock);

    if (!JobResult.IsInitialized() || JobResult->error().code() == TError::OK) {
        JobResult.Assign(jobResult);
    }
}

const TJobResult& TJob::GetResult() const
{
    TGuard<TSpinLock> guard(SpinLock);
    YCHECK(JobResult.IsInitialized());
    return JobResult.Get();
}

void TJob::SetResult(const TError& error)
{
    TJobResult jobResult;
    *jobResult.mutable_error() = error.ToProto();
    SetResult(jobResult);
}

EJobState TJob::GetState() const
{
    return JobState;
}

EJobProgress TJob::GetProgress() const
{
    return JobProgress;
}

TNodeResources TJob::GetResourceUtilization() const
{
    return
        JobState == EJobState::Running || JobState == EJobState::Aborting
        ? ResourceUtilization
        : ZeroResources();
}

void TJob::SetResourceUtilization(const TNodeResources& utilization)
{
    if (JobState == EJobState::Running) {
        ResourceUtilization = utilization;
        ResourceUtilizationSet_.Fire();
    }
}

void TJob::Abort()
{
    JobState = EJobState::Aborting;
    Slot->GetInvoker()->Invoke(BIND(
        &TJob::DoAbort,
        MakeStrong(this),
        TError("Job aborted by scheduler"),
        EJobState::Aborted,
        true));
}

void TJob::DoAbort(const TError& error, EJobState resultState, bool killJobProxy)
{
    VERIFY_THREAD_AFFINITY(JobThread);

    if (JobProgress > EJobProgress::Cleanup) {
        JobState = resultState;
        return;
    }

    YASSERT(JobProgress < EJobProgress::Cleanup);

    const auto jobProgress = JobProgress;
    JobProgress = EJobProgress::Cleanup;

    LOG_INFO("Aborting job");

    if (jobProgress >= EJobProgress::StartedProxy && killJobProxy) {
        try {
            LOG_INFO("Killing job");
            ProxyController->Kill(error);
        } catch (const std::exception& ex) {
            // NB: Retries should be done inside proxy controller (if makes sense).
            LOG_FATAL("Failed to kill job\n%s", ex.what());
        }
    }

    if (jobProgress >= EJobProgress::PreparingSandbox) {
        LOG_INFO("Cleaning slot");
        Slot->Clean();
    }

    SetResult(error);
    JobProgress = EJobProgress::Failed;
    JobState = resultState;
    JobFinished.Set();
}

void TJob::SubscribeFinished(const TClosure& callback)
{
    JobFinished.Subscribe(callback);
}

void TJob::UnsubscribeFinished(const TClosure& callback)
{
    YUNREACHABLE();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NExecAgent
} // namespace NYT

