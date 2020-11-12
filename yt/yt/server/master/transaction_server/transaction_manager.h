#pragma once

#include "public.h"

#include <yt/server/master/cell_master/public.h>

#include <yt/server/master/cypress_server/public.h>

#include <yt/server/lib/hive/transaction_manager.h>

#include <yt/server/lib/hydra/entity_map.h>

#include <yt/server/master/object_server/public.h>

#include <yt/client/election/public.h>

#include <yt/core/actions/signal.h>

#include <yt/core/misc/property.h>

namespace NYT::NTransactionServer {

////////////////////////////////////////////////////////////////////////////////

class TTransactionManager
    : public NHiveServer::ITransactionManager
{
public:
    //! Raised when a new transaction is started.
    DECLARE_SIGNAL(void(TTransaction*), TransactionStarted);

    //! Raised when a transaction is committed.
    DECLARE_SIGNAL(void(TTransaction*), TransactionCommitted);

    //! Raised when a transaction is aborted.
    DECLARE_SIGNAL(void(TTransaction*), TransactionAborted);

    //! A set of native transactions with no parent.
    DECLARE_BYREF_RO_PROPERTY(THashSet<TTransaction*>, NativeTopmostTransactions);

    DECLARE_BYREF_RO_PROPERTY(THashSet<TTransaction*>, NativeTransactions);

public:
    explicit TTransactionManager(NCellMaster::TBootstrap* bootstrap);

    ~TTransactionManager();

    void Initialize();

    TTransaction* StartTransaction(
        TTransaction* parent,
        std::vector<TTransaction*> prerequisiteTransactions,
        const NObjectClient::TCellTagList& replicatedToCellTags,
        std::optional<TDuration> timeout,
        std::optional<TInstant> deadline,
        const std::optional<TString>& title,
        const NYTree::IAttributeDictionary& attributes);
    TTransaction* StartUploadTransaction(
        TTransaction* parent,
        const NObjectClient::TCellTagList& replicatedToCellTags,
        std::optional<TDuration> timeout,
        const std::optional<TString>& title,
        TTransactionId hintId);
    void CommitTransaction(
        TTransaction* transaction,
        TTimestamp commitTimestamp);
    void AbortTransaction(
        TTransaction* transaction,
        bool force);
    TTransactionId ExternalizeTransaction(
        TTransaction* transaction,
        NObjectClient::TCellTag dstCellTag);
    TTransactionId GetNearestExternalizedTransactionAncestor(
        TTransaction* transaction,
        NObjectClient::TCellTag dstCellTag);

    // COMPAT(shakurov). Hide this method to the impl and remove #cachePresence
    // argument once YT-12559 is resolved.
    void FinishTransaction(TTransaction* transaction, bool cachePresence);

    DECLARE_ENTITY_MAP_ACCESSORS(Transaction, TTransaction);

    //! Finds transaction by id, throws if nothing is found.
    TTransaction* GetTransactionOrThrow(TTransactionId transactionId);

    //! Asynchronously returns the (approximate) moment when transaction with
    //! a given #transactionId was last pinged.
    TFuture<TInstant> GetLastPingTime(const TTransaction* transaction);

    //! Sets the transaction timeout. Current lease is not renewed.
    void SetTransactionTimeout(
        TTransaction* transaction,
        TDuration timeout);

    //! Registers and references the object with the transaction.
    //! The same object can only be staged once.
    void StageObject(
        TTransaction* transaction,
        NObjectServer::TObject* object);

    //! Unregisters the object from its staging transaction (which must be equal to #transaction),
    //! calls IObjectTypeHandler::UnstageObject and
    //! unreferences the object. Throws on failure.
    /*!
     *  If #recursive is |true| then all child objects are also released.
     */
    void UnstageObject(
        TTransaction* transaction,
        NObjectServer::TObject* object,
        bool recursive);

    //! Registers (and references) the node with the transaction.
    void StageNode(
        TTransaction* transaction,
        NCypressServer::TCypressNode* trunkNode);

    //! Registers and references the object with the transaction.
    //! The reference is dropped if the transaction aborts
    //! but is preserved if the transaction commits.
    //! The same object as be exported more than once.
    void ExportObject(
        TTransaction* transaction,
        NObjectServer::TObject* object,
        NObjectClient::TCellTag destinationCellTag);

    //! Registers and references the object with the transaction.
    //! The reference is dropped if the transaction aborts or aborts.
    //! The same object as be exported more than once.
    void ImportObject(
        TTransaction* transaction,
        NObjectServer::TObject* object);

    void RegisterTransactionActionHandlers(
        const NHiveServer::TTransactionPrepareActionHandlerDescriptor<TTransaction>& prepareActionDescriptor,
        const NHiveServer::TTransactionCommitActionHandlerDescriptor<TTransaction>& commitActionDescriptor,
        const NHiveServer::TTransactionAbortActionHandlerDescriptor<TTransaction>& abortActionDescriptor);

    using TCtxStartTransaction = NRpc::TTypedServiceContext<
        NTransactionClient::NProto::TReqStartTransaction,
        NTransactionClient::NProto::TRspStartTransaction>;
    using TCtxStartTransactionPtr = TIntrusivePtr<TCtxStartTransaction>;
    std::unique_ptr<NHydra::TMutation> CreateStartTransactionMutation(
        TCtxStartTransactionPtr context,
        const NTransactionServer::NProto::TReqStartTransaction& request);

    using TCtxRegisterTransactionActions = NRpc::TTypedServiceContext<
        NProto::TReqRegisterTransactionActions,
        NProto::TRspRegisterTransactionActions>;
    using TCtxRegisterTransactionActionsPtr = TIntrusivePtr<TCtxRegisterTransactionActions>;
    std::unique_ptr<NHydra::TMutation> CreateRegisterTransactionActionsMutation(
        TCtxRegisterTransactionActionsPtr context);

    using TCtxReplicateTransactions = NRpc::TTypedServiceContext<
        NProto::TReqReplicateTransactions,
        NProto::TRspReplicateTransactions>;
    using TCtxReplicateTransactionsPtr = TIntrusivePtr<TCtxReplicateTransactions>;
    std::unique_ptr<NHydra::TMutation> CreateReplicateTransactionsMutation(
        TCtxReplicateTransactionsPtr context);

    void CreateOrRefTimestampHolder(TTransactionId transactionId);
    void SetTimestampHolderTimestamp(TTransactionId transactionId, TTimestamp timestamp);
    TTimestamp GetTimestampHolderTimestamp(TTransactionId transactionId);
    void UnrefTimestampHolder(TTransactionId transactionId);

    const TTransactionPresenceCachePtr& GetTransactionPresenceCache();

private:
    class TImpl;
    class TTransactionTypeHandler;

    const TIntrusivePtr<TImpl> Impl_;

    // ITransactionManager overrides
    virtual TFuture<void> GetReadyToPrepareTransactionCommit(
        const std::vector<TTransactionId>& prerequisiteTransactionIds,
        const std::vector<NElection::TCellId>& cellIdsToSyncWith) override;
    virtual void PrepareTransactionCommit(
        TTransactionId transactionId,
        bool persistent,
        TTimestamp prepareTimestamp,
        const std::vector<TTransactionId>& prerequisiteTransactionIds) override;
    virtual void PrepareTransactionAbort(
        TTransactionId transactionId,
        bool force) override;
    virtual void CommitTransaction(
        TTransactionId transactionId,
        TTimestamp commitTimestamp) override;
    virtual void AbortTransaction(
        TTransactionId transactionId,
        bool force) override;
    virtual void PingTransaction(
        TTransactionId transactionId,
        bool pingAncestors) override;
};

DEFINE_REFCOUNTED_TYPE(TTransactionManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTransactionServer
