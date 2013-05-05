#include "stdafx.h"
#include "node_tracker.h"
#include "config.h"
#include "node.h"
#include "private.h"

#include <ytlib/misc/id_generator.h>
#include <ytlib/misc/address.h>

#include <ytlib/ytree/convert.h>

#include <ytlib/ypath/token.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>

#include <ytlib/object_client/public.h>

#include <server/chunk_server/job.h>

#include <server/cypress_server/cypress_manager.h>

#include <server/cell_master/bootstrap.h>
#include <server/cell_master/meta_state_facade.h>
#include <server/cell_master/serialization_context.h>

namespace NYT {
namespace NNodeTrackerServer {

using namespace NYTree;
using namespace NYPath;
using namespace NNodeTrackerClient;
using namespace NNodeTrackerClient::NProto;
using namespace NMetaState;
using namespace NCellMaster;
using namespace NObjectClient;
using namespace NCypressClient;
using namespace NNodeTrackerServer::NProto;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = NodeTrackerServerLogger;

////////////////////////////////////////////////////////////////////////////////

class TNodeTracker::TImpl
    : public TMetaStatePart
{
public:
    TImpl(
        TNodeTrackerConfigPtr config,
        TBootstrap* bootstrap)
        : TMetaStatePart(
            bootstrap->GetMetaStateFacade()->GetManager(),
            bootstrap->GetMetaStateFacade()->GetState())
        , Config(config)
        , Bootstrap(bootstrap)
        , OnlineNodeCount(0)
        , RegisteredNodeCount(0)
        , Profiler(NodeTrackerServerProfiler)
    {
        YCHECK(config);
        YCHECK(bootstrap);

        RegisterMethod(BIND(&TImpl::RegisterNode, Unretained(this)));
        RegisterMethod(BIND(&TImpl::UnregisterNode, Unretained(this)));
        RegisterMethod(BIND(&TImpl::FullHeartbeat, Unretained(this)));
        RegisterMethod(BIND(&TImpl::IncrementalHeartbeat, Unretained(this)));

        {
            NCellMaster::TLoadContext context;
            context.SetBootstrap(Bootstrap);

            RegisterLoader(
                "NodeTracker.Keys",
                SnapshotVersionValidator(),
                BIND(&TImpl::LoadKeys, MakeStrong(this)),
                context);
            RegisterLoader(
                "NodeTracker.Values",
                SnapshotVersionValidator(),
                BIND(&TImpl::LoadValues, MakeStrong(this)),
                context);
        }

        {
            NCellMaster::TSaveContext context;

            RegisterSaver(
                ESerializationPriority::Keys,
                "NodeTracker.Keys",
                CurrentSnapshotVersion,
                BIND(&TImpl::SaveKeys, MakeStrong(this)),
                context);
            RegisterSaver(
                ESerializationPriority::Values,
                "NodeTracker.Values",
                CurrentSnapshotVersion,
                BIND(&TImpl::SaveValues, MakeStrong(this)),
                context);
        }

        SubscribeNodeConfigUpdated(BIND(&TImpl::OnNodeConfigUpdated, Unretained(this)));
    }


    TMutationPtr CreateRegisterNodeMutation(
        const TMetaReqRegisterNode& request)
    {
        return Bootstrap
            ->GetMetaStateFacade()
            ->CreateMutation(this, request, &TThis::RegisterNode);
    }

    TMutationPtr CreateUnregisterNodeMutation(
        const TMetaReqUnregisterNode& request)
    {
        return Bootstrap
            ->GetMetaStateFacade()
            ->CreateMutation(this, request, &TThis::UnregisterNode);
    }

    TMutationPtr CreateFullHeartbeatMutation(
        TCtxFullHeartbeatPtr context)
    {
        return Bootstrap
            ->GetMetaStateFacade()
            ->CreateMutation(EStateThreadQueue::Heartbeat)
            ->SetRequestData(context->GetRequestBody())
            ->SetType(context->Request().GetTypeName())
            ->SetAction(BIND(&TThis::FullHeartbeatWithContext, MakeStrong(this), context));
    }

    TMutationPtr CreateIncrementalHeartbeatMutation(
        const TMetaReqIncrementalHeartbeat& request)
    {
        return Bootstrap
            ->GetMetaStateFacade()
            ->CreateMutation(this, request, &TThis::IncrementalHeartbeat, EStateThreadQueue::Heartbeat);
    }


    void RefreshNodeConfig(TNode* node)
    {
        auto attributes = DoFindNodeConfig(node->GetAddress());
        if (!attributes)
            return;

        if (!ReconfigureYsonSerializable(node->GetConfig(), attributes))
            return;

        LOG_INFO("Node configuration updated (Address: %s)", ~node->GetAddress());

        // Check for runtime changes.
        if (IsLeader()) {
            NodeConfigUpdated_.Fire(node);
        }
    }


    DECLARE_METAMAP_ACCESSORS(Node, TNode, TNodeId);

    DEFINE_SIGNAL(void(TNode* node), NodeRegistered);
    DEFINE_SIGNAL(void(TNode* node), NodeUnregistered);
    DEFINE_SIGNAL(void(TNode* node), NodeConfigUpdated);
    DEFINE_SIGNAL(void(TNode* node, const TMetaReqFullHeartbeat& request), FullHeartbeat);
    DEFINE_SIGNAL(void(TNode* node, const TMetaReqIncrementalHeartbeat& request), IncrementalHeartbeat);


    TNode* FindNodeByAddress(const Stroka& address)
    {
        auto it = NodeAddressMap.find(address);
        return it == NodeAddressMap.end() ? nullptr : it->second;
    }

    TNode* GetNodeByAddress(const Stroka& address)
    {
        auto* node = FindNodeByAddress(address);
        YCHECK(node);
        return node;
    }

    TNode* FindNodeByHostName(const Stroka& hostName)
    {
        auto it = NodeHostNameMap.find(hostName);
        return it == NodeAddressMap.end() ? nullptr : it->second;
    }

    TNode* GetNodeOrThrow(TNodeId id)
    {
        auto* node = FindNode(id);
        if (!node) {
            THROW_ERROR_EXCEPTION(
                NNodeTrackerClient::EErrorCode::NoSuchNode,
                "Invalid or expired node id %d",
                id);
        }
        return node;
    }


    TNodeConfigPtr FindNodeConfigByAddress(const Stroka& address)
    {
        auto attributes = DoFindNodeConfig(address);
        if (!attributes) {
            return nullptr;
        }

        try {
            return ConvertTo<TNodeConfigPtr>(attributes);
        } catch (const std::exception& ex) {
            LOG_WARNING(ex, "Error parsing configuration of node %s, defaults will be used", ~address);
            return nullptr;
        }
    }

    TNodeConfigPtr GetNodeConfigByAddress(const Stroka& address)
    {
        auto config = FindNodeConfigByAddress(address);
        return config ? config : New<TNodeConfig>();
    }

    
    TTotalNodeStatistics GetTotalNodeStatistics()
    {
        TTotalNodeStatistics result;
        FOREACH (const auto& pair, NodeMap) {
            const auto* node = pair.second;
            const auto& statistics = node->Statistics();
            result.AvailbaleSpace += statistics.total_available_space();
            result.UsedSpace += statistics.total_used_space();
            result.ChunkCount += statistics.total_chunk_count();
            result.SessionCount += statistics.total_session_count();
            result.OnlineNodeCount++;
        }
        return result;
    }

    int GetRegisteredNodeCount()
    {
        return RegisteredNodeCount;
    }

    int GetOnlineNodeCount()
    {
        return OnlineNodeCount;
    }

private:
    typedef TImpl TThis;

    TNodeTrackerConfigPtr Config;
    TBootstrap* Bootstrap;

    int OnlineNodeCount;
    int RegisteredNodeCount;

    NProfiling::TProfiler& Profiler;

    TIdGenerator NodeIdGenerator;

    TMetaStateMap<TNodeId, TNode> NodeMap;
    yhash_map<Stroka, TNode*> NodeAddressMap;
    yhash_multimap<Stroka, TNode*> NodeHostNameMap;


    TNodeId GenerateNodeId()
    {
        TNodeId id;
        while (true) {
            id = NodeIdGenerator.Next();
            // Beware of sentinels!
            if (id == InvalidNodeId) {
                // Just wait for the next attempt.
            } else if (id > MaxNodeId) {
                NodeIdGenerator.Reset();
            } else {
                break;
            }
        }
        return id;
    }

    IMapNodePtr DoFindNodeConfig(const Stroka& address)
    {
        auto cypressManager = Bootstrap->GetCypressManager();
        auto resolver = cypressManager->CreateResolver();

        auto nodesNode = resolver->ResolvePath("//sys/nodes");
        YCHECK(nodesNode);

        auto nodesMap = nodesNode->AsMap();
        auto nodeNode = nodesMap->FindChild(address);
        if (!nodeNode) {
            return nullptr;
        }

        return nodeNode->Attributes().ToMap();
    }


    TMetaRspRegisterNode RegisterNode(const TMetaReqRegisterNode& request)
    {
        auto descriptor = FromProto<NNodeTrackerClient::TNodeDescriptor>(request.node_descriptor());
        const auto& statistics = request.statistics();
        const auto& address = descriptor.Address;

        // Kick-out any previous incarnation.
        {
            auto* existingNode = FindNodeByAddress(descriptor.Address);
            if (existingNode) {
                LOG_INFO_UNLESS(IsRecovery(), "Node kicked out due to address conflict (Address: %s, ExistingId: %d)",
                    ~address,
                    existingNode->GetId());
                DoUnregisterNode(existingNode);
            }
        }

        auto* node = DoRegisterNode(descriptor, statistics);

        TMetaRspRegisterNode response;
        response.set_node_id(node->GetId());
        return response;
    }

    void UnregisterNode(const TMetaReqUnregisterNode& request)
    {
        auto nodeId = request.node_id();

        // Allow nodeId to be invalid, just ignore such obsolete requests.
        auto* node = FindNode(nodeId);
        if (!node)
            return;

        DoUnregisterNode(node);
    }


    void FullHeartbeatWithContext(TCtxFullHeartbeatPtr context)
    {
        return FullHeartbeat(context->Request());
    }

    void FullHeartbeat(const TMetaReqFullHeartbeat& request)
    {
        PROFILE_TIMING ("/full_heartbeat_time") {
            auto nodeId = request.node_id();
            const auto& statistics = request.statistics();

            auto* node = GetNode(nodeId);

            LOG_DEBUG_UNLESS(IsRecovery(), "Full heartbeat received (NodeId: %d, Address: %s, State: %s, %s)",
                nodeId,
                ~node->GetAddress(),
                ~node->GetState().ToString(),
                ~ToString(statistics));

            node->Statistics() = statistics;

            YCHECK(node->GetState() == ENodeState::Registered);
            UpdateNodeCounters(node, -1);
            node->SetState(ENodeState::Online);
            UpdateNodeCounters(node, +1);

            if (IsLeader()) {
                RenewNodeLease(node);
            }

            LOG_INFO_UNLESS(IsRecovery(), "Node online (NodeId: %d, Address: %s)",
                nodeId,
                ~node->GetAddress());

            FullHeartbeat_.Fire(node, request);
        }
    }


    void IncrementalHeartbeat(const TMetaReqIncrementalHeartbeat& request)
    {
        PROFILE_TIMING ("/incremental_heartbeat_time") {
            auto nodeId = request.node_id();
            const auto& statistics = request.statistics();

            auto* node = GetNode(nodeId);

            LOG_DEBUG_UNLESS(IsRecovery(), "Incremental heartbeat received (NodeId: %d, Address: %s, State: %s, %s)",
                nodeId,
                ~node->GetAddress(),
                ~node->GetState().ToString(),
                ~ToString(statistics));

            YCHECK(node->GetState() == ENodeState::Online);
            node->Statistics() = statistics;

            if (IsLeader()) {
                if (!node->GetConfirmed()) {
                    node->SetConfirmed(true);
                    LOG_DEBUG_UNLESS(IsRecovery(), "Node confirmed (NodeId: %d, Address: %s)",
                        nodeId,
                        ~node->GetAddress());
                }

                RenewNodeLease(node);
            }

            IncrementalHeartbeat_.Fire(node, request);
        }
    }


    void SaveKeys(const NCellMaster::TSaveContext& context) const
    {
        NodeMap.SaveKeys(context);
    }

    void SaveValues(const NCellMaster::TSaveContext& context) const
    {
        Save(context, NodeIdGenerator);
        NodeMap.SaveValues(context);
    }

    void LoadKeys(const NCellMaster::TLoadContext& context)
    {
        NodeMap.LoadKeys(context);
    }

    void LoadValues(const NCellMaster::TLoadContext& context)
    {
        Load(context, NodeIdGenerator);
        NodeMap.LoadValues(context);
    }

    virtual void Clear() override
    {
        NodeIdGenerator.Reset();
        NodeMap.Clear();
        NodeAddressMap.clear();
        NodeHostNameMap.clear();
        OnlineNodeCount = 0;
        RegisteredNodeCount = 0;
    }

    virtual void OnAfterLoaded() override
    {
        // Reconstruct address maps, recompute statistics.
        NodeAddressMap.clear();
        NodeHostNameMap.clear();

        OnlineNodeCount = 0;
        RegisteredNodeCount = 0;

        FOREACH (const auto& pair, NodeMap) {
            auto* node = pair.second;
            const auto& address = node->GetAddress();

            YCHECK(NodeAddressMap.insert(std::make_pair(address, node)).second);
            NodeHostNameMap.insert(std::make_pair(Stroka(GetServiceHostName(address)), node));

            UpdateNodeCounters(node, +1);
            RefreshNodeConfig(node);
        }
    }


    virtual void OnRecoveryStarted() override
    {
        Profiler.SetEnabled(false);

        // Reset runtime info.
        FOREACH (const auto& pair, NodeMap) {
            auto* node = pair.second;

            node->SetConfirmed(false);

            auto lease = node->GetLease();
            if (lease) {
                TLeaseManager::CloseLease(lease);
                node->SetLease(TLeaseManager::NullLease);
            }

            node->SetHintedSessionCount(0);
            
            FOREACH (auto& queue, node->ChunkReplicationQueues()) {
                queue.clear();
            }

            node->ChunkRemovalQueue().clear();
        }
    }

    virtual void OnRecoveryComplete() override
    {
        Profiler.SetEnabled(true);
    }


    virtual void OnActiveQuorumEstablished() override
    {
        // Assign initial leases to nodes.
        // NB: Nodes will remain unconfirmed until the first heartbeat.
        FOREACH (const auto& pair, NodeMap) {
            StartNodeLease(pair.second);
        }
    }

    virtual void OnStopLeading() override
    {
        // Stop existing leases.
        FOREACH (const auto& pair, NodeMap) {
            StopNodeLease(pair.second);
        }
    }


    void UpdateNodeCounters(TNode* node, int delta)
    {
        switch (node->GetState()) {
            case ENodeState::Registered:
                RegisteredNodeCount += delta;
                break;
            case ENodeState::Online:
                OnlineNodeCount += delta;
                break;
            default:
                break;
        }
    }

    void StartNodeLease(TNode* node)
    {
        auto metaStateFacade = Bootstrap->GetMetaStateFacade();
        auto timeout = GetLeaseTimeout(node);
        auto lease = TLeaseManager::CreateLease(
            timeout,
            BIND(&TThis::OnExpired, MakeStrong(this), node->GetId())
                .Via(metaStateFacade->GetEpochInvoker(EStateThreadQueue::Heartbeat)));
        node->SetLease(lease);
    }

    void StopNodeLease(TNode* node)
    {
        auto lease = node->GetLease();
        if (lease) {
            TLeaseManager::CloseLease(node->GetLease());
            node->SetLease(TLeaseManager::NullLease);
        }
    }

    void RenewNodeLease(TNode* node)
    {
        auto timeout = GetLeaseTimeout(node);
        TLeaseManager::RenewLease(node->GetLease(), timeout);
    }

    TDuration GetLeaseTimeout(const TNode* node)
    {
        if (!node->GetConfirmed()) {
            return Config->UnconfirmedNodeTimeout;
        } else if (node->GetState() == ENodeState::Registered) {
            return Config->RegisteredNodeTimeout;
        } else {
            return Config->OnlineNodeTimeout;
        }
    }

    void OnExpired(TNodeId nodeId)
    {
        auto* node = FindNode(nodeId);
        if (!node)
            return;

        LOG_INFO("Node lease expired (NodeId: %d, Address: %s)",
            nodeId,
            ~node->GetAddress());

        PostUnregisterCommit(nodeId);
    }


    void RegisterNodeInCypress(TNode* node)
    {
        // We're already in the state thread but need to postpone the planned changes and enqueue a callback.
        // Doing otherwise will turn node registration and Cypress update into a single
        // logged change, which is undesirable.
        auto metaStateFacade = Bootstrap->GetMetaStateFacade();
        const auto& address = node->GetAddress();
        BIND(&TImpl::DoRegisterNodeInCypress, MakeStrong(this), address)
            .Via(metaStateFacade->GetEpochInvoker())
            .Run();
    }

    void DoRegisterNodeInCypress(const Stroka& address)
    {
        // TODO(babenko): make a single transaction
        // TODO(babenko): check for errors and retry
        auto addressToken = ToYPathLiteral(address);

        auto cypressManager = Bootstrap->GetCypressManager();
        auto rootService = cypressManager->GetRootService();
        {
            auto req = TCypressYPathProxy::Create("/sys/nodes/" + addressToken);
            req->set_type(EObjectType::CellNode);
            req->set_ignore_existing(true);
            ExecuteVerb(rootService, req);
        }

        {
            auto req = TCypressYPathProxy::Create("/sys/nodes/" + addressToken + "/orchid");
            req->set_type(EObjectType::Orchid);
            req->set_ignore_existing(true);

            auto attributes = CreateEphemeralAttributes();
            attributes->Set("remote_address", address);
            ToProto(req->mutable_node_attributes(), *attributes);

            ExecuteVerb(rootService, req);
        }
    }


    TNode* DoRegisterNode(const TNodeDescriptor& descriptor, const TNodeStatistics& statistics)
    {
        PROFILE_TIMING ("/node_register_time") {
            const auto& address = descriptor.Address;
            auto config = GetNodeConfigByAddress(address);
            auto nodeId = GenerateNodeId();

            auto* node = new TNode(nodeId, descriptor, config);
            node->SetState(ENodeState::Registered);
            node->Statistics() = statistics;

            NodeMap.Insert(nodeId, node);
            NodeAddressMap.insert(std::make_pair(address, node));
            NodeHostNameMap.insert(std::make_pair(Stroka(GetServiceHostName(address)), node));
            
            UpdateNodeCounters(node, +1);

            if (IsLeader()) {
                StartNodeLease(node);
                RegisterNodeInCypress(node);
            }

            LOG_INFO_UNLESS(IsRecovery(), "Node registered (NodeId: %d, Address: %s, %s)",
                nodeId,
                ~address,
                ~ToString(statistics));

            NodeRegistered_.Fire(node);

            return node;
        }
    }

    void DoUnregisterNode(TNode* node)
    {
        PROFILE_TIMING ("/node_unregister_time") {
            auto nodeId = node->GetId();

            LOG_INFO_UNLESS(IsRecovery(), "Node unregistered (NodeId: %d, Address: %s)",
                nodeId,
                ~node->GetAddress());

            if (IsLeader()) {
                StopNodeLease(node);
            }

            const auto& address = node->GetAddress();
            YCHECK(NodeAddressMap.erase(address) == 1);
            {
                auto hostNameRange = NodeHostNameMap.equal_range(Stroka(GetServiceHostName(address)));
                for (auto it = hostNameRange.first; it != hostNameRange.second; ++it) {
                    if (it->second == node) {
                        NodeHostNameMap.erase(it);
                        break;
                    }
                }
            }

            UpdateNodeCounters(node, -1);

            NodeUnregistered_.Fire(node);

            NodeMap.Remove(nodeId);
        }
    }


    void PostUnregisterCommit(TNodeId nodeId)
    {
        TMetaReqUnregisterNode message;
        message.set_node_id(nodeId);

        auto invoker = Bootstrap->GetMetaStateFacade()->GetEpochInvoker();
        CreateUnregisterNodeMutation(message)
            ->OnSuccess(BIND(&TThis::OnUnregisterCommitSucceeded, MakeStrong(this), nodeId).Via(invoker))
            ->OnError(BIND(&TThis::OnUnregisterCommitFailed, MakeStrong(this), nodeId).Via(invoker))
            ->PostCommit();
    }

    void OnUnregisterCommitSucceeded(TNodeId nodeId)
    {
        LOG_INFO("Node unregister commit succeeded (NodeId: %d)",
            nodeId);
    }

    void OnUnregisterCommitFailed(TNodeId nodeId, const TError& error)
    {
        LOG_ERROR(error, "Node unregister commit failed (NodeId: %d)",
            nodeId);
    }


    void OnNodeConfigUpdated(TNode* node)
    {
        if (node->GetConfig()->Banned) {
            LOG_INFO("Node banned (Address: %s)", ~node->GetAddress());
            PostUnregisterCommit(node->GetId());
        }
    }

};

DEFINE_METAMAP_ACCESSORS(TNodeTracker::TImpl, Node, TNode, TNodeId, NodeMap)

///////////////////////////////////////////////////////////////////////////////

TNodeTracker::TNodeTracker(
    TNodeTrackerConfigPtr config,
    TBootstrap* bootstrap)
    : Impl(New<TImpl>(config, bootstrap))
{ }

TNodeTracker::~TNodeTracker()
{ }

TNode* TNodeTracker::FindNodeByAddress(const Stroka& address)
{
    return Impl->FindNodeByAddress(address);
}

TNode* TNodeTracker::GetNodeByAddress(const Stroka& address)
{
    return Impl->GetNodeByAddress(address);
}

TNode* TNodeTracker::FindNodeByHostName(const Stroka& hostName)
{
    return Impl->FindNodeByHostName(hostName);
}

TNode* TNodeTracker::GetNodeOrThrow(TNodeId id)
{
    return Impl->GetNodeOrThrow(id);
}

TNodeConfigPtr TNodeTracker::FindNodeConfigByAddress(const Stroka& address)
{
    return Impl->FindNodeConfigByAddress(address);
}

TNodeConfigPtr TNodeTracker::GetNodeConfigByAddress(const Stroka& address)
{
    return Impl->GetNodeConfigByAddress(address);
}

TMutationPtr TNodeTracker::CreateRegisterNodeMutation(
    const TMetaReqRegisterNode& request)
{
    return Impl->CreateRegisterNodeMutation(request);
}

TMutationPtr TNodeTracker::CreateUnregisterNodeMutation(
    const TMetaReqUnregisterNode& request)
{
    return Impl->CreateUnregisterNodeMutation(request);
}

TMutationPtr TNodeTracker::CreateFullHeartbeatMutation(
    TCtxFullHeartbeatPtr context)
{
    return Impl->CreateFullHeartbeatMutation(context);
}

TMutationPtr TNodeTracker::CreateIncrementalHeartbeatMutation(
    const TMetaReqIncrementalHeartbeat& request)
{
    return Impl->CreateIncrementalHeartbeatMutation(request);
}

void TNodeTracker::RefreshNodeConfig(TNode* node)
{
    return Impl->RefreshNodeConfig(node);
}

TTotalNodeStatistics TNodeTracker::GetTotalNodeStatistics()
{
    return Impl->GetTotalNodeStatistics();
}

int TNodeTracker::GetRegisteredNodeCount()
{
    return Impl->GetRegisteredNodeCount();
}

int TNodeTracker::GetOnlineNodeCount()
{
    return Impl->GetOnlineNodeCount();
}

DELEGATE_METAMAP_ACCESSORS(TNodeTracker, Node, TNode, TNodeId, *Impl)

DELEGATE_SIGNAL(TNodeTracker, void(TNode* node), NodeRegistered, *Impl);
DELEGATE_SIGNAL(TNodeTracker, void(TNode* node), NodeUnregistered, *Impl);
DELEGATE_SIGNAL(TNodeTracker, void(TNode* node), NodeConfigUpdated, *Impl);
DELEGATE_SIGNAL(TNodeTracker, void(TNode* node, const TMetaReqFullHeartbeat& request), FullHeartbeat, *Impl);
DELEGATE_SIGNAL(TNodeTracker, void(TNode* node, const TMetaReqIncrementalHeartbeat& request), IncrementalHeartbeat, *Impl);

///////////////////////////////////////////////////////////////////////////////

} // namespace NNodeTrackerServer
} // namespace NYT
