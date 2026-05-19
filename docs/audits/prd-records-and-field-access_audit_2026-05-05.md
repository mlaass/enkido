# Audit: Records and Field Access in Akkado

**PRD:** `docs/prd-records-and-field-access.md`
**Audit base:** `14ee738` — *Add records and field access PRD* (2026-01-28)
**Audit head:** `52c9cd1` (2026-05-05)
**Audited:** 2026-05-05
**Prior audit:** `docs/audits/prd-records-and-field-access_audit_2026-04-24.md` (recommended Status "Shipped" with follow-ups; this audit revises that recommendation per user input)

## Summary

- Goals met: 4 of 5; G1 (`%.field` on pattern events) ships only the 5 fixed fields (`freq`, `vel`, `trig`, `gate`, `type`) plus aliases for the first three. The full set enumerated in PRD §3.1–§3.3 is not exposed.
- Critical findings: 2 unmet goals, 2 coverage gaps, 1 missing test set, plus minor diagnostic-code drift (E062/E063/E064/E065 absent).
- Tests added during audit: 0 (the unmet portion is being held open as remaining work, not closed in this pass).
- Bonus shipped beyond PRD scope: spread `{..base, …}` (PRD §Q4 deferred), `as {field, …}` destructuring binding (§Q2/Q3 deferred).
- Validation: `cmake --build build` clean; `akkado_tests` 572 cases / 137,636 assertions all pass; `cedar_tests` 184 of 185 pass (1 skipped, unrelated to this PRD); `akkado_tests "[records]"` 7 cases / 131 assertions all pass.
- Recommended PRD Status: **PARTIAL — §1–2, §4–6 done; §3 extended fields pending** (rolled back from `DONE`).

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| G1: Field access on pattern event data via `%.field` | **Partial** | Parser handles `%.field` at `akkado/src/parser.cpp:547-578`. Codegen dispatches via `pattern_field()` at `akkado/src/typed_value.cpp:44-62`. Implemented: `freq`/`pitch`/`f`, `vel`/`velocity`/`v`, `trig`/`trigger`/`t`, `gate`/`g`, `type`. **Missing**: `note`/`midi`/`n`, `sample`/`s`, `dur`/`duration`, `chance`, `time`/`t0`/`start`, `phase`/`cycle`/`co`, `voice`, `sample_id`, plus aliases `frequency` and `n`. Confirmed via `./build/tools/akkado-cli/akkado-cli --check` against each of `%.note`, `%.dur`, `%.chance`, `%.voice`, `%.sample_id`, `%.midi`, `%.time` — all error E136 with message *"Unknown field … Available: freq, vel, trig, gate, type"*. |
| G2: Record literals `{field: value, ...}` | Met | `akkado/src/parser.cpp:1503-1577` (incl. empty record, trailing comma, duplicate detection, shorthand `{x, y}`, spread `{..base, …}`). AST nodes `akkado/include/akkado/ast.hpp:66-67, 259-273`. Codegen `akkado/src/codegen.cpp:1959-2070`. Tests `akkado/tests/test_parser.cpp:1210-1284`, `test_codegen.cpp:3681-3759`. |
| G3: Field access on any record via `expr.field` | Met | Parser postfix path `akkado/src/parser.cpp:984-1037`. Codegen `akkado/src/codegen.cpp:2072-2208` dispatches by ValueType. Triply-nested test at `test_codegen.cpp:3740-3758`. |
| G4: Compile-time expansion (no runtime record type) | Met | `TypedValue::record` is a `shared_ptr<RecordPayload>` populated at compile time (`akkado/include/akkado/typed_value.hpp:90-92, 195-203`). `handle_field_access` resolves to a concrete buffer index — no RECORD_* opcodes exist in Cedar VM. |
| G5: Backwards compatibility | Met | `HoleData::field_name` is `std::optional` (`ast.hpp:281-284`); bare `%` path preserved. Closure form `seq("c4", (t,v,p) -> …)` untouched. Block-vs-record disambiguation via leading `identifier:` token enforced in `parse_record_literal` and exercised by polymeter test `test_codegen.cpp:2208-2222`. |

