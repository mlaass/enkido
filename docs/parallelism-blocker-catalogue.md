# Parallelism Blocker Catalogue

Last updated: 2026-08-01 (Phase 9 close-out re-scan)
Maintained by: `prd-parser-codegen-hardening.md` (Phase 1 authored it; every
later phase flipped entries; Phase 9 asserted only PB-F5 remains open).

Scope: shared mutable state reachable from `akkado::compile()` that would
be unsafe under per-import front-end parallelism (audit F5 / PRD-3).
Re-run the queries in §Scan-queries below when re-auditing. Last re-run
2026-08-01: zero new hazards — every hit is either a verified-safe
internal-linkage pure function or the catalogued PB-004 singleton.

## Open

| ID | Site | Hazard | Severity | Resolution path |
|----|------|--------|----------|-----------------|
| PB-F5 | `akkado/src/akkado.cpp` (import source concatenation pre-lex) | The orchestration step that PRD-3 replaces with per-import fan-out. Shared state: concatenated source buffer, merged `SourceMap` regions, single `StringInterner`. Merge point: post-parse arena merge re-indexing `NodeIndex`. | High (blocks PRD-3 by definition) | **Out of scope — PRD-3.** This is the handover entry; it stays open until the per-import parallelism PRD lands. |

## Resolved during this PRD

| ID | Resolved in | Commit | Notes |
|----|-------------|--------|-------|
| PB-001 | Phase 3 (2026-07-31) | `b74107b` | Frozen builtin registry + process-shared name-keyed `builtin_scope()` lookup fallback; zero builtin inserts per `SymbolTable` construction. Shared scope is name-keyed (not a `scopes_[0]` pointer) because SymbolIds are per-compile. |

## Out of scope — owned by other PRDs

These are catalogued for completeness but do not block **per-import**
front-end parallelism (one front-end object per import): they bite only
finer-grained parallelism inside a single compile, or are enforced-safe
by contract.

| ID | Site | Hazard | Owner |
|----|------|--------|-------|
| PB-002 | `akkado/include/akkado/codegen.hpp` (`node_types_` member) | Dual-role: memoization cache AND inter-handler channel. Blocks per-statement codegen parallelism only; per-import parallelism unaffected (one `CodeGenerator` per import). | Future per-statement-codegen PRD |
| PB-003 | `akkado/src/codegen_arrays.cpp` (`apply_lambda` move/clear/restore of `node_types_`) | Same scope as PB-002; per-iteration mutation thrash. | `prd-codegen-sprawl-cleanup.md` Phase 7 (overlay scoping) |
| PB-004 | `akkado/src/host_extensions.cpp` (`registry()` function-local `static Registry`) | Process-wide host-extension registry. Safe by documented contract (register-then-freeze before first compile; read-only afterwards; no lock) — but the freeze is not enforced against a compile running concurrently with a late registration. | PRD-3 (see precondition list below) |

## Precondition list for PRD-3 (per-import parallelism)

What this PRD already delivered — the PRD-3 author can build on these
without re-auditing:

1. **No mutexes, no `thread_local`, no `once_flag`** anywhere in
   `akkado/src` + `akkado/include` (the voicing-registry mutex was
   removed by `prd-parser-codegen-correctness.md` Phase 4;
   scans re-verified 2026-08-01).
2. **Per-compile state is containerised** — `CompileContext` owns the
   `StringInterner` + `VoicingRegistry`; `compile()` has no
   process-global mutable state besides PB-004's frozen registry.
3. **Builtins are process-shared immutable** (Phase 3): `constexpr
   frozen` maps + a `const` name-keyed `builtin_scope()`; SymbolTable
   construction does zero inserts. Per-import workers can share it
   read-only. Caveat: SymbolIds are per-compile interner-sequential —
   any cross-worker symbol exchange must be name-keyed, never id-keyed.
4. **The AST is read-only after analysis** (correctness PRD Phase 1a/1b)
   and generic traversal covers all auxiliary children via
   `Node::extra_children` (Phase 6) — no ghost-field special cases in a
   future arena-merge/re-index step.
5. **Single-copy front-end helpers** — `lex_primitives` (Phase 5),
   `OPERATORS[]` (Phase 4), `expr_kinds` + `named_args` (Phase 7) — so
   per-import worker code paths don't fork behavior.
6. **Tooling reads compile artifacts, not source** — shape_index
   (Phase 8) consumes `CompileArtifacts`; no second front-end pipeline
   to parallelise or keep in sync.
7. **PRD-3 must assert/enforce `host_extensions` `frozen == true`
   before spawning per-import workers** (PB-004).

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
