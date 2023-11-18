# Generated by devtools/yamaker.

LIBRARY()

LICENSE(
    Apache-2.0 WITH LLVM-exception AND
    NCSA
)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/libs/llvm12
    contrib/libs/llvm12/include
    contrib/libs/llvm12/lib/Analysis
    contrib/libs/llvm12/lib/IR
    contrib/libs/llvm12/lib/MC
    contrib/libs/llvm12/lib/ProfileData
    contrib/libs/llvm12/lib/Support
    contrib/libs/llvm12/lib/Transforms/Utils
)

ADDINCL(
    contrib/libs/llvm12/lib/Transforms/Instrumentation
)

NO_COMPILER_WARNINGS()

NO_UTIL()

SRCS(
    AddressSanitizer.cpp
    BoundsChecking.cpp
    CGProfile.cpp
    ControlHeightReduction.cpp
    DataFlowSanitizer.cpp
    GCOVProfiling.cpp
    HWAddressSanitizer.cpp
    IndirectCallPromotion.cpp
    InstrOrderFile.cpp
    InstrProfiling.cpp
    Instrumentation.cpp
    MemProfiler.cpp
    MemorySanitizer.cpp
    PGOInstrumentation.cpp
    PGOMemOPSizeOpt.cpp
    PoisonChecking.cpp
    SanitizerCoverage.cpp
    ThreadSanitizer.cpp
    ValueProfileCollector.cpp
)

END()