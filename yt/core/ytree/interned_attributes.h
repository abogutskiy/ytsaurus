#pragma once

#include "public.h"

namespace NYT::NYTree {

///////////////////////////////////////////////////////////////////////////////

class TInternedAttributeKey
{
public:
    TInternedAttributeKey();

    explicit constexpr TInternedAttributeKey(size_t code)
        : Code_(code)
    { }

    constexpr operator size_t() const
    {
        return Code_;
    }

    // May return #InvalidInternedAttribute if the attribute is not interned.
    static TInternedAttributeKey Lookup(TStringBuf uninternedKey);

    const TString& Unintern() const;

    void Save(TStreamSaveContext& context) const;
    void Load(TStreamLoadContext& context);

private:
    // NB: this codes are subject to change! Do not rely on their values. Do not serialize them.
    // Use Save/Load methods instead.
    size_t Code_;
};

constexpr TInternedAttributeKey InvalidInternedAttribute{0};
constexpr TInternedAttributeKey CountInternedAttribute{1};

//! Interned attribute registry initialization. Should be called once per attribute.
//! Both interned and uninterned keys must be unique.
void InternAttribute(const TString& uninternedKey, TInternedAttributeKey internedKey);

////////////////////////////////////////////////////////////////////////////////

#ifdef __GNUC__
    // Prevent the linker from throwing out static initializers.
    #define REGISTER_INTERNED_ATTRIBUTE_ATTRIBUTES __attribute__((used))
#else
    #define REGISTER_INTERNED_ATTRIBUTE_ATTRIBUTES
#endif

#define REGISTER_INTERNED_ATTRIBUTE(uninternedKey, internedKey) \
    REGISTER_INTERNED_ATTRIBUTE_ATTRIBUTES const void* InternedAttribute_##uninternedKey = [] () -> void* { \
            ::NYT::NYTree::InternAttribute(#uninternedKey, internedKey); \
            return nullptr; \
        } ();

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NYTree