### Spec sub-items

| Spec item | Status | Evidence |
|---|---|---|
| §2.1 Record literal (empty, trailing comma, duplicate detection) | Met | `parser.cpp:1503-1577` (seen_fields dedup at :1540-1543). |
| §2.2 Field access (simple, chained) | Met | `test_parser.cpp:1286-1339`, `test_codegen.cpp:3697-3758`. |
| §2.3 Hole field access `%.field` | Met (for the 5 supported fields) | `parser.cpp:547-578`; `test_parser.cpp:1341-1370`. |
| §2.4 Pipe binding `as` | Met (incl. destructuring) | `parser.cpp:366-404`; `test_parser.cpp:1420-1450`; `test_codegen.cpp:4035-4038`. |
| §2.5 Shorthand field `{x, y}` | Met | `parser.cpp:1560-1568`; `test_parser.cpp:1240-1284`. |
| §2.6 Field access vs method call disambiguation | Met | `parser.cpp:984-1019` keys on `(`. Test `test_parser.cpp:1336`. |
| §3.1 Core pattern fields `trig/vel/freq/note/sample` + aliases | Partial | Only `trig/vel/freq` (+ short aliases) implemented; `note/sample` and full aliases (`frequency`, `n`) missing. |
| §3.2 Extended pattern fields `dur/chance/time/phase` | **Not implemented** | None present in `pattern_field_index()` (`typed_value.cpp:30-42`). |
| §3.3 Unified scalar event model fields (`type`, `voice`, `sample_id`) | Partial | `type` present; `voice` and `sample_id` absent. |
| §3.4 Polyphony per-voice routing via `match(e.voice)` | **Not implemented** | Design pivoted (see Decisions Recorded): polyphony is opt-in via `poly()` / `sampler()` wrappers; polyphonic patterns at scalar slots error E160 by design (`typed_value.hpp:58-65`). |
| §3.5 Mixed pattern handling via `e.type` / `e.sample_id` | Partial | `e.type` works; `e.sample_id` missing — the `match(e.type) { "sample": …, "pitch": … }` example cannot run as written. |
| §3.6 Default field for bare `%` (auto pitch/sample/note dispatch) | Not implemented as specified | Pattern carries a fixed primary buffer set at construction; no content-based pattern-type detection that switches between `%.freq`/`%.sample_id`/`%.note`. Functionally the user picks a typed prefix (`v"…"`, `n"…"`, `s"…"`, `c"…"`) per the patterns-as-scalars PRD; outcome is workable but spec mismatch. |
| §4.3 Pattern pipe transformation (rewrite to `seq(closure)`) | Met via alternative mechanism | Pattern is a first-class `TypedValue` with `PatternPayload` carrying field buffers; `%.field` routes to the matching buffer — no closure rewrite needed. |
| §6.1–§6.4 / §6.10 Diagnostic codes (E060/E061/E062/E063/E065) | Drift | E060/E061 are emitted as specified (`analyzer.cpp:1516, 1522`). E062 (`%.field` outside pipe), E063 (unknown pattern field), E064 (duplicate field), E065 (`%.field` on non-pattern pipe) are NOT present in source. Functionally caught via E135/E136/E140 with different messages. |
| §6.7 Nested records | Met | `test_codegen.cpp:3702-3758` (incl. triply nested). |
| §6.8 Records in arrays | Not directly tested | No test compiles `[{x:0}, {x:1}]` with `map(p, p.x)`. Type system supports `RecordPayload` inside `ArrayPayload`. |
| §Q4 Spread (deferred) | **Shipped beyond scope** | Parser supports `{..base, x: 1}` (`parser.cpp:1514-1521`); codegen `codegen.cpp:1966-2034`; 14 spread-related test sections (`test_codegen.cpp:5995-6193`). |
| §Q2/Q3 Destructuring (deferred) | **Partially shipped beyond scope** | `as {x, y}` pipe-side destructuring shipped (`parser.cpp:369-390`, `test_parser.cpp:780-808`); standalone `{x, y} = pos` and `fn distance({x, y})` remain absent. |

## Validation Commands

| Command | Result | Notes |
|---|---|---|
| `cmake --build build` | Pass | Incremental rebuild; all targets (cedar, akkado, tools, tests) link clean. |
| `./build/akkado/tests/akkado_tests` | Pass | 572 cases / 137,636 assertions. |
| `./build/cedar/tests/cedar_tests` | Pass | 184 of 185 (1 skipped, pre-existing, unrelated to this PRD). |
| `./build/akkado/tests/akkado_tests "[records]"` | Pass | 7 cases / 131 assertions tagged `[records]`. |
| `akkado-cli --check /tmp/test_*.akk` for `%.note`, `%.dur`, `%.chance`, `%.voice`, `%.sample_id`, `%.midi`, `%.time` | All error E136 | Confirms the gap below. |

## Findings

### Unmet Goals

1. **Extended pattern fields (PRD §3.1, §3.2, §3.3)** — `note/midi/n`, `sample/s`, `dur/duration`, `chance`, `time/t0/start`, `phase/cycle/co`, `voice`, `sample_id`, plus aliases `frequency` and `n`, are listed in the PRD as part of the pattern event record but are not in `pattern_field_index()` (`akkado/src/typed_value.cpp:30-42`). Compile-time access to any of them errors E136. **User decision:** PRD should not be marked DONE until these ship. Held open.

2. **Default field for bare `%` (PRD §3.6)** — auto-detection of pattern type (pitch / sample / note) and corresponding default field is not implemented. Held open under same decision.

### Stubs

None.

### Regressions

None. All passing tests on HEAD pass; no test that passed on `<base>` fails on HEAD.

### Coverage Gaps

