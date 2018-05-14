#pragma once

#include "public.h"

#include <yt/core/ytree/yson_serializable.h>

namespace NYT {
namespace NCrypto {

////////////////////////////////////////////////////////////////////////////////

//! Either an inlined value or a file reference.
class TPemBlobConfig
    : public NYTree::TYsonSerializable
{
public:
    TNullable<TString> FileName;
    TNullable<TString> Value;

    TPemBlobConfig();

    TString LoadBlob() const;
};

DEFINE_REFCOUNTED_TYPE(TPemBlobConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NCrypto
} // namespace NYT
