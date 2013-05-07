﻿#pragma once

#include <ytlib/misc/common.h>
#include <ytlib/misc/small_vector.h>

#include <bitset>

namespace NYT {
namespace NErasure {

///////////////////////////////////////////////////////////////////////////////

//! The maximum total number of blocks our erasure codec can handle.
const int MaxTotalPartCount = 16;

//! A vector type for holding block indexes without allocations.
typedef TSmallVector<int, MaxTotalPartCount> TPartIndexList;

//! Each bit corresponds to a possible block index.
typedef std::bitset<MaxTotalPartCount> TPartIndexSet;

struct ICodec;

DECLARE_ENUM(ECodec,
    ((None)           (0))
    ((ReedSolomon_6_3)(1))
    ((Lrc_12_2_2)     (2))
);

///////////////////////////////////////////////////////////////////////////////

} // namespace NErasure
} // namespace NYT
