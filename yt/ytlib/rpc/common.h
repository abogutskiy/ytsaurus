#pragma once

#include "error.h"

#include "../misc/guid.h"

#include "../logging/log.h"

namespace NYT {
namespace NRpc {

////////////////////////////////////////////////////////////////////////////////

extern NLog::TLogger RpcLogger;

////////////////////////////////////////////////////////////////////////////////

typedef TGuid TRequestId;

////////////////////////////////////////////////////////////////////////////////

class TRpcManager
    : private TNonCopyable
{
public:
    TRpcManager();

    static TRpcManager* Get();
    Stroka GetDebugInfo();
    void Shutdown();
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT
