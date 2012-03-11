#pragma once

#include "public.h"
#include "meta_state_manager_proxy.h"

#include <ytlib/misc/periodic_invoker.h>
#include <ytlib/misc/configurable.h>
#include <ytlib/misc/thread_affinity.h>

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////

class TFollowerPinger
    : public TRefCounted
{
public:
    typedef TIntrusivePtr<TFollowerPinger> TPtr;

    struct TConfig
        : public TConfigurable
    {
        typedef TIntrusivePtr<TConfig> TPtr;

        TDuration PingInterval;
        TDuration RpcTimeout;

        TConfig()
        {
            Register("ping_interval", PingInterval)
                .GreaterThan(TDuration())
                .Default(TDuration::MilliSeconds(1000));
            Register("rpc_timeout", RpcTimeout)
                .GreaterThan(TDuration())
                .Default(TDuration::MilliSeconds(1000));
        }
    };

    TFollowerPinger(
        TConfig* config,
        TCellManager* cellManager,
        TLeaderCommitter* committer,
        TFollowerTracker* followerTracker,
        const TEpoch& epoch,
        IInvoker* epochControlInvoker);

    void Start();
    void Stop();

private:
    typedef TMetaStateManagerProxy TProxy;

    void SendPing(TPeerId followerId);
    void SchedulePing(TPeerId followerId);
    void OnPingResponse(TProxy::TRspPingFollower::TPtr response, TPeerId followerId);

    TConfig::TPtr Config;
    TPeriodicInvoker::TPtr PeriodicInvoker;
    TCellManagerPtr CellManager;
    TLeaderCommitterPtr Committer;
    TFollowerTrackerPtr FollowerTracker;
    TSnapshotStorePtr SnapshotStore;
    TEpoch Epoch;
    IInvoker::TPtr EpochControlInvoker;

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT
