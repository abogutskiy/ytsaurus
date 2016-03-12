#pragma once

#include "common.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

//! A simple guard that executes a given function at the end of scope.

template <class TCallback>
class TFinallyGuard
{
public:
    template <class T>
    explicit TFinallyGuard(T&& finally)
        : Finally_(std::forward<T>(finally))
    { }

    void Release()
    {
        Released_ = true;
    }

    ~TFinallyGuard()
    {
        if (!Released_) {
            Finally_();
        }
    }

private:
    bool Released_ = false;
    TCallback Finally_;

};

template <class TCallback>
TFinallyGuard<typename std::decay<TCallback>::type> Finally(TCallback&& callback)
{
    return TFinallyGuard<typename std::decay<TCallback>::type>(std::forward<TCallback>(callback));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
