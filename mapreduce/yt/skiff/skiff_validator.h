#pragma once

#include "public.h"

#include "skiff_schema.h"

#include <util/generic/ptr.h>

namespace NSkiff {

////////////////////////////////////////////////////////////////////////////////

class TValidatorNodeStack;

////////////////////////////////////////////////////////////////////////////////

class TSkiffValidator
{
public:
    explicit TSkiffValidator(TSkiffSchemaPtr skiffSchema);
    ~TSkiffValidator();

    void BeforeVariant8Tag();
    void OnVariant8Tag(ui8 tag);

    void BeforeVariant16Tag();
    void OnVariant16Tag(ui16 tag);

    void OnSimpleType(EWireType value);

    void ValidateFinished();

private:
    const THolder<TValidatorNodeStack> Context_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NSkiff
