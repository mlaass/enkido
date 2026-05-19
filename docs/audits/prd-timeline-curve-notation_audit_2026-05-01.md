# Audit: Timeline Curve Notation PRD

**PRD:** `docs/prd-timeline-curve-notation.md`
**Audit base:** `4977c77217d47b5a9eb1c1c5c37c6cb77e7b3625` (2026-04-24)
**Audit head:** `HEAD` (2026-05-01)
**Audited:** 2026-05-01

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| 1. Compile curve notation to TIMELINE breakpoints at compile time | Met | `akkado/src/codegen_patterns.cpp:2718` — `events_to_breakpoints()` converts curve events to breakpoints, emits `TIMELINE` opcode at line 2747/2850 |
| 2. Integrate with existing mini-notation features (grouping, alternation, modifiers) | Met | `akkado/src/mini_parser.cpp` — curve atoms parsed and work with subdivision `[]`, alternation `<>`, repeat `*`, slow `/`, weight `@`, replicate `!`, chance `?` (tested in `test_mini_notation.cpp:1966+`) |
| 3. Always output 0.0-1.0 (user scales with math) | Met | `akkado/src/codegen_patterns.cpp:2718` — `events_to_breakpoints()` produces 0-1 values; user math happens after (`test_codegen.cpp:6166+`) |
| 4. Feel natural alongside existing `n"…"`/`v"…"`/`s"…"`/`c"…"` and `seq()` patterns | Met | `akkado/src/parser.cpp:1152+` — `t"..."` prefix follows same pattern as `n"..."`; `timeline()` function call also works |

## Validation Commands

| Command | Result | Notes |
|---|---|---|
| `./build/akkado/tests/akkado_tests` | Pass (137428 assertions in 552 test cases) | Full akkado suite green (+13 tests from audit) |
| `./build/akkado/tests/akkado_tests "[curve_lexer]"` | Pass (41 assertions in 8 test cases) | Curve lexer tests |
| `./build/akkado/tests/akkado_tests "[timeline_codegen]"` | Pass (25 assertions in 8 test cases) | Timeline codegen tests |
| `./build/akkado/tests/akkado_tests "[timeline_e2e]"` | Pass (8 assertions in 4 test cases) | Timeline end-to-end tests |
| `./build/akkado/tests/akkado_tests "[timeline_edge_cases]"` | Pass (18 assertions in 8 test cases) | New edge case tests added |
| `./build/cedar/tests/cedar_tests` | Pass (334377 assertions in 167 test cases) | Cedar suite green |

## Findings

### Unmet Goals
None. Phase 5 (Documentation) completed during audit — `web/static/docs/reference/mini-notation/curve-notation.md` created and docs index rebuilt.

### Stubs
None found. All curve-related code paths in `mini_lexer.cpp`, `mini_parser.cpp`, `pattern_eval.cpp`, and `codegen_patterns.cpp` are fully implemented.

### Regressions
None. All validation commands pass on HEAD.

### Coverage Gaps
All PRD edge cases now have test coverage. Gaps identified during audit were closed by adding 8 new test cases in `test_codegen.cpp` (section `[timeline_edge_cases]`).

### Missing Tests
None. All PRD-named test sections (§8.1–§8.7) have corresponding test code.

### Scope Drift

PRD's File-Level Changes table (§7) lists these files to modify:
- `akkado/include/akkado/mini_token.hpp` — **modified** ✓
- `akkado/src/mini_lexer.cpp` — **modified** ✓
- `akkado/include/akkado/mini_parser.hpp` — **modified** ✓
- `akkado/src/mini_parser.cpp` — **modified** ✓
- `akkado/include/akkado/ast.hpp` — **modified** ✓
- `akkado/include/akkado/pattern_event.hpp` — **modified** ✓
- `akkado/src/pattern_eval.cpp` — **modified** ✓
- `akkado/src/codegen_patterns.cpp` — **modified** ✓
- `akkado/include/akkado/token.hpp` — **modified** ✓
- `akkado/src/lexer.cpp` — **modified** ✓
- `akkado/src/parser.cpp` — **modified** ✓
- `akkado/include/akkado/builtins.hpp` — **modified** ✓

All PRD-promised files were touched. No scope drift detected.

### Convention Drift
None. Code follows existing patterns in the mini-notation pipeline. No `print()` statements, no hardcoded paths, no violations of documented conventions.

### Suggestions

- Consider adding a `t"..."` entry to `web/static/docs/reference/language/` for the string prefix.
- The `events_to_breakpoints()` function at `pattern_eval.cpp:684+` could benefit from additional unit tests for edge cases (ramp value resolution with no preceding/following level).

## Decisions Recorded

| Finding | Decision |
|---|---|
| Documentation (Phase 5) | **Completed** — `curve-notation.md` created, docs index rebuilt |
| Coverage gaps (6 edge cases) | **Closed** — 8 new test cases added to `test_codegen.cpp` |

## Tests Added / Extended

| File | Kind | Covers |
|---|---|---|
| `akkado/tests/test_codegen.cpp` | added 8 test cases | `[timeline_edge_cases]` section |
| → `Empty curve string produces no breakpoints` | new | PRD §6.1 — empty curve produces empty breakpoints |
| → `Single-character curve ' produces single breakpoint` | new | PRD §6.2 — `t"'"` single breakpoint |
| → `Single-character curve _ produces single breakpoint` | new | PRD §6.2 — `t"_"` single breakpoint |
| → `Ramp at start interpolates from 0.0 to next level` | new | PRD §6.3 — ramp at start behavior |
| → `Ramp at end defaults to 0.0` | new | PRD §6.4 — ramp at end behavior |
| → `~ on ramp produces compile error` | new | PRD §6.7 — `~` on ramp error |
| → `Many levels produce correct breakpoint count` | new | Edge case — 65 levels = 65 breakpoints |
| → `Breakpoint limit warns and truncates via codegen` | new | PRD §6.11 — `MAX_BREAKPOINTS` truncation |

## Post-Finalize Validation

| Command | Result |
|---|---|
| `./build/akkado/tests/akkado_tests "[timeline_edge_cases]"` | Pass (18 assertions in 8 test cases) |
| `./build/akkado/tests/akkado_tests` | Pass (137428 assertions in 552 test cases) |
| `bun run build:docs` (web/) | Success — docs index rebuilt with 35 documents |

## PRD Status

- Before: `NOT STARTED`
- After: `COMPLETE` — all phases implemented, tests added, documentation written

## Recommended Next Steps

None — PRD is complete. Future improvements can be tracked as separate feature requests.
