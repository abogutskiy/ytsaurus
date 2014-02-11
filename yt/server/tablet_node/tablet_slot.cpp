#include "tablet_slot.h"
#include "config.h"
#include "tablet_cell_controller.h"
#include "serialize.h"
#include "automaton.h"
#include "tablet_manager.h"
#include "transaction_manager.h"
#include "tablet_service.h"
#include "query_executor.h"
#include "private.h"

#include <core/concurrency/thread_affinity.h>
#include <core/concurrency/fiber.h>
#include <core/concurrency/action_queue.h>

#include <core/ytree/fluent.h>

#include <core/rpc/server.h>

#include <core/logging/tagged_logger.h>

#include <ytlib/election/config.h>
#include <ytlib/election/cell_manager.h>

#include <ytlib/hive/cell_directory.h>
#include <ytlib/hive/timestamp_provider.h>

#include <server/election/election_manager.h>

#include <server/hydra/changelog.h>
#include <server/hydra/changelog_catalog.h>
#include <server/hydra/snapshot.h>
#include <server/hydra/hydra_manager.h>
#include <server/hydra/distributed_hydra_manager.h>

#include <server/hive/hive_manager.h>
#include <server/hive/mailbox.h>
#include <server/hive/transaction_supervisor.h>

#include <server/cell_node/bootstrap.h>
#include <server/cell_node/config.h>

#include <server/data_node/config.h>

#include <server/query_agent/query_service.h>

namespace NYT {
namespace NTabletNode {

using namespace NConcurrency;
using namespace NYTree;
using namespace NYson;
using namespace NElection;
using namespace NHydra;
using namespace NHive;
using namespace NNodeTrackerClient::NProto;
using namespace NObjectClient;
using namespace NQueryAgent;

using NHydra::EPeerState;

////////////////////////////////////////////////////////////////////////////////

static auto& Logger = TabletNodeLogger;

////////////////////////////////////////////////////////////////////////////////

class TTabletSlot::TImpl
    : public TRefCounted
{
public:
    TImpl(
        TTabletSlot* owner,
        int slotIndex,
        NCellNode::TCellNodeConfigPtr config,
        NCellNode::TBootstrap* bootstrap)
        : Owner_(owner)
        , SlotIndex_(slotIndex)
        , Config_(config)
        , Bootstrap_(bootstrap)
        , AutomatonQueue_(New<TActionQueue>(Sprintf("TabletSlot:%d", SlotIndex_)))
        , Logger(TabletNodeLogger)
    {
        VERIFY_INVOKER_AFFINITY(AutomatonQueue_->GetInvoker(), AutomatonThread);

        Reset();
    }


    const TCellGuid& GetCellGuid() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return CellGuid_;
    }

    EPeerState GetState() const
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (HydraManager_) {
            State_ = HydraManager_->GetControlState();
        }

        return State_;
    }

    TPeerId GetPeerId() const
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return PeerId_;
    }

    const NHydra::NProto::TCellConfig& GetCellConfig() const
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return CellConfig_;
    }

    IHydraManagerPtr GetHydraManager() const
    {
        return HydraManager_;
    }

    TTabletAutomatonPtr GetAutomaton() const
    {
        return Automaton_;
    }

    IInvokerPtr GetAutomatonInvoker() const
    {
        return AutomatonQueue_->GetInvoker();
    }

    IInvokerPtr GetEpochAutomatonInvoker() const
    {
        return EpochAutomatonInvoker_;
    }

    THiveManagerPtr GetHiveManager() const
    {
        return HiveManager_;
    }

    TMailbox* GetMasterMailbox()
    {
        // Create master mailbox lazily.
        if (!MasterMailbox_) {
            auto masterCellGuid = Bootstrap_->GetCellGuid();
            MasterMailbox_ = HiveManager_->GetOrCreateMailbox(masterCellGuid);
        }
        return MasterMailbox_;
    }

    TTransactionManagerPtr GetTransactionManager() const
    {
        return TransactionManager_;
    }

    TTransactionSupervisorPtr GetTransactionSupervisor() const
    {
        return TransactionSupervisor_;
    }

    TTabletManagerPtr GetTabletManager() const
    {
        return TabletManager_;
    }

    TObjectId GenerateId(EObjectType type)
    {
        auto* mutationContext = HydraManager_->GetMutationContext();

        const auto& version = mutationContext->GetVersion();

        auto random = mutationContext->RandomGenerator().Generate<ui64>();

        int typeValue = static_cast<int>(type);
        YASSERT(typeValue >= 0 && typeValue <= MaxObjectType);

        return TObjectId(
            random ^ CellGuid_.Parts[0],
            (CellGuid_.Parts[1] & 0xff00) + typeValue,
            version.RecordId,
            version.SegmentId);
    }


    void Load(const TCellGuid& cellGuid)
    {
        // NB: Load is called from bootstrap thread.
        YCHECK(State_ == EPeerState::None);

        SetCellGuid(cellGuid);

        LOG_INFO("Loading slot");

        State_ = EPeerState::Initializing;

        auto tabletCellController = Bootstrap_->GetTabletCellController();
        ChangelogStore_ = tabletCellController->GetChangelogCatalog()->GetStore(CellGuid_);
        SnapshotStore_ = tabletCellController->GetSnapshotStore(CellGuid_);

        State_ = EPeerState::Stopped;

        LOG_INFO("Slot loaded");
    }

    void Create(const TCreateTabletSlotInfo& createInfo)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(State_ == EPeerState::None);

        auto cellGuid = FromProto<TCellGuid>(createInfo.cell_guid());
        SetCellGuid(cellGuid);

        LOG_INFO("Creating slot");

        State_ = EPeerState::Initializing;

        auto tabletCellController = Bootstrap_->GetTabletCellController();
        SnapshotStore_ = tabletCellController->GetSnapshotStore(CellGuid_);

        auto this_ = MakeStrong(this);
        BIND([this, this_] () {
            SwitchToIOThread();

            auto tabletCellController = Bootstrap_->GetTabletCellController();
            ChangelogStore_ = tabletCellController->GetChangelogCatalog()->CreateStore(CellGuid_);

            SwitchToControlThread();

            State_ = EPeerState::Stopped;

            LOG_INFO("Slot created");
        })
        .AsyncVia(Bootstrap_->GetControlInvoker())
        .Run();
    }

    void Configure(const TConfigureTabletSlotInfo& configureInfo)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(State_ != EPeerState::Initializing && State_ != EPeerState::Finalizing);

        auto cellConfig = New<TCellConfig>();
        cellConfig->CellGuid = CellGuid_;
        // NB: Missing peers will be represented by empty strings.
        cellConfig->Addresses.resize(configureInfo.config().size());
        for (const auto& peer : configureInfo.config().peers()) {
            cellConfig->Addresses[peer.peer_id()] = peer.address();
        }

        if (HydraManager_) {
            CellManager_->Reconfigure(cellConfig);
        } else {
            PeerId_ = configureInfo.peer_id();
            State_ = EPeerState::Elections;

            CellManager_ = New<TCellManager>(
                cellConfig,
                Bootstrap_->GetTabletChannelFactory(),
                configureInfo.peer_id());

            Automaton_ = New<TTabletAutomaton>(Bootstrap_, Owner_);

            auto rpcServer = Bootstrap_->GetRpcServer();

            HydraManager_ = CreateDistributedHydraManager(
                Config_->TabletNode->HydraManager,
                Bootstrap_->GetControlInvoker(),
                GetAutomatonInvoker(),
                Automaton_,
                rpcServer,
                CellManager_,
                ChangelogStore_,
                SnapshotStore_);

            HydraManager_->SubscribeStartLeading(BIND(&TImpl::OnStartEpoch, MakeWeak(this)));
            HydraManager_->SubscribeStartFollowing(BIND(&TImpl::OnStartEpoch, MakeWeak(this)));
            
            HydraManager_->SubscribeStopLeading(BIND(&TImpl::OnStopEpoch, MakeWeak(this)));
            HydraManager_->SubscribeStopFollowing(BIND(&TImpl::OnStopEpoch, MakeWeak(this)));

            GuardedAutomatonInvoker_ = HydraManager_->CreateGuardedAutomatonInvoker(AutomatonQueue_->GetInvoker());

            HiveManager_ = New<THiveManager>(
                CellGuid_,
                Config_->TabletNode->HiveManager,
                Bootstrap_->GetCellDirectory(),
                GetAutomatonInvoker(),
                rpcServer,
                HydraManager_,
                Automaton_);

            TabletManager_ = New<TTabletManager>(
                Config_->TabletNode->TabletManager,
                Owner_,
                Bootstrap_);

            TransactionManager_ = New<TTransactionManager>(
                Config_->TabletNode->TransactionManager,
                Owner_,
                Bootstrap_);

            TransactionSupervisor_ = New<TTransactionSupervisor>(
                Config_->TabletNode->TransactionSupervisor,
                GetAutomatonInvoker(),
                rpcServer,
                HydraManager_,
                Automaton_,
                HiveManager_,
                TransactionManager_,
                Bootstrap_->GetTimestampProvider());

            TabletService_ = CreateTabletService(
                Owner_,
                Bootstrap_);

            //auto queryExecutor = CreateQueryExecutor(
            //    Bootstrap_->GetQueryWorkerInvoker(),
            //    TabletManager_);

            //QueryService_ = CreateQueryService(
            //    GetCellGuid(),
            //    EpochAutomatonInvoker_,
            //    queryExecutor);

            TransactionSupervisor_->Start();
            TabletManager_->Start();
            HydraManager_->Start();
            HiveManager_->Start();

            rpcServer->RegisterService(TabletService_);
            //rpcServer->RegisterService(QueryService_);
        }

        CellConfig_ = configureInfo.config();

        LOG_INFO("Slot configured");
    }

    void Remove()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(State_ != EPeerState::None);
        
        LOG_INFO("Removing slot");
        
        State_ = EPeerState::Finalizing;

        auto this_ = MakeStrong(this);
        BIND([this, this_] () {
            SwitchToIOThread();

            auto tabletCellController = Bootstrap_->GetTabletCellController();
            tabletCellController->GetChangelogCatalog()->RemoveStore(CellGuid_);

            SwitchToControlThread();

            SnapshotStore_.Reset();
            ChangelogStore_.Reset();
            Reset();

            LOG_INFO("Slot removed");
        })
        .AsyncVia(Bootstrap_->GetControlInvoker())
        .Run();
    }


    void BuildOrchidYson(IYsonConsumer* consumer)
    {
        BuildYsonFluently(consumer)
            .BeginMap()
                .Do(BIND(&TImpl::BuildOrchidYsonControl, Unretained(this)))
                .Do(BIND(&TImpl::BuildOrchidYsonAutomaton, Unretained(this)))
            .EndMap();
    }

private:
    TTabletSlot* Owner_;
    int SlotIndex_;
    NCellNode::TCellNodeConfigPtr Config_;
    NCellNode::TBootstrap* Bootstrap_;

    TCellGuid CellGuid_;
    mutable EPeerState State_;
    TPeerId PeerId_;
    NHydra::NProto::TCellConfig CellConfig_;

    IChangelogStorePtr ChangelogStore_;
    ISnapshotStorePtr SnapshotStore_;
    TCellManagerPtr CellManager_;
    IHydraManagerPtr HydraManager_;
    
    THiveManagerPtr HiveManager_;
    TMailbox* MasterMailbox_;

    TTabletManagerPtr TabletManager_;

    TTransactionManagerPtr TransactionManager_;
    TTransactionSupervisorPtr TransactionSupervisor_;

    NRpc::IServicePtr TabletService_;
    NRpc::IServicePtr QueryService_;

    TTabletAutomatonPtr Automaton_;
    TActionQueuePtr AutomatonQueue_;
    IInvokerPtr EpochAutomatonInvoker_;
    IInvokerPtr GuardedAutomatonInvoker_;

    NLog::TTaggedLogger Logger;


    void Reset()
    {
        SetCellGuid(NullCellGuid);

        State_ = EPeerState::None;
        
        PeerId_ = InvalidPeerId;
        
        CellConfig_ = NHydra::NProto::TCellConfig();
        
        CellManager_.Reset();

        if (HydraManager_) {
            HydraManager_->Stop();
            HydraManager_.Reset();
        }

        GuardedAutomatonInvoker_.Reset();

        if (HiveManager_) {
            HiveManager_->Stop();
            HiveManager_.Reset();
        }

        MasterMailbox_ = nullptr;

        TabletManager_.Reset();

        TransactionManager_.Reset();

        if (TransactionSupervisor_) {
            TransactionSupervisor_->Stop();
            TransactionSupervisor_.Reset();
        }

        auto rpcServer = Bootstrap_->GetRpcServer();

        if (TabletService_) {
            rpcServer->UnregisterService(TabletService_);
            TabletService_.Reset();
        }

        //if (QueryService_) {
        //    rpcServer->UnregisterService(QueryService_);
        //    QueryService_.Reset();
        //}

        Automaton_.Reset();

        EpochAutomatonInvoker_.Reset();
    }

    void SetCellGuid(const TCellGuid& cellGuid)
    {
        CellGuid_ = cellGuid;
        InitLogger();
    }

    void InitLogger()
    {
        Logger = NLog::TTaggedLogger(TabletNodeLogger);
        Logger.AddTag(Sprintf("Slot: %d", SlotIndex_));
        if (CellGuid_ != NullCellGuid) {
            Logger.AddTag(Sprintf("CellGuid_: %s", ~ToString(CellGuid_)));
        }
    }


    void SwitchToIOThread()
    {
        SwitchTo(GetHydraIOInvoker());
        VERIFY_THREAD_AFFINITY(IOThread);
    }

    void SwitchToControlThread()
    {
        SwitchTo(Bootstrap_->GetControlInvoker());
        VERIFY_THREAD_AFFINITY(ControlThread);
    }


    void OnStartEpoch()
    {
        EpochAutomatonInvoker_ = HydraManager_
            ->GetEpochContext()
            ->CancelableContext
            ->CreateInvoker(GetAutomatonInvoker());

        auto queryExecutor = CreateQueryExecutor(
            EpochAutomatonInvoker_,
            Bootstrap_->GetQueryWorkerInvoker(),
            TabletManager_);

        QueryService_ = CreateQueryService(
            GetCellGuid(),
            EpochAutomatonInvoker_,
            queryExecutor);
        Bootstrap_->GetRpcServer()->RegisterService(QueryService_);
    }

    void OnStopEpoch()
    {
        EpochAutomatonInvoker_.Reset();

        if (QueryService_) {
            Bootstrap_->GetRpcServer()->UnregisterService(QueryService_);
            QueryService_.Reset();
        }
    }


    void BuildOrchidYsonControl(IYsonConsumer* consumer)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        BuildYsonMapFluently(consumer)
            .Item("state").Value(GetState())
            .DoIf(GetState() != EPeerState::None, [&] (TFluentMap fluent) {
                fluent
                    .Item("cell_guid").Value(CellGuid_);
            });
    }

    void BuildOrchidYsonAutomaton(IYsonConsumer* consumer)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!HydraManager_)
            return;
        
        auto epochContext = HydraManager_->GetEpochContext();
        if (!epochContext)
            return;

        auto cancelableContext = epochContext->CancelableContext;
        auto actuallyDone = BIND(&TImpl::DoBuildOrchidYsonAutomaton, MakeStrong(this))
            .AsyncVia(GuardedAutomatonInvoker_)
            .Run(cancelableContext, consumer);
        // Wait for actuallyDone to become fulfilled or canceled.
        auto somehowDone = NewPromise();
        actuallyDone.Subscribe(BIND([=] () mutable { somehowDone.Set(); }));
        actuallyDone.OnCanceled(BIND([=] () mutable { somehowDone.Set(); }));
        WaitFor(somehowDone);
    }

    void DoBuildOrchidYsonAutomaton(TCancelableContextPtr context, IYsonConsumer* consumer)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        // Make sure we're still using the same context.
        // Otherwise cell guid, which has alread been printed, might be wrong.
        if (context->IsCanceled())
            return;

        BuildYsonMapFluently(consumer)
            .Item("transactions").Do(BIND(&TTransactionManager::BuildOrchidYson, TransactionManager_))
            .Item("tablets").Do(BIND(&TTabletManager::BuildOrchidYson, TabletManager_));
    }


    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);
    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);
    DECLARE_THREAD_AFFINITY_SLOT(IOThread);

};

////////////////////////////////////////////////////////////////////////////////

TTabletSlot::TTabletSlot(
    int slotIndex,
    NCellNode::TCellNodeConfigPtr config,
    NCellNode::TBootstrap* bootstrap)
    : Impl_(New<TImpl>(
        this,
        slotIndex,
        config,
        bootstrap))
{ }

TTabletSlot::~TTabletSlot()
{ }

const TCellGuid& TTabletSlot::GetCellGuid() const
{
    return Impl_->GetCellGuid();
}

EPeerState TTabletSlot::GetState() const
{
    return Impl_->GetState();
}

TPeerId TTabletSlot::GetPeerId() const
{
    return Impl_->GetPeerId();
}

const NHydra::NProto::TCellConfig& TTabletSlot::GetCellConfig() const
{
    return Impl_->GetCellConfig();
}

IHydraManagerPtr TTabletSlot::GetHydraManager() const
{
    return Impl_->GetHydraManager();
}

TTabletAutomatonPtr TTabletSlot::GetAutomaton() const
{
    return Impl_->GetAutomaton();
}

IInvokerPtr TTabletSlot::GetAutomatonInvoker() const
{
    return Impl_->GetAutomatonInvoker();
}

IInvokerPtr TTabletSlot::GetEpochAutomatonInvoker() const
{
    return Impl_->GetEpochAutomatonInvoker();
}

THiveManagerPtr TTabletSlot::GetHiveManager() const
{
    return Impl_->GetHiveManager();
}

TMailbox* TTabletSlot::GetMasterMailbox()
{
    return Impl_->GetMasterMailbox();
}

TTransactionManagerPtr TTabletSlot::GetTransactionManager() const
{
    return Impl_->GetTransactionManager();
}

TTransactionSupervisorPtr TTabletSlot::GetTransactionSupervisor() const
{
    return Impl_->GetTransactionSupervisor();
}

TTabletManagerPtr TTabletSlot::GetTabletManager() const
{
    return Impl_->GetTabletManager();
}

TObjectId TTabletSlot::GenerateId(EObjectType type)
{
    return Impl_->GenerateId(type);
}

void TTabletSlot::Load(const TCellGuid& cellGuid)
{
    Impl_->Load(cellGuid);
}

void TTabletSlot::Create(const TCreateTabletSlotInfo& createInfo)
{
    Impl_->Create(createInfo);
}

void TTabletSlot::Configure(const TConfigureTabletSlotInfo& configureInfo)
{
    Impl_->Configure(configureInfo);
}

void TTabletSlot::Remove()
{
    Impl_->Remove();
}

void TTabletSlot::BuildOrchidYson(IYsonConsumer* consumer)
{
    return Impl_->BuildOrchidYson(consumer);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
} // namespace NTabletNode
