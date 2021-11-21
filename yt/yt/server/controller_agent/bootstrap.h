#pragma once

#include "public.h"

#include <yt/yt/ytlib/api/native/public.h>

#include <yt/yt/ytlib/monitoring/public.h>

#include <yt/yt/ytlib/node_tracker_client/public.h>

#include <yt/yt/ytlib/transaction_client/public.h>

#include <yt/yt/core/bus/public.h>

#include <yt/yt/core/concurrency/public.h>

#include <yt/yt/core/http/public.h>

#include <yt/yt/core/rpc/public.h>

#include <yt/yt/core/ytree/public.h>

namespace NYT::NControllerAgent {

////////////////////////////////////////////////////////////////////////////////

class TBootstrap
{
public:
    TBootstrap(TControllerAgentBootstrapConfigPtr config, NYTree::INodePtr configNode);
    ~TBootstrap();

    const NControllerAgent::TAgentId& GetAgentId() const;
    const TControllerAgentBootstrapConfigPtr& GetConfig() const;
    const NApi::NNative::IClientPtr& GetMasterClient() const;
    NNodeTrackerClient::TAddressMap GetLocalAddresses() const;
    NNodeTrackerClient::TNetworkPreferenceList GetLocalNetworks() const;
    IInvokerPtr GetControlInvoker() const;
    const IInvokerPtr& GetConnectionInvoker() const;
    const NControllerAgent::TControllerAgentPtr& GetControllerAgent() const;
    const NNodeTrackerClient::TNodeDirectoryPtr& GetNodeDirectory() const;
    const ICoreDumperPtr& GetCoreDumper() const;

    void Run();

private:
    const TControllerAgentBootstrapConfigPtr Config_;
    const NYTree::INodePtr ConfigNode_;

    NControllerAgent::TAgentId AgentId_;
    NMonitoring::TMonitoringManagerPtr MonitoringManager_;
    NConcurrency::TActionQueuePtr ControlQueue_;
    NConcurrency::TThreadPoolPtr ConnectionThreadPool_;
    NBus::IBusServerPtr BusServer_;
    NRpc::IServerPtr RpcServer_;
    NHttp::IServerPtr HttpServer_;
    NApi::NNative::IConnectionPtr Connection_;
    NApi::NNative::IClientPtr Client_;
    TControllerAgentPtr ControllerAgent_;
    ICoreDumperPtr CoreDumper_;

    void DoRun();
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent
