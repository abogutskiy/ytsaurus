#include "stdafx.h"

#include "ref_counted_base.h"
#include "ref_counted_tracker.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

TExtrinsicRefCounted::~TExtrinsicRefCounted()
{
    YASSERT(RefCounter->GetRefCount() == 0);
#ifdef ENABLE_REF_COUNTED_TRACKING
    YASSERT(Cookie);
    TRefCountedTracker::Get()->Unregister(
        static_cast<TRefCountedTracker::TCookie>(Cookie));
#endif
}

TIntrinsicRefCounted::~TIntrinsicRefCounted()
{
    YASSERT(NDetail::AtomicallyFetch(&RefCounter) == 0);
#ifdef ENABLE_REF_COUNTED_TRACKING
    YASSERT(Cookie);
    TRefCountedTracker::Get()->Unregister(
        static_cast<TRefCountedTracker::TCookie>(Cookie));
#endif
}

#ifdef ENABLE_REF_COUNTED_TRACKING
void TExtrinsicRefCounted::BindToCookie(void* cookie)
{
    YASSERT(RefCounter->GetRefCount() > 0);
    YASSERT(!Cookie);
    Cookie = cookie;

    TRefCountedTracker::Get()->Register(
        static_cast<TRefCountedTracker::TCookie>(Cookie));
}

void TIntrinsicRefCounted::BindToCookie(void* cookie)
{
    YASSERT(NDetail::AtomicallyFetch(&RefCounter) > 0);
    YASSERT(!Cookie);
    Cookie = cookie;

    TRefCountedTracker::Get()->Register(
        static_cast<TRefCountedTracker::TCookie>(Cookie));
}
#endif

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT