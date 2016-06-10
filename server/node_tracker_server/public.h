#pragma once

#include <yt/server/hydra/public.h>

#include <yt/ytlib/chunk_client/public.h>

#include <yt/ytlib/node_tracker_client/public.h>

#include <yt/core/misc/small_vector.h>

#include <bitset>

namespace NYT {
namespace NNodeTrackerServer {

///////////////////////////////////////////////////////////////////////////////

namespace NProto {

class TReqRemoveNode;

typedef NNodeTrackerClient::NProto::TReqRegisterNode TReqRegisterNode;
typedef NNodeTrackerClient::NProto::TReqIncrementalHeartbeat TReqIncrementalHeartbeat;
typedef NNodeTrackerClient::NProto::TReqFullHeartbeat TReqFullHeartbeat;

} // namespace NProto

///////////////////////////////////////////////////////////////////////////////

using NNodeTrackerClient::TNodeId;
using NNodeTrackerClient::InvalidNodeId;

using NNodeTrackerClient::TRackId;
using NNodeTrackerClient::NullRackId;

using NNodeTrackerClient::TAddressMap;
using NNodeTrackerClient::TNodeDescriptor;

///////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TNodeTracker)

DECLARE_REFCOUNTED_CLASS(TNodeTrackerConfig)

DECLARE_ENTITY_TYPE(TNode, NObjectClient::TObjectId, ::THash<NObjectClient::TObjectId>)
DECLARE_ENTITY_TYPE(TRack, TRackId, NObjectClient::TDirectObjectIdHash)

using TNodeList = SmallVector<TNode*, NChunkClient::TypicalReplicaCount>;

constexpr int MaxRackCount = 127;
constexpr int NullRackIndex = 0;
// NB: +1 is because of null rack.
using TRackSet = std::bitset<MaxRackCount + 1>;

///////////////////////////////////////////////////////////////////////////////

} // namespace NNodeTrackerServer
} // namespace NYT
