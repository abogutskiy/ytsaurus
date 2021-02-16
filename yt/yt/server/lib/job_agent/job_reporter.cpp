#include "job_reporter.h"

#include "config.h"
#include "job_report.h"

#include <yt/server/lib/misc/archive_reporter.h>

#include <yt/ytlib/api/native/client.h>
#include <yt/ytlib/api/native/connection.h>

#include <yt/ytlib/controller_agent/helpers.h>

#include <yt/ytlib/scheduler/helpers.h>

#include <yt/client/api/connection.h>
#include <yt/client/api/transaction.h>

#include <yt/client/tablet_client/table_mount_cache.h>

#include <yt/client/table_client/row_buffer.h>
#include <yt/client/table_client/name_table.h>

#include <yt/core/compression/codec.h>

#include <yt/core/concurrency/action_queue.h>
#include <yt/core/concurrency/delayed_executor.h>
#include <yt/core/concurrency/nonblocking_batch.h>
#include <yt/core/concurrency/async_semaphore.h>

#include <yt/core/utilex/random.h>

namespace NYT::NJobAgent {

using namespace NNodeTrackerClient;
using namespace NTransactionClient;
using namespace NYson;
using namespace NYTree;
using namespace NConcurrency;
using namespace NControllerAgent;
using namespace NApi;
using namespace NTableClient;
using namespace NTabletClient;
using namespace NProfiling;
using namespace NScheduler;
using namespace NLogging;

////////////////////////////////////////////////////////////////////////////////

namespace {

static const TRegistry ReporterProfiler("/job_reporter");

////////////////////////////////////////////////////////////////////////////////

bool IsSpecEntry(const TJobReport& stat)
{
    return stat.Spec().operator bool();
}

////////////////////////////////////////////////////////////////////////////////

TArchiveReporterConfigPtr MakeCommonJobConfig(const TJobReporterConfigPtr& reporterConfig)
{
    auto config = New<TArchiveReporterConfig>();
    config->Enabled = reporterConfig->Enabled;
    config->ReportingPeriod = reporterConfig->ReportingPeriod;
    config->MinRepeatDelay = reporterConfig->MinRepeatDelay;
    config->MaxRepeatDelay = reporterConfig->MaxRepeatDelay;
    config->MaxItemsInBatch = reporterConfig->MaxItemsInBatch;
    return config;
}

TArchiveReporterConfigPtr MakeJobArchiveReporterConfig(const TJobReporterConfigPtr& reporterConfig)
{
    auto config = MakeCommonJobConfig(reporterConfig);
    config->MaxInProgressDataSize = reporterConfig->MaxInProgressJobDataSize;
    config->Path = GetOperationsArchiveJobsPath();
    return config;
}

TArchiveReporterConfigPtr MakeOperationIdArchiveReporterConfig(const TJobReporterConfigPtr& reporterConfig)
{
    auto config = MakeCommonJobConfig(reporterConfig);
    config->MaxInProgressDataSize = reporterConfig->MaxInProgressOperationIdDataSize;
    config->Path = GetOperationsArchiveOperationIdsPath();
    return config;
}

TArchiveReporterConfigPtr MakeJobSpecArchiveReporterConfig(const TJobReporterConfigPtr& reporterConfig)
{
    auto config = MakeCommonJobConfig(reporterConfig);
    config->MaxInProgressDataSize = reporterConfig->MaxInProgressJobSpecDataSize;
    config->Path = GetOperationsArchiveJobSpecsPath();
    return config;
}

TArchiveReporterConfigPtr MakeJobStderrArchiveReporterConfig(const TJobReporterConfigPtr& reporterConfig)
{
    auto config = MakeCommonJobConfig(reporterConfig);
    config->MaxInProgressDataSize = reporterConfig->MaxInProgressJobStderrDataSize;
    config->Path = GetOperationsArchiveJobStderrsPath();
    return config;
}

TArchiveReporterConfigPtr MakeJobFailContextArchiveReporterConfig(const TJobReporterConfigPtr& reporterConfig)
{
    auto config = MakeCommonJobConfig(reporterConfig);
    config->MaxInProgressDataSize = reporterConfig->MaxInProgressJobFailContextDataSize;
    config->Path = GetOperationsArchiveJobFailContextsPath();
    return config;
}

TArchiveReporterConfigPtr MakeJobProfileArchiveReporterConfig(const TJobReporterConfigPtr& reporterConfig)
{
    auto config = MakeCommonJobConfig(reporterConfig);
    // TODO(dakovalkov): Why we use JobStderr here?
    config->MaxInProgressDataSize = reporterConfig->MaxInProgressJobStderrDataSize;
    config->Path = GetOperationsArchiveJobProfilesPath();
    return config;
}

////////////////////////////////////////////////////////////////////////////////

class TJobRowlet
    : public IArchiveRowlet
{
public:
    TJobRowlet(
        TJobReport&& statistics,
        bool reportStatisticsLz4,
        const std::optional<TString>& localAddress)
        : Statistics_(statistics)
        , ReportStatisticsLz4_(reportStatisticsLz4)
        , DefaultLocalAddress_(localAddress)
    { }

    virtual size_t EstimateSize() const override
    {
        return Statistics_.EstimateSize();
    }

    virtual TUnversionedOwningRow ToRow(int archiveVersion) const override
    {
        const auto& index = TJobTableDescriptor::Get().Index;

        TYsonString coreInfosYsonString;
        TString jobCompetitionIdString;
        TString statisticsLz4;
        TYsonString briefStatisticsYsonString;

        TUnversionedOwningRowBuilder builder;
        builder.AddValue(MakeUnversionedUint64Value(Statistics_.OperationId().Parts64[0], index.OperationIdHi));
        builder.AddValue(MakeUnversionedUint64Value(Statistics_.OperationId().Parts64[1], index.OperationIdLo));
        builder.AddValue(MakeUnversionedUint64Value(Statistics_.JobId().Parts64[0], index.JobIdHi));
        builder.AddValue(MakeUnversionedUint64Value(Statistics_.JobId().Parts64[1], index.JobIdLo));
        if (Statistics_.Type()) {
            builder.AddValue(MakeUnversionedStringValue(*Statistics_.Type(), index.Type));
        }
        if (Statistics_.State()) {
            builder.AddValue(MakeUnversionedStringValue(
                *Statistics_.State(),
                index.TransientState));
        }
        if (Statistics_.StartTime()) {
            builder.AddValue(MakeUnversionedInt64Value(*Statistics_.StartTime(), index.StartTime));
        }
        if (Statistics_.FinishTime()) {
            builder.AddValue(MakeUnversionedInt64Value(*Statistics_.FinishTime(), index.FinishTime));
        }
        if (DefaultLocalAddress_) {
            builder.AddValue(MakeUnversionedStringValue(*DefaultLocalAddress_, index.Address));
        }
        if (Statistics_.Error()) {
            builder.AddValue(MakeUnversionedAnyValue(*Statistics_.Error(), index.Error));
        }
        if (Statistics_.Statistics()) {
            constexpr int Lz4AndBriefStatisticsVersion = 36;
            if (ReportStatisticsLz4_ && archiveVersion >= Lz4AndBriefStatisticsVersion) {
                auto codec = NCompression::GetCodec(NCompression::ECodec::Lz4);
                statisticsLz4 = ToString(codec->Compress(TSharedRef::FromString(*Statistics_.Statistics())));
                builder.AddValue(MakeUnversionedStringValue(statisticsLz4, index.StatisticsLz4));
            } else {
                builder.AddValue(MakeUnversionedAnyValue(*Statistics_.Statistics(), index.Statistics));
            }
            if (archiveVersion >= Lz4AndBriefStatisticsVersion) {
                briefStatisticsYsonString = BuildBriefStatistics(ConvertToNode(TYsonStringBuf(*Statistics_.Statistics())));
                builder.AddValue(MakeUnversionedAnyValue(briefStatisticsYsonString.AsStringBuf(), index.BriefStatistics));
            }
        }
        if (Statistics_.Events()) {
            builder.AddValue(MakeUnversionedAnyValue(*Statistics_.Events(), index.Events));
        }
        if (Statistics_.StderrSize()) {
            builder.AddValue(MakeUnversionedUint64Value(*Statistics_.StderrSize(), index.StderrSize));
        }
        if (archiveVersion >= 31 && Statistics_.CoreInfos()) {
            coreInfosYsonString = ConvertToYsonString(*Statistics_.CoreInfos());
            builder.AddValue(MakeUnversionedAnyValue(coreInfosYsonString.AsStringBuf(), index.CoreInfos));
        }
        if (archiveVersion >= 18) {
            builder.AddValue(MakeUnversionedInt64Value(TInstant::Now().MicroSeconds(), index.UpdateTime));
        }
        if (archiveVersion >= 20 && Statistics_.Spec()) {
            builder.AddValue(MakeUnversionedBooleanValue(Statistics_.Spec().operator bool(), index.HasSpec));
        }
        if (Statistics_.FailContext()) {
            if (archiveVersion >= 23) {
                builder.AddValue(MakeUnversionedUint64Value(Statistics_.FailContext()->size(), index.FailContextSize));
            } else if (archiveVersion >= 21) {
                builder.AddValue(MakeUnversionedBooleanValue(Statistics_.FailContext().operator bool(), index.HasFailContext));
            }
        }
        if (archiveVersion >= 32 && Statistics_.JobCompetitionId()) {
            jobCompetitionIdString = ToString(Statistics_.JobCompetitionId());
            builder.AddValue(MakeUnversionedStringValue(jobCompetitionIdString, index.JobCompetitionId));
        }
        if (archiveVersion >= 33 && Statistics_.HasCompetitors().has_value()) {
            builder.AddValue(MakeUnversionedBooleanValue(Statistics_.HasCompetitors().value(), index.HasCompetitors));
        }
        // COMPAT(gritukan)
        if (archiveVersion >= 34 && Statistics_.ExecAttributes()) {
            builder.AddValue(MakeUnversionedAnyValue(*Statistics_.ExecAttributes(), index.ExecAttributes));
        }
        // COMPAT(gritukan)
        if (archiveVersion >= 35 && Statistics_.TaskName()) {
            builder.AddValue(MakeUnversionedStringValue(*Statistics_.TaskName(), index.TaskName));
        }
        // COMPAT(levysotsky)
        if (archiveVersion >= 37 && Statistics_.TreeId()) {
            builder.AddValue(MakeUnversionedStringValue(*Statistics_.TreeId(), index.PoolTree));
        }
        // COMPAT(levysotsky)
        if (archiveVersion >= 39 && Statistics_.MonitoringDescriptor()) {
            builder.AddValue(MakeUnversionedStringValue(*Statistics_.MonitoringDescriptor(), index.MonitoringDescriptor));
        }

        return builder.FinishRow();
    }

private:
    TJobReport Statistics_;
    bool ReportStatisticsLz4_;
    const std::optional<TString>& DefaultLocalAddress_;
};

////////////////////////////////////////////////////////////////////////////////

class TOperationIdRowlet
    : public IArchiveRowlet
{
public:
    TOperationIdRowlet(TJobReport&& statistics)
        : Statistics_(statistics)
    { }

    virtual size_t EstimateSize() const override
    {
        return Statistics_.EstimateSize();
    }

    virtual TUnversionedOwningRow ToRow(int archiveVersion) const override
    {
        const auto& index = TOperationIdTableDescriptor::Get().Index;

        TUnversionedOwningRowBuilder builder;
        builder.AddValue(MakeUnversionedUint64Value(Statistics_.JobId().Parts64[0], index.JobIdHi));
        builder.AddValue(MakeUnversionedUint64Value(Statistics_.JobId().Parts64[1], index.JobIdLo));
        builder.AddValue(MakeUnversionedUint64Value(Statistics_.OperationId().Parts64[0], index.OperationIdHi));
        builder.AddValue(MakeUnversionedUint64Value(Statistics_.OperationId().Parts64[1], index.OperationIdLo));

        return builder.FinishRow();
    }

private:
    TJobReport Statistics_;
};

////////////////////////////////////////////////////////////////////////////////

class TJobSpecRowlet
    : public IArchiveRowlet
{
public:
    TJobSpecRowlet(TJobReport&& statistics)
        : Statistics_(statistics)
    { }

    virtual size_t EstimateSize() const override
    {
        return Statistics_.EstimateSize();
    }

    virtual TUnversionedOwningRow ToRow(int archiveVersion) const override
    {
        const auto& index = TJobSpecTableDescriptor::Get().Index;

        TUnversionedOwningRowBuilder builder;
        builder.AddValue(MakeUnversionedUint64Value(Statistics_.JobId().Parts64[0], index.JobIdHi));
        builder.AddValue(MakeUnversionedUint64Value(Statistics_.JobId().Parts64[1], index.JobIdLo));
        if (Statistics_.Spec()) {
            builder.AddValue(MakeUnversionedStringValue(*Statistics_.Spec(), index.Spec));
        }
        if (Statistics_.SpecVersion()) {
            builder.AddValue(MakeUnversionedInt64Value(*Statistics_.SpecVersion(), index.SpecVersion));
        }
        if (Statistics_.Type()) {
            builder.AddValue(MakeUnversionedStringValue(*Statistics_.Type(), index.Type));
        }

        return builder.FinishRow();
    }

private:
    TJobReport Statistics_;
};

////////////////////////////////////////////////////////////////////////////////

class TJobStderrRowlet
    : public IArchiveRowlet
{
public:
    TJobStderrRowlet(TJobReport&& statistics)
        : Statistics_(statistics)
    { }

    virtual size_t EstimateSize() const override
    {
        return Statistics_.EstimateSize();
    }

    virtual TUnversionedOwningRow ToRow(int archiveVersion) const override
    {
        const auto& index = TJobStderrTableDescriptor::Get().Index;

        if (!Statistics_.Stderr()) {
            return {};
        }

        TUnversionedOwningRowBuilder builder;
        builder.AddValue(MakeUnversionedUint64Value(Statistics_.OperationId().Parts64[0], index.OperationIdHi));
        builder.AddValue(MakeUnversionedUint64Value(Statistics_.OperationId().Parts64[1], index.OperationIdLo));
        builder.AddValue(MakeUnversionedUint64Value(Statistics_.JobId().Parts64[0], index.JobIdHi));
        builder.AddValue(MakeUnversionedUint64Value(Statistics_.JobId().Parts64[1], index.JobIdLo));
        builder.AddValue(MakeUnversionedStringValue(*Statistics_.Stderr(), index.Stderr));
        
        return builder.FinishRow();
    }

private:
    TJobReport Statistics_;
};

////////////////////////////////////////////////////////////////////////////////

class TJobFailContextRowlet
    : public IArchiveRowlet
{
public:
    TJobFailContextRowlet(TJobReport&& statistics)
        : Statistics_(statistics)
    { }

    virtual size_t EstimateSize() const override
    {
        return Statistics_.EstimateSize();
    }

    virtual TUnversionedOwningRow ToRow(int archiveVersion) const override
    {
        const auto& index = TJobFailContextTableDescriptor::Get().Index;

        if (archiveVersion < 21 || !Statistics_.FailContext() || Statistics_.FailContext()->size() > MaxStringValueLength) {
            return {};
        }

        TUnversionedOwningRowBuilder builder;
        builder.AddValue(MakeUnversionedUint64Value(Statistics_.OperationId().Parts64[0], index.OperationIdHi));
        builder.AddValue(MakeUnversionedUint64Value(Statistics_.OperationId().Parts64[1], index.OperationIdLo));
        builder.AddValue(MakeUnversionedUint64Value(Statistics_.JobId().Parts64[0], index.JobIdHi));
        builder.AddValue(MakeUnversionedUint64Value(Statistics_.JobId().Parts64[1], index.JobIdLo));
        builder.AddValue(MakeUnversionedStringValue(*Statistics_.FailContext(), index.FailContext));
        
        return builder.FinishRow();
    }

private:
    TJobReport Statistics_;
};

////////////////////////////////////////////////////////////////////////////////

class TJobProfileRowlet
    : public IArchiveRowlet
{
public:
    TJobProfileRowlet(TJobReport&& statistics)
        : Statistics_(statistics)
    { }

    virtual size_t EstimateSize() const override
    {
        return Statistics_.EstimateSize();
    }

    virtual TUnversionedOwningRow ToRow(int archiveVersion) const override
    {
        const auto& index = TJobProfileTableDescriptor::Get().Index;
        const auto& profile = Statistics_.Profile();

        if (archiveVersion < 27 || !profile) {
            return {};
        }

        TUnversionedOwningRowBuilder builder;
        builder.AddValue(MakeUnversionedUint64Value(Statistics_.OperationId().Parts64[0], index.OperationIdHi));
        builder.AddValue(MakeUnversionedUint64Value(Statistics_.OperationId().Parts64[1], index.OperationIdLo));
        builder.AddValue(MakeUnversionedUint64Value(Statistics_.JobId().Parts64[0], index.JobIdHi));
        builder.AddValue(MakeUnversionedUint64Value(Statistics_.JobId().Parts64[1], index.JobIdLo));
        builder.AddValue(MakeUnversionedInt64Value(0, index.PartIndex));
        builder.AddValue(MakeUnversionedStringValue(profile->Type, index.ProfileType));
        builder.AddValue(MakeUnversionedStringValue(profile->Blob, index.ProfileBlob));
        
        return builder.FinishRow();
    }

private:
    TJobReport Statistics_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace

////////////////////////////////////////////////////////////////////////////////

class TJobReporter::TImpl
    : public TRefCounted
{
public:
    TImpl(
        TJobReporterConfigPtr reporterConfig,
        const NApi::NNative::IConnectionPtr& masterConnection,
        std::optional<TString> localAddress)
        : Client_(
            masterConnection->CreateNativeClient(TClientOptions::FromUser(reporterConfig->User)))
        , Config_(std::move(reporterConfig))
        , LocalAddress_(std::move(localAddress))
        , JobHandler_(
            New<TArchiveReporter>(
                Version_,
                MakeJobArchiveReporterConfig(Config_),
                TJobTableDescriptor::Get().NameTable,
                "jobs",
                Client_,
                Reporter_->GetInvoker(),
                ReporterProfiler.WithTag("reporter_type", "jobs")))
        , OperationIdHandler_(
            New<TArchiveReporter>(
                Version_,
                MakeOperationIdArchiveReporterConfig(Config_),
                TOperationIdTableDescriptor::Get().NameTable,
                "operation_ids",
                Client_,
                Reporter_->GetInvoker(),
                ReporterProfiler.WithTag("reporter_type", "operation_ids")))
        , JobSpecHandler_(
            New<TArchiveReporter>(
                Version_,
                MakeJobSpecArchiveReporterConfig(Config_),
                TJobSpecTableDescriptor::Get().NameTable,
                "job_specs",
                Client_,
                Reporter_->GetInvoker(),
                ReporterProfiler.WithTag("reporter_type", "job_specs")))
        , JobStderrHandler_(
            New<TArchiveReporter>(
                Version_,
                MakeJobStderrArchiveReporterConfig(Config_),
                TJobStderrTableDescriptor::Get().NameTable,
                "stderrs",
                Client_,
                Reporter_->GetInvoker(),
                ReporterProfiler.WithTag("reporter_type", "stderrs")))
        , JobFailContextHandler_(
            New<TArchiveReporter>(
                Version_,
                MakeJobFailContextArchiveReporterConfig(Config_),
                TJobFailContextTableDescriptor::Get().NameTable,
                "fail_contexts",
                Client_,
                Reporter_->GetInvoker(),
                ReporterProfiler.WithTag("reporter_type", "fail_contexts")))
        , JobProfileHandler_(
            New<TArchiveReporter>(
                Version_,
                MakeJobProfileArchiveReporterConfig(Config_),
                TJobProfileTableDescriptor::Get().NameTable,
                "profiles",
                Client_,
                Reporter_->GetInvoker(),
                ReporterProfiler.WithTag("reporter_type", "profiles")))
    { }

    void ReportStatistics(TJobReport&& statistics)
    {
        if (IsSpecEntry(statistics)) {
            JobSpecHandler_->Enqueue(std::make_unique<TJobSpecRowlet>(statistics.ExtractSpec()));
        }
        if (statistics.Stderr()) {
            JobStderrHandler_->Enqueue(std::make_unique<TJobStderrRowlet>(statistics.ExtractStderr()));
        }
        if (statistics.FailContext()) {
            JobFailContextHandler_->Enqueue(std::make_unique<TJobFailContextRowlet>(statistics.ExtractFailContext()));
        }
        if (statistics.Profile()) {
            JobProfileHandler_->Enqueue(std::make_unique<TJobProfileRowlet>(statistics.ExtractProfile()));
        }
        if (!statistics.IsEmpty()) {
            OperationIdHandler_->Enqueue(std::make_unique<TOperationIdRowlet>(statistics.ExtractIds()));
            JobHandler_->Enqueue(std::make_unique<TJobRowlet>(
                std::move(statistics),
                Config_->ReportStatisticsLz4,
                LocalAddress_));
        }
    }

    void SetEnabled(bool enable)
    {
        JobHandler_->SetEnabled(enable);
        OperationIdHandler_->SetEnabled(enable);
    }

    void SetSpecEnabled(bool enable)
    {
        JobSpecHandler_->SetEnabled(enable);
    }

    void SetStderrEnabled(bool enable)
    {
        JobStderrHandler_->SetEnabled(enable);
    }

    void SetProfileEnabled(bool enable)
    {
        JobProfileHandler_->SetEnabled(enable);
    }

    void SetFailContextEnabled(bool enable)
    {
        JobFailContextHandler_->SetEnabled(enable);
    }

    void SetJobProfileEnabled(bool enable)
    {
        JobProfileHandler_->SetEnabled(enable);
    }

    void SetOperationArchiveVersion(int version)
    {
        Version_->Set(version);
    }

    int ExtractWriteFailuresCount()
    {
        return
            JobHandler_->ExtractWriteFailuresCount() +
            JobSpecHandler_->ExtractWriteFailuresCount() +
            JobStderrHandler_->ExtractWriteFailuresCount() +
            JobFailContextHandler_->ExtractWriteFailuresCount() +
            JobProfileHandler_->ExtractWriteFailuresCount();
    }

    bool GetQueueIsTooLarge()
    {
        return
            JobHandler_->QueueIsTooLarge() ||
            JobSpecHandler_->QueueIsTooLarge() ||
            JobStderrHandler_->QueueIsTooLarge() ||
            JobFailContextHandler_->QueueIsTooLarge() ||
            JobProfileHandler_->QueueIsTooLarge();
    }

private:
    const NNative::IClientPtr Client_;
    const TJobReporterConfigPtr Config_;
    const std::optional<TString> LocalAddress_;
    const TActionQueuePtr Reporter_ = New<TActionQueue>("JobReporter");
    const TArchiveVersionHolderPtr Version_ = New<TArchiveVersionHolder>();
    const TArchiveReporterPtr JobHandler_;
    const TArchiveReporterPtr OperationIdHandler_;
    const TArchiveReporterPtr JobSpecHandler_;
    const TArchiveReporterPtr JobStderrHandler_;
    const TArchiveReporterPtr JobFailContextHandler_;
    const TArchiveReporterPtr JobProfileHandler_;
};

////////////////////////////////////////////////////////////////////////////////

TJobReporter::TJobReporter(
    TJobReporterConfigPtr reporterConfig,
    const NApi::NNative::IConnectionPtr& masterConnection,
    std::optional<TString> localAddress)
    : Impl_(
        reporterConfig->Enabled
            ? New<TImpl>(std::move(reporterConfig), masterConnection, std::move(localAddress))
            : nullptr)
{ }

TJobReporter::~TJobReporter()
{ }

void TJobReporter::ReportStatistics(TJobReport&& statistics)
{
    if (Impl_) {
        Impl_->ReportStatistics(std::move(statistics));
    }
}

void TJobReporter::SetEnabled(bool enable)
{
    if (Impl_) {
        Impl_->SetEnabled(enable);
    }
}

void TJobReporter::SetSpecEnabled(bool enable)
{
    if (Impl_) {
        Impl_->SetSpecEnabled(enable);
    }
}

void TJobReporter::SetStderrEnabled(bool enable)
{
    if (Impl_) {
        Impl_->SetStderrEnabled(enable);
    }
}

void TJobReporter::SetProfileEnabled(bool enable)
{
    if (Impl_) {
        Impl_->SetProfileEnabled(enable);
    }
}

void TJobReporter::SetFailContextEnabled(bool enable)
{
    if (Impl_) {
        Impl_->SetFailContextEnabled(enable);
    }
}

void TJobReporter::SetOperationArchiveVersion(int version)
{
    if (Impl_) {
        Impl_->SetOperationArchiveVersion(version);
    }
}

int TJobReporter::ExtractWriteFailuresCount()
{
    if (Impl_) {
        return Impl_->ExtractWriteFailuresCount();
    }
    return 0;
}

bool TJobReporter::GetQueueIsTooLarge()
{
    if (Impl_) {
        return Impl_->GetQueueIsTooLarge();
    }
    return false;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJobAgent
