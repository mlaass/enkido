# Audit: Akkado Builtin & Function Overload Resolution PRD

**PRD:** `docs/prd-builtin-overload-resolution.md`
**Audit base:** `67bafa46` (PRD introduced, 2026-06-08)
**Audit head:** `0e8a0a6` (Phase 5, 2026-06-20) + audit fixes (working tree)
**Audited:** 2026-06-21

The PRD shipped as five phases (`f449af9` → `0e8a0a6`), all marked
`ALL PHASES DONE`. The implementation is clean — no stubs/TODOs in any
changed file, and the full `akkado_tests` suite was green before the audit
(321561 assertions / 1193 cases). The audit's job was coverage: do the
passing tests actually exercise what the PRD promised? Four gaps surfaced;
all four were closed during the audit (one of them a real, untested bug).

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| 1. One declarative dispatch model for builtins, operators, user fns | Met | `overload.hpp/.cpp`; `make_builtin_pattern`, `lookup_operator_overloads`, `lookup_builtin_overloads`, `make_user_fn_pattern` + `resolve` |
| 2. First-match, declaration-order resolution with coercion as matching | Met | `overload.cpp:300` `resolve()`, `matches_arg` via `type_compatible`; `test_overload.cpp` resolve/order tests |
| 3. Literal-value guards (`String == "ms"`) | Met (resolver-only, descoped consumer) | `ArgMatcher::{StringLiteral,NumberLiteral}` + `matches_arg` (`overload.cpp:205`), unit-tested; no live codegen consumer — the `delay(sig,"ms",t)` form was descoped by product decision (documented in status block) |
| 4. Single enforcement path making `param_types` live for every builtin | Met (after audit fix) | Single-pattern builtins resolve via `make_builtin_pattern`+`matches_arg` (`codegen.cpp:1409/1695`). Custom-handled builtins enforce via their LegacyHandler guards (transport E133, poly E423, **midi E425 — added by this audit**) |
| 5. Migrate all ad-hoc overload handlers to declarative patterns | Met | `lookup_builtin_overloads` routes pan/pingpong/smooch/delay*/sample + poly/each/transport/midi; ad-hoc `if (name==…)` ladders retired |
| 6. No-match diagnostics that print the closest candidate signature | Met (after audit fix) | `closest_candidate()` (`overload.cpp:261`) now surfaced in the user-fn E424 message (`codegen_functions.cpp`) — added by this audit |

## Findings

### Unmet Goals

- **midi `{Record}` `param_type` was silently unenforced** (Goal 4 / §10).
  `midi(440)` and `midi("x")` compiled with **zero diagnostics** — the
  `{Record}` annotation was decorative, exactly the gap the PRD claimed to
  close. midi routes through `LegacyHandler::Midi` (never calls `resolve()`),
  and `handle_midi_call` fed any options node straight into
  `extract_options`, which silently returns an empty payload for a
  non-`RecordLit` (`codegen/options.cpp:120`) → opened the default device.
  No test caught it. **Resolved:** added a type guard in `handle_midi_call`
  (`codegen_patterns.cpp`) emitting **E425** on a non-record options arg
  (hard error, consistent with transport's E133) + regression test.

- **§9.1 overload shadowing warning was never implemented.** The PRD
  promised "a W-class shadowing warning emitted on by default when an earlier
  pattern makes a later one unreachable." No such code existed — only the
  unrelated match-expr "Unreachable pattern after wildcard" warning
  (`analyzer.cpp`). A user-fn `f(x: Signal)` before `f(x: Number)` left the
  Number form silently dead. **Resolved:** added `pattern_subsumes()`
  (`overload.cpp`) + a `detect_shadowed_overloads()` analyzer pass emitting
  **W171**, on by default, user-source overloads only (stdlib excluded), with
  the polyphonic-pattern escape honoured + tests.

### Stubs

- None. No TODO/FIXME/placeholder markers in any changed file.

### Coverage Gaps

- **§4.4 / §5.3 closest-candidate line was computed but never surfaced.**
  `closest_candidate()` was implemented and unit-tested in isolation, but the
  emitted E424 messages ignored it — they listed all candidates without the
  "closest: … — not coercible" line, and `out("x")` (the §10 example) emits
  the legacy per-slot E160, not a multi-candidate message at all. **Resolved:**
  the user-fn E424 now appends `; closest: name(sig) — argument N (T) is not
  coercible to U` from `resolve()`'s `closest_index`/`failures`
  (`codegen_functions.cpp`) + an end-to-end assertion on the message text.

- **§10 "hot-swap determinism test with overloaded names" was absent.** The
  Phase-4 commit did not touch `test_hot_swap_determinism.cpp`; determinism
  was covered only indirectly by the redefine-replace bytecode test.
  **Resolved:** added a recompile-twice byte-identical-bytecode test over a
  three-overload program (`test_overload.cpp`), matching the Phase-4 as-built
  reframe (hot-swap = fresh atomic recompilation).

### Missing Tests

- **§10 "midi `{Record}` … a mismatch test for each" was absent.** Upgraded to
  an Unmet Goal above once the underlying enforcement was found missing; the
  test now exists alongside the E425 fix.

### Scope Drift

The PRD's §7 File-Level Changes table diverged from the as-built diff. All of
this is reconciled in the PRD's status block (the migration approach moved from
per-family handler edits to a central `overload.cpp` registry), so it is
intentional, not accidental:

- **PRD-named files NOT touched:** `builtins.hpp` (pattern lists synthesized in
  `overload.cpp`, not declared on `BuiltinInfo`), `codegen_stereo.cpp` /
  `codegen_patterns.cpp` (families routed via `lookup_builtin_overloads`,
  handlers unchanged for Phase 3), `diagnostics.cpp` (E424 reused, no new code
  minted).
- **Files touched NOT in the PRD table:** `codegen.hpp`, `symbol_table.hpp/.cpp`
  (Phase-4 `overloads` vector + `DefineFunctionResult`), `codegen_arrays.cpp`,
  `const_eval.cpp` (overload-aware readers), `akkado/CMakeLists.txt`.
- **Audit fixes add:** `analyzer.hpp/.cpp` (W171 pass), `codegen_patterns.cpp`
  (E425 guard), `overload.hpp/.cpp` (`pattern_subsumes`),
  `codegen_functions.cpp` (closest-candidate surfacing).

### Convention Drift

- None. No documented project rule (CLAUDE.md) is violated by the diff.

### Suggestions

- The `closest_candidate()` result is still not surfaced on the **sample**
  multi-pattern E424 path (`codegen.cpp:1311`), whose message already names the
  two expected id forms — adequate, but it could share the user-fn formatting
  for consistency. Non-blocking.
- Literal-value matchers (`StringLiteral`/`NumberLiteral`) remain resolver-only
  with no live codegen consumer (Goal 3, descoped by product decision). If a
  future builtin wants literal-unit dispatch, the matchers are ready.

## Decisions Recorded

- midi `{Record}`: user chose **fix the guard + regression test** (hard error,
  consistent with transport). The decorative `param_type` is now live.
- §9.1 shadowing: user chose **implement now + test** (W171, warn-don't-fail).
- §4.4 closest-candidate: user chose **wire it + test** (surfaced in user-fn
  E424).
- §10 hot-swap test: user chose **add an explicit determinism test**.
- PRD Status: user chose **leave 'ALL PHASES DONE' as-is + a one-line audit
  note**.
- Report location: `docs/audits/` (existing convention).

## Tests Added / Extended

| File | Kind | Covers |
|---|---|---|
| `akkado/tests/test_overload.cpp` | new case | midi non-record options arg rejected (E425); valid record + bare midi() unaffected |
| `akkado/tests/test_overload.cpp` | 3 new cases | `pattern_subsumes`: Signal-shadows-Number, Signal-not-shadows-Pattern (poly escape), Any-shadows + arity ranges |
| `akkado/tests/test_overload.cpp` | 2 new cases | W171 fires on a coercing-shadow overload pair; does NOT fire on non-shadowing / poly-escape pairs |
| `akkado/tests/test_overload.cpp` | new case | E424 names the closest candidate + the failing argument (§4.4) |
| `akkado/tests/test_overload.cpp` | new case | recompiling an overloaded program is byte-identical (hot-swap determinism) |

## Source Changes Made During Audit

| File | Change |
|---|---|
| `akkado/include/akkado/overload.hpp` / `src/overload.cpp` | New `pattern_subsumes()` — lattice-decided overload shadowing predicate (honours coercion + polyphonic escape) |
| `akkado/include/akkado/analyzer.hpp` / `src/analyzer.cpp` | New `detect_shadowed_overloads()` pass (W171) + coded `warning(code,…)` overload |
| `akkado/src/codegen_functions.cpp` | User-fn E424 now appends the closest-candidate signature + failing-slot reason |
| `akkado/src/codegen_patterns.cpp` | `handle_midi_call` rejects a non-record options arg with E425 |

## Test Run

| Test | Result |
|---|---|
| `akkado_tests "[overload]"` | Pass (457 assertions, 55 cases — was 47) |
| `akkado_tests` (full suite) | Pass (321595 assertions, 1201 cases — was 1193) |

## PRD Status

- Before: `ALL PHASES DONE (2026-06-20).`
- After: unchanged value; appended a one-line audit note to the status block
  recording that the midi `{Record}` enforcement, W171 shadowing warning, and
  closest-candidate surfacing gaps were closed on 2026-06-21.

## Recommended Next Steps

None blocking. All six goals are Met; all four findings closed with tests; full
suite green. Optional polish only (see Suggestions).
