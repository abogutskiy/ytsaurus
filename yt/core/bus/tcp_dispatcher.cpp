#include "tcp_dispatcher.h"
#include "tcp_dispatcher_impl.h"

#include <yt/core/misc/singleton.h>
#include <yt/core/misc/shutdown.h>

#include <yt/core/profiling/profile_manager.h>

namespace NYT {
namespace NBus {

////////////////////////////////////////////////////////////////////////////////

TTcpDispatcher::TTcpDispatcher()
    : Impl_(New<TImpl>())
{ }

TTcpDispatcher::~TTcpDispatcher() = default;

TTcpDispatcher* TTcpDispatcher::Get()
{
    return Singleton<TTcpDispatcher>();
}

void TTcpDispatcher::StaticShutdown()
{
    Get()->Shutdown();
}

void TTcpDispatcher::Shutdown()
{
    Impl_->Shutdown();
}

TTcpDispatcherStatistics TTcpDispatcher::GetStatistics(ETcpInterfaceType interfaceType)
{
    return Impl_->GetStatistics(interfaceType);
}

////////////////////////////////////////////////////////////////////////////////

REGISTER_SHUTDOWN_CALLBACK(6, TTcpDispatcher::StaticShutdown);

////////////////////////////////////////////////////////////////////////////////

} // namespace NBus
} // namespace NYT
