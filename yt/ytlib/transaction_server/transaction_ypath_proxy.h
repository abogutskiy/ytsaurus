#pragma once

#include "transaction_ypath.pb.h"

#include <yt/ytlib/ytree/ypath_proxy.h>

namespace NYT {
namespace NTransactionServer {

////////////////////////////////////////////////////////////////////////////////

struct TTransactionYPathProxy
    : NYTree::TYPathProxy
{
    DEFINE_YPATH_PROXY_METHOD(NProto, Commit);
    DEFINE_YPATH_PROXY_METHOD(NProto, Abort);
    DEFINE_YPATH_PROXY_METHOD(NProto, RenewLease);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTransactionServer
} // namespace NYT
