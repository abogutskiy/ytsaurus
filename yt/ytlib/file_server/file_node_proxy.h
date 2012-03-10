#pragma once

#include "common.h"
#include "file_node.h"
#include "file_ypath.pb.h"

#include <ytlib/ytree/ypath_service.h>
#include <ytlib/cypress/node_proxy_detail.h>
#include <ytlib/chunk_server/chunk_manager.h>
#include <ytlib/cell_master/public.h>

namespace NYT {
namespace NFileServer {

////////////////////////////////////////////////////////////////////////////////

class TFileNodeProxy
    : public NCypress::TCypressNodeProxyBase<NYTree::IEntityNode, TFileNode>
{
public:
    typedef TIntrusivePtr<TFileNodeProxy> TPtr;

    TFileNodeProxy(
        NCypress::INodeTypeHandler* typeHandler,
        NCellMaster::TBootstrap* bootstrap,
        const NObjectServer::TTransactionId& transactionId,
        const NCypress::TNodeId& nodeId);

    bool IsExecutable();
    Stroka GetFileName();

private:
    typedef NCypress::TCypressNodeProxyBase<NYTree::IEntityNode, TFileNode> TBase;

    virtual void GetSystemAttributes(std::vector<TAttributeInfo>* attributes);
    virtual bool GetSystemAttribute(const Stroka& name, NYTree::IYsonConsumer* consumer);

    virtual void DoInvoke(NRpc::IServiceContext* context);

    DECLARE_RPC_SERVICE_METHOD(NProto, Fetch);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NFileServer
} // namespace NYT

