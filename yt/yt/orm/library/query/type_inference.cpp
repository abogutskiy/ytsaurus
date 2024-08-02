#include "type_inference.h"

#include <yt/yt/library/query/base/public.h>
#include <yt/yt/library/query/base/functions.h>

namespace NYT::NOrm::NQuery {

////////////////////////////////////////////////////////////////////////////////

std::optional<NTableClient::EValueType> TryInferFunctionReturnType(const TString& functionName)
{
    auto inferrers = NQueryClient::GetBuiltinTypeInferrers();
    auto functionIterator = inferrers->find(functionName);
    if (functionIterator == inferrers->end()) {
        return std::nullopt;
    }
    auto function = functionIterator->second->As<NQueryClient::TFunctionTypeInferrer>();
    if (!function) {
        return std::nullopt;
    }

    std::vector<NQueryClient::TTypeSet> typeConstraints;
    std::vector<int> formalArguments;
    std::optional<std::pair<int, bool>> repeatedType;

    int index = function->GetNormalizedConstraints(&typeConstraints, &formalArguments, &repeatedType);
    auto returnTypes = typeConstraints[index];

    if (returnTypes.GetSize() != 1) {
        return std::nullopt;
    }

    return returnTypes.GetFront();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NOrm::NQuery
