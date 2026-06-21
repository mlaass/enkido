// Allocation-trap hooks for the zero-allocation invariant (Leg 3 of
// docs/prd-memory-integrity-tests.md). Linked ONLY into cedar_tests.
//
// Mechanism:
//   * Override global operator new / new[] / delete / delete[] (portable —
//     the minimum-viable trap, §9.7).
//   * On Linux (non-ASan), also wrap raw malloc/calloc/realloc/free via the
//     linker's -Wl,--wrap= flag to catch C allocations that bypass operator
//     new. operator new then allocates via __real_malloc so a C++ `new` is
//     never double-counted.
//
// Recording is gated on a thread-local "armed" flag flipped by ZeroAllocGuard,
// so the rest of the test binary (and Catch2) is unaffected. record() does no
// allocation itself — it only bumps integer counters — so it is safe to call
// from inside the allocator.
//
// Build-time configuration comes from cedar/tests/CMakeLists.txt (single
// source of truth — the test TU is not necessarily compiled with
// -fsanitize=address, so it can't reliably detect the ASan build itself):
//   ZERO_ALLOC_DISABLE      → compile the overrides out (ASan owns new/malloc);
//                             counters stay zero, [zero_alloc] passes trivially.
//   ZERO_ALLOC_WRAP_MALLOC  → emit __wrap_*/use __real_* (Linux, non-ASan).

#include "zero_alloc_guard.hpp"

#include <cstdlib>
#include <new>

namespace cedar::testing {
namespace {
thread_local bool        g_armed = false;
thread_local std::size_t g_violations = 0;
thread_local std::size_t g_first_bytes = 0;
}  // namespace

void zero_alloc_arm() noexcept {
    g_violations = 0;
    g_first_bytes = 0;
    g_armed = true;
}
void zero_alloc_disarm() noexcept { g_armed = false; }
std::size_t zero_alloc_violations() noexcept { return g_violations; }
std::size_t zero_alloc_first_bytes() noexcept { return g_first_bytes; }

#if defined(ZERO_ALLOC_DISABLE)
bool zero_alloc_active() noexcept { return false; }
#else
bool zero_alloc_active() noexcept { return true; }
#endif

namespace {
inline void record(std::size_t n) noexcept {
    if (!g_armed) return;
    if (g_violations == 0) g_first_bytes = n;
    ++g_violations;
}
}  // namespace
}  // namespace cedar::testing

#if !defined(ZERO_ALLOC_DISABLE)

// Pick the raw allocator. With the malloc wrap (Linux, non-ASan) operator new
// must bypass the wrap via __real_malloc so a `new` is not counted twice.
#if defined(ZERO_ALLOC_WRAP_MALLOC)
extern "C" void* __real_malloc(std::size_t);
extern "C" void  __real_free(void*) noexcept;
extern "C" void* __real_calloc(std::size_t, std::size_t);
extern "C" void* __real_realloc(void*, std::size_t);
#  define ZA_RAW_MALLOC __real_malloc
#  define ZA_RAW_FREE   __real_free
#else
#  define ZA_RAW_MALLOC std::malloc
#  define ZA_RAW_FREE   std::free
#endif

namespace {
inline void* za_alloc(std::size_t size) {
    cedar::testing::record(size);
    void* p = ZA_RAW_MALLOC(size != 0 ? size : 1);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
}  // namespace

void* operator new(std::size_t size) { return za_alloc(size); }
void* operator new[](std::size_t size) { return za_alloc(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    cedar::testing::record(size);
    return ZA_RAW_MALLOC(size != 0 ? size : 1);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    cedar::testing::record(size);
    return ZA_RAW_MALLOC(size != 0 ? size : 1);
}

void operator delete(void* p) noexcept { ZA_RAW_FREE(p); }
void operator delete[](void* p) noexcept { ZA_RAW_FREE(p); }
void operator delete(void* p, std::size_t) noexcept { ZA_RAW_FREE(p); }
void operator delete[](void* p, std::size_t) noexcept { ZA_RAW_FREE(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { ZA_RAW_FREE(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { ZA_RAW_FREE(p); }

#if defined(ZERO_ALLOC_WRAP_MALLOC)
// malloc-family wraps catch C allocations that bypass operator new. operator
// new above allocates via __real_malloc (not the wrapped malloc), so a C++
// `new` is never double-counted here.
extern "C" void* __wrap_malloc(std::size_t size) {
    cedar::testing::record(size);
    return __real_malloc(size);
}
extern "C" void __wrap_free(void* p) noexcept { __real_free(p); }
extern "C" void* __wrap_calloc(std::size_t n, std::size_t size) {
    cedar::testing::record(n * size);
    return __real_calloc(n, size);
}
extern "C" void* __wrap_realloc(void* p, std::size_t size) {
    cedar::testing::record(size);
    return __real_realloc(p, size);
}
#endif  // ZERO_ALLOC_WRAP_MALLOC

#endif  // !ZERO_ALLOC_DISABLE
