# Audit: Strudel-Style Pattern System Extensions

> **Update 2026-04-27:** The unmet goals identified by this audit (Phases 4, 5, 7, 8) were closed by the Phase 2 PRD `docs/prd-pattern-array-note-extensions-phase-2.md`, shipped 2026-04-27. One sub-piece (§5.5a custom-property pipe-binding accessor) remains as a known follow-up; see the Phase 2 PRD status line.

**PRD:** `docs/prd-pattern-array-note-extensions.md`
**Audit base:** `4977c77217d47b5a9eb1c1c5c37c6cb77e7b3625` (rename commit; no earlier add commit found for this exact path)
**Audit head:** `8fc1e05c81ca10d27bde68e6519bc65deb4f9859`
**Audited:** 2026-04-24
**Mode:** Read-only bulk audit

## Summary
- Goals met: 4 of 8 (phases 1, 3, 6, 2-via-desugaring done; phases 4, 5, 7, 8 incomplete)
- Critical findings: 11 (Unmet=4, Stubs=2, Coverage Gaps=2, Missing Tests=3)
- Recommended PRD Status: `PARTIAL` (matches existing status; no change needed)
- One-line verdict: Foundational phases (arrays/map, chords, polymeter, dot-call) ship and are tested; voicing (Phase 4), extended note properties (Phase 5: bend/aftertouch), and most time/structure modifiers (Phase 7) plus algorithmic generators `run`/`binary`/`binaryN` (Phase 8) are not implemented.

## Follow-Up

