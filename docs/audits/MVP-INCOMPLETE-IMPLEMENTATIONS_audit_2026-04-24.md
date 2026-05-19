# Audit: MVP Incomplete Implementations

**PRD:** `docs/reports/2026-01-31-mvp-incomplete-implementations.md`
**Audit base:** `17a765a` (PRD introduction, 2026-01-31)
**Audit head:** `4977c77` (2026-04-24)
**Audited:** 2026-04-24

## Summary

The tracking document claimed two items as FIXED (pattern transform chaining with `velocity`/`bank`/`n`, and direct string literal syntax). Both hold up: the code is present, the tests exist, and the suite is green. More importantly, three items the document still listed as BLOCKED/DEFERRED/STUB are in fact fully resolved by later refactors — the PRD had drifted from reality. New regression tests were added to close coverage gaps on the now-working behaviors, and the PRD has been flipped to **COMPLETE**.

## Goal Verification

| Goal (from PRD) | Status | Evidence |
|---|---|---|
| §1.1 Chained transformations (velocity/bank/n as inner transforms) | **Met** | `akkado/src/codegen_patterns.cpp:1479-1482` (is_transform list includes velocity/bank/variant/tune); tests at `akkado/tests/test_codegen.cpp:2146-2182` |
| §1.2 Direct string literal syntax (e.g. `slow("c4 e4 g4", 2)`) | **Met** | `akkado/src/codegen_patterns.cpp:1407` (is_pattern_node accepts StringLit), `:1455-1472` (compile_pattern_for_transform parses StringLit via parse_mini); tests at `akkado/tests/test_codegen.cpp:2184-2207` |
| §2.1 Nested field access | **Met (stale PRD claim)** | E134 removed entirely; `handle_field_access` at `akkado/src/codegen.cpp:1860-1988` dispatches recursively on `TypedValue::Record`; new tests at `akkado/tests/test_codegen.cpp:2553-2609` |
| §2.2 Method calls | **Met via UFCS** | `desugar_method_call` at `akkado/src/analyzer.cpp:1098` rewrites `a.f(b)` → `f(a, b)`; tests at `akkado/tests/test_codegen.cpp:1760-1898` check bytecode equivalence |
| §2.3 Post statements | **Intentionally unimplemented** | `akkado/src/codegen.cpp:1518-1520` still fires E115; syntax is `post(closure)` per `akkado/src/parser.cpp:302-322`. No shipping code uses it. Not a blocker. |
| §2.4 Field access on arbitrary expressions | **Met for valid cases** | `handle_field_access` type-dispatches to Pattern/Record; E135 at `akkado/src/codegen.cpp:1968-1983` correctly rejects field access on Signal/Number/Array/Function/String/Void |
| §3.1 Chord expansion (`C4'`) | **Correctly obsolete** | `C4'` deprecated in favor of `chord("Am")` / `c"…"`; PRD already notes this |
| §3.2 Array indexing | **Met (stale PRD claim)** | `ARRAY_INDEX` opcode exists in Cedar; `akkado/src/codegen.cpp:294-420` emits it for dynamic indices and constant-folds static ones; new tests at `akkado/tests/test_codegen.cpp:3503-3557` |

## Validation Commands

| Command | Result | Notes |
|---|---|---|
| `cmake --build build --target akkado` | Pass | Clean build |
| `cmake --build build --target akkado_tests cedar_tests` | Pass | Test binaries built |
| `./build/akkado/tests/akkado_tests "[codegen][patterns]"` | Pass (116 assertions / 14 cases) | Covers both FIXED items |
| `./build/akkado/tests/akkado_tests` (pre-audit) | Pass (84543 assertions / 437 cases) | Baseline |
| `./build/cedar/tests/cedar_tests` | Pass (329788 assertions / 131 cases) | |

## Findings

### Unmet Goals
None.

### Stubs
- `akkado/src/codegen.cpp:1518-1520` — `E115 Post statements not supported in MVP`. Kept by design; the parser accepts `post(closure)` but codegen bails. Not a blocker because `post` is not used anywhere in shipping code. **Resolution:** keep as-is; PRD updated to mark as "non-blocking, needs semantics clarification".

### Regressions
None. Full suite green both before and after audit.

### Coverage Gaps
All closed during finalize:

- **Nested record field access** (`a.b.c`) had no test. `handle_field_access` path for `ValueType::Record` was functionally exercised only via single-level access. **Resolved:** added three SECTIONs in `Codegen: Record handling` at `akkado/tests/test_codegen.cpp:2555-2609` that verify the correct inner value is returned (not the first field) at two and three levels of nesting.
- **Array indexing correctness** (beyond "it compiles") had no dedicated test. The PRD's original claim that "stub returns first element" was untested. **Resolved:** added `Codegen: Array indexing` test case at `akkado/tests/test_codegen.cpp:3503-3557` that verifies `arr[0]` / `arr[1]` / `arr[2]` return distinct correct values, and that a dynamic index emits the `ARRAY_INDEX` opcode.

### Missing Tests
None. The PRD's "Test Commands" section only lists the command to invoke tests; it did not promise specific test artifacts.

### Scope Drift
- The PRD's file-level references were badly out of sync with the current code (line numbers quoted for codegen.cpp referenced `:915-923`, `:924-928`, `:648-650`, `:655-657` — all wrong by hundreds of lines after the TypedValue refactor in `c733f66`). **Resolved:** PRD updated with current file:line references.
- The PRD made three BLOCKED/DEFERRED/STUB claims (§2.1, §2.2, §3.2) that were silently resolved by later refactors: TypedValue system (enabling nested field access and richer expression field access), `desugar_method_call` in the analyzer (UFCS), and the `ARRAY_INDEX` Cedar opcode. **Resolved:** PRD updated to reflect reality with evidence links.

### Convention Drift
None flagged. `CLAUDE.md` conventions around commit messages, bun/npm, test-behavior preservation were followed.

### Suggestions
- The edge case `p = n"…"; p.slow(2)` errors with E130 on the pattern-arg compile path — the method call is correctly rewritten to `slow(p, 2)`, but `compile_pattern_for_transform` in `codegen_patterns.cpp` doesn't follow `n` back to its pattern-literal binding. Not an MVP blocker (direct `n"c4 e4".slow(2)` works), but worth noting in a future pattern-arg polish pass. Documented inline in the PRD as a known edge.
- If `post(closure)` is to stay unimplemented long-term, consider either removing the grammar/parser support (currently accepts the syntax just to error in codegen), or implementing minimal "run this closure at end of block" semantics.

## Decisions Recorded

- User chose "Add regression test + update PRD" for §2.1 (nested fields).
- User chose "Update PRD to reflect reality" for §2.2 (method calls / UFCS); existing test coverage deemed sufficient.
- User chose "Add end-to-end index test + update PRD" for §3.2 (array indexing).
- User chose "Flip Status to COMPLETE" — the one remaining open item (`post`) is explicitly non-blocking.

## Tests Added / Extended

| File | Kind | Covers |
|---|---|---|
| `akkado/tests/test_codegen.cpp` — `Codegen: Record handling` (SECTIONs extended) | extended | Two-level and three-level nested record field access; verifies correct field is selected (not first) |
| `akkado/tests/test_codegen.cpp` — `Codegen: Array indexing` (new test case) | new | Constant index returns correct element (index 0, 1, 2); dynamic index emits `ARRAY_INDEX` opcode |

## Post-Finalize Validation

| Command | Result |
|---|---|
| `cmake --build build --target akkado_tests` | Pass |
| `./build/akkado/tests/akkado_tests "Codegen: Record handling"` | Pass (10 assertions / 1 case) |
| `./build/akkado/tests/akkado_tests "Codegen: Array indexing"` | Pass (8 assertions / 1 case) |
| `./build/akkado/tests/akkado_tests` | Pass (84557 assertions / 438 cases, +14 assertions / +1 case) |

## PRD Status

- Before: `TRACKING — Active blockers for MVP completeness.`
- After: `COMPLETE — All concrete MVP blockers resolved. Remaining items are informational.`

The only remaining informational item is §2.3 (Post statements), which the parser still accepts but codegen refuses; it is not a blocker because `post` is not used in any shipping patch.

## Recommended Next Steps

None required. All concrete blockers resolved, regression tests in place, suite green.
