#pragma once

#include "common.h"

#include "../misc/configurable.h"
#include "../cypress/node.h"
#include "../cypress/cypress_manager.h"

namespace NYT {
namespace NOrchid {

////////////////////////////////////////////////////////////////////////////////

struct TOrchidManifest
    : public TConfigurable
{
    typedef TIntrusivePtr<TOrchidManifest> TPtr;

    Stroka RemoteAddress;
    Stroka RemoteRoot;
    TDuration Timeout;

    TOrchidManifest()
    {
        Register("remote_address", RemoteAddress);
        Register("remote_root", RemoteRoot).Default("/");
        Register("timeout", Timeout).Default(TDuration::MilliSeconds(3000));
    }
};

////////////////////////////////////////////////////////////////////////////////

NCypress::INodeTypeHandler::TPtr CreateOrchidTypeHandler(
    NCypress::TCypressManager* cypressManager);

////////////////////////////////////////////////////////////////////////////////

} // namespace NOrchid
} // namespace NYT
