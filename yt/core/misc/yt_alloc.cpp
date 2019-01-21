// Support build without YTAlloc

#include <util/system/compiler.h>

#include <cstdlib>

namespace NYT::NYTAlloc {

////////////////////////////////////////////////////////////////////////////////

Y_WEAK void* Allocate(size_t size)
{
    return ::malloc(size);
}

Y_WEAK void Free(void* ptr)
{
    ::free(ptr);
}

Y_WEAK size_t GetAllocationSize(void* /*ptr*/)
{
    return 0;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NYTAlloc

