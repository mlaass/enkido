# Audit: Scale & Key — Note Quantization and Degree Mapping

**PRD:** `docs/prd-scale-quantize.md`
**Audit base:** `74832d0` (2026-05-23 — commit introducing the PRD)
**Audit head:** `4e588a5` (2026-06-22)
**Audited:** 2026-07-04

The implementation shipped 2026-05-24 as parent-PRD commits
(prd-runtime-event-transforms Phase 5): `13de2a5` (Commit B — stdlib
bundle wiring), `14db122` (Commit C — scales.ak dispatcher catalog +
generator), `8c19539` (Commit F — stdlib `key` + `scale`), refactor
`405be8f`.

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| 1. Two builtins `scale`/`key` with §4 semantics | Met (with accepted deviations) | `akkado/stdlib/scale_quantize.ak` (generated `fn key`, `fn scale`); pipe + method forms verified via `akkado --check` |
| 2. Full tonal.js catalog, generated | **Descoped, accepted** | Curated 8 types × 12 roots; `web/scripts/generate-scale-quantize.ts:88` (`SCALES`), compile-cost rationale in `8c19539` |
| 3. User-defined `(root, intervals)` form | Unmet — Phase 4, scheduled next | — |
| 4. Patternable argument | Deferred by design (PRD §11.8) to `prd-pattern-array-transforms.md` | — |
| 5. Lowering (corrected scope: `event_map`, no new opcode) | Met | `scale_quantize.ak` branches call `event_map`; no Opcode enum change (`cedar/include/cedar/vm/instruction.hpp` untouched) |
| 6. §4/§8 behaviors covered by tests | Met after audit | `akkado/tests/test_scale_quantize.cpp` — 15 test cases (9 pre-audit, 6 added) |

## Findings

### Unmet Goals

- **`key` octave digit silently no-oped.** PRD §4.3/§8 promise the
  octave digit is "accepted and ignored"; shipped `fn key` had zero
  octave-carrying branches, so `key(@, "d2:minor")` fell through to
  `_: events` (identity) with no warning. — **resolved: FIXED.**
  `generate-scale-quantize.ts` now emits octave aliases (octaves 2–4)
  for every key name; regression test
  "key: octave digit in the name is accepted and ignored".
- **Full tonal.js catalog** (Goal 2) — **resolved: descope accepted.**
  8-type curated catalog stays; rationale: the embedded stdlib re-parses
  on every `akkado::compile()` and catalog size is per-compile cost.
  Rarer scales route to the Phase 4 user-defined form.
- **Multi-word colon convention** (`"c:harmonic:minor"`, PRD §3) —
  **resolved: accepted.** Shipped convention is underscores
  (`"c:harmonic_minor"`); PRD §3 updated.
- **`scale` octave range** — PRD implied any octave digit; shipped
  supports 2–4 (+octave-less → 3). **resolved: accepted**, documented in
  PRD Implementation Record and the docs page.
- **MIDI 0..127 clamp on results** (PRD §8) — not implemented;
  `overlay_event_field` EVENT_OUT_NOTE
  (`cedar/include/cedar/opcodes/event_transforms.hpp:129`) writes
  unclamped. **resolved: accepted as-is** — unclamped is the general
  `event_map` note-write contract; PRD row superseded.
- **E184/E185 error codes** (PRD §7) — never implemented; unknown name →
  identity pass-through (pinned by test). **resolved: accepted** — matches
  the live-coding coerce-don't-fail philosophy; PRD §7 rewritten. E185/E186
  were meanwhile assigned to unrelated features, so Phase 4 allocates a
  fresh code.

### Stubs

None found (no TODO/FIXME/placeholder markers in the shipped files).

### Coverage Gaps

- Fractional degree via `scale` (PRD §8 "d=2.7 → 3") — **resolved: test
  added** ("scale: fractional degree rounds to the nearest integer degree").
- Fractional/microtonal note via `key` (round-half-up boundary) —
  **resolved: test added** ("key: fractional input note rounds half-up
  before quantizing", exercises the exact .5 boundary).
- Negative degree below the root (PRD §4.4 floor-division arithmetic) —
  **resolved: test added** ("scale: negative degree maps below the root",
  d=−1 → MIDI 48).
- `key` octave-alias behavior — **resolved: regression test added** (see
  Unmet Goals).

### Missing Tests

- PRD Phase 1 "Generator unit test; spot-check ~10 scales vs tonal.js" —
  **resolved: extended the generator's `spotCheck()`** to pin all 8
  interval sets against tonal.js values and validate all 96 key-delta
  tables (minimality, in-scale-zero, tie→lower invariants). The script
  hard-fails on mismatch, so every regeneration is a test run.
- PRD Phases 2/3 "≥300 s WAV" renders — **resolved: two ≥300 s trace
  tests added** (`[scale-quantize][long]`): a chromatic sweep asserting
  every emitted note stays in the A-minor pitch-class set for 300
  simulated seconds, and an alternating-cycle degree map asserting the
  exact expected note set. Failures report block index + simulated time.

### Scope Drift

- `akkado/include/akkado/builtins.hpp` was promised (`BuiltinInfo`
  entries) but never touched — `scale`/`key` shipped as plain stdlib
  `fn`s. Consequence: **no editor autocomplete** (builtins JSON only
  covers `BuiltinInfo`). **Accepted**; F1 discoverability restored via
  the new docs page keywords. Exposing stdlib fns to autocomplete is a
  general follow-up (also affects `voice`/`invert`/`swing`/`unison`).
- Extra file `web/scripts/generate-scale-quantize.ts` (second generator,
  not in the PRD file list) — benign, accepted.
- §4.8's helper decomposition (`snap_to_scale`, `degree_to_note`,
  `parse_scale_root`, `parse_scale_intervals`) was replaced by generated
  match branches + shared `snap`/`pc12`/`key_q`/`scale_q` fns — accepted,
  recorded in the PRD Implementation Record.

