#include "private.h"
#include "operations_cleaner.h"
#include "config.h"
#include "operation.h"
#include "bootstrap.h"
#include "helpers.h"

#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/ytlib/api/native_client.h>
#include <yt/ytlib/api/transaction.h>
#include <yt/ytlib/api/operation_archive_schema.h>

#include <yt/ytlib/table_client/row_buffer.h>

#include <yt/core/concurrency/action_queue.h>
#include <yt/core/concurrency/nonblocking_batch.h>
#include <yt/core/concurrency/periodic_executor.h>
#include <yt/core/concurrency/async_semaphore.h>

#include <yt/core/profiling/profiler.h>

#include <yt/core/utilex/random.h>

namespace NYT {
namespace NScheduler {

using namespace NConcurrency;
using namespace NYTree;
using namespace NYson;
using namespace NObjectClient;
using namespace NApi;
using namespace NTableClient;
using namespace NTransactionClient;
using namespace NProfiling;

static const NLogging::TLogger Logger("OperationsCleaner");

////////////////////////////////////////////////////////////////////////////////

struct TOrderedByIdTag
{ };

struct TOrderedByStartTimeTag
{ };

////////////////////////////////////////////////////////////////////////////////

void TArchiveOperationRequest::InitializeFromOperation(const TOperationPtr& operation)
{
    Id = operation->GetId();
    StartTime = operation->GetStartTime();
    FinishTime = *operation->GetFinishTime();
    State = operation->GetState();
    AuthenticatedUser = operation->GetAuthenticatedUser();
    OperationType = operation->GetType();
    Spec = ConvertToYsonString(operation->GetSpec());
    Result = operation->BuildResultString();
    Events = ConvertToYsonString(operation->Events());
    Alerts = operation->BuildAlertsString();
    BriefSpec = operation->BriefSpec();

    const auto& attributes = operation->ControllerAttributes();
    const auto& initializationAttributes = attributes.InitializationAttributes;
    if (initializationAttributes) {
        UnrecognizedSpec = initializationAttributes->UnrecognizedSpec;
        FullSpec = initializationAttributes->FullSpec;
    }
}

const std::vector<TString>& TArchiveOperationRequest::GetAttributeKeys()
{
    // Keep the stuff below synchronized with InitializeFromAttributes method.
    static const std::vector<TString> attributeKeys = {
        "key",
        "start_time",
        "finish_time",
        "state",
        "authenticated_user",
        "operation_type",
        "progress",
        "brief_progress",
        "spec",
        "brief_spec",
        "result",
        "events",
        "alerts",
        "full_spec",
        "unrecognized_spec"
    };

    return attributeKeys;
}

void TArchiveOperationRequest::InitializeFromAttributes(const IAttributeDictionary& attributes)
{
    Id = TOperationId::FromString(attributes.Get<TString>("key"));
    StartTime = attributes.Get<TInstant>("start_time");
    FinishTime = attributes.Get<TInstant>("finish_time");
    State = attributes.Get<EOperationState>("state");
    AuthenticatedUser = attributes.Get<TString>("authenticated_user");
    OperationType = attributes.Get<EOperationType>("operation_type");
    Progress = attributes.FindYson("progress");
    BriefProgress = attributes.FindYson("brief_progress");
    Spec = attributes.GetYson("spec");
    BriefSpec = attributes.FindYson("brief_spec");
    Result = attributes.GetYson("result");
    Events = attributes.GetYson("events");
    Alerts = attributes.GetYson("alerts");
    FullSpec = attributes.FindYson("full_spec");
    UnrecognizedSpec = attributes.FindYson("unrecognized_spec");
}

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

TString GetFilterFactors(const TArchiveOperationRequest& request)
{
    auto briefSpecMapNode = ConvertToNode(request.BriefSpec)->AsMap();

    std::vector<TString> parts;
    parts.push_back(ToString(request.Id));
    parts.push_back(request.AuthenticatedUser);
    parts.push_back(FormatEnum(request.State));
    parts.push_back(FormatEnum(request.OperationType));

    for (const auto& key : {"pool", "title"}) {
        auto node = briefSpecMapNode->FindChild(key);
        if (node) {
            parts.push_back(node->AsString()->GetValue());
        }
    }

    for (const auto& key : {"input_table_paths", "output_table_paths"}) {
        auto node = briefSpecMapNode->FindChild(key);
        if (node) {
            auto listNode = node->AsList();
            for (const auto& child : listNode->GetChildren()) {
                parts.push_back(child->AsString()->GetValue());
            }
        }
    }

    auto result = JoinToString(parts.begin(), parts.end(), STRINGBUF(" "));
    return to_lower(result);
}

TUnversionedRow BuildOrderedByIdTableRow(
    const TRowBufferPtr& rowBuffer,
    const TArchiveOperationRequest& request,
    const TOrderedByIdTableDescriptor::TIndex& index,
    int version)
{
    // All any and string values passed to MakeUnversioned* functions MUST be alive till
    // they are captured in row buffer (they are not owned by unversioned value or builder).
    auto state = FormatEnum(request.State);
    auto operationType = FormatEnum(request.OperationType);
    auto filterFactors = GetFilterFactors(request);

    TUnversionedRowBuilder builder;
    builder.AddValue(MakeUnversionedUint64Value(request.Id.Parts64[0], index.IdHi));
    builder.AddValue(MakeUnversionedUint64Value(request.Id.Parts64[1], index.IdLo));
    builder.AddValue(MakeUnversionedStringValue(state, index.State));
    builder.AddValue(MakeUnversionedStringValue(request.AuthenticatedUser, index.AuthenticatedUser));
    builder.AddValue(MakeUnversionedStringValue(operationType, index.OperationType));
    if (request.Progress) {
        builder.AddValue(MakeUnversionedAnyValue(request.Progress.GetData(), index.Progress));
    }
    if (request.BriefProgress) {
        builder.AddValue(MakeUnversionedAnyValue(request.BriefProgress.GetData(), index.BriefProgress));
    }
    builder.AddValue(MakeUnversionedAnyValue(request.Spec.GetData(), index.Spec));
    if (request.BriefSpec) {
        builder.AddValue(MakeUnversionedAnyValue(request.BriefSpec.GetData(), index.BriefSpec));
    }
    builder.AddValue(MakeUnversionedInt64Value(request.StartTime.MicroSeconds(), index.StartTime));
    builder.AddValue(MakeUnversionedInt64Value(request.FinishTime.MicroSeconds(), index.FinishTime));
    builder.AddValue(MakeUnversionedStringValue(filterFactors, index.FilterFactors));
    builder.AddValue(MakeUnversionedAnyValue(request.Result.GetData(), index.Result));
    builder.AddValue(MakeUnversionedAnyValue(request.Events.GetData(), index.Events));
    builder.AddValue(MakeUnversionedAnyValue(request.Alerts.GetData(), index.Alerts));
    if (request.SlotIndex) {
        builder.AddValue(MakeUnversionedInt64Value(*request.SlotIndex, index.SlotIndex));
    }
    if (version >= 17) {
        if (request.UnrecognizedSpec) {
            builder.AddValue(MakeUnversionedAnyValue(request.UnrecognizedSpec.GetData(), index.UnrecognizedSpec));
        }
        if (request.FullSpec) {
            builder.AddValue(MakeUnversionedAnyValue(request.FullSpec.GetData(), index.FullSpec));
        }
    }

    return rowBuffer->Capture(builder.GetRow());
}

TUnversionedRow BuildOrderedByStartTimeTableRow(
    const TRowBufferPtr& rowBuffer,
    const TArchiveOperationRequest& request,
    const TOrderedByStartTimeTableDescriptor::TIndex& index,
    int version)
{
    // All any and string values passed to MakeUnversioned* functions MUST be alive till
    // they are captured in row buffer (they are not owned by unversioned value or builder).
    auto state = FormatEnum(request.State);
    auto operationType = FormatEnum(request.OperationType);
    auto filterFactors = GetFilterFactors(request);

    TUnversionedRowBuilder builder;
    builder.AddValue(MakeUnversionedInt64Value(request.StartTime.MicroSeconds(), index.StartTime));
    builder.AddValue(MakeUnversionedUint64Value(request.Id.Parts64[0], index.IdHi));
    builder.AddValue(MakeUnversionedUint64Value(request.Id.Parts64[1], index.IdLo));
    builder.AddValue(MakeUnversionedStringValue(operationType, index.OperationType));
    builder.AddValue(MakeUnversionedStringValue(state, index.State));
    builder.AddValue(MakeUnversionedStringValue(request.AuthenticatedUser, index.AuthenticatedUser));
    builder.AddValue(MakeUnversionedStringValue(filterFactors, index.FilterFactors));

    return rowBuffer->Capture(builder.GetRow());
}

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

class TOperationsCleaner::TImpl
    : public TRefCounted
{
public:
    TImpl(TOperationsCleanerConfigPtr config, TBootstrap* bootstrap)
        : Config_(std::move(config))
        , Bootstrap_(bootstrap)
        , ArchivationExecutor_(New<TPeriodicExecutor>(
            GetInvoker(),
            BIND(&TImpl::OnAnalyzeOperations, MakeWeak(this)),
            Config_->ArchivationPeriod))
        , RemoveBatcher_(Config_->RemoveBatchSize, Config_->RemoveBatchTimeout)
        , ArchiveBatcher_(Config_->ArchiveBatchSize, Config_->ArchiveBatchTimeout)
    {
        VERIFY_INVOKER_THREAD_AFFINITY(GetInvoker(), ControlThread);
    }

    void Start()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        DoStart(/* fetchFinishedOperations */ false);
    }

    void Stop()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        DoStop();
    }

    void UpdateConfig(const TOperationsCleanerConfigPtr& config)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        bool enable = Config_->Enable;
        bool enableArchivation = Config_->EnableArchivation;
        Config_ = config;

        if (enable != Config_->Enable) {
            if (Config_->Enable) {
                DoStart(/* fetchFinishedOperations */ true);
            } else {
                DoStop();
            }
        }

        if (enableArchivation != Config_->EnableArchivation) {
            if (Config_->EnableArchivation) {
                DoStartArchivation();
            } else {
                DoStopArchivation();
            }
        }

        LOG_INFO("Operations cleaner config updated (Enable: %v, EnableArchivation: %v)",
            Config_->Enable,
            Config_->EnableArchivation);
    }

    void SubmitForArchivation(TArchiveOperationRequest request)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!IsEnabled()) {
            return;
        }

        auto id = request.Id;

        // Can happen if scheduler reported operation and archiver was turned on and
        // fetched the same operation from Cypress.
        if (OperationMap_.find(id) != OperationMap_.end()) {
            return;
        }

        auto deadline = request.FinishTime + Config_->CleanDelay;

        ArchiveTimeToOperationIdMap_.emplace(deadline, id);
        YCHECK(OperationMap_.emplace(id, std::move(request)).second);

        LOG_DEBUG("Operation submitted for archivation (OperationId: %v, ArchivationStartTime: %v)",
            id,
            deadline);
    }

    void SubmitForRemoval(TRemoveOperationRequest request)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!IsEnabled()) {
            return;
        }

        EnqueueForRemoval(request.Id);
        LOG_DEBUG("Operation submitted for removal (OperationId: %v)", request.Id);
    }

    void SetArchiveVersion(int version)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        ArchiveVersion_ = version;
    }

    bool IsEnabled() const
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return Enabled_;
    }

    void BuildOrchid(TFluentMap fluent) const
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        fluent
            .Item("enable").Value(IsEnabled())
            .Item("enable_archivation").Value(IsArchivationEnabled())
            .Item("remove_pending").Value(RemovePendingCounter_.GetCurrent())
            .Item("archive_pending").Value(ArchivePendingCounter_.GetCurrent())
            .Item("submitted_count").Value(ArchiveTimeToOperationIdMap_.size());
    }

private:
    TOperationsCleanerConfigPtr Config_;
    const TBootstrap* const Bootstrap_;

    const NLogging::TLogger Logger;

    const TPeriodicExecutorPtr ArchivationExecutor_;

    int ArchiveVersion_ = -1;

    bool Enabled_ = false;
    bool ArchivationEnabled_ = false;

    TDelayedExecutorCookie ArchivationStartCookie_;

    std::multimap<TInstant, TOperationId> ArchiveTimeToOperationIdMap_;
    THashMap<TOperationId, TArchiveOperationRequest> OperationMap_;

    TNonblockingBatch<TOperationId> RemoveBatcher_;
    TNonblockingBatch<TOperationId> ArchiveBatcher_;

    TProfiler Profiler = {"/operations_cleaner"};

    TSimpleCounter RemovePendingCounter_ = {"/remove_pending"};
    TSimpleCounter ArchivePendingCounter_ = {"/archive_pending"};
    TSimpleCounter ArchivedCounter_ = {"/archived"};
    TSimpleCounter RemovedCounter_ = {"/removed"};
    TSimpleCounter CommittedDataWeightCounter_ = {"/committed_data_weight"};
    TSimpleCounter ArchiveErrorCounter_ = {"/archive_errors"};
    TSimpleCounter RemoveErrorCounter_ = {"/remove_errors"};

    TAggregateCounter AnalyzeOperationsTimeCounter_ = {"/analyze_operations_time"};
    TAggregateCounter OperationsRowsPreparationTimeCounter_ = {"/operations_rows_preparation_time"};

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);

private:
    IInvokerPtr GetInvoker() const
    {
        return Bootstrap_->GetControlInvoker(EControlQueue::OperationsCleaner);
    }

    void ScheduleRemoveOperations()
    {
        GetInvoker()->Invoke(BIND(&TImpl::RemoveOperations, MakeStrong(this)));
    }

    void ScheduleArchiveOperations()
    {
        GetInvoker()->Invoke(BIND(&TImpl::ArchiveOperations, MakeStrong(this)));
    }

    void DoStart(bool fetchFinishedOperations)
    {
        if (Config_->Enable && !Enabled_) {
            Enabled_ = true;
            ScheduleRemoveOperations();
            ScheduleArchiveOperations();
            ArchivationExecutor_->Start();
            DoStartArchivation();

            // If operations cleaner was disabled during scheduler runtime and then
            // enabled then we should fetch all finished operation since scheduler did not
            // reported them.
            if (fetchFinishedOperations) {
                GetInvoker()->Invoke(BIND(&TImpl::FetchFinishedOperations, MakeStrong(this)));
            }

            LOG_INFO("Operations cleaner started");
        }
    }

    void DoStartArchivation()
    {
        if (Config_->Enable && Config_->EnableArchivation && !ArchivationEnabled_) {
            ArchivationEnabled_ = true;
            TDelayedExecutor::CancelAndClear(ArchivationStartCookie_);

            LOG_INFO("Operations archivation started");
        }
    }

    void DoStopArchivation()
    {
        if (!ArchivationEnabled_) {
            return;
        }

        ArchivationEnabled_ = false;
        TDelayedExecutor::CancelAndClear(ArchivationStartCookie_);

        LOG_INFO("Operations archivation stopped");
    }

    void DoStop()
    {
        if (!Enabled_) {
            return;
        }

        Enabled_ = false;

        TDelayedExecutor::CancelAndClear(ArchivationStartCookie_);

        ArchivationExecutor_->Stop();

        DoStopArchivation();

        ArchiveBatcher_.Drop();
        RemoveBatcher_.Drop();
        ArchiveTimeToOperationIdMap_.clear();
        OperationMap_.clear();
        Profiler.Update(ArchivePendingCounter_, 0);
        Profiler.Update(RemovePendingCounter_, 0);

        LOG_INFO("Operations cleaner stopped");
    }

    void OnAnalyzeOperations()
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());

        LOG_INFO("Analyzing operations submitted for archivation (SubmittedOperationCount: %v)",
            ArchiveTimeToOperationIdMap_.size());

        if (ArchiveTimeToOperationIdMap_.empty()) {
            LOG_INFO("No operations submitted for archivation");
            return;
        }

        auto now = TInstant::Now();

        int retainedCount = 0;
        int enqueuedForArchivationCount = 0;
        THashMap<TString, int> operationCountPerUser;

        auto canArchive = [&] (const auto& request) {
            if (retainedCount > Config_->HardRetainedOperationCount) {
                return true;
            }

            if (now - request.FinishTime > Config_->MaxOperationAge) {
                return true;
            }

            if (!IsOperationWithUserJobs(request.OperationType) &&
                request.State == EOperationState::Completed)
            {
                return true;
            }

            if (operationCountPerUser[request.AuthenticatedUser] > Config_->MaxOperationCountPerUser) {
                return true;
            }

            // TODO(asaitgalin): Consider only operations without stderrs?
            if (retainedCount > Config_->SoftRetainedOperationCount &&
                request.State != EOperationState::Failed)
            {
                return true;
            }

            return false;
        };

        // Analyze operations with expired grace timeout, from newest to oldest.
        PROFILE_AGGREGATED_TIMING(AnalyzeOperationsTimeCounter_) {
            auto it = ArchiveTimeToOperationIdMap_.lower_bound(now);
            while (it != ArchiveTimeToOperationIdMap_.begin()) {
                --it;

                auto operationId = it->second;
                const auto& request = GetRequest(operationId);
                if (canArchive(request)) {
                    it = ArchiveTimeToOperationIdMap_.erase(it);
                    CleanOperation(operationId);
                    enqueuedForArchivationCount += 1;
                } else {
                    retainedCount += 1;
                    operationCountPerUser[request.AuthenticatedUser] += 1;
                }
            }
        }

        LOG_INFO("Finished analyzing operations submitted for archivation "
            "(RetainedCount: %v, EnqueuedForArchivationCount: %v)",
            retainedCount,
            enqueuedForArchivationCount);
    }

    void EnqueueForRemoval(const TOperationId& operationId)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        LOG_DEBUG("Operation enqueued for removal (OperationId: %v)", operationId);
        Profiler.Increment(RemovePendingCounter_, 1);
        RemoveBatcher_.Enqueue(operationId);
    }

    void EnqueueForArchivation(const TOperationId& operationId)
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());

        LOG_DEBUG("Operation enqueued for archivation (OperationId: %v)", operationId);
        Profiler.Increment(ArchivePendingCounter_, 1);
        ArchiveBatcher_.Enqueue(operationId);
    }

    void CleanOperation(const TOperationId& operationId)
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());

        if (IsArchivationEnabled()) {
            EnqueueForArchivation(operationId);
        } else {
            EnqueueForRemoval(operationId);
        }
    }

    void TryArchiveOperations(const std::vector<TOperationId>& operationIds)
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());

        int version = ArchiveVersion_;
        if (version == -1) {
            THROW_ERROR_EXCEPTION("Unknown operations archive version");
        }

        auto asyncTransaction = Bootstrap_->GetMasterClient()->StartTransaction(
            ETransactionType::Tablet, TTransactionStartOptions{});
        auto transaction = WaitFor(asyncTransaction)
            .ValueOrThrow();

        LOG_DEBUG("Operations archivation transaction started (TransactionId: %v, OperationCount: %v)",
            transaction->GetId(),
            operationIds.size());

        i64 orderedByIdRowsDataWeight = 0;
        i64 orderedByStartTimeRowsDataWeight = 0;

        PROFILE_AGGREGATED_TIMING(OperationsRowsPreparationTimeCounter_) {
            // ordered_by_id table rows
            {
                TOrderedByIdTableDescriptor desc;
                auto rowBuffer = New<TRowBuffer>(TOrderedByIdTag{});
                std::vector<TUnversionedRow> rows;
                rows.reserve(operationIds.size());

                for (const auto& operationId : operationIds) {
                    const auto& request = GetRequest(operationId);
                    auto row = NDetail::BuildOrderedByIdTableRow(rowBuffer, request, desc.Ids, version);
                    rows.push_back(row);
                    orderedByIdRowsDataWeight += GetDataWeight(row);
                }

                transaction->WriteRows(
                    GetOperationsArchivePathOrderedById(),
                    desc.NameTable,
                    MakeSharedRange(std::move(rows), std::move(rowBuffer)));
            }

            // ordered_by_start_time rows
            {
                TOrderedByStartTimeTableDescriptor desc;
                auto rowBuffer = New<TRowBuffer>(TOrderedByStartTimeTag{});
                std::vector<TUnversionedRow> rows;
                rows.reserve(operationIds.size());

                for (const auto& operationId : operationIds) {
                    const auto& request = GetRequest(operationId);
                    auto row = NDetail::BuildOrderedByStartTimeTableRow(rowBuffer, request, desc.Ids, version);
                    rows.push_back(row);
                    orderedByStartTimeRowsDataWeight += GetDataWeight(row);
                }

                transaction->WriteRows(
                    GetOperationsArchivePathOrderedByStartTime(),
                    desc.NameTable,
                    MakeSharedRange(std::move(rows), std::move(rowBuffer)));
            }
        }

        i64 totalDataWeight = orderedByIdRowsDataWeight + orderedByStartTimeRowsDataWeight;

        LOG_DEBUG("Started committing archivation transaction (TransactionId: %v, OperationCount: %v, OrderedByIdRowsDataWeight: %v, "
            "OrderedByStartTimeRowsDataWeight: %v, TotalDataWeight: %v)",
            transaction->GetId(),
            operationIds.size(),
            orderedByIdRowsDataWeight,
            orderedByStartTimeRowsDataWeight,
            totalDataWeight);

        WaitFor(transaction->Commit())
            .ThrowOnError();

        LOG_DEBUG("Finished committing archivation transaction (TransactionId: %v)", transaction->GetId());

        Profiler.Increment(CommittedDataWeightCounter_, totalDataWeight);
        Profiler.Increment(ArchivedCounter_, operationIds.size());
    }

    bool IsArchivationEnabled() const
    {
        return IsEnabled() && ArchivationEnabled_;
    }

    void ArchiveOperations()
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());

        auto batch = WaitFor(ArchiveBatcher_.DequeueBatch())
            .ValueOrThrow();

        if (!batch.empty()) {
            while (IsArchivationEnabled()) {
                try {
                    TryArchiveOperations(batch);
                    break;
                } catch (const std::exception& ex) {
                    LOG_WARNING(ex, "Failed to archive operations (PendingCount: %v)",
                        ArchivePendingCounter_.GetCurrent());
                    Profiler.Increment(ArchiveErrorCounter_, 1);
                }

                if (ArchivePendingCounter_.GetCurrent() > Config_->MaxOperationCountEnqueuedForArchival) {
                    TemporaryDisableArchivation();
                    break;
                } else {
                    auto sleepDelay = Config_->MinArchivationRetrySleepDelay +
                        RandomDuration(Config_->MaxArchivationRetrySleepDelay - Config_->MinArchivationRetrySleepDelay);
                    TDelayedExecutor::WaitForDuration(sleepDelay);
                }
            }

            for (const auto& operationId : batch) {
                YCHECK(OperationMap_.erase(operationId) == 1);
                EnqueueForRemoval(operationId);
            }

            Profiler.Increment(ArchivePendingCounter_, -batch.size());
        }

        if (IsEnabled()) {
            ScheduleArchiveOperations();
        }
    }

    void RemoveOperations()
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());

        auto batch = WaitFor(RemoveBatcher_.DequeueBatch())
            .ValueOrThrow();

        if (!batch.empty()) {
            LOG_DEBUG("Removing operations from Cypress (BatchSize: %v)", batch.size());

            auto channel = Bootstrap_->GetMasterClient()->GetMasterChannelOrThrow(
                EMasterChannelKind::Leader, PrimaryMasterCellTag);

            TObjectServiceProxy proxy(channel);
            auto batchReq = proxy.ExecuteBatch();

            for (const auto& operationId : batch) {
                // Remove old operation node.
                {
                    auto req = TYPathProxy::Remove(GetOperationPath(operationId));
                    req->set_recursive(true);
                    batchReq->AddRequest(req, "remove_operation");
                }
                // Remove new operation node.
                {
                    auto req = TYPathProxy::Remove(GetNewOperationPath(operationId));
                    req->set_recursive(true);
                    batchReq->AddRequest(req, "remove_operation_new");
                }
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            auto error = GetCumulativeError(batchRspOrError);
            if (error.IsOK()) {
                LOG_DEBUG("Successfully removed operations from Cypress (BatchSize: %v)", batch.size());
                Profiler.Increment(RemovedCounter_, batch.size());
                Profiler.Increment(RemovePendingCounter_, -batch.size());
            } else {
                LOG_WARNING(error, "Failed to remove finished operations from Cypress");
                Profiler.Increment(RemoveErrorCounter_, 1);
            }
        }

        if (IsEnabled()) {
            ScheduleRemoveOperations();
        }
    }

    void TemporaryDisableArchivation()
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());

        DoStopArchivation();

        auto enableCallback = BIND(&TImpl::DoStartArchivation, MakeStrong(this))
            .Via(GetInvoker());

        ArchivationStartCookie_ = TDelayedExecutor::Submit(
            enableCallback,
            Config_->ArchivationEnableDelay);

        auto enableTime = TInstant::Now() + Config_->ArchivationEnableDelay;
        LOG_INFO("Archivation is temporary disabled (EnableTime: %v)", enableTime);
    }

    void FetchFinishedOperations()
    {
        try {
            DoFetchFinishedOperations();
        } catch (const std::exception& ex) {
            // NOTE(asaitgalin): Maybe disconnect? What can we do here?
            LOG_WARNING(ex, "Failed to fetch finished operation from Cypress");
        }
    }

    void DoFetchFinishedOperations()
    {
        LOG_INFO("Fetching all finished operations from Cypress");

        auto createBatchRequest = BIND([this] {
            auto channel = Bootstrap_->GetMasterClient()->GetMasterChannelOrThrow(
                EMasterChannelKind::Follower, PrimaryMasterCellTag);

            TObjectServiceProxy proxy(channel);
            return proxy.ExecuteBatch();
        });

        auto listOperationsResult = ListOperations(createBatchRequest);

        // Remove some operations.
        for (const auto& operation : listOperationsResult.OperationsToRemove) {
            SubmitForRemoval({operation});
        }

        auto operations = FetchOperationsFromCypressForCleaner(
            listOperationsResult.OperationsToArchive,
            createBatchRequest);

        for (auto& operation : operations) {
            SubmitForArchivation(std::move(operation));
        }

        LOG_INFO("Fetched and processed all finished operations");
    }

    const TArchiveOperationRequest& GetRequest(const TOperationId& operationId) const
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());

        auto it = OperationMap_.find(operationId);
        YCHECK(it != OperationMap_.end());
        return it->second;
    }
};

////////////////////////////////////////////////////////////////////////////////

TOperationsCleaner::TOperationsCleaner(TOperationsCleanerConfigPtr config, TBootstrap* bootstrap)
    : Impl_(New<TImpl>(std::move(config), bootstrap))
{ }

void TOperationsCleaner::Start()
{
    Impl_->Start();
}

void TOperationsCleaner::Stop()
{
    Impl_->Stop();
}

void TOperationsCleaner::SubmitForArchivation(TArchiveOperationRequest request)
{
    Impl_->SubmitForArchivation(std::move(request));
}

void TOperationsCleaner::SubmitForRemoval(TRemoveOperationRequest request)
{
    Impl_->SubmitForRemoval(std::move(request));
}

void TOperationsCleaner::UpdateConfig(const TOperationsCleanerConfigPtr& config)
{
    Impl_->UpdateConfig(config);
}

void TOperationsCleaner::SetArchiveVersion(int version)
{
    Impl_->SetArchiveVersion(version);
}

bool TOperationsCleaner::IsEnabled() const
{
    return Impl_->IsEnabled();
}

void TOperationsCleaner::BuildOrchid(TFluentMap fluent) const
{
    Impl_->BuildOrchid(fluent);
}

////////////////////////////////////////////////////////////////////////////////

std::vector<TArchiveOperationRequest> FetchOperationsFromCypressForCleaner(
    const std::vector<TOperationId>& operationIds,
    TCallback<TObjectServiceProxy::TReqExecuteBatchPtr()> createBatchRequest)
{
    using NYT::ToProto;

    std::vector<TArchiveOperationRequest> result;

    auto batchReq = createBatchRequest();

    for (const auto& operationId : operationIds) {
        auto req = TYPathProxy::Get(GetNewOperationPath(operationId) + "/@");
        ToProto(req->mutable_attributes()->mutable_keys(), TArchiveOperationRequest::GetAttributeKeys());
        batchReq->AddRequest(req, "get_op_attributes");
    }

    auto rspOrError = WaitFor(batchReq->Invoke());
    auto error = GetCumulativeError(rspOrError);
    THROW_ERROR_EXCEPTION_IF_FAILED(error, "Error requesting operations attributes for archivation");

    auto rsps = rspOrError.Value()->GetResponses<TYPathProxy::TRspGet>("get_op_attributes");
    YCHECK(operationIds.size() == rsps.size());

    for (int index = 0; index < operationIds.size(); ++index) {
        const auto& id = operationIds[index];
        const auto& rspOrError = rsps[index];

        auto attributes = ConvertToAttributes(TYsonString(rspOrError.Value()->value()));
        YCHECK(TOperationId::FromString(attributes->Get<TString>("key")) == id);

        TArchiveOperationRequest req;
        req.InitializeFromAttributes(*attributes);
        result.push_back(req);
    }

    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
