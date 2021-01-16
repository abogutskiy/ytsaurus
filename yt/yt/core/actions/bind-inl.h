#pragma once
#ifndef BIND_INL_H_
#error "Direct inclusion of this file is not allowed, include bind.h"
// For the sake of sane code completion.
#include "bind.h"
#endif
#undef BIND_INL_H_

#include <yt/core/misc/mpl.h>

#include <yt/core/tracing/trace_context.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

template <class T>
class TUnretainedWrapper
{
public:
    explicit TUnretainedWrapper(T* x)
        : T_(x)
    { }

    T* Get() const
    {
        return T_;
    }

private:
    T* T_;
};

template <class T>
class TOwnedWrapper
{
public:
    explicit TOwnedWrapper(T* x)
        : T_(x)
    { }

    TOwnedWrapper(const TOwnedWrapper& other)
        : T_(other.T_)
    {
        other.T_ = nullptr;
    }

    ~TOwnedWrapper()
    {
        delete T_;
    }

    T* Get() const
    {
        return T_;
    }

private:
    mutable T* T_;
};

template <class T>
class TPassedWrapper
{
public:
    explicit TPassedWrapper(T&& x)
        : IsValid_(true)
        , T_(std::move(x))
    { }

    TPassedWrapper(const TPassedWrapper& other)
        : IsValid_(other.IsValid_)
        , T_(std::move(other.T_))
    {
        other.IsValid_ = false;
    }

    TPassedWrapper(TPassedWrapper&& other)
        : IsValid_(other.IsValid_)
        , T_(std::move(other.T_))
    {
        other.IsValid_ = false;
    }

    T&& Get() const
    {
        YT_ASSERT(IsValid_);
        IsValid_ = false;
        return std::move(T_);
    }

private:
    mutable bool IsValid_;
    mutable T T_;
};

template <class T>
class TConstRefWrapper
{
public:
    explicit TConstRefWrapper(const T& x)
        : T_(&x)
    { }

    const T& Get() const
    {
        return *T_;
    }

private:
    const T* T_;
};

////////////////////////////////////////////////////////////////////////////////

template <class T>
inline const T& Unwrap(T& value)
{
    return value;
}

template <class T>
inline T* Unwrap(const TUnretainedWrapper<T>& wrapper)
{
    return wrapper.Get();
}

template <class T>
inline T* Unwrap(const TOwnedWrapper<T>& wrapper)
{
    return wrapper.Get();
}

template <class T>
inline T&& Unwrap(const TPassedWrapper<T>& wrapper)
{
    return wrapper.Get();
}

template <class T>
inline const T& Unwrap(const TConstRefWrapper<T>& wrapper)
{
    return wrapper.Get();
}

////////////////////////////////////////////////////////////////////////////////

template <class T>
class TIgnoreResultWrapper
{
public:
    explicit TIgnoreResultWrapper(const T& functor)
        : Functor_(functor)
    { }

    T& Get()
    {
        return Functor_;
    }

private:
    T Functor_;
};

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

template <class T>
static auto IgnoreResult(const T& x)
{
    return NYT::NDetail::TIgnoreResultWrapper<T>(x);
}

template <class T>
static auto Unretained(T* x)
{
    return NYT::NDetail::TUnretainedWrapper<T>(x);
}

template <class T>
static auto Owned(T* x)
{
    return NYT::NDetail::TOwnedWrapper<T>(x);
}

template <class T>
static auto Passed(T&& x)
{
    return NYT::NDetail::TPassedWrapper<T>(std::forward<T>(x));
}

template <class T>
static auto ConstRef(const T& x)
{
    return NYT::NDetail::TConstRefWrapper<T>(x);
}

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

template <class F>
struct TFunctorTraits;

template <class TMethod>
class TMethodInvoker
{
public:
    explicit TMethodInvoker(TMethod method)
        : Method_(method)
    { }

    template <class D, class... XAs>
    auto operator()(D* object, XAs&&... args) const
    {
        static_assert(
            !NMpl::TIsArray<D>::Value,
            "First bound argument to a method cannot be an array");

        return (object->*Method_)(std::forward<XAs>(args)...);
    }

    template <class D, class... XAs>
    void operator()(const TWeakPtr<D>& ref, XAs&&... args) const
    {
        using TResult = typename TFunctorTraits<TMethod>::TResult;
        static_assert(
            NMpl::TIsVoid<TResult>::Value,
            "Weak calls are only supported for methods with a void return type");

        auto strongRef = ref.Lock();
        auto* object = strongRef.Get();

        if (!object) {
            return;
        }

        (object->*Method_)(std::forward<XAs>(args)...);
    }

    template <class D, class... XAs>
    auto operator()(const TIntrusivePtr<D>& ref, XAs&&... args) const
    {
        auto* object = ref.Get();
        return (object->*Method_)(std::forward<XAs>(args)...);
    }

private:
    const TMethod Method_;
};

