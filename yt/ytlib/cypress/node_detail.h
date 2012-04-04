#pragma once

#include "node.h"
#include "cypress_manager.h"
#include <ytlib/ytree/ypath.pb.h>

#include <ytlib/misc/serialize.h>
#include <ytlib/ytree/node_detail.h>
#include <ytlib/ytree/fluent.h>
#include <ytlib/ytree/ephemeral.h>
#include <ytlib/ytree/tree_builder.h>
#include <ytlib/object_server/object_detail.h>
#include <ytlib/cell_master/bootstrap.h>

namespace NYT {
namespace NCypress {

////////////////////////////////////////////////////////////////////////////////

template <class TImpl, class TProxy>
class TNodeBehaviorBase
    : public INodeBehavior
{
public:
    TNodeBehaviorBase(
        NCellMaster::TBootstrap* bootstrap,
        const TNodeId& nodeId)
        : Bootstrap(bootstrap)
        , NodeId(nodeId)
    { }

    virtual void Destroy()
    { }

protected:
    NCellMaster::TBootstrap* Bootstrap;
    TNodeId NodeId;

    TImpl& GetImpl()
    {
        return Bootstrap->GetCypressManager()->GetNode(NodeId);
    }

    TIntrusivePtr<TProxy> GetProxy()
    {
        auto proxy = Bootstrap->GetCypressManager()->GetVersionedNodeProxy(NodeId, NullTransactionId);
        auto* typedProxy = dynamic_cast<TProxy*>(~proxy);
        YASSERT(typedProxy);
        return typedProxy;
    }

};

////////////////////////////////////////////////////////////////////////////////

template <class TImpl>
class TCypressNodeTypeHandlerBase
    : public INodeTypeHandler
{
public:
    explicit TCypressNodeTypeHandlerBase(NCellMaster::TBootstrap* bootstrap)
        : Bootstrap(bootstrap)
    { }

    virtual TAutoPtr<ICypressNode> Create(const TVersionedNodeId& id)
    {
        return new TImpl(id);
    }

    virtual void CreateFromManifest(
        const TNodeId& nodeId,
        const TTransactionId& transactionId,
        NYTree::IMapNode* manifest)
    {
        UNUSED(manifest);
        auto node = Create(nodeId);
        auto cypressManager = Bootstrap->GetCypressManager();
        cypressManager->RegisterNode(transactionId, node);
        auto proxy = cypressManager->GetVersionedNodeProxy(nodeId, transactionId);
        proxy->Attributes().MergeFrom(manifest);
    }

    virtual void Destroy(ICypressNode& node)
    {
        auto id = node.GetId();
        auto objectManager = Bootstrap->GetObjectManager();
        if (objectManager->FindAttributes(id)) {
            objectManager->RemoveAttributes(id);
        }

        DoDestroy(dynamic_cast<TImpl&>(node));
    }

    virtual bool IsLockModeSupported(ELockMode mode)
    {
        return
            mode == ELockMode::Exclusive ||
            mode == ELockMode::Snapshot;
    }

    virtual TAutoPtr<ICypressNode> Branch(
        const ICypressNode& originatingNode,
        const TTransactionId& transactionId,
        ELockMode mode)
    {
        const auto& typedOriginatingNode = dynamic_cast<const TImpl&>(originatingNode);

        auto originatingId = originatingNode.GetId();
        auto branchedId = TVersionedNodeId(originatingId.ObjectId, transactionId);

        // Create a branched copy.
        TAutoPtr<TImpl> branchedNode = new TImpl(branchedId, typedOriginatingNode);
        branchedNode->SetLockMode(mode);

        // Branch user attributes.
        Bootstrap->GetObjectManager()->BranchAttributes(originatingId, branchedId);
        
        // Run custom branching.
        DoBranch(typedOriginatingNode, *branchedNode);

        return branchedNode.Release();
    }

    virtual void Merge(
        ICypressNode& originatingNode,
        ICypressNode& branchedNode)
    {
        auto originatingId = originatingNode.GetId();
        auto branchedId = branchedNode.GetId();
        YASSERT(branchedId.IsBranched());

        // Merge user attributes.
        Bootstrap->GetObjectManager()->MergeAttributes(originatingId, branchedId);

        // Merge parent id.
        originatingNode.SetParentId(branchedNode.GetParentId());

        // Run custom merging.
        DoMerge(dynamic_cast<TImpl&>(originatingNode), dynamic_cast<TImpl&>(branchedNode));
    }

    virtual INodeBehavior::TPtr CreateBehavior(const TNodeId& id)
    {
        UNUSED(id);
        return NULL;
    }

protected:
    NCellMaster::TBootstrap* Bootstrap;

    virtual void DoDestroy(TImpl& node)
    {
        UNUSED(node);
    }

    virtual void DoBranch(
        const TImpl& originatingNode,
        TImpl& branchedNode)
    {
        UNUSED(originatingNode);
        UNUSED(branchedNode);
    }

    virtual void DoMerge(
        TImpl& originatingNode,
        TImpl& branchedNode)
    {
        UNUSED(originatingNode);
        UNUSED(branchedNode);
    }

private:
    typedef TCypressNodeTypeHandlerBase<TImpl> TThis;

};

//////////////////////////////////////////////////////////////////////////////// 

class TCypressNodeBase
    : public NObjectServer::TObjectBase
    , public ICypressNode
{
    // This also overrides appropriate methods from ICypressNode.
    DEFINE_BYREF_RW_PROPERTY(yhash_set<TLock*>, Locks);
    DEFINE_BYREF_RW_PROPERTY(yhash_set<TLock*>, SubtreeLocks);

    DEFINE_BYVAL_RW_PROPERTY(TNodeId, ParentId);
    DEFINE_BYVAL_RW_PROPERTY(ELockMode, LockMode);

public:
    explicit TCypressNodeBase(const TVersionedNodeId& id);
    TCypressNodeBase(const TVersionedNodeId& id, const TCypressNodeBase& other);

    virtual EObjectType GetObjectType() const;
    virtual TVersionedNodeId GetId() const;

    virtual i32 RefObject();
    virtual i32 UnrefObject();
    virtual i32 GetObjectRefCounter() const;

    virtual void Save(TOutputStream* output) const;
    virtual void Load(const NCellMaster::TLoadContext& context, TInputStream* input);

protected:
    TVersionedNodeId Id;

};

//////////////////////////////////////////////////////////////////////////////// 

namespace NDetail {

template <class TValue>
struct TCypressScalarTypeTraits
{ };

template <>
struct TCypressScalarTypeTraits<Stroka>
    : NYTree::NDetail::TScalarTypeTraits<Stroka>
{
    static const EObjectType::EDomain ObjectType;
};

template <>
struct TCypressScalarTypeTraits<i64>
    : NYTree::NDetail::TScalarTypeTraits<i64>
{
    static const EObjectType::EDomain ObjectType;
};

template <>
struct TCypressScalarTypeTraits<double>
    : NYTree::NDetail::TScalarTypeTraits<double>
{
    static const EObjectType::EDomain ObjectType;
};

} // namespace NDetail

//////////////////////////////////////////////////////////////////////////////// 

template <class TValue>
class TScalarNode
    : public TCypressNodeBase
{
    typedef TScalarNode<TValue> TThis;

    DEFINE_BYREF_RW_PROPERTY(TValue, Value)

public:
    explicit TScalarNode(const TVersionedNodeId& id)
        : TCypressNodeBase(id)
    { }

    TScalarNode(const TVersionedNodeId& id, const TThis& other)
        : TCypressNodeBase(id, other)
        , Value_(other.Value_)
    { }

    virtual void Save(TOutputStream* output) const
    {
        TCypressNodeBase::Save(output);
        ::Save(output, Value_);
    }
    
    virtual void Load(const NCellMaster::TLoadContext& context, TInputStream* input)
    {
        TCypressNodeBase::Load(context, input);
        ::Load(input, Value_);
    }
};

typedef TScalarNode<Stroka> TStringNode;
typedef TScalarNode<i64>    TIntegerNode;
typedef TScalarNode<double> TDoubleNode;

//////////////////////////////////////////////////////////////////////////////// 

template <class TValue>
class TScalarNodeTypeHandler
    : public TCypressNodeTypeHandlerBase< TScalarNode<TValue> >
{
public:
    TScalarNodeTypeHandler(NCellMaster::TBootstrap* bootstrap)
        : TCypressNodeTypeHandlerBase< TScalarNode<TValue> >(bootstrap)
    { }

    virtual EObjectType GetObjectType()
    {
        return NDetail::TCypressScalarTypeTraits<TValue>::ObjectType;
    }

    virtual NYTree::ENodeType GetNodeType()
    {
        return NDetail::TCypressScalarTypeTraits<TValue>::NodeType;
    }

    virtual TIntrusivePtr<ICypressNodeProxy> GetProxy(const TVersionedNodeId& id);

protected:
    virtual void DoMerge(
        TScalarNode<TValue>& originatingNode,
        TScalarNode<TValue>& branchedNode)
    {
        originatingNode.Value() = branchedNode.Value();
    }

};

typedef TScalarNodeTypeHandler<Stroka> TStringNodeTypeHandler;
typedef TScalarNodeTypeHandler<i64>    TIntegerNodeTypeHandler;
typedef TScalarNodeTypeHandler<double> TDoubleNodeTypeHandler;

//////////////////////////////////////////////////////////////////////////////// 

class TMapNode
    : public TCypressNodeBase
{
    typedef yhash_map<Stroka, TNodeId> TKeyToChild;
    typedef yhash_map<TNodeId, Stroka> TChildToKey;

    DEFINE_BYREF_RW_PROPERTY(TKeyToChild, KeyToChild);
    DEFINE_BYREF_RW_PROPERTY(TChildToKey, ChildToKey);
    DEFINE_BYREF_RW_PROPERTY(i32, ChildCountDelta); // It's very inconvenient to access it by ref

public:
    explicit TMapNode(const TVersionedNodeId& id);
    TMapNode(const TVersionedNodeId& id, const TMapNode& other);

    virtual void Save(TOutputStream* output) const;
    virtual void Load(const NCellMaster::TLoadContext& context, TInputStream* input);
};

//////////////////////////////////////////////////////////////////////////////// 

class TMapNodeTypeHandler
    : public TCypressNodeTypeHandlerBase<TMapNode>
{
public:
    explicit TMapNodeTypeHandler(NCellMaster::TBootstrap* bootstrap);

    virtual EObjectType GetObjectType();
    virtual NYTree::ENodeType GetNodeType();

    virtual TIntrusivePtr<ICypressNodeProxy> GetProxy(const TVersionedNodeId& id);

private:
    typedef TMapNodeTypeHandler TThis;

    virtual void DoDestroy(TMapNode& node);

    virtual void DoBranch(
        const TMapNode& originatingNode,
        TMapNode& branchedNode);

    virtual void DoMerge(
        TMapNode& originatingNode,
        TMapNode& branchedNode);
};

//////////////////////////////////////////////////////////////////////////////// 

class TListNode
    : public TCypressNodeBase
{
    typedef yvector<TNodeId> TIndexToChild;
    typedef yhash_map<TNodeId, int> TChildToIndex;

    DEFINE_BYREF_RW_PROPERTY(TIndexToChild, IndexToChild);
    DEFINE_BYREF_RW_PROPERTY(TChildToIndex, ChildToIndex);

public:
    explicit TListNode(const TVersionedNodeId& id);
    TListNode(const TVersionedNodeId& id, const TListNode& other);

    virtual void Save(TOutputStream* output) const;
    virtual void Load(const NCellMaster::TLoadContext& context, TInputStream* input);

};

//////////////////////////////////////////////////////////////////////////////// 

class TListNodeTypeHandler
    : public TCypressNodeTypeHandlerBase<TListNode>
{
public:
    TListNodeTypeHandler(NCellMaster::TBootstrap* bootstrap);

    virtual EObjectType GetObjectType();
    virtual NYTree::ENodeType GetNodeType();

    virtual TIntrusivePtr<ICypressNodeProxy> GetProxy(const TVersionedNodeId& id);

private:
    typedef TListNodeTypeHandler TThis;

    virtual void DoDestroy(TListNode& node);

    virtual void DoBranch(
        const TListNode& originatingNode,
        TListNode& branchedNode);

    virtual void DoMerge(
        TListNode& originatingNode,
        TListNode& branchedNode);

};

//////////////////////////////////////////////////////////////////////////////// 

} // namespace NCypress
} // namespace NYT
