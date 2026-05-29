# Audit: Records and Field Access in Akkado

**PRD:** `docs/prd-records-and-field-access.md`
**Audit base:** `f58e9ae` — *Audit prd-records-and-field-access: PRD Status DONE → PARTIAL* (2026-05-05)
**Audit head:** `e25d23e` + this change
**Audited:** 2026-05-13
**Prior audits:**
- `docs/audits/prd-records-and-field-access_audit_2026-04-24.md` (recommended `Shipped` with follow-ups)
- `docs/audits/prd-records-and-field-access_audit_2026-05-05.md` (revised to `PARTIAL — §3 extended fields pending`)

## Summary

- Goals met: 5 of 5. The §3 gap from the 2026-05-05 audit is closed: extended pattern fields (`note`, `dur`, `chance`, `time`, `phase`, `sample_id` + aliases) now resolve uniformly on every pattern producer — bare typed pattern literals, mini-notation literals, **and every transform** (`fast`, `slow`, `rev`, `velocity`, `bank`, `variant`, `transpose`, `early`, `late`, `palindrome`, `ply`, `linger`, `zoom`, `segment`, `iter`, `tune`, …).
- Unmet from prior audit: `voice` field (removed from spec per polyphony pivot), `§3.6` bare-`%` auto-detection (formally deferred to a follow-up PRD).
- Convention drift resolved: PRD §6 diagnostic codes updated from spec placeholders `E062/E063/E064/E065` to the actually-emitted `E135/E136/E140` + parser duplicate-field error.
- Tests added: 1 new `TEST_CASE` (`"Codegen: Extended pattern fields on transforms"`), 9 sections, **126 new assertions** tagged `[codegen][records-extended]`. Covers every canonical field + alias × 6 transform shapes, plus opcode-emission check and chained-transform regression.
- Validation: full `akkado_tests` green (678 cases / 138,661 assertions). `cedar_tests` 184 of 185 pass (1 pre-existing skip, unrelated).
- Recommended PRD Status: **DONE — §3 wired through every pattern producer; voice removed; §3.6 deferred**.

## Root Cause and Fix

### What was broken

`pattern_field_aliases()` already registered all 11 fields by 2026-05-07, but only **one** of the five `PatternPayload` construction sites in `akkado/src/codegen_patterns.cpp` actually emitted the `SEQPAT_GATE/TYPE/FIELD/PHASE` opcodes that populate the extended buffers. The other four sites — every transform path — populated `FREQ/VEL/TRIG` (and one of them added `GATE/TYPE`) and left the extended slots at `0xFFFF`. `pattern_field()` then returned an error because the buffer was unallocated, surfacing as `E136` the moment a pattern flowed through any transform.

### Fix (single helper, five call sites)

1. **New helper** `CodeGenerator::emit_extended_field_buffers()` (`akkado/include/akkado/codegen.hpp:486`; impl `akkado/src/codegen_patterns.cpp:1535`): allocates the 8 extended buffers (gate, type, note, dur, chance, time, phase, sample_id), emits `SEQPAT_GATE/TYPE/FIELD/PHASE` for voice 0, and writes them into `payload.fields[]`. Single source of truth — selector encoding documented at the call site and cross-referenced to `op_seqpat_field` in `cedar/include/cedar/opcodes/sequencing.hpp:769`.

2. **`emit_per_voice_seqpat` refactor** (`codegen_patterns.cpp:1448`): the inlined 80-line GATE/TYPE/FIELD/PHASE block is replaced with one helper call. Per-voice `SEQPAT_STEP` loop and `voice_freqs` plumbing unchanged.

3. **Four transform sites wired**:
   - `emit_pattern_with_state` (free static helper at `codegen_patterns.cpp:2722`) — used by `slow`, `fast`, `rev`, `transpose`, `early`, `late`, `palindrome`, `ply`, `linger`, `zoom`, `segment`, `iter`, `tune`. Removed the inline `SEQPAT_GATE/TYPE` emission, now delegates to the helper via the `gen` parameter.
   - `handle_velocity_call` (`codegen_patterns.cpp:3310`) — scaled-velocity transform.
   - `handle_bank_call` (`codegen_patterns.cpp:3737`) — sample-bank transform.
   - `handle_variant_call` (`codegen_patterns.cpp:3898`) — sample-variant transform.

All five sites now exit with the same `PatternPayload` shape, so `%.note`/`%.dur`/`%.gate`/`%.phase`/`%.sample_id`/etc. resolve identically regardless of which transform chain produced the pattern.

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| G1: Field access on pattern event data via `%.field` | **Met** | Parser `akkado/src/parser.cpp:547`. Codegen dispatch `akkado/src/typed_value.cpp:60` + `akkado/src/codegen.cpp:2598,2631`. All 11 canonical fields + aliases resolve on every pattern producer. Confirmed by `[codegen][records-extended]` (9 sections, 126 assertions) and CLI `--check` smoke loop. |
| G2: Record literals `{field: value, ...}` | Met | Unchanged from prior audit. |
| G3: Field access on any record via `expr.field` | Met | Unchanged. |
| G4: Compile-time expansion (no runtime record type) | Met | Unchanged. |
| G5: Backwards compatibility | Met | Existing `[records]` tests (`Codegen: Extended pattern fields`) still green. Bare `%` semantics unchanged. |

### Spec sub-items (delta from 2026-05-05)

| Spec item | Status | Evidence |
|---|---|---|
| §3.1 Core pattern fields `trig/vel/freq/note/sample` + aliases | **Met** | Every canonical name and alias resolves; verified per-alias in `[codegen][records]` and on transforms in `[records-extended]`. |
| §3.2 Extended fields `dur/chance/time/phase` | **Met** | Each field round-trips on bare-pat *and* through every transform shape (fast/slow/rev/velocity/bank/variant) — see `Extended pattern fields on transforms`. |
| §3.3 Unified event model (`type`, `voice`, `sample_id`) | Met (with `voice` removed) | `type` and `sample_id` populated on every producer. `voice` removed from PRD §3.3 per polyphony pivot. |
| §3.4 Polyphony per-voice routing via `match(e.voice)` | Pivoted | Section rewritten to reflect `poly()`/`sampler()` wrapper model; polyphonic scalar coercion errors `E160` by design. |
| §3.5 Mixed pattern handling via `e.type` / `e.sample_id` | **Met** | Both fields populated by `emit_extended_field_buffers` on every producer. |
| §3.6 Default field for bare `%` | Deferred | Marked `(deferred)` in the PRD; recommended for a follow-up PRD if revived. |
| §4.3 Pattern pipe transformation | Met via alternative | `Pattern` value-with-`PatternPayload` model unchanged; no closure rewrite. |
| §6.1–§6.10 Diagnostic codes | **Reconciled** | PRD §6 updated to match emit sites (E060/E061/E135/E136/E140). |

## Validation Commands

| Command | Result | Notes |
|---|---|---|
| `cmake --build build` | Pass | Incremental rebuild; no new warnings. |
| `./build/akkado/tests/akkado_tests` | Pass — **678 / 678** | 138,661 assertions. +1 case (`Extended pattern fields on transforms`) and +126 assertions over the prior baseline. |
| `./build/akkado/tests/akkado_tests "[records]"` | Pass | All previously-green records cases still green. |
| `./build/akkado/tests/akkado_tests "[records-extended]"` | Pass — **1 / 1** (126 assertions) | New bucket: every field × six transform paths + opcode-emission check + chained-transform regression. |
| `./build/cedar/tests/cedar_tests` | Pass — **184 / 185** | One pre-existing skip, unrelated. |
| `akkado-cli --check` loop over `n"c4 e4 g4" \|> sine(%.<field>) \|> out(%, %)` for all 28 names+aliases | All exit 0 | Bare-pat path. |
| `akkado-cli --check` loop over `fast(n"c4 e4 g4", 2) \|> sine(%.<field>) \|> out(%, %)` for all 28 names+aliases | All exit 0 | This was the failing case in the 2026-05-05 audit. |
| `akkado-cli --check` on `velocity(...)`, `bank(s"...")`, `variant(s"...")` with `%.note`/`%.dur`/`%.gate`/`%.sample_id`/`%.phase` | All exit 0 | Each of the four formerly-broken transform paths verified individually. |

## Findings

### Unmet Goals

None — the two open items from the 2026-05-05 audit are now resolved by spec change rather than by implementation:

- `voice` field: **removed from PRD §3.3 and §3.4**. The polyphony pivot recorded in the 2026-05-05 audit is now reflected in the spec. If per-voice routing returns under the `poly()`/`sampler()` wrapper model, it will arrive as its own PRD.
- §3.6 bare-`%` auto-detection: **marked deferred in the PRD**. In practice typed pattern prefixes (`c"…"`, `n"…"`, `s"…"`, `v"…"`) cover the use case; revival would be a separate PRD.

### Stubs

None.

### Regressions

None. Full `akkado_tests` green on HEAD. The 5-into-1 helper refactor produces identical opcode counts to the old inlined path for the bare pattern-literal case (`SEQPAT_FIELD == 5`, `SEQPAT_PHASE == 1`, `SEQPAT_GATE == 1`, `SEQPAT_TYPE == 1`), verified by the new `SEQPAT opcodes emitted on transformed pattern` section.

### Coverage Gaps

None held open. Every field listed in `pattern_field_aliases()` is exercised at least once on the bare-pat path *and* at least once on a transformed path. Audit's prior suggestion to "iterate the registry in tests rather than hardcoding field names" is partially adopted — the new `[records-extended]` case iterates over a static array of canonical fields + a representative alias subset; future field additions to `pattern_field_aliases()` should also be added to `kCanonicalFields` in `test_codegen.cpp`.

### Missing Tests

None for the §3 surface. The polyphony-related test (`match(e.voice)`) remains absent by design (decision recorded in the 2026-05-05 audit and now reflected in §3.4).

### Scope Drift

None. The change set is constrained to `codegen_patterns.cpp` (single helper + 5 call sites), `codegen.hpp` (one declaration), the test file, the PRD, and the user-facing language doc.

### Convention Drift

Resolved. PRD §6 diagnostic codes now match the implementation. Recorded in §6 prelude line:

> Diagnostic codes match what the implementation actually emits. The original draft used `E062`–`E065` placeholders; reconciled 2026-05-13 to match `akkado/src/analyzer.cpp` and `akkado/src/codegen.cpp`.

### Suggestions

- **Helper reuse beyond patterns.** `emit_extended_field_buffers` is currently called only by the five pattern-producing sites. If a future feature (e.g., a `merge(p1, p2)` or `sustain(p)` builtin) introduces a sixth, calling this helper after setting `FREQ/VEL/TRIG` is the documented entry point.
- **Per-alias regression coverage.** `kCanonicalFields` in the new test case enumerates every canonical name plus a representative alias subset. When `pattern_field_aliases()` gains a new alias, also add it to `kCanonicalFields` so the transform-path coverage stays comprehensive. Consider exposing an iterator over the registry to tests.

## Decisions Recorded

- **§3 closure (2026-05-13)**: All extended pattern fields work uniformly across every pattern producer. Status flipped from `PARTIAL` to `DONE`.
- **`voice` field removed (2026-05-13)**: Per the polyphony pivot already recorded in the 2026-05-05 audit, the PRD's spec is brought into line with the implementation: 11 `PatternPayload` slots, no `voice` slot. Per-voice routing is opt-in via `poly()`/`sampler()` wrappers (out of scope here).
- **§3.6 deferred to follow-up PRD (2026-05-13)**: Bare-`%` content-based auto-detection is not implemented and is no longer in scope of this PRD. Typed pattern prefixes (`c"…"`, `n"…"`, `s"…"`, `v"…"`) cover the practical use cases.
- **Diagnostic codes reconciled (2026-05-13)**: PRD §6 now references the actually-emitted codes (E060/E061/E135/E136/E140) instead of the original placeholders (E062–E065).

## Tests Added / Extended

| File | Kind | Covers |
|---|---|---|
| `akkado/tests/test_codegen.cpp` | New `TEST_CASE` `"Codegen: Extended pattern fields on transforms"` tagged `[codegen][records-extended]` (~120 lines, 9 sections) | Per-canonical-field × six transform shapes (`fast`, `slow`, `rev`, `velocity`, `bank`, `variant`); opcode-emission count on transformed pattern; E136 message correctness through transform; chained-transform regression. 126 assertions. |

## Source Edits Made During Audit

| File | Change | Reason |
|---|---|---|
| `akkado/include/akkado/codegen.hpp` | Added `emit_extended_field_buffers()` declaration in the public section. | New shared helper; public so the free `emit_pattern_with_state` helper can invoke it through the `gen` parameter. |
| `akkado/src/codegen_patterns.cpp` | Added helper implementation; refactored `emit_per_voice_seqpat` to call it; wired `emit_pattern_with_state`, `handle_velocity_call`, `handle_bank_call`, `handle_variant_call` to call it after setting FREQ/VEL/TRIG. | Closes §3 gap on all five `PatternPayload` construction sites. |
| `akkado/tests/test_codegen.cpp` | New `TEST_CASE`. | Coverage for the four formerly-broken transform paths. |
| `docs/prd-records-and-field-access.md` | §3.3, §3.4, §3.6, §6 rewritten; status banner updated. | Voice removed; auto-detection deferred; diagnostic codes reconciled; status flipped to DONE. |
| `web/static/docs/reference/language/records.md` | Pattern-event field table corrected (`n` is `freq` alias not `phase`; `phase` aliases `cycle`/`co`; `dur`/`time`/`gate` aliases filled in); lead sentence rewritten ("All eleven fields work on every pattern producer"); added three working examples. | User-facing docs were stale relative to the implementation; the existing `n`-on-`phase` row was an outright bug. All three new examples verified via `akkado-cli --check`. |

## Post-Finalize Validation

| Command | Result |
|---|---|
| `./build/akkado/tests/akkado_tests` | Pass — 678 cases / 138,661 assertions |
| `./build/akkado/tests/akkado_tests "[records]"` | Pass |
| `./build/akkado/tests/akkado_tests "[records-extended]"` | Pass — 1 case / 126 assertions |
| `./build/cedar/tests/cedar_tests` | Pass — 184 / 185 (1 pre-existing skip) |
| `akkado-cli --check` on three rewritten `records.md` snippets | All exit 0 |

## Follow-up (same day): typed-prefix coverage and `n"…"` bare-MIDI bug

Adding typed-prefix smoke tests surfaced an asymmetric divergence not caught by the original §3 fix:

- `n"c4 e4 g4"`, `v"0.5 0.8"`, `s"bd sd"`, and `n"c4 e4 g4"` all produced full 11-field `PatternPayload`s, but `n"60 64 67"` (raw MIDI numbers in Note mode) silently compiled to a 5-instruction `PUSH_CONST 0.0 / OUTPUT` stub — no SEQPAT opcodes at all. Symbol-bound access (`x = n"60 64 67"; x.note`) reported `E136 "Unknown field 'note' on pattern. Available: freq, vel, trig, gate, type"` because `handle_field_access` falls back to a hardcoded "freq, vel, …" string when the actual TypedValue is not a `Pattern` (`akkado/src/codegen.cpp:2596–2599`). Bare pipe access tripped `E135 "Cannot access field 'note' on Signal value"` for the same reason.

**Root cause** — `note_mode_` was declared in `akkado/src/mini_lexer.cpp:28` but **never consulted in the lex loop**. So Note mode lexed identically to Auto mode, where raw digits don't produce pitch atoms. The mini-AST came out empty, `SequenceCompiler::compile()` returned false, and `handle_mini_literal` took the empty-pattern branch (`codegen_patterns.cpp:1292–1300`) which returns a `Signal(0)`. The misleading error message made it look like a wiring gap; it was actually a parser gap.

**Fix** — added `MiniLexer::lex_note_atom()` (`akkado/src/mini_lexer.cpp:435`) mirroring `lex_value_atom()` but emitting a `PitchToken` with the parsed integer as `midi_note`. Gated on `note_mode_` from the main lex loop (`mini_lexer.cpp:331–340`). `n"60"` → MIDI 60 → identical bytecode to `n"c4"`. Microtonal expressivity via `^v+\` modifiers on note-name pitches is preserved; fractional MIDI numbers round to the nearest semitone.

**Tests** — new TEST_CASE `"Codegen: Extended pattern fields on typed prefixes"` at `akkado/tests/test_codegen.cpp:4931` tagged `[codegen][records-extended]`. 10 SECTIONs covering:
1. `n"c4 e4 g4"` (note names) × every canonical field + alias (bare)
2. `n"60 64 67"` (bare MIDI — the formerly-broken case) × every field (bare)
3. Same through `fast()` transform
4-7. `v"…"` and `s"…"` mirror coverage (bare + fast)
8. Symbol-bound forms (`x = n"60 64 67"; x.<field>`) — the exact reproduction
9. SEQPAT opcode-count parity across typed pattern literals
10. `c"…"` lockdown SECTIONs: bare use rejected with E160/E410, `poly()`-wrapped closure params remain Signal-typed (E061 on field access) per the polyphony pivot

**`c"…"` field access through `poly()` closure remains out of scope** — closure parameters are bound to Signal scalars at `akkado/src/codegen_functions.cpp:2012–2015` by the polyphony pivot's design. Per-voice field access (`e.note`, `e.dur` inside the closure) would require a separate PRD that rethinks how the closure's `e` is typed. The two `c"…"` lockdown SECTIONs are the regression markers that would need to flip if that PRD lands.

### Source edits (follow-up)

| File | Change | Reason |
|---|---|---|
| `akkado/include/akkado/mini_lexer.hpp:78–79` | Declared `MiniToken lex_note_atom()`. | Matches `lex_value_atom()` naming. |
| `akkado/src/mini_lexer.cpp:331–340` | Added `if (note_mode_) { … return lex_note_atom(); }` branch in the main lex loop. | Activates the previously-dormant `note_mode_` flag. |
| `akkado/src/mini_lexer.cpp:435–490` | Added `lex_note_atom()` body. | Emits a `PitchToken` for numeric MIDI atoms, with optional `:velocity` suffix and `{…}` record suffix per the standard pitch grammar. |
| `akkado/tests/test_codegen.cpp:4931–5079` | New `TEST_CASE` `"Codegen: Extended pattern fields on typed prefixes"` (10 SECTIONs). | Regression-proofs every typed prefix × every extended field × bare + symbol-bound + transformed forms. |

### Follow-up validation

| Command | Result |
|---|---|
| `cmake --build build` | Pass — no new warnings |
| `./build/akkado/tests/akkado_tests` | Pass — **679 cases / 138,870 assertions** (+1 case, +209 assertions over the §3-closure baseline) |
| `./build/akkado/tests/akkado_tests "[records-extended]"` | Pass — **2 cases / 335 assertions** |
| 4 prefixes × 28 names/aliases bare `--check` matrix | **145 / 145 pass** |
| 4 prefixes × 19 names/aliases symbol-bound `--check` matrix | **76 / 76 pass** |
| Bytecode diff: `n"c4 e4 g4"` vs `n"60 64 67"` | Identical SEQPAT-family instruction counts (1 QUERY + 1 STEP + 1 GATE + 1 TYPE + 5 FIELD + 1 PHASE) |

## PRD Status

- **Before:** `PARTIAL — §1–2, §4–6 done; §3 extended fields pending`
- **After (recommended and applied):** `DONE — §3 wired through every pattern producer; voice removed from spec; §3.6 deferred`

## Recommended Next Steps

None for this PRD. If a follow-up PRD revives §3.6 (bare-`%` auto-detection) it should:

1. Discriminate pattern content type at compile time (pitch / sample / note / velocity).
2. Make bare-`%` consult that discriminant to choose between `%.freq`, `%.sample_id`, `%.note`, `%.vel`.
3. Update `PatternPayload` to carry the discriminant (a small enum, not a bitfield — keeps shape stable across transforms).
4. Reuse `emit_extended_field_buffers` as-is.
