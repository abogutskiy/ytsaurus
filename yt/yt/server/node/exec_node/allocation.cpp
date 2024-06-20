#include "allocation.h"

#include "controller_agent_connector.h"
#include "job_controller.h"

#include <yt/yt/core/actions/new_with_offloaded_dtor.h>

namespace NYT::NExecNode {

using namespace NClusterNode;
using namespace NJobAgent;
using namespace NScheduler;
using namespace NControllerAgent;
using namespace NControllerAgent::NProto;
using namespace NNodeTrackerClient::NProto;
using namespace NConcurrency;

using TJobStartInfo = TControllerAgentConnectorPool::TControllerAgentConnector::TJobStartInfo;

////////////////////////////////////////////////////////////////////////////////

namespace {

NClusterNode::TJobResources PatchJobResources(
    NClusterNode::TJobResources initial,
    const TAllocationAttributes& attributes,
    i64 minRequiredDiskSpace)
{
    const auto& diskRequest = attributes.DiskRequest;

    initial.DiskSpaceRequest = diskRequest.DiskSpace.value_or(minRequiredDiskSpace);
    initial.InodeRequest = diskRequest.InodeCount.value_or(0);

    return initial;
}

TAllocationAttributes BuildAttributesFromJobSpec(const TJobSpecExt* jobSpecExt)
{
    const auto& userJobSpec = jobSpecExt->user_job_spec();
    TAllocationAttributes attributes{
        .DiskRequest = {
            .InodeCount = userJobSpec.disk_request().inode_count(),
        },
        .AllowIdleCpuPolicy = jobSpecExt->allow_idle_cpu_policy(),
        .PortCount = userJobSpec.port_count(),
    };
    if (userJobSpec.disk_request().has_medium_index()) {
        attributes.DiskRequest.MediumIndex = userJobSpec.disk_request().medium_index();
    }

    if (userJobSpec.has_cuda_toolkit_version()) {
        attributes.CudaToolkitVersion = userJobSpec.cuda_toolkit_version();
    }
    if (jobSpecExt->has_waiting_job_timeout()) {
        attributes.WaitingForResourcesOnNodeTimeout = FromProto<TDuration>(jobSpecExt->waiting_job_timeout());
    }
    return attributes;
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

TAllocation::TAllocation(
    TAllocationId id,
    TOperationId operationId,
    const NClusterNode::TJobResources& resourceDemand,
    std::optional<NScheduler::TAllocationAttributes> attributes,
    TControllerAgentDescriptor agentDescriptor,
    IBootstrap* bootstrap)
    : TResourceOwner(
        id.Underlying(),
        bootstrap->GetJobResourceManager().Get(),
        EResourcesConsumerType::SchedulerAllocation,
        resourceDemand)
    , Bootstrap_(bootstrap)
    , Id_(id)
    , OperationId_(operationId)
    , Logger(ExecNodeLogger().WithTag(
        "AllocationId: %v, OperationId: %v",
        id,
        operationId))
    , RequestedGpu_(resourceDemand.Gpu)
    , RequestedCpu_(resourceDemand.Cpu)
    , RequestedMemory_(resourceDemand.UserMemory)
    , Attributes_(std::move(attributes))
    , ControllerAgentDescriptor_(std::move(agentDescriptor))
    , ControllerAgentConnector_(
        Bootstrap_->GetControllerAgentConnectorPool()->GetControllerAgentConnector(ControllerAgentDescriptor_))
{
    YT_VERIFY(bootstrap);

    VERIFY_INVOKER_THREAD_AFFINITY(Bootstrap_->GetJobInvoker(), JobThread);
}

TAllocation::~TAllocation()
{
    YT_LOG_DEBUG("Allocation destroyed");
}

TAllocationId TAllocation::GetId() const noexcept
{
    VERIFY_THREAD_AFFINITY_ANY();

    return Id_;
}

TOperationId TAllocation::GetOperationId() const noexcept
{
    VERIFY_THREAD_AFFINITY_ANY();

    return OperationId_;
}

EAllocationState TAllocation::GetState() const noexcept
{
    VERIFY_THREAD_AFFINITY(JobThread);

    return State_;
}

const TError& TAllocation::GetFinishError() const noexcept
{
    VERIFY_THREAD_AFFINITY(JobThread);

    return FinishError_;
}

int TAllocation::GetRequestedGpu() const noexcept
{
    VERIFY_THREAD_AFFINITY_ANY();

    return RequestedGpu_;
}

double TAllocation::GetRequestedCpu() const noexcept
{
    VERIFY_THREAD_AFFINITY_ANY();

    return RequestedCpu_;
}

i64 TAllocation::GetRequestedMemory() const noexcept
{
    VERIFY_THREAD_AFFINITY_ANY();

    return RequestedMemory_;
}

void TAllocation::Start()
{
    VERIFY_THREAD_AFFINITY(JobThread);

    if (Attributes_) {
        PrepareAllocationFromAttributes(*Attributes_);
    }

    SettleJob();
}

void TAllocation::Cleanup()
{
    VERIFY_THREAD_AFFINITY(JobThread);

    YT_VERIFY(State_ == EAllocationState::Finished);
}

TJobPtr TAllocation::EvictJob()
{
    VERIFY_THREAD_AFFINITY(JobThread);

    YT_VERIFY(Job_);

    YT_LOG_DEBUG("Job evicted from allocation (JobId: %v)", Job_->GetId());

    Job_->OnEvictedFromAllocation();

    auto job = std::move(Job_);

    return job;
}

const TJobPtr& TAllocation::GetJob() const
{
    VERIFY_THREAD_AFFINITY(JobThread);

    return Job_;
}

void TAllocation::UpdateControllerAgentDescriptor(TControllerAgentDescriptor descriptor)
{
    VERIFY_THREAD_AFFINITY(JobThread);

    TForbidContextSwitchGuard guard;

    if (ControllerAgentDescriptor_ == descriptor) {
        return;
    }

    YT_LOG_DEBUG(
        "Update controller agent for allocation (ControllerAgentAddress: %v -> %v, ControllerAgentIncarnationId: %v)",
        ControllerAgentDescriptor_.Address,
        descriptor.Address,
        descriptor.IncarnationId);

    ControllerAgentDescriptor_ = std::move(descriptor);

    ControllerAgentConnector_ = Bootstrap_
        ->GetControllerAgentConnectorPool()
        ->GetControllerAgentConnector(ControllerAgentDescriptor_);

    if (Job_) {
        Job_->UpdateControllerAgentDescriptor(ControllerAgentDescriptor_);
    }
}

const TControllerAgentDescriptor& TAllocation::GetControllerAgentDescriptor() const
{
    VERIFY_THREAD_AFFINITY(JobThread);

    return ControllerAgentDescriptor_;
}

NClusterNode::TJobResources TAllocation::GetResourceUsage(bool excludeReleasing) const noexcept
{
    VERIFY_THREAD_AFFINITY(JobThread);

    if (ResourceHolder_) {
        return ResourceHolder_->GetResourceUsage(excludeReleasing);
    }

    return {};
}

void TAllocation::Abort(TError error)
{
    VERIFY_THREAD_AFFINITY(JobThread);

    if (std::exchange(State_, EAllocationState::Finished) != EAllocationState::Finished) {
        YT_LOG_INFO(error, "Aborting allocation (CurrentState: %v)", State_);
    } else {
        YT_LOG_DEBUG(
            "Skip allocation abortion request since allocation is already finished (AbortionError: %v)",
            error);
        return;
    }

    FinishError_ = std::move(error);

    if (Job_) {
        TransferResourcesToJob();
        auto job = EvictJob();
        job->Abort(FinishError_);
    } else {
        YT_LOG_DEBUG("Empty allocation aborted");
    }

    OnAllocationFinished();
}

void TAllocation::Complete()
{
    VERIFY_THREAD_AFFINITY(JobThread);

    if (std::exchange(State_, EAllocationState::Finished) != EAllocationState::Finished) {
        YT_LOG_INFO("Completing allocation");
    } else {
        YT_LOG_DEBUG("Skip allocation completion request since allocation is already finished");
        return;
    }

    State_ = EAllocationState::Finished;

    if (Job_) {
        TransferResourcesToJob();
        EvictJob();
    }

    OnAllocationFinished();
}

void TAllocation::Preempt(
    TDuration timeout,
    TString preemptionReason,
    const std::optional<NScheduler::TPreemptedFor>& preemptedFor)
{
    VERIFY_THREAD_AFFINITY(JobThread);

    if (State_ == EAllocationState::Finished) {
        YT_LOG_DEBUG("Ignore allocation preemption request since allocation is already finished");
        return;
    } else {
        YT_LOG_INFO(
            "Preempting allocation (PreemptionReason: %v, PreemptedFor: %v, Timeout: %v)",
            preemptionReason,
            preemptedFor,
            timeout);
    }

    Preempted_ = true;

    if (!Job_) {
        YT_LOG_DEBUG("Allocation is empty; aborting it");

        auto error = TError("Allocation preempted")
            << TErrorAttribute("preemption_reason", preemptionReason)
            << TErrorAttribute("interruption_reason", EInterruptReason::Preemption)
            << TErrorAttribute("abort_reason", EAbortReason::Preemption);

        Abort(std::move(error));
        return;
    }

    Job_->PrepareResourcesRelease();
    Job_->Interrupt(
        timeout,
        EInterruptReason::Preemption,
        std::move(preemptionReason),
        preemptedFor);
}

bool TAllocation::IsResourceUsageOverdraftOccurred() const
{
    return ResourceHolder_->GetResourceUsage().UserMemory > GetRequestedMemory();
}

bool TAllocation::IsEmpty() const noexcept
{
    VERIFY_THREAD_AFFINITY(JobThread);

    return !Job_;
}

void TAllocation::SettleJob()
{
    VERIFY_THREAD_AFFINITY(JobThread);

    YT_VERIFY(!Job_);

    auto controllerAgentConnector = ControllerAgentConnector_.Lock();
    YT_LOG_FATAL_UNLESS(controllerAgentConnector, "Job owned by outdated controller agent");

    YT_LOG_INFO(
        "Requesting controller agent to settle new job (ControllerAgentDescriptor: %v)",
        ControllerAgentDescriptor_);

    controllerAgentConnector->SettleJob(OperationId_, Id_)
        .SubscribeUnique(BIND(&TAllocation::OnSettledJobReceived, MakeStrong(this))
            .Via(Bootstrap_->GetJobInvoker()));
}

void TAllocation::OnSettledJobReceived(
    TErrorOr<TJobStartInfo>&& jobInfoOrError)
{
    VERIFY_THREAD_AFFINITY(JobThread);

    TForbidContextSwitchGuard guard;

    if (!jobInfoOrError.IsOK()) {
        auto error = TError("Failed to get job spec")
            << jobInfoOrError;

        YT_LOG_INFO(error, "Failed to settle job in allocation; aborting allocation");

        error <<= TErrorAttribute("abort_reason", EAbortReason::GetSpecFailed);

        Abort(std::move(error));
        return;
    }

    auto& jobInfo = jobInfoOrError.Value();

    if (State_ == EAllocationState::Finished) {
        // Job will be aborted by controller agent in this case.

        YT_LOG_INFO("Received settled job for aborted allocation; ignore it (JobId: %v)", jobInfo.JobId);
        return;
    }

    // NB(arkady-e1ppa): Waiting is legacy. Remove when
    // sched and CA are updated to 24.2.
    YT_VERIFY(
        State_ == EAllocationState::Waiting ||
        State_ == EAllocationState::Running);

    if (!Attributes_.has_value()) {
        LegacyPrepareAllocationFromStartInfo(jobInfo);
    }

    try {
        CreateAndSettleJob(jobInfo.JobId, std::move(jobInfo.JobSpec));
    } catch (const std::exception& ex) {
        TError error(ex);
        YT_LOG_WARNING(
            error,
            "Failed to create job (JobId: %v, OperationId: %v)",
            jobInfo.JobId,
            OperationId_);

        Abort(TError("Failed to create job") << error);
    } catch (...) {
        YT_LOG_FATAL(
            "Unexpected failure during job creation (JobId: %v, OperationId: %v)",
            jobInfo.JobId,
            OperationId_);
    }
}

void TAllocation::CreateAndSettleJob(
    TJobId jobId,
    NControllerAgent::NProto::TJobSpec&& jobSpec)
{
    VERIFY_THREAD_AFFINITY(JobThread);

    auto job = CreateJob(
        jobId,
        OperationId_,
        MakeStrong(this),
        std::move(jobSpec),
        ControllerAgentDescriptor_,
        Bootstrap_,
        Bootstrap_->GetJobController()->GetDynamicConfig()->JobCommon);

    job->SubscribeJobPrepared(
        BIND_NO_PROPAGATE(&TAllocation::OnJobPrepared, MakeStrong(this))
            .Via(Bootstrap_->GetJobInvoker()));
    job->SubscribeJobFinished(
        BIND_NO_PROPAGATE(&TAllocation::OnJobFinished, MakeStrong(this))
            .Via(Bootstrap_->GetJobInvoker()));

    YT_VERIFY(!std::exchange(Job_, std::move(job)));

    // COMPAT(arkady-e1ppa): Non-legacy version
    // always has state == Running at this point.
    // Remove branch when sched and CA are 24.2.
    if (State_ == EAllocationState::Running) {
        Job_->Start();
    }

    JobSettled_.Fire(Job_);

    YT_LOG_INFO(
        "Job created (JobId: %v, OperationId: %v)",
        jobId,
        OperationId_);
}

void TAllocation::OnResourcesAcquired() noexcept
{
    VERIFY_THREAD_AFFINITY(JobThread);

    YT_LOG_INFO("Resources acquired; starting job");

    State_ = EAllocationState::Running;

    // NB(arkady-e1ppa): In non-legacy version of
    // allocation preparation resources are acquired
    // immediately. That is, before the spec
    // was received and job created. Thus,
    // we will simply start the job when it
    // is created.
    if (Job_) {
        Job_->Start();
    }
}

const NJobAgent::TResourceHolderPtr& TAllocation::GetResourceHolder() const noexcept
{
    VERIFY_THREAD_AFFINITY(JobThread);

    return ResourceHolder_;
}

void TAllocation::OnAllocationFinished()
{
    VERIFY_THREAD_AFFINITY_ANY();

    Job_.Reset();

    AllocationFinished_.Fire(MakeStrong(this));
}

void TAllocation::OnJobPrepared(TJobPtr job)
{
    VERIFY_THREAD_AFFINITY(JobThread);

    JobPrepared_.Fire(std::move(job));
}

void TAllocation::OnJobFinished(TJobPtr job)
{
    VERIFY_THREAD_AFFINITY(JobThread);

    JobFinished_.Fire(std::move(job));
}

void TAllocation::TransferResourcesToJob()
{
    VERIFY_THREAD_AFFINITY(JobThread);

    ResourceHolder_.Reset();
}

void TAllocation::PrepareAllocationFromAttributes(
    const NScheduler::TAllocationAttributes& attributes)
{
    VERIFY_THREAD_AFFINITY(JobThread);

    auto jobControllerConfig = Bootstrap_->GetJobController()->GetDynamicConfig();
    auto resources = PatchJobResources(
        GetResourceUsage(),
        attributes,
        jobControllerConfig->MinRequiredDiskSpace);

    ResourceHolder_->UpdateResourceDemand(
        resources,
        attributes);

    AllocationPrepared_.Fire(
        MakeStrong(this),
        attributes
            .WaitingForResourcesOnNodeTimeout
            .value_or(jobControllerConfig->WaitingForResourcesTimeout));
}

void TAllocation::LegacyPrepareAllocationFromStartInfo(TJobStartInfo& jobInfo)
{
    VERIFY_THREAD_AFFINITY(JobThread);

    auto jobSpecExtId = TJobSpecExt::job_spec_ext;
    YT_VERIFY(jobInfo.JobSpec.HasExtension(jobSpecExtId));
    auto* jobSpecExt = &jobInfo.JobSpec.GetExtension(jobSpecExtId);

    auto attributes = BuildAttributesFromJobSpec(jobSpecExt);
    PrepareAllocationFromAttributes(attributes);
}

TAllocationPtr CreateAllocation(
    TAllocationId id,
    TOperationId operationId,
    const NClusterNode::TJobResources& resourceUsage,
    std::optional<NScheduler::TAllocationAttributes> attributes,
    TControllerAgentDescriptor agentDescriptor,
    IBootstrap* bootstrap)
{
    return NewWithOffloadedDtor<TAllocation>(
        bootstrap->GetJobInvoker(),
        id,
        operationId,
        resourceUsage,
        std::move(attributes),
        std::move(agentDescriptor),
        bootstrap);
}

void FillStatus(NScheduler::NProto::TAllocationStatus* status, const TAllocationPtr& allocation)
{
    using NYT::ToProto;

    ToProto(status->mutable_allocation_id(), allocation->GetId());

    status->set_state(ToProto<int>(allocation->GetState()));

    ToProto(status->mutable_operation_id(), allocation->GetOperationId());

    status->set_status_timestamp(ToProto<ui64>(TInstant::Now()));

    ToProto(status->mutable_result()->mutable_error(), allocation->GetFinishError());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NExecNode