////////////////////////////////////////////////////////////////////////////////

template <class T>
struct TToVoidSignature;

template <class TR, class... TAs>
struct TToVoidSignature<TR(TAs...)>
{
    using TType = void (TAs...);
};

////////////////////////////////////////////////////////////////////////////////

template <class>
struct TCallableSignature;

template <class TR, class TC, class... TAs>
struct TCallableSignature<TR (TC::*)(TAs...) const>
{
    using TSignature = TR (TAs...);
};

template <class TR, class TC, class... TAs>
struct TCallableSignature<TR (TC::*)(TAs...)>
{
    using TSignature = TR (TAs...);
};

////////////////////////////////////////////////////////////////////////////////

// Matches functor and TCallback
template <class F>
struct TFunctorTraits
    : public TCallableSignature<decltype(&F::operator())>
{
    using TInvoker = F;
};

template <class TR, class... TAs>
struct TFunctorTraits<TR (TAs...)>
{
    using TInvoker = TR (*)(TAs...);
    using TSignature = TR (TAs...);
};

// Matches function.
template <class TR, class... TAs>
struct TFunctorTraits<TR (*)(TAs...)>
{
    using TInvoker = TR (*)(TAs...);
    using TSignature = TR (TAs...);
};

// Matches method.
template <class TR, class C, class... TAs>
struct TFunctorTraits<TR (C::*)(TAs...)>
{
    using TInvoker = TMethodInvoker<TR (C::*)(TAs...)>;
    using TSignature = TR (C*, TAs...);
    using TResult = TR;
};

template <class TR, class C, class... TAs>
struct TFunctorTraits<TR (C::*)(TAs...) const>
{
    using TInvoker = TMethodInvoker<TR (C::*)(TAs...) const>;
    using TSignature = TR (const C*, TAs...);
    using TResult = TR;
};

template <class TR, class C, class... TAs>
struct TFunctorTraits<TR (C::*)(TAs...) noexcept>
{
    using TInvoker = TMethodInvoker<TR (C::*)(TAs...) noexcept>;
    using TSignature = TR (C*, TAs...);
    using TResult = TR;
};

template <class TR, class C, class... TAs>
struct TFunctorTraits<TR (C::*)(TAs...) const noexcept>
{
    using TInvoker = TMethodInvoker<TR (C::*)(TAs...) const noexcept>;
    using TSignature = TR (const C*, TAs...);
    using TResult = TR;
};

////////////////////////////////////////////////////////////////////////////////

template <class T>
class TOmitResultInvoker
{
public:
    using TInvoker = typename TFunctorTraits<T>::TInvoker;

    explicit TOmitResultInvoker(NDetail::TIgnoreResultWrapper<T>&& invoker)
        : Invoker_(std::move(invoker.Get()))
    { }

    template <class... XAs>
    void operator()(XAs&&... args) const
    {
        Invoker_(std::forward<XAs>(args)...);
    }

private:
    TInvoker Invoker_;
};

template <class T>
struct TFunctorTraits<NDetail::TIgnoreResultWrapper<T>>
{
    using TInvoker = TOmitResultInvoker<T>;
    using TSignature = typename TToVoidSignature<typename TFunctorTraits<T>::TSignature>::TType;
};

////////////////////////////////////////////////////////////////////////////////

template <class T>
struct TIsNonConstReference
    : public NMpl::TFalseType
{ };

template <class T>
struct TIsNonConstReference<T&>
    : public NMpl::TTrueType
{ };

template <class T>
struct TIsNonConstReference<const T&>
    : public NMpl::TFalseType
{ };

template <class T>
struct TCheckNoRawPtrToRefCountedType
{
    enum {
        Value = (NMpl::TIsPointer<T>::Value && (
            NMpl::TIsConvertible<T, const TRefCounted*>::Value ||
            NMpl::TIsConvertible<T, TRefCounted*>::Value
        ))
    };

    static_assert(
        !Value,
        "T has reference-counted type and should not be bound by the raw pointer");

    using TType = T;
};

template <class... TArgs>
struct TCheckParamsIsRawPtrToRefCountedType
    : public NYT::NMpl::TTypesPack<typename TCheckNoRawPtrToRefCountedType<TArgs>::TType...>
{ };

////////////////////////////////////////////////////////////////////////////////

template <unsigned N, class TSignature>
struct TSplitHelper;

template <unsigned N, class TSignature>
struct TSplit
    : public TSplitHelper<N, TSignature>
{ };

template <class TSignature>
struct TSplit<0, TSignature>
{
    using TResult = TSignature;
};

template <unsigned N, class TR, class TA0, class... TAs>
struct TSplitHelper<N, TR (TA0, TAs...)>
    : public TSplit<N - 1, TR (TAs...)>
{
    static_assert(
        !TIsNonConstReference<TA0>::Value,
        "T is a non-const reference and should not be bound.");
};

////////////////////////////////////////////////////////////////////////////////

template <bool CaptureTraceContext>
class TTraceContextMixin
{
public:
    TTraceContextMixin()
        : TraceContext_(NTracing::GetCurrentTraceContext())
    { }

    NTracing::TCurrentTraceContextGuard GetTraceContextGuard()
    {
        return NTracing::TCurrentTraceContextGuard(TraceContext_);
    }

private:
    const NTracing::TTraceContextPtr TraceContext_;
};

template <>
class TTraceContextMixin<false>
{
public:
    struct TDummyGuard { };

    TDummyGuard GetTraceContextGuard()
    {
        return TDummyGuard();
    }
};

////////////////////////////////////////////////////////////////////////////////

template <bool CaptureTraceContext, class TFunctor, class TSequence, class... TBs>
class TBindState;

template <bool CaptureTraceContext, class TFunctor, class... TBs, unsigned... BoundIndexes>
class TBindState<CaptureTraceContext, TFunctor, NMpl::TSequence<BoundIndexes...>, TBs...>
    : public NDetail::TBindStateBase
    , public TTraceContextMixin<CaptureTraceContext>
{
public:
    template <class XFunctor, class... XBs>
    TBindState(
#ifdef YT_ENABLE_BIND_LOCATION_TRACKING
        const NYT::TSourceLocation& location,
#endif
        XFunctor&& functor,
        XBs&&... boundArgs)
        : TBindStateBase(
#ifdef YT_ENABLE_BIND_LOCATION_TRACKING
            location
#endif
        )
        , Functor(std::forward<XFunctor>(functor))
        , BoundArgs(std::forward<XBs>(boundArgs)...)
    { }

    // Keep minimum frame count.
    template <class... TAs>
    static auto Run(NDetail::TBindStateBase* base, TAs&&... args)
    {
        auto* state = static_cast<TBindState*>(base);

        auto guard = state->GetTraceContextGuard();
        Y_UNUSED(guard);

        return state->Functor(
            NDetail::Unwrap(std::get<BoundIndexes>(state->BoundArgs))...,
            std::forward<TAs>(args)...);
    }

private:
    TFunctor Functor;
    const std::tuple<TBs...> BoundArgs;
};

////////////////////////////////////////////////////////////////////////////////

template <class TSignature>
struct TBindHelper;

template <class TR, class... TAs>
struct TBindHelper<TR(TAs...)>
{
    template <class TState>
    static constexpr auto GetInvokeFunction()
    {
        return &TState::template Run<TAs...>;
    }
};

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

template <
    bool CaptureTraceContext,
#ifdef YT_ENABLE_BIND_LOCATION_TRACKING
    class TTag,
    int Counter,
#endif
    class TFunctor,
    class... TBs>
auto Bind(
#ifdef YT_ENABLE_BIND_LOCATION_TRACKING
    const TSourceLocation& location,
#endif
    TFunctor&& functor,
    TBs&&... bound)
{
#ifdef YT_ENABLE_BIND_LOCATION_TRACKING
    Y_UNUSED(location);
#endif

    using TTraits = NDetail::TFunctorTraits<typename NMpl::TDecay<TFunctor>::TType>;
    using TRunSignature = typename NDetail::TSplit<sizeof...(TBs), typename TTraits::TSignature>::TResult;

    NYT::NDetail::TCheckParamsIsRawPtrToRefCountedType<typename NMpl::TDecay<TBs>::TType...> checkParamsIsRawPtrToRefCountedType;
    Y_UNUSED(checkParamsIsRawPtrToRefCountedType);

    using TState = NYT::NDetail::TBindState<
        CaptureTraceContext,
        typename TTraits::TInvoker,
        typename NMpl::TGenerateSequence<sizeof...(TBs)>::TType,
        typename NMpl::TDecay<TBs>::TType...>;

    using THelper = NYT::NDetail::TBindHelper<TRunSignature>;

    return TExtendedCallback<TRunSignature>{
#ifdef YT_ENABLE_BIND_LOCATION_TRACKING
        NewWithLocation<TState, TTag, Counter>(location, location, std::forward<TFunctor>(functor), std::forward<TBs>(bound)...),
#else
        New<TState>(std::forward<TFunctor>(functor), std::forward<TBs>(bound)...),
#endif
        THelper::template GetInvokeFunction<TState>()};
}

template <
    bool CaptureTraceContext,
#ifdef YT_ENABLE_BIND_LOCATION_TRACKING
    class TTag,
    int Counter,
#endif
    class T
>
auto Bind(
#ifdef YT_ENABLE_BIND_LOCATION_TRACKING
    const TSourceLocation& location,
#endif
    const TCallback<T>& callback)
{
#ifdef YT_ENABLE_BIND_LOCATION_TRACKING
    Y_UNUSED(location);
#endif
    return TExtendedCallback<T>(callback);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
