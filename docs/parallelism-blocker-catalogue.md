# Parallelism Blocker Catalogue

Last updated: 2026-07-31
Maintained by: `prd-parser-codegen-hardening.md` (Phase 1 authors it; every
later phase flips entries; Phase 9 asserts only PB-F5 remains open).

Scope: shared mutable state reachable from `akkado::compile()` that would
be unsafe under per-import front-end parallelism (audit F5 / PRD-3).
Re-run the queries in §Scan-queries below when re-auditing.

## Open

| ID | Site | Hazard | Severity | Resolution path |
|----|------|--------|----------|-----------------|
| PB-002 | `akkado/include/akkado/codegen.hpp` (`node_types_` member) | Dual-role: memoization cache AND inter-handler channel. Blocks per-statement codegen parallelism only; per-import parallelism unaffected (one `CodeGenerator` per import) | Medium | Out of scope — future per-statement-codegen PRD (catalogued for completeness) |
| PB-003 | `akkado/src/codegen_arrays.cpp` (`apply_lambda` move/clear/restore of `node_types_`) | Same scope as PB-002; per-iteration mutation thrash | Medium | `prd-codegen-sprawl-cleanup.md` Phase 7 (overlay scoping) |
| PB-004 | `akkado/src/host_extensions.cpp` (`registry()` function-local `static Registry`) | Process-wide host-extension registry. Safe by documented contract (register-then-freeze before first compile; read-only afterwards; no lock) — but the freeze is not enforced against a compile running concurrently with a late registration | Low | PRD-3 must assert/enforce `frozen == true` before spawning per-import workers; no change needed in this PRD |

## Resolved during this PRD

| ID | Resolved in | Commit | Notes |
|----|-------------|--------|-------|
| PB-001 | Phase 3 (2026-07-31) | backfilled at Phase 9 | Frozen builtin registry + process-shared name-keyed `builtin_scope()` lookup fallback; zero builtin inserts per `SymbolTable` construction. Shared scope is name-keyed (not a `scopes_[0]` pointer) because SymbolIds are per-compile. |

## Out of scope — future PRD-3 (per-import parallelism)

| ID | Site | Note |
|----|------|------|
| PB-F5 | `akkado/src/akkado.cpp` (import source concatenation pre-lex) | The orchestration step that PRD-3 replaces with per-import fan-out. Shared state: concatenated source buffer, merged `SourceMap` regions, single `StringInterner`. Merge point: post-parse arena merge re-indexing `NodeIndex`. |

## Verified safe (do not re-investigate)

| Site | Why safe |
|------|----------|
| `codegen_patterns.cpp` `static` fns (`sample_refs_from_mappings`, `synth_run_events`, `synth_binary_events`) | Internal-linkage pure functions; no mutable state |
| `host_extensions.cpp` other statics | All inside the `Registry` singleton covered by PB-004 |
| `static const` / `static constexpr` tables throughout `akkado/` | Immutable after static init |
| Mutex/lock scan | Zero hits in `akkado/src` + `akkado/include` (voicing registry mutex removed by `prd-parser-codegen-correctness.md` Phase 4) |
| `thread_local` / `once_flag` / `call_once` scan | Zero hits |

## Scan queries

```bash
grep -rn 'static std::mutex\|static std::unordered_map\|static std::map\|static std::vector\|static std::atomic' akkado/src/ akkado/include/
grep -rn 'once_flag\|call_once\|thread_local' akkado/src/ akkado/include/
grep -rn 'std::lock_guard\|std::scoped_lock\|std::unique_lock\|std::mutex' akkado/src/ akkado/include/
grep -rn '^\s*static ' akkado/src/*.cpp | grep -v 'static const\|static constexpr\|static_assert'
```
