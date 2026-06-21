#pragma once
//
// ZeroAllocGuard — RAII allocation trap for the memory-integrity harness
// (docs/prd-memory-integrity-tests.md Leg 3).
//
// While a guard is in scope on the current thread, every heap allocation
// (operator new / new[] and, on Linux, raw malloc/calloc/realloc) is counted
// as a violation. The guard does NOT abort — it records and lets the
// allocation proceed, so the run continues and the test reports the full
// count via a normal Catch2 assertion (cleaner than std::terminate, which
// would crash the process mid-test).
//
// Usage:
//   {
//       cedar::testing::ZeroAllocGuard guard;   // arm (resets counters)
//       for (int b = 0; b < N; ++b) vm.process_block(L, R);
//   }                                            // disarm
//   CHECK(cedar::testing::zero_alloc_violations() == 0);
//
// The actual operator-new / malloc overrides live in zero_alloc_hooks.cpp and
// are linked ONLY into the cedar_tests binary — production builds are
// untouched. Under an AddressSanitizer build the overrides are compiled out
// (ASan owns operator new/delete), so the guard is inert there and the
// [zero_alloc] test passes trivially; its authoritative run is the release
// build (scripts/check-release.sh, scripts/memory/run_all.sh).

#include <cstddef>

namespace cedar::testing {

// Arm/disarm the per-thread allocation trap. Prefer the RAII guard below.
void zero_alloc_arm() noexcept;
void zero_alloc_disarm() noexcept;

// Number of allocations seen while armed since the last arm()/reset().
std::size_t zero_alloc_violations() noexcept;
// Byte size of the first allocation seen while armed (0 if none) — handy for
// diagnostics when a violation is reported.
std::size_t zero_alloc_first_bytes() noexcept;
// True when the trap overrides are actually compiled in (false under ASan).
bool zero_alloc_active() noexcept;

class ZeroAllocGuard {
public:
    ZeroAllocGuard() noexcept { zero_alloc_arm(); }
    ~ZeroAllocGuard() noexcept { zero_alloc_disarm(); }

    ZeroAllocGuard(const ZeroAllocGuard&) = delete;
    ZeroAllocGuard& operator=(const ZeroAllocGuard&) = delete;
};

}  // namespace cedar::testing
