#include "chaos_cell_proxy.h"
#include "private.h"
#include "chaos_cell.h"

#include <yt/yt/server/master/cell_master/bootstrap.h>

#include <yt/yt/server/master/cell_server/cell_bundle.h>
#include <yt/yt/server/master/cell_server/cell_proxy_base.h>

#include <yt/yt/server/lib/misc/interned_attributes.h>

#include <yt/yt/server/master/object_server/object_detail.h>

#include <yt/yt/core/ytree/convert.h>

#include <yt/yt/core/misc/protobuf_helpers.h>

namespace NYT::NChaosServer {

using namespace NObjectServer;
using namespace NCellMaster;
using namespace NCellServer;

////////////////////////////////////////////////////////////////////////////////

class TChaosCellProxy
    : public TCellProxyBase
{
public:
    using TCellProxyBase::TCellProxyBase;
};

////////////////////////////////////////////////////////////////////////////////

IObjectProxyPtr CreateChaosCellProxy(
    TBootstrap* bootstrap,
    TObjectTypeMetadata* metadata,
    TChaosCell* cell)
{
    return New<TChaosCellProxy>(bootstrap, metadata, cell);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChaosServer
