#pragma once

#include <core/misc/common.h>

namespace NYT {
namespace NQueryAgent {

////////////////////////////////////////////////////////////////////////////////

class TQueryExecutor;
typedef TIntrusivePtr<TQueryExecutor> TQueryManagerPtr;

class TQueryAgentConfig;
typedef TIntrusivePtr<TQueryAgentConfig> TQueryAgentConfigPtr;

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryAgent
} // namespace NYT
