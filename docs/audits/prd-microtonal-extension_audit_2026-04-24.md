# Audit: Akkado Microtonal Extension

**PRD:** `docs/prd-microtonal-extension.md`
**Audit base:** `4977c77217d47b5a9eb1c1c5c37c6cb77e7b3625`
**Audit head:** `8fc1e05c81ca10d27bde68e6519bc65deb4f9859`
**Audited:** 2026-04-24
**Mode:** Read-only bulk audit

## Summary
- Goals met: 12 of 17 (2 partial, 3 unmet)
- Critical findings: 6 (Unmet=3, Stubs=0, Coverage Gaps=2, Missing Tests=1)
- Recommended PRD Status: `Phase 1-3 Implemented (partial); Aliases & JI/BP Deferred`
- One-line verdict: Core microtonal pipeline (lexer `^`/`v`/`+`, `micro_offset` threading, `tune("Nedo", pattern)`, SoundFont fractional-MIDI fix) is implemented and tested; however the `d` alias, `\` alias, numeric-argument shorthand (`^4`, `v2`), and the JI/BP presets listed in Section 6.1 are not implemented — the PRD's "NOT STARTED" banner is stale.

## Goal Verification
| Goal | Status | Evidence |
|---|---|---|
| G1. Extended pitch atom: note + modifier stream + octave | Met | `akkado/src/mini_lexer.cpp:513-566` parses modifier stream before octave digit |
| G2. `^` Step Up operator (`micro_offset+1`) | Met | `akkado/src/mini_lexer.cpp:526` |
| G3. `v` Step Down operator (`micro_offset-1`) | Met | `akkado/src/mini_lexer.cpp:527` |
| G4. Numeric argument form `^n`, `vn` | Unmet | Only stacking (`^^^^`) supported; digit after operator is consumed as octave (`akkado/src/mini_lexer.cpp:520-541`) |
| G5. Alias `+` (micro up) | Met | `akkado/src/mini_lexer.cpp:528` |
| G6. Alias `d` (quarter-flat micro down) | Unmet | Not in `lex_pitch` modifier loop (`akkado/src/mini_lexer.cpp:521-530`); ambiguity rule from PRD Section 4.2 not implemented |
| G7. Alias `\` (alt micro down) | Unmet | Not handled in `lex_pitch`; `\` is tokenised as `CurveRamp` elsewhere (`akkado/src/mini_lexer.cpp:241-243`) |
| G8. Alias `x` (double sharp `accidental_std+2`) | Met | `akkado/src/mini_lexer.cpp:525` |
| G9. `is_accidental()` extended to new operators | Partial | `MiniLexer::is_accidental()` at `akkado/src/mini_lexer.cpp:84-86` still only recognises `#` and `b`; PRD Section 4.4 step 1 required extending it. `lex_pitch` reimplements the modifier set in-line so behaviour is correct, but the helper is now semantically stale |
| G10. `MiniPitchData.micro_offset` field | Met | `akkado/include/akkado/mini_token.hpp:96` |
| G11. `MiniAtomData.micro_offset` field | Met | `akkado/include/akkado/ast.hpp:197` |
| G12. Codegen Hz conversion uses tuning context + micro_offset | Met | `akkado/src/codegen_patterns.cpp:316-317` calls `tuning_.resolve_hz(atom_data.midi_note, atom_data.micro_offset)` |
| G13. `tune()` compile-time scope modifier | Met | Builtin at `akkado/include/akkado/builtins.hpp:860-863`; pre-compile hook at `codegen_patterns.cpp:1485-1494`; handler at `codegen_patterns.cpp:2817-2870` |
| G14. `TuningContext` struct + `parse_tuning()` | Met | `akkado/include/akkado/tuning.hpp:15-31`; `akkado/src/tuning.cpp:7-33` |
| G15. Built-in presets: 12/17/19/22/24/31/41/53 EDO + JI + BP | Partial | `parse_tuning()` accepts any `Nedo` string generically (all EDOs listed work), but `ji` and `bp` are **not** implemented — `TuningContext` exposes only `edo_count` (`tuning.hpp:15-26`); JI ratio-array mode from PRD 5.2 has no data structure |
| G16. SoundFont microtonal fix (fractional MIDI for pitch, rounded for zone lookup) | Met | `cedar/include/cedar/opcodes/soundfont.hpp:75-78` keeps `midi_note` as float, rounds only for `note`; `soundfont.hpp:127-131` uses fractional `midi_note` for `pitch_cents` |
| G17. Backward compatibility (standard `c#4`, `Bb5` unchanged) | Met | Regression test at `akkado/tests/test_mini_notation.cpp:1668-1684`; default `TuningContext{12}` at `codegen_patterns.cpp:799` |

## Findings

### Unmet Goals
- **`d` alias (quarter-flat micro down):** Section 4.2 lists `d` → `micro_offset -1` and documents the ambiguity rule (`cd4` parses as c + d-alias at octave 4). Neither is implemented — `cd4` would be lexed as `c` (no modifier, default octave) followed by `d4`. See `akkado/src/mini_lexer.cpp:521-530`.
- **`\` alias (alt micro down):** Not handled inside `lex_pitch`. The existing `CurveRamp` use of `\` at `akkado/src/mini_lexer.cpp:241-243` would also need context-sensitive disambiguation.
- **Numeric operator arguments (`^n`, `vn`):** Section 3.3 promises `^4` == `^^^^`. Current loop at `akkado/src/mini_lexer.cpp:521-541` increments by one per character and then treats the next digit as the octave. So `c^2` yields `micro_offset=+1, octave=2`, not `micro_offset=+2, octave=4`.

### Stubs
None. Implemented features are functional, not stubbed.

### Coverage Gaps
- **`is_accidental()` helper not extended** (`akkado/src/mini_lexer.cpp:84-86`): PRD Section 4.4 step 1 required this. Current behaviour is correct only because `lex_pitch` rewrites the modifier set in-line; the helper is semantically stale and any future refactor/caller could silently miss `^ v + x`. Latent defect.
- **No JI / Bohlen-Pierce tuning data:** PRD Section 6.1 lists `ji` and `bp` as shipping presets. `parse_tuning()` returns `nullopt` for both. `TuningContext` has no ratio-array or non-octave field, so PRD Section 5.2's "JI ratio-array traversal" resolution rule is un-buildable with the current type.

### Missing Tests
- **End-to-end Hz verification through `tune()` pipeline:** `test_mini_notation.cpp:1845-1872` only checks compile success/failure for `tune("24edo", ...)` and `tune("31edo", ...)`. No test inspects `CompilationResult.sequences[0].events[i].values[0]` to confirm the emitted Hz actually matches the expected microtonal frequency. Unit tests for `TuningContext::resolve_hz` exist in isolation (`test_mini_notation.cpp:1755-1797`), but coverage of the full codegen path is absent.
- **No SoundFont microtonal playback tests:** PRD Phase 3 test plan asked for "quarter-tone pitches through SF player" verification. No such test found under `cedar/tests/` or `akkado/tests/`.
- **No lexer tests for deferred aliases** (`d`, `\`, `^n`) — correct since they aren't implemented, but once they ship the test cases from PRD Section 4.3 (`cbd4`, numeric forms) should be added.

### Scope Drift
None. Implementation stayed within the PRD's "compile-time resolution, runtime unchanged" philosophy. No new Cedar opcode was introduced; `Sequence::Event`, `OutputEvent`, and `PolyAllocState` are unchanged as promised in Section 2.

### Suggestions
1. Update `MiniLexer::is_accidental()` to recognise the full modifier set per PRD Section 4.4 step 1, or add a comment clarifying that `lex_pitch` is now the source of truth.
2. Either implement the remaining aliases (`d`, `\`) and numeric shorthand (`^n`/`vn`), or amend the PRD to mark them explicitly as "Future" — currently Section 4.2 / 3.3 lists them without deferral, which overstates shipped scope.
3. Add an integration test that compiles `tune("24edo", n"a4 a^4")` and asserts the two emitted event frequencies are `~440.0` Hz and `~453.08` Hz (50-cent step).
4. If JI / BP are wanted in MVP, extend `TuningContext` with an optional ratio-array path and teach `parse_tuning` to return a JI/BP context; otherwise drop them from Section 6.1's shipping list.

## PRD Status
- Current: `Draft / Planned` (Section 0 frontmatter) / `NOT STARTED` (top-of-file banner)
- Recommended: `Phase 1-3 Implemented (partial); Aliases & JI/BP Deferred`
- Reason: The "NOT STARTED" banner is clearly stale — lexer, `TuningContext`, `tune()` builtin, codegen wiring, and the SoundFont fractional-MIDI fix are all present with passing tests for the implemented subset. However, the PRD materially over-promises vs. what shipped: `d` / `\` aliases, `^n` numeric shorthand, and JI/BP presets are not implemented. Relabel the PRD to reflect partial completion and file a follow-up for the missing items, or demote those items to a "Future" section.