1. **Missing pattern-field tests for documented fields** (`note`, `dur`, `chance`, `time`, `phase`, `voice`, `sample_id`). No test asserts the *current* error message for these (so a silent regression that drops `%.freq` to `%.f` would only be caught by adjacent tests). **Resolution:** accepted as un-tested for now — these will be implemented (Unmet Goal #1); writing tests for the error message would just have to be deleted. Reason: filed under Unmet Goal, not held open as a long-term gap.

2. **Records inside arrays (PRD §6.8)** — `[{x: 0, y: 0}, {x: 1, y: 1}]` + `map(points, p -> p.x + p.y)` has no direct codegen test. Type system supports it but it's not exercised. **Resolution:** accepted as un-tested — low risk and outside the held-open §3 work.

### Missing Tests

1. **Per-voice routing example (PRD §3.4)** — no test exists for `match(e.voice) { 0: …, 1: … }`. **Resolution:** classified as Decision Recorded (intentional pivot, see below), not as a missing test to write.

2. **Spec error codes E062, E063, E064, E065** — PRD §6 names these but no test asserts them (because they aren't emitted; E135/E136/E140 cover the cases instead). **Resolution:** accepted as un-tested — see Convention Drift below; if the codes are renumbered to match spec, tests would follow. Low risk.

### Scope Drift

- Diff since `14ee738` touches 580+ files / ~158,611 insertions across many features (patterns-as-scalars, soundfonts, sample banks, microtonal extension, viz revisions, etc.). The records-and-fields PRD's own scope landed alongside many unrelated PRDs on the same branch — that's expected for a 3-month window. No drift inside the records-and-fields code paths beyond what the PRD anticipated.
- Spread `{..base, …}` and `as {x, y}` destructuring shipped despite being deferred per PRD §Q2/Q4. Tracked under "Decisions Recorded" — useful additions, just not strictly in the original MVP scope.

### Convention Drift

- **Diagnostic code drift (PRD §6.1–§6.10)**: PRD specifies E060/E061/E062/E063/E064/E065. Implementation uses E060/E061 as specified, plus E135/E136/E140 in place of the rest. CLAUDE.md does not document a rule about diagnostic code allocation, so this is informational, not a hard rule break. Worth resolving if the PRD ever ships fully.

### Suggestions

- When extended pattern fields land, also add a one-line "Available fields:" generator that walks the same registry rather than the hard-coded `"freq, vel, trig, gate, type"` string in `available_fields()` (`typed_value.cpp:64-71`) — keeps error messages truthful as fields are added.
- Consider adding a `[records-extended]` test tag bucket so future field tests are runnable in isolation.

## Decisions Recorded

- **Polyphony model pivot (PRD §3.4 / §D2)**: User confirmed the design moved from "%.freq returns an array for chords + per-voice routing via `match(e.voice)`" to "polyphonic patterns reject scalar coercion (E160) and require explicit `poly()`/`sampler()` wrapping". `is_sample_pattern` and `max_voices` flags on `PatternPayload` (`typed_value.hpp:58-65`) implement the new model. Documented here so future readers don't try to revive the array-returning behavior. PRD §3.4 paragraph should be rewritten when the §3 follow-up lands.
- **Extended pattern fields held open**: Will be tracked as remaining work for the PRD; Status moves to PARTIAL rather than DONE until they ship.
- **`%.note` doc reference fix**: Two examples in `web/static/docs/reference/language/conditionals.md` (eq, neq) used `%.note` plus a variable named `note` (which shadows the `note()` builtin). Replaced the field with `%.freq`, the variable name with `freq`, and adjusted comparison values to keep the operator examples correct. Verified each compiles via `akkado-cli --check` (exit 0).

## Tests Added / Extended

| File | Kind | Covers |
|---|---|---|
| (none) | — | The gap is being held open as Unmet Goal, not closed in this audit pass. Tests will land with the §3 extended-fields implementation. |

## Source Edits Made During Audit

| File | Change | Reason |
|---|---|---|
| `web/static/docs/reference/language/conditionals.md:138-142` | `%.note` → `%.freq`; `note` → `freq`; `eq(note, 60)` → `eq(freq, 261.6)`; comment updated to `~261.6 Hz` | The published docs referenced a field that errors at compile time. |
| `web/static/docs/reference/language/conditionals.md:157-161` | `%.note` → `%.freq`; `note` → `freq` | Same. |

## Post-Finalize Validation

| Command | Result |
|---|---|
| `./build/akkado/tests/akkado_tests` | Pass — 572 cases / 137,636 assertions |
| `akkado-cli --check` of both updated doc snippets | Both exit 0 |

## PRD Status

- **Before:** `DONE`
- **After (recommended and applied):** `PARTIAL — §1–2, §4–6 done; §3 extended fields pending`

## Recommended Next Steps

1. Implement the extended pattern fields (`note`, `dur`, `chance`, `time`, `phase`, `voice`, `sample_id`) plus aliases `frequency` and `n` in `pattern_field_index()` and ensure `PatternPayload::fields[]` (or `custom_fields`) carries the data. Wire each field from the `PatternEvent` already produced by `codegen_patterns.cpp` (`duration`, `chance`, `time`, `velocity`, `num_values`, `values[]` are already there).
2. Update `available_fields()` to enumerate the full set so E136 messages stay truthful.
3. Add codegen tests asserting each new field round-trips an expected value through a small pattern.
4. Reconcile PRD §6 diagnostic codes with implementation (either implement E062/E063/E064/E065, or update the PRD to reflect E135/E136/E140).
5. Decide whether PRD §3.4 / §D2 should be rewritten to reflect the polyphony pivot, or whether per-voice routing comes back when the rest of §3 lands.
6. Once §3 ships fully, re-run this audit and consider flipping Status to `DONE`.
