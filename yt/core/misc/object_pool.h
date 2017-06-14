#pragma once

#include "common.h"

#include <yt/core/profiling/public.h>

#include <util/generic/singleton.h>

#include <util/thread/lfstack.h>

#include <atomic>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

//! A pool for reusable objects.
/*
 * Instances are tracked via shared pointers with a special deleter
 * that returns spare instances back to the pool.
 *
 * Both the pool and the references are thread-safe.
 *
 */
template <class TObject>
class TObjectPool
{
public:
    using TObjectPtr = std::shared_ptr<TObject>;

    ~TObjectPool();

    //! Either creates a fresh instance or returns a pooled one.
    TObjectPtr Allocate();

    //! Calls #TPooledObjectTraits::Clean and returns the instance back into the pool.
    void Reclaim(TObject* obj);

private:
    TLockFreeStack<TObject*> PooledObjects_;
    std::atomic<int> PoolSize_ = {0};

    TObject* AllocateInstance();
    void FreeInstance(TObject* obj);

    Y_DECLARE_SINGLETON_FRIEND();
};

template <class TObject>
TObjectPool<TObject>& ObjectPool();

////////////////////////////////////////////////////////////////////////////////

//! Provides various traits for pooled objects of type |T|.
/*!
 * |Clean| method is called before an object is put into the pool.
 *
 * |GetMaxPoolSize| method is called to determine the maximum number of
 * objects allowed to be pooled.
 */
template <class TObject, class = void>
struct TPooledObjectTraits
{ };

//! Basic version of traits. Others may consider inheriting from it.
struct TPooledObjectTraitsBase
{
    template <class TObject>
    static void Clean(TObject*)
    { }

    static int GetMaxPoolSize()
    {
        return 256;
    }
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define OBJECT_POOL_INL_H_
#include "object_pool-inl.h"
#undef OBJECT_POOL_INL_H_
