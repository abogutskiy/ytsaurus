#pragma once

#include "operation_controller.h"

#include <yt/server/scheduler/public.h>

#include <yt/ytlib/cypress_client/public.h>

#include <yt/ytlib/chunk_client/public.h>

#include <yt/core/misc/ref.h>

#include <yt/core/actions/signal.h>

namespace NYT::NControllerAgent {

////////////////////////////////////////////////////////////////////////////////

//! Mediates communication between controller agent and master.
/*!
 *  \note Thread affinity: control unless noted otherwise
 */
class TMasterConnector
{
public:
    TMasterConnector(
        TControllerAgentConfigPtr config,
        TBootstrap* bootstrap);
    ~TMasterConnector();

    void Initialize();

    void RegisterOperation(const TOperationId& operationId);
    void UnregisterOperation(const TOperationId& operationId);

    void CreateJobNode(
        const TOperationId& operationId,
        const TCreateJobNodeRequest& request);

    TFuture<void> FlushOperationNode(const TOperationId& operationId);

    TFuture<void> UpdateInitializedOperationNode(const TOperationId& operationId);

    TFuture<void> AttachToLivePreview(
        const TOperationId& operationId,
        NObjectClient::TTransactionId transactionId,
        NCypressClient::TNodeId tableId,
        const std::vector<NChunkClient::TChunkTreeId>& childIds);

    TFuture<TOperationSnapshot> DownloadSnapshot(const TOperationId& operationId);
    TFuture<void> RemoveSnapshot(const TOperationId& operationId);

    void AddChunkTreesToUnstageList(
        std::vector<NChunkClient::TChunkTreeId> chunkTreeIds,
        bool recursive);

    TFuture<void> UpdateConfig();

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent

