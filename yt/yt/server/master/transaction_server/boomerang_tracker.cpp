#include "boomerang_tracker.h"

#include "private.h"
#include "config.h"

#include <yt/yt/server/master/cell_master/automaton.h>
#include <yt/yt/server/master/cell_master/bootstrap.h>
#include <yt/yt/server/master/cell_master/config.h>
#include <yt/yt/server/master/cell_master/config_manager.h>
#include <yt/yt/server/master/cell_master/hydra_facade.h>
#include <yt/yt/server/master/cell_master/serialize.h>

#include <yt/yt/server/master/object_server/proto/object_manager.pb.h>

#include <yt/yt/server/lib/hydra/mutation_context.h>

#include <yt/yt/core/concurrency/fls.h>
#include <yt/yt/core/concurrency/periodic_executor.h>

#include <yt/yt/core/rpc/response_keeper.h>

namespace NYT::NTransactionServer {

using namespace NCellMaster;
using namespace NConcurrency;
using namespace NHydra;
using namespace NRpc;

///////////////////////////////////////////////////////////////////////////////

static const auto& Logger = TransactionServerLogger;

////////////////////////////////////////////////////////////////////////////////

static NConcurrency::TFls<bool> BoomerangMutation;

bool IsBoomerangMutation()
{
    return *BoomerangMutation;
}

class TBoomerangMutationGuard
    : private TNonCopyable
{
public:
    TBoomerangMutationGuard()
    {
        YT_VERIFY(!*BoomerangMutation);
        *BoomerangMutation = true;
    }

    ~TBoomerangMutationGuard()
    {
        *BoomerangMutation = false;
    }
};


////////////////////////////////////////////////////////////////////////////////

TBoomerangTracker::TBoomerangWaveDescriptor::TBoomerangWaveDescriptor(
    TInstant firstEncounterTime,
    int size)
    : FirstEncounterTime(firstEncounterTime)
    , Size(size)
{ }

void TBoomerangTracker::TBoomerangWaveDescriptor::Save(NCellMaster::TSaveContext& context) const
{
    using NYT::Save;

    Save(context, FirstEncounterTime);
    Save(context, Size);
    Save(context, ReturnedBoomerangCount);
}

void TBoomerangTracker::TBoomerangWaveDescriptor::Load(NCellMaster::TLoadContext& context)
{
    using NYT::Load;

    Load(context, FirstEncounterTime);
    Load(context, Size);
    Load(context, ReturnedBoomerangCount);
}

////////////////////////////////////////////////////////////////////////////////

TBoomerangTracker::TBoomerangTracker(TBootstrap* bootstrap)
    : Bootstrap_(bootstrap)
    , DynamicConfigChangedCallback_(BIND(&TBoomerangTracker::OnDynamicConfigChanged, MakeWeak(this)))
{ }

TBoomerangTracker::~TBoomerangTracker() = default;

void TBoomerangTracker::Save(NCellMaster::TSaveContext& context) const
{
    using NYT::Save;

    Save(context, InFlightBoomerangWaves_);
}

void TBoomerangTracker::Load(NCellMaster::TLoadContext& context)
{
    using NYT::Load;

    Load(context, InFlightBoomerangWaves_);

    for (const auto& [waveId, waveDescriptor] : InFlightBoomerangWaves_) {
        BoomerangWavesByTime_.emplace(waveDescriptor.FirstEncounterTime, waveId);
    }
}

void TBoomerangTracker::Start()
{
    YT_VERIFY(!CheckExecutor_);
    CheckExecutor_ = New<TPeriodicExecutor>(
        Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(NCellMaster::EAutomatonThreadQueue::Periodic),
        BIND(&TBoomerangTracker::OnCheck, MakeWeak(this)));
    CheckExecutor_->Start();

    const auto& configManager = Bootstrap_->GetConfigManager();
    configManager->SubscribeConfigChanged(DynamicConfigChangedCallback_);
    OnDynamicConfigChanged();
}

void TBoomerangTracker::Stop()
{
    const auto& configManager = Bootstrap_->GetConfigManager();
    configManager->UnsubscribeConfigChanged(DynamicConfigChangedCallback_);

    if (CheckExecutor_) {
        CheckExecutor_->Stop();
        CheckExecutor_.Reset();
    }
}

void TBoomerangTracker::Clear()
{
    InFlightBoomerangWaves_.clear();
    BoomerangWavesByTime_.clear();
}

void TBoomerangTracker::OnCheck()
{
    const auto& hydraManager = Bootstrap_->GetHydraFacade()->GetHydraManager();
    if (!hydraManager->IsActiveLeader()) {
        return;
    }

    YT_LOG_DEBUG("Starting removal commit for stuck boomerang waves");

    NProto::TReqRemoveStuckBoomerangWaves request;
    CreateMutation(hydraManager, request)
        ->CommitAndLog(Logger);
}

void TBoomerangTracker::RemoveStuckBoomerangWaves(NProto::TReqRemoveStuckBoomerangWaves* /*request*/)
{
    const auto* mutationContext = GetCurrentMutationContext();
    auto deadline = mutationContext->GetTimestamp() + GetDynamicConfig()->StuckBoomerangWaveExpirationTime;

    const auto maxRemovalCount = GetDynamicConfig()->MaxExpiredBoomerangWaveRemovalsPerCheck;
    auto it = BoomerangWavesByTime_.begin();
    for (auto i = 0; i < maxRemovalCount && it != BoomerangWavesByTime_.end(); ++i, ++it) {
        if (it->first > deadline) {
            break;
        }
        InFlightBoomerangWaves_.erase(it->second);
    }
    BoomerangWavesByTime_.erase(BoomerangWavesByTime_.begin(), it);
}

void TBoomerangTracker::ProcessReturnedBoomerang(NProto::TReqReturnBoomerang* request)
{
    YT_VERIFY(HasMutationContext());

    const auto waveId = FromProto<TBoomerangWaveId>(request->boomerang_wave_id());
    const auto waveSize = request->boomerang_wave_size();
    auto* waveDescriptor = GetOrCreateBoomerangWaveDescriptor(waveId, waveSize);

    const auto returnedBoomerangCount = ++waveDescriptor->ReturnedBoomerangCount;
    YT_VERIFY(returnedBoomerangCount <= waveDescriptor->Size);

    if (returnedBoomerangCount == waveDescriptor->Size) {
        ApplyBoomerangMutation(request);
        RemoveBoomerangWave(waveId);
    }

    auto mutationId = FromProto<TMutationId>(request->boomerang_mutation_id());

    YT_LOG_DEBUG("Boomerang returned (MutationId: %v, BoomerangWaveId: %v, ReturnedBoomerangCount: %v, BoomerangWaveSize: %v)",
        mutationId,
        waveId,
        returnedBoomerangCount,
        waveSize);
}

TBoomerangTracker::TBoomerangWaveDescriptor* TBoomerangTracker::GetOrCreateBoomerangWaveDescriptor(TBoomerangWaveId waveId, int waveSize)
{
    auto it = InFlightBoomerangWaves_.find(waveId);
    if (it == InFlightBoomerangWaves_.end()) {
        const auto* mutationContext = GetCurrentMutationContext();
        auto now = mutationContext->GetTimestamp();
        auto emplaceResult = InFlightBoomerangWaves_.emplace(waveId, TBoomerangWaveDescriptor(now, waveSize));
        YT_ASSERT(emplaceResult.second);
        BoomerangWavesByTime_.emplace(now, waveId);
        return &emplaceResult.first->second;
    } else {
        if (it->second.Size != waveSize) {
            YT_LOG_ALERT("Two boomerangs from the same wave declare different wave size (BoomerangWaveId: %v, ExpectedBoomerangWaveSize: %v, ActualBoomerangWaveSize: %v, ReturnedBoomerangCount: %v)",
                waveId,
                it->second.Size,
                waveSize,
                it->second.ReturnedBoomerangCount);
        }
        return &it->second;
    }
}

void TBoomerangTracker::RemoveBoomerangWave(TBoomerangWaveId waveId)
{
    auto it = InFlightBoomerangWaves_.find(waveId);
    YT_VERIFY(it != InFlightBoomerangWaves_.end());
    BoomerangWavesByTime_.erase({it->second.FirstEncounterTime, waveId});
    InFlightBoomerangWaves_.erase(it);
}

void TBoomerangTracker::ApplyBoomerangMutation(NProto::TReqReturnBoomerang* request)
{
    TMutationRequest mutationRequest;
    auto reign = GetCurrentMutationContext()->Request().Reign;
    mutationRequest.Reign = reign;
    mutationRequest.MutationId = FromProto<TMutationId>(request->boomerang_mutation_id());
    mutationRequest.Type = request->boomerang_mutation_type();
    mutationRequest.Data = TSharedRef::FromString(request->boomerang_mutation_data());

    // TODO(shakurov): use mutation idempotizer.

    TMutationContext mutationContext(GetCurrentMutationContext(), mutationRequest);
    const auto& hydraFacade = Bootstrap_->GetHydraFacade();

    {
        const auto& automaton = hydraFacade->GetAutomaton();
        TMutationContextGuard mutationContextGuard(&mutationContext);
        TBoomerangMutationGuard boomerangMutationGuard;
        automaton->ApplyMutation(&mutationContext);
    }

    const auto& responseKeeper = hydraFacade->GetResponseKeeper();

    if (!mutationContext.GetResponseKeeperSuppressed()) {
        responseKeeper->EndRequest(
            mutationRequest.MutationId,
            mutationContext.GetResponseData());
    }
}

const TBoomerangTrackerConfigPtr& TBoomerangTracker::GetDynamicConfig()
{
    const auto& configManager = Bootstrap_->GetConfigManager();
    return configManager->GetConfig()->TransactionManager->BoomerangTracker;
}

void TBoomerangTracker::OnDynamicConfigChanged(TDynamicClusterConfigPtr /*oldConfig*/)
{
    if (CheckExecutor_) {
        CheckExecutor_->SetPeriod(GetDynamicConfig()->StuckBoomerangWaveExpirationCheckPeriod);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTransactionServer
