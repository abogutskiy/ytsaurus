#include "stdafx.h"
#include "cell_master_server.h"

#include <yt/ytlib/chunk_manager/chunk_manager.h>

#include <yt/ytlib/meta_state/composite_meta_state.h>

#include <yt/ytlib/transaction_manager/transaction_manager.h>
#include <yt/ytlib/transaction_manager/transaction_service.h>

#include <yt/ytlib/cypress/cypress_manager.h>
#include <yt/ytlib/cypress/cypress_service.h>

#include <yt/ytlib/file_server/file_type_handler.h>

#include <yt/ytlib/monitoring/monitoring_manager.h>

namespace NYT {

static NLog::TLogger Logger("CellMasterSever");

using NTransaction::TTransactionManager;
using NTransaction::TTransactionService;

using NChunkManager::TChunkManagerConfig;
using NChunkManager::TChunkManager;

using NMetaState::TCompositeMetaState;

using NCypress::TCypressManager;
using NCypress::TCypressService;

using NFileServer::TFileTypeHandler;

using NMonitoring::TMonitoringManager;

////////////////////////////////////////////////////////////////////////////////

void TCellMasterServer::TConfig::Read(TJsonObject* json)
{
    TJsonObject* cellJson = GetSubTree(json, "Cell");
    if (cellJson != NULL) {
        MetaState.Cell.Read(cellJson);
    }

    TJsonObject* metaStateJson = GetSubTree(json, "MetaState");
    if (metaStateJson != NULL) {
        MetaState.Read(metaStateJson);
    }
}

////////////////////////////////////////////////////////////////////////////////

TCellMasterServer::TCellMasterServer(const TConfig& config)
    : Config(config)
{ }

void TCellMasterServer::Run()
{
    // TODO: extract method
    Stroka address = Config.MetaState.Cell.Addresses.at(Config.MetaState.Cell.Id);
    size_t index = address.find_last_of(":");
    int port = FromString<int>(address.substr(index + 1));

    LOG_INFO("Starting cell master on port %d", port);

    auto metaState = New<TCompositeMetaState>();

    auto controlQueue = New<TActionQueue>();

    auto server = New<NRpc::TServer>(port);

    auto metaStateManager = New<TMetaStateManager>(
        Config.MetaState,
        controlQueue->GetInvoker(),
        ~metaState,
        server);

    auto transactionManager = New<TTransactionManager>(
        TTransactionManager::TConfig(),
        metaStateManager,
        metaState);

    auto transactionService = New<TTransactionService>(
        transactionManager,
        metaStateManager->GetStateInvoker(),
        server);

    auto chunkManager = New<TChunkManager>(
        TChunkManagerConfig(),
        metaStateManager,
        metaState,
        server,
        transactionManager);

    auto cypressManager = New<TCypressManager>(
        metaStateManager,
        metaState,
        transactionManager);

    auto cypressService = New<TCypressService>(
        cypressManager,
        transactionManager,
        metaStateManager->GetStateInvoker(),
        server);

    auto fileTypeHandler = New<TFileTypeHandler>();
    cypressManager->RegisterDynamicType(~fileTypeHandler);

    auto monitoringManager = New<TMonitoringManager>();
    monitoringManager->Register(
        "/refcounted",
        FromMethod(&TRefCountedTracker::GetMonitoringInfo));
    monitoringManager->Register(
        "/meta_state",
        FromMethod(&TMetaStateManager::GetMonitoringInfo, metaStateManager));

    // TODO: register more monitoring infos

    monitoringManager->Start();

    MonitoringServer = new THttpTreeServer(
        monitoringManager->GetProducer(),
        Config.MonitoringPort);

    MonitoringServer->Start();
    metaStateManager->Start();
    server->Start();

    Sleep(TDuration::Max());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
