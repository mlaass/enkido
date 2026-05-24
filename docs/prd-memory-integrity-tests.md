> **Status: NOT STARTED** — Test infrastructure only; the 48GB
> `akkado-cli --check` explosion that triggered this PRD is tracked
> separately and will be reproduced + fixed using the harness this PRD
> ships.

# PRD: Memory Integrity Tests

## Executive Summary

NKIDO has no automated way to detect memory failures in its CLI tools,
its VM, or its compiler. The trigger incident: a single `akkado-cli
--check /tmp/test_5.ak` invocation ballooned to **48 GB RSS / 190 GB
VSZ** before being killed manually. No existing test would have caught
that — the closest, `cedar/tests/test_memory_stress.cpp`, is a
*behavioral* stress test (it exercises pool/state APIs under load) and
never measures actual process memory.

This PRD ships a four-legged memory-integrity harness that covers:

1. **Explosion guard** — RSS-ceiling + wall-clock wrapper around
   `akkado-cli` and `nkido-cli` invocations against existing fixtures
   and corpora. Per-binary RLIMIT_AS provides a hard runtime cap; an
   external wrapper records peak RSS for reporting.
2. **Sanitizer build** — an ASan + LSan + UBSan build configuration and
   a CI workflow (`workflow_dispatch` for v1) that rebuilds and runs the
   full Cedar + Akkado Catch2 suites. Existing `NKIDO_ENABLE_ASAN` /
   `NKIDO_ENABLE_UBSAN` flags are leveraged; LSan suppressions live in
   the repo.
3. **Zero-allocation invariant** — a test-only `operator new` /
   `malloc` hook that traps inside Cedar VM block processing, making the
   "no allocations in the audio path" rule mechanically enforced rather
   than convention.
4. **Mutating recompile drift fuzz** — a long-running harness that
   seeds from `akkado/stdlib/*.ak` + a curated corpus + a grammar
   synthesizer, applies both token-level and AST-level mutations
   (including ones that produce invalid code, exercising error
   recovery), recompiles + hot-swaps into a live VM, and asserts
   bounded RSS via *both* a peak ceiling and a linear slope check on
   sampled RSS.

**Key design decisions (locked — see §10):**

- **All four legs ship; comprehensive scope, not minimal.** This PRD
  treats memory integrity as a first-class concern alongside audio
  correctness.
- **Coverage stops at native code.** v1 covers `akkado-cli`, Cedar VM,
  and `nkido-cli`. WASM and Python (`cedar_core`) are out of scope.
- **No PR-blocking CI gate in v1.** The new memory workflow is
  `workflow_dispatch`-only initially. Scheduled cron is a deliberate
  follow-up so the harness stabilises before it gates merges.
- **Release gate via a new `scripts/check-release.sh`,** not by
  extending `bump-version.sh`. The bump script stays focused on
  version mechanics; the new release-check script runs the memory
  suite, sanitizer suite, and any future pre-release gates.
- **Existing `test_memory_stress.cpp` is renamed to
  `test_logical_stress.cpp`** to free the `memory_*` namespace for the
  new tests. Its contents are unchanged.
- **Phased delivery** in four commits, each independently shippable
  (§8).

---

## 1. Current State

### 1.1 What exists today

| Capability | Status | Location |
|---|---|---|
| Catch2 unit tests for Cedar + Akkado | Exists | `cedar/tests/`, `akkado/tests/` |
| Behavioral "memory stress" test (pool/state coordination) | Exists | `cedar/tests/test_memory_stress.cpp` |
| Determinism fuzz (compile + single hot-swap) | Exists | `akkado/tests/test_fuzz_determinism.cpp` |
| Recompile audio-continuity fuzz | Exists | `akkado/tests/test_fuzz_recompile_audio.cpp` |
| Audio-artifact metrics (click, beat monotonicity) | Exists | `akkado/tests/fuzz/artifact_metrics.hpp` |
| Python recompile-trigger fuzz | Exists | `experiments/test_fuzz_trigger_recompile.py` |
| `NKIDO_ENABLE_ASAN`, `NKIDO_ENABLE_UBSAN` CMake flags | Exists, off | `cmake/CompilerOptions.cmake` |
| Native C++ tests in CI | **None** | — (CI is WASM + Playwright only) |
| Sanitizer CI job | **None** | — |
| RSS-ceiling / explosion-guard wrapper | **None** | — |
| Audio-path zero-allocation enforcement | **None** | — (convention only) |
| Long-running memory drift test | **None** | — |
| Pre-release verification script | **None** | — (only `bump-version.sh`) |

### 1.2 The gap

The trigger incident — `akkado-cli --check /tmp/test_5.ak` consuming
48 GB RSS — surfaced via a human noticing the system slow down. There
is no automated tripwire. More broadly:

- **No process-level RSS measurement** anywhere in the test suite. The
  existing `test_memory_stress.cpp` exercises pool APIs but cannot
  observe `getrusage` or `/proc/self/status`.
- **No leak detection.** The sanitizer flags exist but are never
  enabled in any default build or CI pipeline.
- **The audio-path zero-alloc invariant is enforced by code review and
  vigilance.** A regression that adds `std::string` use to a hot opcode
  body would compile and pass all tests.
- **No long-running workload.** The longest existing run is the
  recompile-audio fuzz at a fixed iteration count, which focuses on
  audio artifacts, not memory.
- **No pre-release gate** beyond a clean git tree and a CHANGELOG
  entry. A leak or budget regression can ship.

---

## 2. Goals and Non-Goals

### 2.1 Goals

1. Catch the akkado-cli class of unbounded-growth bug automatically on
   any of the inputs in the test corpus.
2. Detect leaks in Cedar + Akkado on every sanitizer run.
3. Mechanically enforce the audio-path zero-allocation invariant.
4. Detect slow leaks via long-running mutating recompile drift, even
   when peak RSS stays under the explosion ceiling.
5. Provide a `scripts/check-release.sh` that runs the full suite and
   gates the release ritual.
6. Provide local-developer targets so any contributor can run
   `./scripts/memory/run_all.sh` in under a minute.
7. Every leg has a documented failure mode, a clear pass/fail signal,
   and a quoted RSS / iteration / sanitizer report.

### 2.2 Non-Goals (deferred or out-of-scope)

- **WASM memory tests.** WASM has a 4 GB cap (much lower on mobile) and
  failure modes here are catastrophic, but the tooling (Emscripten
  memory profiling, browser-side measurement) is its own project.
  Future PRD.
- **Python `cedar_core` memory tests.** Smaller surface; not where
  recent bugs lived.
- **Valgrind support.** Tracked as a follow-up to the ASan job, once
  the ASan build stabilises. Initial harness is ASan + LSan + UBSan
  only.
- **ThreadSanitizer (TSan).** Cedar has lock-free SPSC queues and a
  triple-buffer audio↔compiler handoff that warrant TSan, but it adds
  another build variant and is deferred.
- **Per-PR CI gating.** v1 ships the workflow as
  `workflow_dispatch`-only. Moving to scheduled cron and eventually a
  PR gate is a follow-up once the harness has stabilised over real
  use.
- **Fixing the 48GB `akkado-cli --check` bug.** This PRD ships the
  harness; the bug is reproduced and fixed in a separate commit/issue
  using the harness.

---

## 3. Architecture

### 3.1 The four legs

```
┌─────────────────────────────────────────────────────────────────┐
│                   Memory Integrity Harness                      │
├──────────────────┬──────────────────┬───────────────┬───────────┤
│ 1) Explosion     │ 2) Sanitizer     │ 3) Zero-alloc │ 4) Drift  │
│    guard         │    build         │    trap       │    fuzz   │
│                  │                  │               │           │
│ scripts/memory/  │ cmake + CI       │ Catch2 test   │ Catch2    │
│ + RLIMIT in CLI  │ workflow         │ in cedar/     │ test in   │
│                  │                  │ tests/        │ akkado/   │
│                  │                  │               │ tests/    │
│                  │                  │               │ fuzz/     │
├──────────────────┴──────────────────┴───────────────┴───────────┤
│   scripts/memory/run_all.sh   →   scripts/check-release.sh      │
└─────────────────────────────────────────────────────────────────┘
```

### 3.2 Leg 1: Explosion guard

Two pieces:

**(a) In-process cap via `setrlimit(RLIMIT_AS, ...)`** added to
`tools/akkado-cli/main.cpp` and `tools/nkido-cli/main.cpp`. Activated
by a `NKIDO_RLIMIT_MB` environment variable read at startup; when
unset, no cap (current behavior). The OS kills the process cleanly if
exceeded. This is the worst-case backstop — when a runaway compile
happens during development, the user gets an OOM instead of a swap-out.

**(b) External wrapper script** `scripts/memory/run_with_limit.py`:

```python
# scripts/memory/run_with_limit.py
# Usage:
#   run_with_limit.py --binary BIN --rss-mb N --timeout-sec T -- ARGS...
#
# Sets NKIDO_RLIMIT_MB=N, then runs BIN with ARGS. Polls /proc/PID/status
# (Linux) or `ps` (macOS) every 100ms. Records peak RSS. Kills the
# process and exits non-zero if either ceiling is exceeded. On success,
# prints "PEAK_RSS_MB=<n> WALL_MS=<n>".
```

A second wrapper, `scripts/memory/check_corpus.sh`, iterates over a
fixture corpus and invokes the wrapper per fixture, aggregating results
into a single pass/fail line.

**Per-binary RSS budgets:**

| Binary | RSS ceiling | Wall-clock timeout |
|---|---|---|
| `akkado-cli` | 1024 MB | 60 s |
| `nkido-cli` (render mode) | 2048 MB | 300 s |
| Catch2 test binaries (under sanitizers) | 4096 MB | 600 s |

The akkado-cli budget is 48× under the trigger-incident peak. If a
legitimate compile-time computation exceeds it (e.g. very large
constant folding), the budget gets revisited in a separate PRD — not
silently raised.

### 3.3 Leg 2: Sanitizer build + CI job

New CMake preset `sanitize` (extends `debug`):

```json
{
  "name": "sanitize",
  "displayName": "Debug with ASan + UBSan",
  "inherits": "debug",
  "cacheVariables": {
    "NKIDO_ENABLE_ASAN": "ON",
    "NKIDO_ENABLE_UBSAN": "ON"
  }
}
```

LSan is enabled implicitly by ASan on Linux. A repo-checked-in
suppressions file `cedar/tests/lsan.supp` carries justified entries
(third-party libs, intentional one-shot leaks during static init).

New CI workflow `.github/workflows/memory-tests.yml`:

```yaml
name: Memory Tests
on:
  workflow_dispatch:    # v1: manual only

jobs:
  sanitizer:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: cmake --preset sanitize
      - run: cmake --build build/sanitize -j
      - env:
          LSAN_OPTIONS: "suppressions=$PWD/cedar/tests/lsan.supp"
          ASAN_OPTIONS: "detect_leaks=1:abort_on_error=0"
          UBSAN_OPTIONS: "print_stacktrace=1:halt_on_error=1"
        run: ctest --test-dir build/sanitize --output-on-failure

  explosion-guard:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: cmake --preset release && cmake --build build/release -j
      - run: scripts/memory/check_corpus.sh

  drift-fuzz:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: cmake --preset release && cmake --build build/release -j
      - run: ./build/release/akkado/tests/akkado_tests "[drift_fuzz]" --iters 10000
```

### 3.4 Leg 3: Zero-allocation trap

A test-only helper in `cedar/tests/zero_alloc_guard.hpp`:

```cpp
// cedar/tests/zero_alloc_guard.hpp
//
// RAII guard that traps allocations while in scope. Implementation:
//   - Overrides operator new / operator new[] in the test binary.
//   - Intercepts malloc via __wrap_malloc (linker flag) on Linux,
//     or via DYLD_INSERT_LIBRARIES on macOS.
//   - Thread-local "armed" flag flipped by the RAII guard.
//   - When armed, any allocation calls std::terminate with a
//     descriptive abort message, captured by Catch2.

namespace cedar::testing {
class ZeroAllocGuard {
public:
    ZeroAllocGuard();      // arm
    ~ZeroAllocGuard();     // disarm
};
}  // namespace cedar::testing
```

Usage in a new `cedar/tests/test_zero_alloc.cpp`:

```cpp
TEST_CASE("VM block processing does not allocate", "[zero_alloc]") {
    VM vm;
    // ... load a representative program (oscillators, filters, FX) ...
    {
        cedar::testing::ZeroAllocGuard guard;
        for (int b = 0; b < 1000; ++b) {
            vm.run_block();
        }
    }
}
```

The new operator overrides live only in the test binary's link target
— production builds are unaffected.

### 3.5 Leg 4: Mutating recompile drift fuzz

A new Catch2 test `akkado/tests/test_drift_fuzz.cpp` with the
`[drift_fuzz]` tag, parameterised by `--iters` (Catch2 custom arg).

```
seed corpus
   ├─ akkado/stdlib/*.ak                (auto-discovered)
   ├─ akkado/tests/fuzz/corpus/*.ak     (curated representatives)
   └─ grammar synthesizer (optional)    (generates random valid programs)
       │
       ▼
   pick a seed (round-robin)
       │
       ▼
   apply N mutations (mix of strategies)
       ├─ token-level: delete / duplicate / swap / replace
       └─ AST-level:   subtree swap / expression deletion / pretty-print
       │  (~30% of mutations produce invalid code by design)
       ▼
   akkado::compile(mutated)
       │
       ├─ success → vm.hot_swap(bytecode); vm.run_block() × M
       └─ error   → assert recovery (compiler does not leak, VM stays alive)
       │
       ▼
   every K iterations: sample RSS via /proc/self/status
       │
       ▼
   end of run: assert peak < ceiling AND slope(after warmup) < threshold
```

Mutation tier weighting (default): 50% token-level, 30% AST-level, 20%
synthesized-from-grammar.

**Drift detection — both ceiling and slope:**

| Check | Threshold | Why |
|---|---|---|
| Peak RSS | 1 GB | Catches sudden explosions (compiler or VM goes off the rails on one input). |
| Linear slope of RSS post-warmup | < 1 KB / iteration | Catches slow steady leaks of any size given enough iterations. Warmup = first 10% of iterations (pool fills, sample caches warm up). |
| RSS at iter N == RSS at iter N/2 (within 5%) | — | Sanity: rough steady-state assertion at long horizon. |

### 3.6 The release-check script

```bash
# scripts/check-release.sh
# Runs all gates that should pass before bump-version.sh + tagging.
# Exits non-zero if any gate fails.

set -e
echo "== Sanitizer build =="
cmake --preset sanitize && cmake --build build/sanitize -j
LSAN_OPTIONS=suppressions=cedar/tests/lsan.supp ctest --test-dir build/sanitize

echo "== Memory tests (full) =="
cmake --preset release && cmake --build build/release -j
scripts/memory/check_corpus.sh
./build/release/cedar/tests/cedar_tests "[zero_alloc]"
./build/release/akkado/tests/akkado_tests "[drift_fuzz]" --iters 100000
```

The release ritual becomes: `./scripts/check-release.sh && ./scripts/bump-version.sh <bump>`. The bump script itself remains unchanged.

---

## 4. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| Cedar core (non-test C++) | **Stays** | No production code changes. |
| Akkado compiler core | **Stays** | No production code changes. |
| `tools/akkado-cli/main.cpp` | **Modified** | Read `NKIDO_RLIMIT_MB`, call `setrlimit` if set. |
| `tools/nkido-cli/main.cpp` | **Modified** | Same RLIMIT_AS hook. |
| `cedar/tests/test_memory_stress.cpp` | **Renamed** | → `test_logical_stress.cpp`. Contents unchanged. |
| `cmake/CompilerOptions.cmake` | **Stays** | Existing ASan/UBSan flags reused as-is. |
| `CMakePresets.json` | **Modified** | New `sanitize` preset. |
| `.github/workflows/deploy.yml` | **Stays** | Unchanged. |
| `.github/workflows/memory-tests.yml` | **New** | `workflow_dispatch` for v1. |
| `scripts/bump-version.sh` | **Stays** | Release-check is a separate script; bump is unchanged. |
| `scripts/memory/` | **New** | RSS wrapper, corpus runner, run-all. |
| `scripts/check-release.sh` | **New** | Aggregated pre-release gate. |
| `cedar/tests/zero_alloc_guard.hpp` | **New** | RAII allocation trap. |
| `cedar/tests/test_zero_alloc.cpp` | **New** | Audio-path zero-alloc test. |
| `cedar/tests/lsan.supp` | **New** | Justified leak suppressions. |
| `akkado/tests/test_drift_fuzz.cpp` | **New** | Mutating recompile drift fuzz. |
| `akkado/tests/fuzz/corpus/*.ak` | **New** | Curated seed corpus. |
| `akkado/tests/fuzz/mutator.hpp` | **New** | Token + AST mutator. |
| `akkado/tests/fuzz/grammar_synth.hpp` | **New** | Grammar synthesizer. |

---

## 5. File-Level Changes

### 5.1 New files

| File | Purpose |
|---|---|
| `scripts/memory/run_with_limit.py` | RSS+timeout wrapper around a single binary invocation. |
| `scripts/memory/check_corpus.sh` | Iterates fixture corpus, calls wrapper per fixture, reports. |
| `scripts/memory/run_all.sh` | Local entry point: builds release + runs explosion-guard + zero-alloc + drift (100 iters). |
| `scripts/check-release.sh` | Pre-release gate: sanitizer build + memory suite at deep iterations. |
| `cedar/tests/zero_alloc_guard.hpp` | RAII allocation-trap guard for tests. |
| `cedar/tests/zero_alloc_hooks.cpp` | `operator new`/malloc overrides linked into cedar_tests. |
| `cedar/tests/test_zero_alloc.cpp` | Catch2 test enforcing audio-path zero-alloc invariant. |
| `cedar/tests/lsan.supp` | LSan suppressions with one-comment-per-entry justifications. |
| `akkado/tests/test_drift_fuzz.cpp` | Catch2 mutating drift fuzz, tag `[drift_fuzz]`. |
| `akkado/tests/fuzz/mutator.hpp` | Token + AST mutators with weighted strategy selection. |
| `akkado/tests/fuzz/grammar_synth.hpp` | Generates random valid programs from the grammar. |
| `akkado/tests/fuzz/corpus/*.ak` | Curated seed patches (mini-notation, polyphony, FX, hot-swap). |
| `.github/workflows/memory-tests.yml` | `workflow_dispatch` workflow for the three jobs. |

### 5.2 Modified files

| File | Change |
|---|---|
| `tools/akkado-cli/main.cpp` | Add `NKIDO_RLIMIT_MB` reader + `setrlimit` call at startup. |
| `tools/nkido-cli/main.cpp` | Same. |
| `CMakePresets.json` | Add `sanitize` preset (inherits `debug` + ASan/UBSan). |
| `cedar/tests/CMakeLists.txt` | Add `zero_alloc_hooks.cpp` + `test_zero_alloc.cpp`; rename `test_memory_stress.cpp` reference. |
| `akkado/tests/CMakeLists.txt` | Add `test_drift_fuzz.cpp` + fuzz headers. |
| `CLAUDE.md` | Document `scripts/memory/run_all.sh` + the release-check workflow. |

### 5.3 Renamed

| Old | New |
|---|---|
| `cedar/tests/test_memory_stress.cpp` | `cedar/tests/test_logical_stress.cpp` (contents unchanged). |

### 5.4 Files explicitly NOT changed

| File | Reason |
|---|---|
| `scripts/bump-version.sh` | Release-check is a separate script that runs *before* bump. Coupling them ties version mechanics to test infra; we want them decoupled. |
| `.github/workflows/deploy.yml` | Deploy workflow remains as-is. Memory tests are a separate workflow. |
| `cmake/CompilerOptions.cmake` | Existing ASan/UBSan plumbing is reused as-is; the preset wires it. |

---

## 6. Configuration

### 6.1 Runtime environment variables

| Variable | Default | Effect |
|---|---|---|
| `NKIDO_RLIMIT_MB` | unset | If set in `akkado-cli`/`nkido-cli`, calls `setrlimit(RLIMIT_AS, n*1024*1024)` at startup. Unset → no cap. |
| `LSAN_OPTIONS` | unset | Set in CI to `suppressions=$PWD/cedar/tests/lsan.supp`. |
| `ASAN_OPTIONS` | unset | Set in CI to `detect_leaks=1:abort_on_error=0`. |
| `UBSAN_OPTIONS` | unset | Set in CI to `print_stacktrace=1:halt_on_error=1`. |

### 6.2 Drift fuzz iteration counts

| Run target | Iterations | Approx wall time | Trigger |
|---|---|---|---|
| Local (`scripts/memory/run_all.sh`) | 100 | ~30 s | Developer invocation. |
| CI / nightly (manual via `workflow_dispatch` v1) | 10,000 | ~10 min | Manual trigger, eventually cron. |
| Pre-release (`scripts/check-release.sh`) | 100,000 | ~1 hr | Before `bump-version.sh`. |

Iteration count is parameterised via Catch2 custom arg `--iters N`.

### 6.3 RSS budgets (centralised)

`scripts/memory/budgets.sh` exports the budgets as shell vars so both
local wrappers and CI jobs read the same numbers:

```sh
export NKIDO_BUDGET_AKKADO_CLI_MB=1024
export NKIDO_BUDGET_NKIDO_CLI_MB=2048
export NKIDO_BUDGET_TEST_BIN_MB=4096
export NKIDO_BUDGET_DRIFT_PEAK_MB=1024
export NKIDO_BUDGET_DRIFT_SLOPE_BYTES_PER_ITER=1024
```

Changing a budget = touching one file with a rationale in the commit
message.

---

## 7. Mutation Strategies

### 7.1 Token-level mutator

Operates on the lexer's token stream. Re-prints by joining tokens with
spaces. Cheap, no AST roundtrip. Cheerfully produces invalid programs
— that's wanted, since invalid input exercises error recovery.

| Operator | Behavior | Frequency |
|---|---|---|
| `DELETE` | Remove one random token. | 25% |
| `DUPLICATE` | Duplicate one random token. | 15% |
| `SWAP` | Swap two adjacent tokens. | 20% |
| `REPLACE` | Replace one token with a random one drawn from the grammar's vocabulary. | 25% |
| `INSERT` | Insert a random vocabulary token at a random position. | 15% |

### 7.2 AST-level mutator

Parses the seed, mutates the AST, pretty-prints. Mostly produces valid
programs; targets semantic-richness coverage.

| Operator | Behavior | Frequency |
|---|---|---|
| `SUBTREE_SWAP` | Swap two same-kind subtrees at random positions. | 35% |
| `EXPRESSION_DELETE` | Replace an expression with `0` (constant). | 20% |
| `PIPE_REORDER` | Reorder steps in a `\|>` chain. | 20% |
| `BUILTIN_REPLACE` | Replace a builtin call with another of the same arity. | 25% |

### 7.3 Grammar synthesizer

Generates a small random valid program from the grammar (osc + filter
+ out, with random arguments). Provides programs unrelated to the seed
corpus so the fuzz isn't limited to mutations of existing patches.

### 7.4 Strategy selection (default weights)

50% token-level, 30% AST-level, 20% grammar synthesizer. Overridable
per-test-run via Catch2 custom args.

---

## 8. Implementation Phases

Four phases, each independently shippable. After each phase, the new
leg runs in `scripts/memory/run_all.sh` and (where applicable) in the
CI workflow.

### 8.1 Phase 1 — Explosion guard (the trigger fix)

**Goal:** detect the akkado-cli class of bug on any corpus input.

**Files:**
- New: `scripts/memory/run_with_limit.py`, `scripts/memory/check_corpus.sh`, `scripts/memory/budgets.sh`, `scripts/memory/run_all.sh` (skeleton — only invokes the guard).
- Modified: `tools/akkado-cli/main.cpp`, `tools/nkido-cli/main.cpp` (add `NKIDO_RLIMIT_MB` reader).
- Modified: `CLAUDE.md` (document `scripts/memory/run_all.sh`).

**Verification:**
- `./scripts/memory/run_all.sh` passes on a healthy build.
- Running against a synthetic looping `.ak` (e.g. an infinite recursive
  `import` if the compiler permits) reports failure with peak RSS and
  the offending fixture.

### 8.2 Phase 2 — Sanitizer build + CI workflow

**Goal:** rebuild with ASan + LSan + UBSan and run the full Catch2
suite.

**Files:**
- New: `cedar/tests/lsan.supp` (start empty + add justified entries as
  they're found).
- Modified: `CMakePresets.json` (add `sanitize` preset).
- New: `.github/workflows/memory-tests.yml` (`workflow_dispatch`, with
  `sanitizer` job only in this phase — other jobs added in later
  phases).

**Verification:**
- `cmake --preset sanitize && cmake --build build/sanitize -j` succeeds
  locally.
- `ctest --test-dir build/sanitize` runs and either passes cleanly or
  surfaces a real leak with a useful stack trace.
- Manual `workflow_dispatch` of `memory-tests.yml` on GitHub passes.

### 8.3 Phase 3 — Zero-allocation trap

**Goal:** mechanically enforce no-allocations-in-audio-path.

**Files:**
- New: `cedar/tests/zero_alloc_guard.hpp`, `cedar/tests/zero_alloc_hooks.cpp`, `cedar/tests/test_zero_alloc.cpp`.
- Modified: `cedar/tests/CMakeLists.txt` (link hooks into cedar_tests, add `-Wl,--wrap=malloc` on Linux).

**Verification:**
- `./build/release/cedar/tests/cedar_tests "[zero_alloc]"` passes.
- A deliberate `new int(0)` added inside `VM::run_block()` makes the
  test fail with a clear abort message.

### 8.4 Phase 4 — Mutating recompile drift fuzz

**Goal:** detect slow drift over long horizons under mixed-validity
input.

**Files:**
- New: `akkado/tests/fuzz/mutator.hpp`, `akkado/tests/fuzz/grammar_synth.hpp`, `akkado/tests/fuzz/corpus/*.ak`, `akkado/tests/test_drift_fuzz.cpp`.
- Modified: `akkado/tests/CMakeLists.txt`.
- Modified: `.github/workflows/memory-tests.yml` (add `drift-fuzz` and `explosion-guard` jobs).
- New: `scripts/check-release.sh` (aggregated pre-release script,
  invokes all four legs at high iteration counts).

**Verification:**
- `./build/release/akkado/tests/akkado_tests "[drift_fuzz]" --iters 100` passes locally in under a minute.
- `--iters 10000` passes under the explosion-guard wrapper without
  exceeding 1 GB peak.
- A deliberate `std::vector` leak in the parser (allocated and
  abandoned per compile) makes the slope check fail with a quoted
  bytes-per-iteration figure.

### 8.5 Out-of-phase follow-ups (NOT in this PRD)

- Migrate `memory-tests.yml` to scheduled cron (e.g. nightly).
- Promote to PR-blocking gate once stable.
- Add Valgrind job.
- Add TSan job for SPSC + triple-buffer paths.
- Extend to WASM (Emscripten memory profiling).
- Extend to Python (`cedar_core`) leak surface.
- Reproduce + fix the original 48 GB `akkado-cli --check` bug.

---

## 9. Edge Cases

### 9.1 Compiler errors during drift fuzz

**Input:** a mutated program that produces an invalid AST.

**Expected behavior:** `akkado::compile(...)` returns an error result;
no allocations leak; the in-process VM stays alive; the next iteration
proceeds. This is the *primary* path for ~30% of iterations and is
load-bearing for the "error recovery" guarantee the user asked for.

### 9.2 Compiler crashes (segfault, throw)

**Input:** a mutated program that hits an internal `assert` or
unhandled exception.

**Expected behavior:** the drift fuzz aborts with a captured stack
trace, the seed + mutation history that produced the crash is dumped
to `build/release/drift_fuzz_crash_<timestamp>.txt`, and the failure
is reported as a distinct kind from a memory-budget failure.

### 9.3 Legitimate large constant folding

**Input:** an `.ak` file that pre-computes a 100 MB wavetable at
compile time.

**Expected behavior:** if it exceeds the 1 GB `akkado-cli` budget, the
test fails. Resolution: the budget gets revisited in a separate PRD
(or the test fixture moves to a higher-budget tier), not silently
raised. Hard ceilings stay loud.

### 9.4 Sanitizer-incompatible test (third-party UB)

**Input:** an existing Catch2 test that triggers a third-party
library's known UB.

**Expected behavior:** the test is added to an `UBSAN_DISABLED_TESTS`
list in `cedar/tests/lsan.supp`-adjacent `ubsan.supp`, with a
one-line comment justifying the suppression and a tracking issue. The
ASan job continues to run it; only UBSan is muted.

### 9.5 Zero-alloc test with a stateful program that needs preallocation

**Input:** a program with delay lines / reverbs whose state pool needs
to allocate on first block.

**Expected behavior:** the `ZeroAllocGuard` is constructed *after*
sufficient warmup blocks have run. The test pattern is: load program →
run N warmup blocks → arm guard → run M measured blocks → disarm. The
test docstring says exactly when the guard arms.

### 9.6 RLIMIT vs threading

**Input:** `nkido-cli serve` with multiple threads.

**Expected behavior:** `RLIMIT_AS` is process-wide (virtual address
space), not per-thread. Setting it works for `serve` mode. We do
NOT use `RLIMIT_RSS` (Linux-specific and only advisory on most
kernels).

### 9.7 macOS support

**Input:** a contributor on macOS runs `scripts/memory/run_all.sh`.

**Expected behavior:** `run_with_limit.py` uses `ps` rather than
`/proc/<pid>/status` on macOS. `setrlimit(RLIMIT_AS)` works. The
zero-alloc trap uses operator-new overrides (portable) — the
malloc-wrap via `-Wl,--wrap` is Linux-only and gracefully skipped on
macOS; operator-new coverage is the minimum-viable subset.

### 9.8 LSan suppression rot

**Input:** a suppression entry for a library that has since been
removed.

**Expected behavior:** the suppressions file is reviewed during each
release. Stale entries (matching no symbol on the current build) are
flagged by a check in `scripts/check-release.sh`. Adding an entry
requires a comment explaining what's leaking and why it's accepted.

---

## 10. Open Decisions (resolved)

| Decision | Choice | Round |
|---|---|---|
| Failure modes covered | All four (explosions, leaks, zero-alloc, pool budgets + drift) | 1 |
| Component scope | `akkado-cli` + Cedar VM + `nkido-cli` (no WASM / Python in v1) | 1 |
| Run targets | Local + manual CI (`workflow_dispatch`) + pre-release | 1 |
| Ambition | Comprehensive — all four legs | 1 |
| RSS measurement | Both `setrlimit` + external wrapper | 2 |
| Sanitizers | ASan + LSan + UBSan in v1; Valgrind as follow-up; TSan deferred | 2 |
| Zero-alloc enforcement | Test-only `operator new` / `malloc` overrides | 2 |
| Drift fuzz strategy | Seed corpus + token + AST mutations + invalid-code mix | 2 |
| RSS budgets | akkado-cli 1 GB / nkido-cli 2 GB / tests 4 GB | 3 |
| Drift detection | Both peak ceiling AND slope check | 3 |
| File layout | `scripts/memory/` + `cedar/tests/` + `akkado/tests/` (per-component) | 3 |
| Suppressions | Checked-in `lsan.supp` with per-entry justification | 3 |
| CI runner | GitHub-hosted ubuntu-latest, `workflow_dispatch` only in v1 | 4 |
| Release gate | Separate `scripts/check-release.sh` (don't extend `bump-version.sh`) | 4 |
| Mutation strategy | Both token-level AND AST-level | 4 |
| Bug-fix scope | Test infra only; 48 GB bug filed and fixed separately | 4 |
| Seed corpus | Stdlib `.ak` files + curated fixtures + grammar synthesizer | 5 |
| Iteration counts | Local 100 / nightly 10 k / pre-release 100 k | 5 |
| Existing `test_memory_stress.cpp` | Rename to `test_logical_stress.cpp` | 5 |
| Phase order | 1) Explosion guard → 2) ASan/UBSan → 3) Zero-alloc → 4) Drift fuzz | 5 |

---

## 11. Testing / Verification Strategy

Each phase has its own verification step (§8). Beyond per-phase
verification:

### 11.1 Per-leg negative tests

To prove each leg actually catches bugs (not just passes on healthy
code), each phase ships a `tests/manual/negative_*.md` walkthrough
showing how to deliberately introduce the bug and confirm the test
fails with a clear message. Examples:

- Explosion guard: add `std::vector<int> v; while(true) v.push_back(0);` to akkado-cli main; confirm wrapper kills + reports.
- ASan: add `int* p = new int[10]; p[10] = 0;` to a Cedar test; confirm ASan reports heap-buffer-overflow.
- LSan: add `new int(42); // leaked` to a Cedar test; confirm LSan reports the leak with stack.
- Zero-alloc: add `new int(0)` inside `VM::run_block()`; confirm guard aborts.
- Drift fuzz: add a per-iteration leak in compiler; confirm slope check reports `slope=N bytes/iter > 1024`.

These are documented manual walkthroughs, not committed test cases —
shipping a deliberately-broken test would be hostile to CI. They live
in markdown so the next maintainer can re-verify.

### 11.2 Acceptance criteria for v1

1. `./scripts/memory/run_all.sh` exits 0 on a clean checkout in under
   1 minute on a laptop.
2. Manual `workflow_dispatch` of `memory-tests.yml` on GitHub passes
   in under 1 hour.
3. `./scripts/check-release.sh` exits 0 in under 2 hours on a clean
   checkout.
4. All five negative walkthroughs in §11.1 produce the expected
   failure when manually applied.
5. The original 48 GB `akkado-cli --check /tmp/test_5.ak` input
   (when reproducible) is caught by the explosion guard with peak RSS
   and timeout reported.

### 11.3 Test corpora

The fixture corpus for `check_corpus.sh` initially includes:

| Input | Source |
|---|---|
| Each file in `akkado/stdlib/*.ak` (compile-only) | Existing |
| Each file in `akkado/tests/fuzz/corpus/*.ak` | New (this PRD) |
| A representative subset of `experiments/*.akk` | Existing |
| Each `.ak` test fixture under `akkado/tests/` if any | Existing |

The corpus grows over time. Any input that catches a real bug should
be added to `akkado/tests/fuzz/corpus/` with a comment naming the bug.

---

## 12. Future Work

- **Promote to scheduled CI + PR gate.** Once `workflow_dispatch`
  passes reliably for a few weeks, add a cron schedule. Once cron is
  green for a few cycles, promote to PR-blocking.
- **Add Valgrind job.** Reuse most of the sanitizer plumbing; new
  preset, new CI job.
- **Add TSan job.** Cover the lock-free SPSC queues and the
  triple-buffer audio↔compiler handoff.
- **Extend to WASM.** Browser memory profiling + Emscripten heap
  tracking. Significant tooling investment; deserves its own PRD.
- **Extend to Python `cedar_core`.** Smaller surface; useful but
  lowest priority.
- **Per-fixture RSS baselines + regression alerts.** Today's design
  enforces fixed ceilings; a future iteration could track per-fixture
  baselines and alert on 2× regressions even when still under ceiling.
- **Public dashboard / trend graphs.** Aggregate the peak-RSS and
  slope numbers from each nightly run; surface trends on the project
  website.