All four unmet goals (Phases 4, 5, 7, 8) are now scoped in [`docs/prd-pattern-array-note-extensions-phase-2.md`](../prd-pattern-array-note-extensions-phase-2.md). The findings, suggestions, and missing tests below feed into that follow-up PRD; no further action against the original PRD is expected — its status table has been updated to defer Phases 4/5/7/8 to Phase 2.

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| Phase 1: Arrays + `map()` foundation (ArrayLit, map, indexing, len) | Met | `akkado/include/akkado/ast.hpp:24` (ArrayLit), `akkado/src/parser.cpp:609-629` (parse_array), `akkado/src/codegen_arrays.cpp:291-320,727,1116` (map codegen), `akkado/src/codegen.cpp:262-300` (Index codegen), `akkado/include/akkado/builtins.hpp:694` (len), `akkado/include/akkado/builtins.hpp:701` (map). Tests: `akkado/tests/test_parser.cpp:821` `[parser][array]`, `akkado/tests/test_chord.cpp:247,336,362,382` `[array][map]`, `akkado/tests/test_codegen.cpp:140,283` |
| Phase 2: Pattern objects & method chaining (dot-call) | Met via desugaring | `akkado/src/parser.cpp:991-1027` (parse_method_call producing NodeType::MethodCall), `akkado/src/analyzer.cpp:732-733,1021-1022,1098-1157` (desugar_method_call → Call). `akkado/src/codegen.cpp:1511-1513` errors for un-desugared MethodCall (expected — analyzer rewrites it). PRD explicitly records this as resolved in §8. No separate `pattern_object.hpp` file created (consistent with PRD's resolved note). String-as-pattern parsing is NOT implemented (explicit typed pattern literals / bare StringLit accepted in pattern slots only — see `akkado/src/codegen_patterns.cpp:1407,1455-1456`). |
| Phase 3: Chord system (Strudel-compatible) | Met | `akkado/include/akkado/chord_parser.hpp:23,35`, `akkado/src/chord_parser.cpp:8-58` (quality table), `:106-149` (parse_chord_symbol), `:151-169` (expand_chord), `:171-205` (parse_chord_pattern). `akkado/include/akkado/builtins.hpp:819-822` `chord()` builtin. Tests: `akkado/tests/test_chord.cpp:12,102,138,167,201,451,479,494,517,537,545,552`. Note: chord expansion in audio graph now requires `poly()` wrapper (enforced with E410 errors) — this is a hard constraint added after PRD was written but consistent with chord spirit. |
| Phase 4: Voicing system (`anchor`, `mode`, voice leading, voicing dictionaries) | Unmet | No `akkado/include/akkado/voicing.hpp`, no `akkado/src/voicing.cpp`. No `anchor`/`mode`/`addVoicings` entries in `akkado/include/akkado/builtins.hpp`. PRD §8 confirms this is deferred. |
| Phase 5: Extended note properties (velocity, aftertouch, pitch bend, extensible) | Partial | `akkado/include/akkado/pattern_event.hpp:42` has `velocity` but no `bend` / `aftertouch` / `dur` override fields. Velocity transform implemented: `akkado/src/codegen_patterns.cpp:2202` handle_velocity_call; velocity suffix `c4:0.8` lexed: `akkado/src/mini_lexer.cpp:484,547,618` and tested: `akkado/tests/test_mini_notation.cpp:1532,1575`. `.bend()`, `.aftertouch()`, `.dur()` methods/transforms absent. |
| Phase 6: Polymeter (`{x y}` and `{x}%n`) | Met | Tokens: `akkado/include/akkado/mini_token.hpp:36-37,47`. Lexer: `akkado/src/mini_lexer.cpp:308-309,319`. AST: `akkado/include/akkado/ast.hpp:50,105,224-225`. Parser: `akkado/src/mini_parser.cpp:268-269,441-469`. Evaluator: `akkado/src/pattern_eval.cpp:128-129,304-311,580`. Tests: `akkado/tests/test_mini_notation.cpp:252,428,438,448,766,782,803`, `akkado/tests/test_ast_arena.cpp:572`. |
| Phase 7: Time & structure modifiers | Unmet (mostly) | Implemented only: `slow`, `fast`, `rev`, `transpose`, `velocity`, `bank`, `variant`, `tune` (`akkado/include/akkado/builtins.hpp:832-863`, handlers in `akkado/src/codegen_patterns.cpp:1947,2008,2069,2129,2202,2349,2479,2818`). NOT implemented: `early`, `late`, `palindrome`, `iter`, `iterBack`, `ply`, `linger`, `zoom`, `compress` (as pattern-transform), `segment`, `swingBy`, `swing` — no builtin entries, no handlers. |
| Phase 8: Algorithmic generators | Partial | `euclid` present: `akkado/include/akkado/builtins.hpp:684` (cedar opcode EUCLID) and `(k,n)` mini-notation form inside patterns. NOT implemented: `run(n)`, `binary(n)`, `binaryN(n, bits)` — no builtin entries, no constructors. |

## Findings

### Unmet Goals
- **Phase 4 Voicing system** — No `voicing.hpp`/`voicing.cpp`. `anchor()`, `mode()`, voice leading, and `addVoicings()` are entirely absent. PRD §8 labels this deferred.
- **Phase 7 Time & structure modifiers** — 12 of the 12 new transforms in the PRD table (`early`, `late`, `palindrome`, `iter`, `iterBack`, `ply`, `linger`, `zoom`, `compress`, `segment`, `swingBy`, `swing`) have no builtin registration and no handler. The PRD marks this "IN PROGRESS" but no work is visible in the tree.
- **Phase 8 generators** — `run()`, `binary()`, `binaryN()` absent from builtins and codegen.
- **String-as-pattern (try-parse) auto-detection** — Bare strings are NOT auto-parsed as patterns in arbitrary expression positions. Pattern transform handlers accept `StringLit` nodes in pattern argument slots (`akkado/src/codegen_patterns.cpp:1407,1455-1456`), but there is no global "try mini-notation first" behavior and no `str("...")` escape. PRD §8 lists this as deferred.

### Stubs
- **`C4'` standalone syntax** — PRD §3 calls it "Deprecated Stub (root only)". ChordLit AST node still exists (`akkado/include/akkado/ast.hpp:23`) and lexer still produces chord tokens (`akkado/tests/test_lexer.cpp:292`, `akkado/tests/test_mini_notation.cpp:1318,1403`). Acceptable per PRD, but the "root only" stub path should be verified or removed.
- **Phase 5 extended note properties** — `velocity` is real; `bend`, `aftertouch`, `dur` exist only as field-name mentions in `pattern_event_fields`-style handling and/or are not surfaced at all. PatternEvent lacks storage fields for bend/aftertouch (`akkado/include/akkado/pattern_event.hpp:29-55`).

### Coverage Gaps
- **Phase 7 transforms have zero coverage** — No test files mention `early`, `late`, `palindrome`, `ply`, `linger`, `zoom`, `segment`, `swing`, `swingBy`, `iter`, `iterBack`. Expected: none, since feature is absent; flagging as a future-TODO.
- **Voicing system** — No tests reference `anchor`, `.mode(`, voice leading, or `addVoicings`.

### Missing Tests
- No tests for `run()`, `binary()`, `binaryN()` — these are listed as deliverables for Phase 8.
- No tests for `.bend()`, `.aftertouch()`, `.dur()` pattern transforms (Phase 5).
- No explicit integration test for `chord("Am C F G") |> poly(mtof, ...)` end-to-end playback audition (acceptance criterion under §7). Chord tests instead focus on E410 error emission when `poly()` is missing (`test_chord.cpp:201-545`). One integration path exists at `test_chord.cpp:517` `chord integration with audio graph requires poly()` but is error-oriented, not success-oriented.

### Scope Drift
- **`poly()` requirement for chord expansion** — Expansion to multi-voice requires an explicit `poly()` wrapper (see `akkado/tests/test_chord.cpp` E410 tests). This is stricter than the PRD's stated auto-expand sugar (§2.1 "`osc(chord)` → N oscillators summed"). The effective model is: arrays auto-expand but chord pattern events only expand under `poly()`. This is an intentional design change post-PRD (tracked in `prd-polyphony-system.md`) but the PRD itself was not updated to reflect it — minor drift.
- **`tune()` transform** — Implemented (`akkado/src/codegen_patterns.cpp:2818`) but not mentioned in the PRD's time/structure modifier table. Out-of-scope addition.

### Suggestions
1. Update PRD §3 status table: mark Phase 2 as "Complete via dot-call desugaring (no pattern_object.hpp needed)" for clarity; currently only §8 says so.
2. Either implement one more Phase 7 modifier (e.g., `early`/`late`, which are pure time-shift compile transforms) to justify "IN PROGRESS" status, or downgrade Phase 7 to "Not started" in the status table.
3. Add `run(n)` since it's the simplest Phase 8 constructor and would unblock idiomatic Strudel patterns.
4. Document the `poly()` requirement directly in PRD §2.1 (auto-expand sugar) to resolve scope drift with `prd-polyphony-system.md`.
5. Add positive (audio-producing) end-to-end test for `chord()` + `poly()` pipeline that asserts correct MIDI→freq output per voice.
6. Flesh out PatternEvent with optional `bend`, `aftertouch`, `dur_override` fields to unblock Phase 5 follow-up work.

## PRD Status
- Current: `PARTIAL — Phases 1, 3, 6 done. Phase 2 mostly resolved via dot-call desugaring. Phase 7 (time/structure modifiers) and Phase 8 (algorithmic generators) are unblocked and in progress. Phase 4 (voicing) deferred. Phase 5 (extended note properties) partially done.`
- Recommended: `PARTIAL` (unchanged) — with the caveat that Phase 7 should be flagged "Not started" rather than "in progress" until the first new modifier lands, and the Phase 8 status should be narrowed to "euclid only".
- Reason: The PRD-documented status is essentially accurate. Phases 1, 3, 6 are fully shipped and tested. Dot-call desugaring resolves Phase 2 architectural needs. Phases 4 (voicing), 5 (bend/aftertouch), 7 (11 new modifiers), and 8 (run/binary) remain unimplemented. No evidence found in the tree of partial work toward Phase 7 modifiers or Phase 8 generators beyond what already existed (`euclid`), so "in progress" overstates current state.
