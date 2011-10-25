#include "stdafx.h"
#include "file_node_proxy.h"

namespace NYT {
namespace NFileServer {

////////////////////////////////////////////////////////////////////////////////

TFileNodeProxy::TFileNodeProxy(
    TCypressManager::TPtr cypressManager,
    const TTransactionId& transactionId,
    const TNodeId& nodeId)
    : TCypressNodeProxyBase<IEntityNode, TFileNode>(
        cypressManager,
        transactionId,
        nodeId)
{ }

NYT::NYTree::ENodeType TFileNodeProxy::GetType() const
{
    return ENodeType::Entity;
}

Stroka TFileNodeProxy::GetTypeName() const
{
    return FileTypeName;
}

TChunkId TFileNodeProxy::GetChunkId() const
{
    return GetTypedImpl().GetChunkId();
}

void TFileNodeProxy::SetChunkId(const TChunkId& chunkId)
{
    EnsureModifiable();
    GetTypedImplForUpdate().SetChunkId(chunkId);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NFileServer
} // namespace NYT

