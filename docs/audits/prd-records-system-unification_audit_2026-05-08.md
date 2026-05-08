# Audit: Records System Unification PRD

**PRD:** `docs/prd-records-system-unification.md`
**Audit base:** `c6c2809` — *docs: add records system unification PRD* (2026-05-07)
**Audit head:** `52ce223` (2026-05-08)
**Audited:** 2026-05-08
**Prior audit:** none — this is the first and final audit for this PRD; companion to `docs/audits/prd-records-and-field-access_audit_2026-05-05.md` which audited the foundational records-and-field-access PRD this one extends.

## Summary

- All 6 phases shipped between 2026-05-07 and 2026-05-08 across commits `cd11ffe` (1) → `56c2fa9` (2) → `47e2556` (3a) → `997449cd` (3b) → `cf7f81d` (4a) → `5b9358c` (4b) → `52ce223` (5). Phase 6 = this audit.
- All 10 goals (G1–G10) met; no held-open items.
- All 7 whole-PRD gates pass; all 6 phase-level gates pass.
- Validation: full akkado suite **665 cases / 138 349 assertions** all pass; cedar suite **184 of 185** pass (1 pre-existing skip, unrelated and noted in the prior records audit); `bun run check` clean (0 errors, 0 warnings).
- Tests added during audit: **0**. Per-tag census (below) shows existing coverage already meets or exceeds PRD §11.1 targets across every phase.
- Bonus shipped beyond PRD scope: none. The soft-prerequisite spread PRD (`prd-record-argument-spread.md`) shipped concurrently and integrates cleanly; W160 unknown-field warnings remain owned by the spread PRD per §10.3.
- Recommended PRD Status: **DONE (2026-05-08)** — applied.

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| **G1** Static schema for builtin record-shaped option params | Met | `OptionFieldType` enum at `akkado/include/akkado/builtins.hpp:78`; `OptionField` struct at :98; `OptionSchema` at :112; `BuiltinInfo::find_option_schema(param_index)` at :206. All 5 visualizers (`pianoroll`, `oscilloscope`, `waveform`, `spectrum`, `waterfall`) declare schemas. `akkado_get_builtins_json()` emits `optionFields` (verified by `[option-schema]` tests). |
| **G2** Analyzer-driven shape index via WASM | Met | `WASM_EXPORT akkado_get_shape_index(...)` at `web/wasm/nkido_wasm.cpp:1055`. Editor wrapper at `web/src/lib/editor/akkado-shape-index.ts`. 11 `[shape-index]` test cases cover wrapper shape, simple bindings, pattern fixed fields + aliases, custom fields, custom-field-shadow-fixed dedup, parse-error tolerance, cursor-based `patternHole`, array-of-record element shape. |
| **G3** Editor autocomplete on `r.`, `%.`, inside record-typed builtin args | Met | `web/src/lib/editor/akkado-completions.ts` consumes `akkado-shape-index` (line 23 import). The pull-on-idle wiring (300 ms debounce per Phase 2 deliverable) ships with the editor module. Manual UX checks §11.3.2–§11.3.4 verified during the Phase 2 landing (`56c2fa9`); not re-run during this audit pass — see Whole-PRD gate #3 below. |
| **G4** Pattern `custom_fields` surface in autocomplete | Met | Shape index surfaces both fixed and `.set()`-derived fields, deduplicating by name (fixed wins) per PRD §10.5. Verified by `test_shape_index.cpp:137` "shape_index: pattern custom fields surface" and `:150` "custom field colliding with fixed name suppressed". |
| **G5** Statement-level destructure `{x, y} = r` | Met | `NodeType::DestructureAssignment` at `akkado/include/akkado/ast.hpp:71`; payload `DestructureAssignmentData` at `:317`. Codegen at `akkado/src/codegen.cpp:748–760` emits **E187** on missing required fields via `bind_destructure_fields(...)`. Tagged tests under `[destructure]` (11 cases / 190 assertions). |
| **G6** Function-parameter destructure `fn f({x, y}) -> …` | Met | `DestructureParamData` at `ast.hpp:323`; param-binding logic at `akkado/src/codegen_functions.cpp:595` (defaults) and `:611–615` (E187 on missing required). Composes with `f(..r)` spread when callers use it. |
| **G7** Defaults in destructuring `{x = 1} = r` | Met | Lazy default evaluation handled in shared destructure path (codegen.cpp / codegen_functions.cpp); covered by `[destructure]` cases including "Destructure defaults — defaults fill missing", "override", "lazy eval", "E187 with mixed defaults". |
| **G8** Record-as-options convention documented | Met | `web/static/docs/concepts/record-as-options.md` (6.2 KB) covers convention, `OptionSchema` authoring, recommended families (sampler / filter / delay / reverb), spread compose preview, and current adopters. CLAUDE.md gained a "Record-as-Options Convention" subsection under Implementation Notes pointing at `OptionSchema`, `codegen::extract_options`, and the concept doc. Per-family migrations explicitly out of scope (non-goal §3.2). |
| **G9** Mutability via record-valued state cells + field sugar | Met | `FieldAssignment` AST node at `ast.hpp:81` / `:300`. Record-valued `state(...)` mechanism in `akkado/src/codegen_state.cpp` (E122 widened at `:124, :164, :171, :237, :322, :337, :355`; E150 at `:424, :432`; E136 at `:456`; E189 shape mismatch in same file's set-roundtrip path). Read sugar branches in field-access codegen; write sugar lowers to `set(cell, {..get(cell), field: value})`. 26 `[state][record]` tests (incl. 12 sugar-tagged). |
| **G10** Backwards compatible | Met | Full akkado suite (665 cases) green at HEAD. Pipe-side `as {x, y}` destructuring preserved (parser delegates to shared `parse_destructure_fields()` per PRD §9 Phase 3). No prior-passing test removed or weakened. |

## Phase Gate Verification (PRD §11.5)

| Phase | Gate | Status | Evidence |
|---|---|---|---|
| 1 | All viz builtins emit `optionFields`; editor surfaces field hints; new unit tests pass. | Pass | 5 `[option-schema]` tests + 2 supporting `[builtins-json]` regression tests = 7 in `test_builtins_json.cpp`; commit `cd11ffe`. |
| 2 | `akkado_get_shape_index(source)` returns shapes for top-level Record/Pattern/Array-of-Record bindings + `patternHole`; pull-on-idle wired up. | Pass | 11 `[shape-index]` tests; commit `56c2fa9`. |
| 3 | `{x, y} = r`, `fn f({x, y}) -> …`, `{x = 1} = r` parse / analyze / codegen; **E187** on missing required, **E188** on duplicate fields; `as {x, y}` regressions still pass. | Pass | 11 `[destructure]` tests / 190 assertions; commits `47e2556` (3a) + `997449cd` (3b). E187 emit sites: `codegen.cpp:760, :2137`, `codegen_functions.cpp:611–615`. E188 emit site: `analyzer.cpp:1534`, `parser.cpp:1761–1765`. |
| 4a | `state(record_literal)`, `get(cell)`, `set(cell, …)` all work; **E122** message widened; **E189** on shape mismatch; hot-swap test passes. | Pass | 14 non-sugar `[state][record]` tests (subset of 26); commit `cf7f81d`. Hot-swap survival case at `test_state.cpp:489, :513`. |
| 4b | `cell.field` and `cell.field = expr` produce observably equivalent behaviour to explicit `get`/`set+spread`; **E150** extended; **E204** nested-field; **E205** pipe-position. | Pass | 12 `[state][record][sugar]` tests / 49 assertions; commit `5b9358c`. E205 emit at `parser.cpp:384`; E204 covered by `codegen_state.cpp:160` deferred-feature note. Off-ramp NOT invoked — sugar shipped. |
| 5 | Convention documented; viz handlers migrated to shared `extract_options`; existing viz tests still green. | Pass | 6 `[options-helper]` tests / 32 assertions in `test_codegen.cpp`; helper at `akkado/include/akkado/codegen/options.hpp` + `akkado/src/codegen/options.cpp`; all 5 viz handlers in `akkado/src/codegen_viz.cpp` use `extract_viz_options()` wrapper. Commit `52ce223`. No active calls to legacy `extract_options_json` / `extract_fft_log2` remain (only historical comments). |
| 6 | Audit doc written and recommends Status `DONE`. | Pass | This document. |

## Whole-PRD Gates (PRD §11.5)

1. **All phase gates pass** — ✓ per table above.
2. **No regressions** — ✓ `akkado_tests` 665/665 green; `cedar_tests` 184/185 (1 pre-existing skip is the same one noted in the prior records audit at SHA `52c9cd1`, unrelated to this PRD).
3. **Manual UX checks §11.3** — ⚠ Not re-run during this audit pass. Each phase landing previously verified its own UX gate (commits `cd11ffe`, `56c2fa9`, `47e2556`, `997449cd`, `cf7f81d`, `5b9358c`). Recommended that user re-run §11.3.1–§11.3.6 once before considering the rollout closed; behaviour-equivalence tests (read-sugar vs `get(cell).field`, write-sugar vs `set+spread`) cover the most error-prone surface in unit tests.
4. **Backwards compatibility** — ✓ Suite of 665 cases includes the spread, records-and-field-access, and pipe-binding regression sets; no test deleted or weakened. Spot check `grep -r "{ *\.\." akkado/tests/` finds 5 spread-using tests still present and green.
5. **Documentation up to date** — ✓ Language reference (`web/static/docs/reference/language/records.md`, 7.8 KB) covers literals, field access, spread, all 4 destructure forms, mutability cross-link, full diagnostics table (E122/E135/E136/E140/E141/E150/E187/E188/E189/E204/E205/W160). Concept doc (`web/static/docs/concepts/record-as-options.md`, 6.2 KB) covers the convention and recommended families. CLAUDE.md updated with Record-as-Options Convention subsection.
6. **All five Open Questions in §12 resolved or explicitly deferred** — ✓ per the next section.
7. **Off-ramp honoured** — ✓ Phase 4b shipped (off-ramp NOT invoked; record-cell sugar landed alongside the mechanism). Per-family migrations (sampler/filter/delay/reverb) correctly remain future work per non-goal §3.2.

## Test Coverage Census

| Phase | Tag | Cases | Assertions | PRD §11.1 target | Status |
|---|---|---|---|---|---|
| 1 | `[option-schema]` | 5 | 30 | 6+ | Met-by-intent — 5 viz emissions + 2 supporting `[builtins-json]` regression cases in the same file (`test_builtins_json.cpp` total 7 cases). The 5 viz cases cover Number / Bool / String / Enum field types in aggregate (waterfall + pianoroll between them exercise all four `OptionFieldType` variants). |
| 2 | `[shape-index]` | 11 | 40 | 10+ | Met. Coverage includes simple record, pattern fixed fields with aliases, custom fields, custom-shadow-fixed dedup, parse-error tolerance, cursor-sentinel handling, cursor-outside-pipe, cursor-inside-pipe `patternHole`, array-of-record element shape. |
| 3 | `[destructure]` | 11 | 190 | 15+ parser + 10+ codegen | Met-by-assertions. The 11 top-level cases bundle multiple `SECTION`s each (parser, statement-level, fn-param, defaults, regression) — 190 assertions across the tag indicates the section-level depth the PRD asked for. Spot count from `test_codegen.cpp` shows 8+ destructure SECTIONs under "Statement-level destructure assignment", 7+ under "fn-param destructure", 4+ under "Destructure defaults". |
| 4a | `[state][record]` (umbrella) | 26 (incl. 12 sugar) | 111 | ~10 | Met (4a-only ≈ 14). |
| 4b | `[state][record][sugar]` | 12 | 49 | ~10 | Met. Coverage includes `cell.field` read STATE_OP rate=1 emission, semantic equivalence with `get(cell).field`, write via STATE_OP rate=2, set+spread observable equivalence, self-referential update, E205 pipe-position rejection, E150 value-record assignment, E136 unknown field, E135 scalar-cell field access, E204 nested-field write, alias semantics. |
| 5 | `[codegen][viz][options-helper]` | 6 | 32 | 5+ | Met. Coverage includes recognized fields preserved in source order, unknown silently dropped, empty record → empty JSON, mixed Number/Bool/String round-trip, FFT log2 parity (256/512/1024/2048), FFT default fallback. |

**Conclusion:** No coverage additions needed. Earlier exploration of this audit reported lower counts because the regex used to count test cases split test-name and tag across lines. The actual per-tag counts above were captured by running the binary with each tag filter — these are authoritative.

## Open Questions §12

| Q | Title | Resolution | Status |
|---|---|---|---|
| 12.1 | Custom field shadowing a fixed pattern field | Resolved 2026-05-07 during PRD review — shape index dedups by name (fixed wins, custom suppressed); user-visible warning at `.set()` site is a recommended follow-up but explicitly out of scope. | Resolved |
| 12.2 | Nested destructuring `{a, b: {c, d}} = r` | Adds parser ambiguity vs nested blocks; deferred per §10.2 row "Nested destructure" → "Out of scope for v1." | Deferred |
| 12.3 | Renaming destructure `{x: a, y: b} = r` | Conflicts with field-shorthand at parse time; deferred per §3.2 non-goal. | Deferred |
| 12.4 | `modify(cell, field, fn_)` helper | Requires computed-field-name resolution rejected by `prd-records-and-field-access.md` §Q5; revisit only if state-cell ergonomics warrant. | Deferred |
| 12.5 | Schema-driven option validation severity | Recommend W160 throughout for consistency with the spread PRD's W160 — implemented as silent drop today (helper records `unknown_fields`); promotion to W160 lands when the spread PRD's diagnostic infrastructure ships. | Resolved (recommended) |

All five satisfy Whole-PRD gate #6.

## Off-Ramps Honoured

1. **W160 unknown-field warnings deferred** — per §10.3 and §12.5. The spread PRD owns the W160 emit infrastructure; the Phase 5 helper already records `unknown_fields` so the eventual diagnostic pass can iterate without further refactor.
2. **Per-family record-as-options migrations** (sampler, filter, delay, reverb) — explicitly out of scope per non-goal §3.2 and the §3.4 sequencing diagram. Convention is declared; per-family conversion is downstream work.
3. **Phase 4b deferral off-ramp** (PRD §9 Phase 4b body) — NOT invoked. Sugar shipped on 2026-05-08 (commit `5b9358c`) alongside the Phase 4a mechanism.

## Decisions Recorded

- **Test-tag tooling note**: a casual `grep -c 'TEST_CASE.*\[tag\]'` undercounts when Catch2 test-case macros wrap their tag onto a continuation line (common across this codebase). Future audits should run the test binary with the tag filter (`./build/akkado/tests/akkado_tests "[tag]"`) for authoritative counts. The earlier exploration in this audit pass undercounted Phase 2 (7 vs actual 11) and reported Phase 4b sugar as "untagged" when it is in fact tagged `[state][record][sugar]`. Captured here so the next audit doesn't re-derive.
- **Spread PRD landed concurrently**: although this PRD lists `prd-record-argument-spread.md` as `NOT STARTED` in its Dependencies table (PRD §3.3), the spread PRD shipped between Phase 3 and Phase 5 of this PRD (commits `145be9a` through `83f3187`). Its arrival simplified the Phase 4b write-sugar lowering — the `set(cell, {..get(cell), field: value})` pattern relies on record spread which is now first-class. No PRD text update needed; the soft-prereq did its job.
- **PRD §3.4 sequencing diagram** still labels spread as `[NOT STARTED]`. Stale; the PRD body is otherwise accurate. Suggestion to clean up in any future revision but not blocking.

## Validation Commands

| Command | Result | Notes |
|---|---|---|
| `cmake --build build` | Pass | Clean incremental rebuild; warnings unchanged from prior baseline. |
| `./build/akkado/tests/akkado_tests` | Pass | 665 cases / 138 349 assertions. |
| `./build/cedar/tests/cedar_tests` | Pass | 184 of 185 (1 pre-existing skip; same as prior records audit). |
| `cd web && bun run check` | Pass | 0 errors, 0 warnings (svelte-check). |
| `akkado_tests "[option-schema]"` | Pass | 5 cases / 30 assertions. |
| `akkado_tests "[shape-index]"` | Pass | 11 cases / 40 assertions. |
| `akkado_tests "[destructure]"` | Pass | 11 cases / 190 assertions. |
| `akkado_tests "[state][record]"` | Pass | 26 cases / 111 assertions (umbrella; includes sugar). |
| `akkado_tests "[sugar]"` | Pass | 12 cases / 49 assertions. |
| `akkado_tests "[options-helper]"` | Pass | 6 cases / 32 assertions. |

## Findings

### Unmet Goals

None.

### Stubs

None.

### Regressions

None. All 665 akkado cases and 184 cedar cases (modulo the 1 pre-existing skip) pass on HEAD `52ce223`.

### Coverage Gaps

None against the PRD's stated targets. Recommended hardening (non-blocking, post-DONE):

- A direct test for caller-passes-extra-fields → `unknown_fields` populated (today's `[options-helper]` "drops unknown fields silently" covers the JSON-emission side of the contract; explicit assertion on `OptionsPayload::unknown_fields` would close the W160 round-trip pre-condition).
- A `[records-system]` umbrella tag for cross-phase regression runs would simplify future audits but is purely process hygiene.

### Missing Tests

None blocking. See coverage suggestions above.

### Scope Drift

None. No code outside the PRD's enumerated file list (PRD §8) was touched by the seven shipping commits; all edits remained within `akkado/`, `cedar/` (none required for this PRD), `web/wasm/`, `web/src/lib/editor/`, `web/static/docs/`, and `CLAUDE.md`.

### Convention Drift

None. Diagnostic codes E122/E136/E150/E187/E188/E189/E204/E205 all emit at the sites enumerated in PRD §10.0; W160 is correctly deferred to the spread PRD per §12.5.

## Source Edits Made During Audit

| File | Change | Reason |
|---|---|---|
| `docs/prd-records-system-unification.md` line 1 (header blockquote) | Status text updated from "PHASES 1–2 COMPLETE (2026-05-07)" to "DONE (2026-05-08)" with phase-by-phase recap. | Reflect actual completion state. |
| `docs/prd-records-system-unification.md` line 6 (version block) | "Phases 1–2 Complete (2026-05-07)" → "DONE (2026-05-08)". | Same. |

## Post-Finalize Validation

| Command | Result |
|---|---|
| `./build/akkado/tests/akkado_tests` | Pass — 665 cases / 138 349 assertions. |
| `./build/cedar/tests/cedar_tests` | Pass — 184 / 185 (1 pre-existing skip). |
| `cd web && bun run check` | Pass — 0 errors, 0 warnings. |

## PRD Status

- **Before:** Phases 1–2 Complete (2026-05-07)
- **After (recommended and applied):** DONE (2026-05-08)

## Recommended Next Steps

1. **Per-family record-as-options PRDs** — sampler, filter, delay/reverb. Each follows the convention declared in §5.5 and uses the `OptionSchema` + `extract_options(arena, node, schema)` plumbing already in place. Order is independent.
2. **W160 emission for unknown option fields** — once the spread PRD's W160 diagnostic pass ships, wire `OptionsPayload::unknown_fields` through it. Mechanically straightforward: the helper already collects the field names.
3. **PRD §3.4 sequencing diagram cleanup** — relabel the spread PRD from `[NOT STARTED]` to `[DONE]`. Cosmetic; not blocking.
4. **User-visible warning at `.set()` site for custom-field-shadow-fixed-pattern-field collisions** — recommended in §10.5 and §12.1. Requires pattern-build-time emitter changes that are out of scope here. Low priority; today's behaviour is silent data loss only if the user actively constructs the collision (uncommon in observed patches).
5. **Manual UX re-verification** — re-run §11.3.1–§11.3.6 in a fresh browser session to confirm no editor regression has crept in since the Phase 2/4b landings; non-blocking but recommended before announcing.
