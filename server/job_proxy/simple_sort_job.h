#pragma once

#include "public.h"

namespace NYT {
namespace NJobProxy {

////////////////////////////////////////////////////////////////////////////////

IJobPtr CreateSimpleSortJob(IJobHostPtr host);

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
