# Audit: Records and Field Access in Akkado

**PRD:** `docs/prd-records-and-field-access.md`
**Audit base:** `14ee738505a069cd0045cdd2a1d18be86177e12d` (PRD's first-add under former `PRD-RECORDS-AND-FIELD-ACCESS.md`; current path added at `4977c77` as a rename)
**Audit head:** `8fc1e05c81ca10d27bde68e6519bc65deb4f9859`
**Audited:** 2026-04-24
**Mode:** Read-only bulk audit

## Summary
- Goals met: 4 of 5 (Goal 1 partially — only 5 of the documented pattern event fields are implemented)
- Critical findings: 4 (Unmet=0, Stubs=0, Coverage Gaps=2, Missing Tests=2)
- Recommended PRD Status: **Shipped** (with follow-up issue for extended pattern fields §3.1/§3.2)
- One-line verdict: MVP scope is implemented and tested; only the extended pattern fields set (dur/chance/time/phase/voice/note/sample/sample_id) is missing — PRD Status "Draft" is stale (file header already says "DONE").

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| G1: Enable `%.field` on pattern event data | Partial (core fields only) | Parser `akkado/src/parser.cpp:554-588` handles `%.field`. Codegen dispatches via `pattern_field()` in `akkado/src/typed_value.cpp:44-56` — only supports `freq`/`pitch`/`f`, `vel`/`velocity`/`v`, `trig`/`trigger`/`t`, `gate`/`g`, `type` (`akkado/include/akkado/typed_value.hpp:38-60`). Codegen test `akkado/tests/test_codegen.cpp:2882-2885`, parser test `akkado/tests/test_parser.cpp:1352-1381`. Fields `note/midi/n`, `sample/s`, `sample_id`, `dur/duration`, `chance`, `time/t0/start`, `phase/cycle/co`, `voice` listed in PRD §3.1/§3.2 are NOT exposed. |
| G2: Record literals `{field: value, …}` | Met | Parser `akkado/src/parser.cpp:1489-1562` (incl. empty record, trailing comma, duplicate detection). AST nodes `akkado/include/akkado/ast.hpp:67-71, 254-275`. Analyzer collects record types `akkado/src/analyzer.cpp:236-306`. Codegen via `RecordPayload` at `akkado/include/akkado/typed_value.hpp:62-65`. Tests: `akkado/tests/test_parser.cpp:1221-1295`, `akkado/tests/test_codegen.cpp:2533-2610`. |
| G3: Field access via `expr.field` | Met | Parser (postfix `.`) + `parse_method_expr` path. Codegen dispatches in `handle_field_access` at `akkado/src/codegen.cpp:1860-1988`. Supports nested `a.b.c.d` (tests at `akkado/tests/test_codegen.cpp:2554-2610` — triply nested). |
| G4: Compile-time expansion (no runtime record type) | Met | `TypedValue::record` is a `shared_ptr<RecordPayload>` populated at compile time (`typed_value.hpp:90`). `handle_field_access` resolves to a concrete buffer index at compile time — no runtime record dispatch in VM. No RECORD_* opcodes exist in Cedar. |
| G5: Backwards compatibility | Met | Bare `%` path preserved in `parse_hole()` (`parser.cpp:583-585`) — `HoleData::field_name` is `std::optional`, defaulting to `nullopt`. Existing closure form `seq("c4", (t, v, p) -> ...)` unaffected (closures untouched by these changes). Block-vs-record disambiguation documented in §8.2 and is enforced via the `identifier:` leadoff in `parse_record_literal`. |

### Spec sub-items (from Implementation Phases)

| Spec item | Status | Evidence |
|---|---|---|
| §2.1 Record literal (incl. empty, trailing comma, duplicate detection) | Met | `parser.cpp:1489-1562` (seen_fields dedup at :1525). Tests `test_parser.cpp:1221-1295`. |
| §2.2 Field access (simple, chained) | Met | `test_parser.cpp:1297-1350`, `test_codegen.cpp:2554-2610`. |
| §2.3 Hole field access `%.field` | Met | `parser.cpp:554-588`; `test_parser.cpp:1352-1381`; codegen `test_codegen.cpp:2882-2885`. |
| §2.4 Pipe binding `as` (incl. destructuring) | Met | `parser.cpp:377-410` (normal + `as {fields}`); analyzer `analyzer.cpp:1532-1624`; tests `test_parser.cpp:1431-1457`, `test_codegen.cpp:2714-2728`. |
| §2.5 Shorthand field `{x, y}` | Met | `parser.cpp:1545-1553`; tests `test_parser.cpp:1251-1286`. |
| §2.6 Field access vs method call | Met | Test `test_parser.cpp:1338-1349`; also UFCS desugar `analyzer.cpp:1098` (per MVP doc). |
| §6.5 Duplicate field detection | Met (parser-level) | `parser.cpp:1525-1527` emits "Duplicate field" — but uses a bare `error_at`, not the `E064` code defined in PRD §6.5. |
| §6.10 Pattern field on non-pattern pipe (`E065`) | Not implemented as specified | No occurrence of `E065` in `akkado/src` or `akkado/include`. Actual behavior: depends on path — if LHS is a scalar Signal, `handle_field_access` hits `E135` (`codegen.cpp:1968-1971`). Functionally catches the error, but with a different code/message than spec. |
| §6.3 `%.freq` outside any pipe (`E062`) | Met via different code | Test `test_codegen.cpp:2291-2300` confirms undefined-identifier path; bare `%` outside a pipe raises `E003` (`test_codegen.cpp:2917-2925`). No dedicated `E062` code exists. |
| §3.6 Default field for bare `%` (pitch/sample/note auto-detect) | Not implemented | No pattern-type auto-detection that chooses `%.freq` vs `%.sample_id` vs `%.note`. Bare `%` in a pipe targets the primary buffer of the pattern (which the codegen sets to a single concrete field). |
| §4.3 Pattern pipe transformation (detect record LHS, scan RHS) | Met (alternative mechanism) | Pattern is a first-class `TypedValue` (`ValueType::Pattern`) with a `PatternPayload` that carries field buffers directly; `%.field` routes to the matching `PatternPayload::fields[i]` buffer via `pattern_field()`. No AST-rewrite-to-`seq(closure)` transformation needed. |
| §6.7 Nested records (shadowing) | Met | Triply nested test confirms deep access (`test_codegen.cpp:2592-2610`). |
| §6.8 Records in arrays | Partially covered | No direct test of `[{x:0},{x:1}]` + `map` in `test_codegen.cpp`. `RecordPayload` nested inside `ArrayPayload` is supported by the type system (`typed_value.hpp:68-70`) but not explicitly exercised. |

## Findings

### Unmet Goals
None outright unmet. G1 is partial but the core scenario from the PRD motivating example (`n"c4 e4 g4" |> osc("sin", %.freq) * %.vel * ar(%.trig, 0.01, 0.1)`) works — only the extended fields table (§3.2) and unified event model (§3.3) fields are absent.

### Stubs
None.

### Coverage Gaps
1. **Extended pattern fields (PRD §3.1/§3.2/§3.3)**: `dur`, `chance`, `time`, `phase`, `voice`, `note`/`midi`/`n`, `sample`/`s`, `sample_id` are listed in the PRD but not in `pattern_field_index()` (`typed_value.cpp:30-42`). Accessing `%.dur` or `%.voice` would raise `E136`. This blocks the §3.4 per-voice routing example and §3.5 pitched-sample example from the PRD.
2. **Default field for bare `%` (§3.6)**: PRD specifies that bare `%` should default to `freq`/`sample_id`/`note` based on pattern type. Implementation instead uses a fixed "primary buffer" on `PatternPayload` set at pattern construction time (see `codegen.hpp:339-340`). Pattern-type auto-detection from content is not implemented as the PRD describes. Behavior is acceptable in practice but differs from spec.

### Missing Tests
1. **Records-in-arrays (§6.8)**: No test compiles `[{x:0,y:0},{x:1,y:1}]` and `map(points, p -> p.x + p.y)`. Worth adding.
2. **Per-voice routing examples (§3.4)**: No test for `%.voice` with `match` or `select` for per-voice routing. Would require adding the `voice` field first.
3. **Record equality not supported (§D4)**: No negative test asserting `r1 == r2` errors.
4. **`E064` duplicate-field diagnostic code**: Parser emits a message but not the documented `E064` code. No test asserts the code.

### Scope Drift
- **Record spreading `{..base, foo: 1}`** (PRD §Q4, marked "defer to future") — actually **implemented and tested** (`parser.cpp:1501-1506`; `test_codegen.cpp:4490-4681`). Not a bug but a PRD drift: update PRD §Q4 from "Defer" to "Shipped".
- **Match destructuring `match(r) { {a, b}: ... }`** is shipped (`test_codegen.cpp:2680-2728`) — PRD §Q2/§Q3 defer this. Minor drift.
- **UFCS / dot-call** (`x.method(args)` → `method(x, args)`) is implemented via `desugar_method_call` (`analyzer.cpp:1098`). PRD §2.6 says "Method Calls: parser differentiates based on parens" but does not mandate UFCS semantics; nevertheless, this is a material addition not in PRD scope.
- **Pipe binding `as` on non-record expressions** works as PRD §2.4 "Key Points" describes (`saw(440) as s |> …`), verified in `test_codegen.cpp:2887-2890`.

### Suggestions
1. Promote PRD Status from `Draft` to `Shipped` — the header line already says **"Status: DONE"**, which conflicts with the body's "Status: Draft". Fix the inconsistency.
2. File a follow-up issue for extended pattern fields (note, sample, sample_id, dur, chance, time, phase, voice). Either (a) implement by extending `PatternPayload::fields` + `pattern_field_index`, or (b) update the PRD to explicitly defer them.
3. Unify the diagnostic code used for "duplicate field" into `E064` and for "pattern field on non-pattern LHS" into `E065` to match PRD — currently the analyzer/codegen use `E131`/`E135`/`E136` and a raw parser error.
4. Add an explicit records-in-arrays integration test.
5. Document §3.6's actual behavior (fixed primary buffer rather than type-based field defaulting) or implement the auto-detection as specced.

## PRD Status

- **Current (body):** `Draft`
- **Current (header line 1):** `**Status: DONE**` (contradicts body)
- **Recommended:** `Shipped` (with noted follow-ups)
- **Reason:** All major phases (parser, analyzer, codegen, pattern-field wiring) are implemented. The `MVP-INCOMPLETE-IMPLEMENTATIONS.md` cross-reference confirms the key blockers (nested field access, UFCS) are resolved. The five pattern-event fields implemented (`freq/vel/trig/gate/type`) cover the motivating example from PRD §1.1. Remaining gaps (extended pattern fields, some diagnostic-code harmonization) are refinements rather than MVP blockers and should be tracked as follow-ups rather than keeping this PRD in Draft.
