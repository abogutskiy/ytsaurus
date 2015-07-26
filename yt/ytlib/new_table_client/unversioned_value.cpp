#include "unversioned_value.h"

#include <core/misc/farm_hash.h>

namespace NYT {
namespace NVersionedTableClient {

////////////////////////////////////////////////////////////////////////////////

ui64 GetHash(const TUnversionedValue& value)
{
    // NB: hash function may change in future. Use fingerprints for persistent hashing.
    return GetFarmFingerprint(value);
}

// Forever-fixed Google FarmHash fingerprint.
TFingerprint GetFarmFingerprint(const TUnversionedValue& value)
{
    switch (value.Type) {
        case EValueType::String:
            return FarmFingerprint(value.Data.String, value.Length);

        case EValueType::Int64:
        case EValueType::Uint64:
        case EValueType::Double:
            // These types are aliased.
            return FarmFingerprint(value.Data.Int64);

        case EValueType::Boolean:
            return FarmFingerprint(value.Data.Boolean);

        case EValueType::Null:
            return FarmFingerprint(0);

        default:
            // No idea how to hash other types.
            YUNREACHABLE();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NVersionedTableClient
} // namespace NYT