### Convention Drift

None (no documented rule violated).

### Suggestions

- Stdlib-fn autocomplete exposure (own small PRD).
- Flat root spellings (`"eb:minor"`) currently fall through to identity;
  the docs page calls this out. Could ship as generator aliases if users
  trip on it.

## Decisions Recorded

| Decision | Outcome |
|---|---|
| Catalog size | Keep curated 8 types; full tonal.js rejected (compile cost) |
| `key` octave digit | Fix (generator octave aliases) |
| Underscore names + octave 2–4 | Accept shipped surface |
| MIDI clamp | Accept unclamped (general event_map contract) |
| Unknown name → identity | Accept (live-coding philosophy), keep test |
| Autocomplete | Accept gap; docs page + F1 keywords now; stdlib-autocomplete as follow-up |
| Generator test | Extend `spotCheck()` self-check |
| Long renders | Add ≥300 s trace tests |

**Implementation note (2026-07-04):** the octave-alias fix initially
inflated the embedded stdlib 94 KB → 141 KB and slowed `[drift_fuzz]`
7.5 s → 10.1 s (+33% compile cost). Re-factoring the generated dispatchers
to delegate to shared `key_q(events, deltas)` / `scale_q(events, rm, k,
ivals)` fns (branches = short literal-arg calls; the literal-only
constraint binds the match *scrutinee*, not arithmetic delegation)
shrank the file to 72.5 KB and cut `[drift_fuzz]` to 4.2 s — **44%
faster than the pre-audit baseline**, with the octave aliases included.

## Tests Added / Extended

| File | Kind | Covers |
|---|---|---|
| `akkado/tests/test_scale_quantize.cpp` | extended (+6 test cases) | key octave-alias regression; fractional degree; fractional note round-half-up; negative degree; 300 s key chromatic sweep; 300 s scale degree map |
| `web/scripts/generate-scale-quantize.ts` `spotCheck()` | extended | All 8 interval sets vs tonal.js; all 96 key-delta tables (minimality, zero-iff-in-scale, tie→lower) |

## Test Run

| Test | Result |
|---|---|
| `akkado_tests "[scale-quantize]~[long]"` (13 cases) | Pass |
| `akkado_tests "[scale-quantize][long]"` (2 × 300 s, 225k assertions) | Pass |
| `akkado_tests "[drift_fuzz]"` | Pass (4.2 s, down from 7.5 s) |
| `bun run scripts/generate-scale-quantize.ts` self-check | Pass |
| Full `akkado_tests "~[long]"` | Pass (see companion commit) |

## PRD Status

- Before: `IN FLIGHT — corrected scope as of 2026-05-24`
- After: `PARTIAL — phases 1–3 shipped 2026-05-24; phase 4 (user-defined
  scales) remaining; phase 5 moved to prd-pattern-array-transforms
  (audited 2026-07-04)`

## Recommended Next Steps

- Stdlib-fn autocomplete exposure (follow-up PRD, out of this audit's
  scope).

## Addendum (same day): Phase 4 shipped

Immediately after the audit, Phase 4 (user-defined `(root, intervals)`
scales, PRD §4.6) was implemented per user design decisions: root as
note-name string or MIDI number; `key`'s quantize table computed at
compile time by a new `key_deltas` C++ builtin
(`akkado/src/codegen_arrays.cpp` `handle_key_deltas_call`,
`akkado/include/akkado/music_theory.hpp` `compute_key_deltas`,
const-eval mirror in `akkado/src/const_eval.cpp`); **no interval
validation** (coerce mod 12). Required one core codegen change:
transitive param-literal propagation (`resolve_param_literal_in`, three
recording sites in `codegen_functions.cpp`) so literals fold through
stdlib wrapper fns. Covered by 5 new tests in
`test_scale_quantize.cpp` (string/number roots, custom-key tie-break,
octave-ignored equivalence, malformed-root `E203`). Docs page and PRD
updated; full suite green (see final test run in session log).
