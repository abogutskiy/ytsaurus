#pragma once

#include "private.h"
#include "output_order.h"

#include <yt/client/table_client/unversioned_row.h>

namespace NYT::NChunkPools {

////////////////////////////////////////////////////////////////////////////////

struct TBoundaryKeys
{
    NTableClient::TLegacyKey MinKey;
    NTableClient::TLegacyKey MaxKey;

    void Persist(const TPersistenceContext& context);

    operator bool() const;
};

//! A generic key that allows us to sort chunk stripes.
class TChunkStripeKey
{
public:
    TChunkStripeKey(int index);
    TChunkStripeKey(TBoundaryKeys boundaryKeys);
    TChunkStripeKey(NChunkPools::TOutputOrder::TEntry entry);
    //! Used only for persistence.
    TChunkStripeKey();

    bool IsIndex() const;
    bool IsBoundaryKeys() const;
    bool IsOutputOrderEntry() const;

    operator bool() const;

    int& AsIndex();
    int AsIndex() const;

    TBoundaryKeys& AsBoundaryKeys();
    const TBoundaryKeys& AsBoundaryKeys() const;

    NChunkPools::TOutputOrder::TEntry& AsOutputOrderEntry();
    const NChunkPools::TOutputOrder::TEntry& AsOutputOrderEntry() const;

    void Persist(const TPersistenceContext& context);

private:
    std::variant<int, TBoundaryKeys, NChunkPools::TOutputOrder::TEntry> Key_;
};

TString ToString(const TChunkStripeKey& key);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkPools
