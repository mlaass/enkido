**Status: NOT STARTED** — Reworks the `poly()` / `mono()` / `legato()` instrument callback so it can read *any* pattern event field, not just `(freq, gate, vel)`. Adds a record-destructure callback form, a positional "take the prefix you need" form, mixing of the two, and a rest-param escape hatch for binding the whole event.

# Poly Callback Event-Record PRD — flexible instrument callbacks

## Executive Summary

Today the instrument function passed to `poly()` (and `mono()` / `legato()`) **must have exactly three positional parameters**, bound by position to `(freq, gate, vel)`. The names are ignored; the order is fixed; `trig` is allocated by codegen but never bound; and the other eight pattern event fields (`note`, `dur`, `chance`, `time`, `phase`, `type`, `sample_id`) plus any custom `.set()` fields are simply unreachable from inside a voice.

This PRD makes the callback parameter list a first-class, flexible thing — consistent with how the rest of the language already treats pattern events as records:

- **Positional form, take-the-prefix-you-need.** `(freq) ->`, `(freq, gate) ->`, `(freq, gate, vel) ->` … up to all eleven fixed fields, in a fixed canonical order. The historical `(freq, gate, vel)` callback keeps compiling unchanged — it is just the 3-prefix of the canonical order.
- **Record-destructure form.** `({note, vel, cutoff}) -> …` pulls named fields (fixed *or* custom) straight off the event record. This is the "catchall with names" — variadic by name.
- **Mixed form.** `(freq, gate, {cutoff, dur}) -> …` — leading positionals plus a trailing destructure, both reading the same event.
- **Rest-param escape hatch.** `(...e) -> osc("sin", e.freq) * e.vel` binds the whole event record to one name.
- **All eleven fixed fields + custom fields** are exposed. Missing fields bind to `0` (no error); an explicit destructure default (`{cutoff = 0.5}`) is honoured when the field is absent.
- **Custom fields are per-voice.** Today every custom field is a per-event scalar (mini-notation record suffixes like `c4{cutoff:0.8}`, stored in `OutputEvent::prop_vals[]`, max 4 per pattern). Each voice sees its own note's value. There is no signal-valued custom-field path in the current implementation, so the per-voice/shared distinction collapses to "always per-voice".

### Key design decisions (from the design rounds)

1. **Both forms compile.** Positional and record-destructure are both first-class; they can be mixed in one callback.
2. **The "hard break" is the *exactly-3* rule, not existing code.** Because the canonical positional order starts `freq, gate, vel`, every `(freq, gate, vel)` callback in the wild still compiles. What changes: the `E404` "must have exactly 3 parameters" enforcement is removed, and the record form becomes the canonical idiom in all docs/examples/tests.
3. **A single plain identifier is positional.** `(v) ->` means `v` *is* `freq`. To bind the whole event you must destructure (`({...})`) or use the rest param (`(...e)`). This is a documented footgun.
4. **Missing field = `0`.** Any field name binds; a name the pattern never produced yields `0` (silent — typos in destructure names are not caught). An explicit `= expr` default overrides the `0`.
5. **Applies to all three** voice managers: `poly`, `mono`, `legato`.

---

## 1. Problem Statement / Current State

### 1.1 What exists today

`poly`, `mono`, and `legato` are registered as builtins with opcode `NOP`; they are lowered specially by `CodeGenerator::handle_poly_call` (`akkado/src/codegen_functions.cpp:1931`). The handler:

- resolves the instrument argument via `resolve_function_arg` into a `FunctionRef`,
- **requires `func_ref->params.size() == 3`** (`codegen_functions.cpp:2020`, error `E404`),
- binds the three params, *by position*, to three scratch buffers — `voice_freq_buf`, `voice_gate_buf`, `voice_vel_buf` (`codegen_functions.cpp:2077-2079`),
- allocates a `voice_trig_buf` and wires it to `POLY_BEGIN.inputs[3]` but **never binds it to a callback parameter**,
- inlines the function body between `POLY_BEGIN` / `POLY_END`.

At runtime `VM::execute_poly_block` (`cedar/src/vm/vm.cpp:262`) fills `freq_buf` / `gate_buf` / `vel_buf` / `trig_buf` per active voice and re-runs the inlined body once per voice with XOR-isolated DSP state.

### 1.2 Limitations

| Today | Consequence |
|---|---|
| Callback must have *exactly* 3 params | `(freq) -> saw(freq)` is a compile error even though gate/vel are unused |
| Params bound by position to `freq, gate, vel` only | `note`, `dur`, `chance`, `time`, `phase`, `type`, `sample_id` are unreachable inside a voice |
| `trig` buffer allocated but unbound | The per-voice trigger pulse exists in the VM but no callback can read it |
| Custom `.set()` / mini-notation `{…}` fields invisible | `pat("c4{cutoff:0.8} e4{cutoff:0.3}") \|> poly(...)` cannot use per-note `cutoff` |
| No record-style access | Inconsistent with `pat(...) as {freq, vel}` pipe destructuring and `fn f({x, y})` param destructuring, which already work everywhere else |

The rest of the language already treats every pattern event as an 11-field record with custom fields (`web/static/docs/reference/language/records.md` §"Pattern events are records"). The poly callback is the one place that ignores this.

### 1.3 What does NOT change

- The `poly(input, instrument, voices=64)` *call* signature — only the *instrument callback's* parameter list changes.
- Voice allocation, stealing, mono/legato retrigger semantics, stereo-native output, XOR state isolation.
- `spread()` and the `voice` / `voices` concepts.
- The `voices` argument still must be a number literal (1–128).

---

## 2. Target Syntax / User Experience

### 2.1 Positional — take the prefix you need

```akkado
// 1 param — just frequency
pat("c4 e4 g4") |> poly(%, (freq) -> saw(freq)) |> out(%)

// 3 params — the historical form, still valid verbatim
pat("c4 e4 g4 b4") |> poly(%, (freq, gate, vel) -> osc("sin", freq) * vel * gate, 8) |> out(%)

// More positionals — canonical order continues past vel
pat("c4 e4 g4") |> poly(%, (freq, gate, vel, trig, type, note) ->
    osc("sin", freq) * ar(trig, 0.01, 0.3) * vel) |> out(%)
```

**Canonical positional order** (positions 1…11):

```
freq, gate, vel, trig, type, note, dur, chance, time, phase, sample_id
```

`freq, gate, vel` lead the list so every existing 3-param callback keeps its meaning. Parameter *names* remain irrelevant in positional form — `(a, b, c) ->` is identical to `(freq, gate, vel) ->`.

### 2.2 Record destructure — "catchall with names"

```akkado
// Pull exactly the fields you want, in any order, by name
chord("C Em Am G") |> poly(%, ({freq, vel, gate}) ->
    saw(freq) * ar(gate, 0.05, 0.4) * vel) |> out(%)

// Reach fields that positional users never could
pat("c4 e4 g4") |> poly(%, ({freq, note, dur, phase}) ->
    osc("sin", freq) * (1 - phase) * (dur > 0.5 ? 1 : 0.6)) |> out(%)

// Custom per-note fields straight off mini-notation record suffixes
pat("c4{cutoff:0.9} e4{cutoff:0.3} g4{cutoff:0.6}") |> poly(%, ({freq, gate, cutoff}) ->
    saw(freq) |> lp(%, cutoff * 4000) |> % * ar(gate, 0.01, 0.3)) |> out(%)

// Aliases resolve like everywhere else (pitch → freq, n → note, …)
pat("c4 e4 g4") |> poly(%, ({pitch, v}) -> osc("sin", pitch) * v) |> out(%)
```

### 2.3 Mixed — positional prefix + trailing destructure

```akkado
// freq/gate/vel positionally (familiar), cutoff/dur by name
pat("c4{cutoff:0.8} e4 g4") |> poly(%, (freq, gate, vel, {cutoff, dur}) ->
    saw(freq) |> lp(%, cutoff * 5000) |> % * adsr(gate, 0.01, dur * 0.4, 0.6, 0.2) * vel)
    |> out(%)
```

The leading params consume positions 1…k of the canonical order; the trailing `{…}` destructure reads further named fields off the *same* event.

### 2.4 Rest param — bind the whole event record

```akkado
// `e` is the full event record; use e.field freely
chord("C Em Am G") |> poly(%, (...e) ->
    osc("sin", e.freq) * e.vel * ar(e.gate, 0.02, 0.5))
    |> out(%)

// Rest may follow positionals too
pat("c4 e4 g4") |> poly(%, (freq, ...e) -> osc("saw", freq) * e.vel) |> out(%)
```

### 2.5 Defaults for absent fields

```akkado
// Pattern has no `cutoff` on some/all notes → cutoff binds to 0 by default,
// or to the explicit default when one is given.
pat("c4 e4{cutoff:0.7} g4") |> poly(%, ({freq, gate, cutoff = 0.5}) ->
    saw(freq) |> lp(%, cutoff * 4000) |> % * ar(gate, 0.01, 0.3)) |> out(%)
// note c4 → cutoff 0.5 (default), e4 → 0.7, g4 → 0.5
```

### 2.6 mono / legato — identical callback rules

```akkado
pat("c2 e2 g2 c3") |> mono(({freq, gate, vel}) ->
    saw(freq) * adsr(gate, 0.01, 0.1, 0.6, 0.3) * vel) |> out(%)

pat("c2 e2 g2 c3") |> legato((freq, gate) ->
    saw(freq) * adsr(gate, 0.01, 0.2, 0.8, 0.4)) |> out(%)
```

---

## 3. Architecture / Technical Design

### 3.1 Pipeline overview

```
instrument arg ──► resolve_function_arg ──► FunctionRef
                                              │  params: [PositionalParam | DestructureParam | RestParam]
                                              ▼
                              handle_poly_call: classify param list
                                              │
                          ┌───────────────────┼────────────────────┐
                          ▼                   ▼                    ▼
                  positional names      destructure {…}        rest ...name
                          │                   │                    │
                          └─────────► referenced-field set ◄────────┘
                                              │
                            allocate one per-voice scratch buffer per
                            referenced field; record (field_id → buffer)
                                              │
                                              ▼
                       POLY_BEGIN + PolyFieldTable (StateInitData) ──► VM
                                              │
                              VM::execute_poly_block: per voice, fill each
                              field buffer from the voice's OutputEvent,
                              then run the inlined body
```

### 3.2 Callback parameter model

`resolve_function_arg` already produces a `FunctionRef` with a `params` vector and supports `DestructureParamData` (used by `fn f({x, y})`) and closure rest params (`ClosureParamData.is_rest`). `handle_poly_call` must classify the resolved param list into exactly one of these shapes:

| Shape | Param list | Binding |
|---|---|---|
| Positional | N plain identifiers | param *i* → canonical field *i* |
| Destructure | one `{…}` param | each field name → that event field |
| Mixed | K plain identifiers, then one `{…}` | positionals → canonical fields 1…K; destructure names → event fields |
| Rest | optional plain identifiers, then `...name` | positionals as above; `name` bound to the whole event record |

Rules:
- A param list is **at most one** of `{…}`-destructure *or* `...name`-rest, and it must be **trailing**.
- Empty param list `() ->` is legal (a voice that ignores the event).
- Positional params past position 11 → error `E406`.
- A field bound both positionally and in the trailing destructure (e.g. `(freq, {freq})`) → error `E407`.
- Multiple rest params, or a rest param not in trailing position → error `E408`.
- Both a destructure and a rest param → error `E409`.

### 3.3 Field resolution and the referenced-field set

The compiler computes the **referenced-field set** — every fixed or custom field the callback can read:

- **Positional params** → canonical fields by index.
- **Destructure params** → each field name, normalised through the existing alias table (`pitch`→`freq`, `n`→`note`, `g`→`gate`, …).
- **Rest param** → the compiler scans the callback body for `name.field` accesses (the same mechanism that already resolves field access on an `as`-bound record) and adds each accessed field. A rest-bound name whose fields cannot all be statically enumerated is acceptable — only statically-seen `.field` accesses are plumbed; anything else would already be a compile error under existing field-access rules.

For each referenced field:

| Field kind | Plumbing |
|---|---|
| Fixed field (freq … sample_id) | Allocate a per-voice scratch buffer; VM fills it from the voice's event each block. |
| Custom field (mini-notation `c4{cutoff:0.8}` record suffix) | Allocate a per-voice scratch buffer; VM fills it from `OutputEvent::prop_vals[propIdx]` for the voice's event. |
| Unknown name (not fixed, not a custom field on the pattern) | Bind to a constant-`0` buffer. No error (decision #4). An explicit destructure default replaces the `0`. |

**Custom fields are uniformly per-event scalars** — confirmed against the codegen: `PatternPayload::custom_fields` is a plain `map<name, buffer_idx>`, populated only from mini-notation record suffixes (`ast.hpp` `MiniAtomData::properties` is `vector<pair<string, float>>` — a signal cannot even be expressed there) and from `bend()`/`aftertouch()` transforms, which sample-and-hold to a `float`. There is **no signal-valued custom-field path**, so no scalar-vs-signal flag is needed on `PatternPayload`.

The compiler needs the **name → `prop_vals` slot** mapping to build `PolyFieldBinding.prop_index`. That mapping lives in `SequenceCompiler::custom_slots_` (`map<name, uint8_t slot>`), used today by `emit_custom_property_buffers`; `handle_poly_call` reads the same source. `MAX_PROPS_PER_EVENT` is 4, so at most 4 custom fields exist per pattern.

> Note: `web/static/docs/reference/language/records.md` currently shows `pat(...).set("cutoff", saw(0.5))` as a signal-valued custom field. No pattern-level `.set` builtin exists in codegen today (only state-cell `.set`); that doc example is out of scope for this PRD and its accuracy should be tracked separately.

### 3.4 POLY_BEGIN field-buffer table

Today `POLY_BEGIN` carries five input slots: `freq, gate, vel, trig, voice_out`. Eleven fixed fields + custom fields do not fit in five slots. Design:

- Keep `POLY_BEGIN.inputs[4] = voice_out` (L; R derived `+1`) and `out_buffer = mix` as today.
- Replace the fixed `inputs[0..3] = freq/gate/vel/trig` wiring with a **field-buffer table** carried in the `StateInitData` for the `PolyAlloc` state: an array of `{ field_id, buffer_idx }` pairs (field_id ∈ the 11 fixed-field enum + a custom-field discriminator carrying the `prop_vals` index). Up to `11 + MAX_PROPS_PER_EVENT` entries.
- `VM::execute_poly_block`, in its per-voice loop, walks the table and fills each `buffer_idx` from the voice's `OutputEvent` (looked up via `PolyVoice::event_index`) and the block's cycle position — *before* running the inlined body for that voice.

`PolyVoice` already stores `event_index`, so the originating event (and all its fields, `prop_vals` included) is reachable without widening `PolyVoice`. Per-sample-accurate fields (`gate`, `trig`) keep their current per-sample fill logic; the rest are event-scalar and held for the voice's lifetime.

### 3.5 Data structures

```cpp
// New: describes one per-voice field buffer the VM must fill.
struct PolyFieldBinding {
    std::uint8_t  field_id;     // FixedField enum, or CUSTOM marker
    std::uint8_t  prop_index;   // valid when field_id == CUSTOM: OutputEvent::prop_vals index (0..3)
    std::uint16_t buffer_idx;   // per-voice scratch buffer to fill
};

// StateInitData (akkado side) — PolyAlloc gains:
//   std::vector<PolyFieldBinding> poly_field_bindings;
// PolyAllocState (cedar side) — gains an arena-allocated copy:
//   PolyFieldBinding* field_bindings; std::uint8_t field_binding_count;
```

`OutputEvent` (`cedar/include/cedar/opcodes/sequence.hpp:123`) already carries `time`, `duration`, `velocity`, `chance`, `midi_note`, `values[]`, `velocities[]`, `type_id`, and `prop_vals[]` — no new event fields required. `phase` is derived in the VM from cycle position vs `event.time`/`event.duration`.

### 3.6 Constraints

| Constraint | Value | Rationale |
|---|---|---|
| Max positional params | 11 | One per fixed field |
| Canonical order | `freq, gate, vel, trig, type, note, dur, chance, time, phase, sample_id` | `freq,gate,vel` first preserves the historical 3-param callback |
| Field-buffer table size | ≤ `11 + MAX_PROPS_PER_EVENT` (= 15) | Worst case: every fixed field + all 4 custom props referenced |
| Body length | ≤ 255 instructions (unchanged) | `POLY_BEGIN.rate` is `uint8` |
| Voice count | 1–128 literal (unchanged) | Static allocation |

---

## 4. Goals and Non-Goals

### Goals
- Callback accepts 0–11 positional params (canonical order), a `{…}` destructure, a mix of both, or a trailing `...name` rest param.
- All 11 fixed fields + custom mini-notation record-suffix fields readable per voice.
- Custom fields are per-event scalars, bound per-voice — each voice sees its own note's value.
- Missing field → `0`; explicit destructure default honoured.
- `poly`, `mono`, `legato` all updated.
- Existing `(freq, gate, vel)` callbacks compile unchanged.
- Full migration of docs, example patches, and tests to the record form as the canonical idiom.

### Non-Goals
- Changing the `poly(input, instrument, voices)` call signature.
- Signal-valued custom fields — no such path exists in codegen today; if one is added later, per-voice routing of it is a future PRD.
- A pattern-level `.set("name", value)` builtin (does not exist today; not introduced here).
- Writing back to event fields from inside a voice (callbacks remain read-only consumers).
- New mini-notation syntax for custom fields — uses what exists today.
- Changes to `spread()` / unison.

---

## 5. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| `poly` / `mono` / `legato` *call* signature | **Stays** | Only the callback param list changes |
| `handle_poly_call` param binding | **Modified** | Classify param shapes; build referenced-field set; emit field-buffer table |
| `resolve_function_arg` | **Modified (maybe)** | Must surface destructure/rest param info to `handle_poly_call` — may already suffice |
| Error codes `E403` / `E404` | **Modified** | Messages no longer say "exactly 3 parameters" |
| Error codes `E406`–`E409` | **New** | Param-list validation (too many positionals, dup binding, rest placement, destructure+rest) |
| `POLY_BEGIN` input wiring | **Modified** | `inputs[0..3]` replaced by field-buffer table in StateInitData |
| `StateInitData` (PolyAlloc) | **Modified** | Add `poly_field_bindings` |
| `PolyAllocState` | **Modified** | Add arena-allocated `field_bindings` + count |
| `PolyVoice` | **Stays** | `event_index` already links to the full event |
| `OutputEvent` | **Stays** | Already carries all needed fields incl. `prop_vals[]` |
| `VM::execute_poly_block` | **Modified** | Per-voice loop fills field buffers from the event before running the body |
| `PatternPayload::custom_fields` | **Stays** | No flag needed — all custom fields are per-event scalars; the compiler reads the name→slot map from `SequenceCompiler::custom_slots_` |
| Voice allocation / stealing / XOR isolation | **Stays** | Untouched |
| `spread()` / `voice` / `voices` | **Stays** | Out of scope |

---

## 6. File-Level Changes

### Code
| File | Change |
|---|---|
| `akkado/src/codegen_functions.cpp` | `handle_poly_call`: classify positional/destructure/mixed/rest; build referenced-field set; allocate per-voice field buffers; emit field-buffer table; drop the `==3` check |
| `akkado/include/akkado/builtins.hpp` | Update `poly`/`mono`/`legato` `param_names` + `description` to reflect the flexible callback |
| `akkado/src/codegen_arrays.cpp` | `resolve_function_arg`: ensure destructure + rest param metadata reach `handle_poly_call` |
| `akkado/include/akkado/codegen.hpp` (or wherever `StateInitData` lives) | Add `poly_field_bindings` to the `PolyAlloc` variant; define `PolyFieldBinding` |
| `cedar/include/cedar/opcodes/dsp_state.hpp` | `PolyAllocState`: add arena-allocated `field_bindings` + count; `ensure_voices`/init path copies the table |
| `cedar/include/cedar/vm/vm.hpp` / `cedar/src/vm/vm.cpp` | `execute_poly_block`: fill per-voice field buffers from the event; `init_poly_state` copies the field table |
| `akkado/src/codegen_patterns.cpp` | Expose the `custom_slots_` name→`prop_vals`-slot map so `handle_poly_call` can build `PolyFieldBinding.prop_index` |

### Docs
| File | Change |
|---|---|
| `web/static/docs/reference/builtins/polyphony.md` | Rewrite `poly`/`mono`/`legato` sections + frontmatter snippets to the record-form idiom; document canonical positional order, mixed form, rest param, missing=0 |
| `web/static/docs/reference/language/records.md` | Add a note that the poly callback consumes the event record (cross-link) |
| `web/static/docs/tutorials/06-pattern-modulation.md` | Migrate poly examples |
| `web/static/docs/reference/pattern/literals.md` | Migrate poly examples |
| `web/static/docs/reference/mini-notation/chords.md` | Migrate poly examples |
| `web/static/docs/concepts/signals.md` | Migrate poly examples |
| `web/static/docs/reference/builtins/audio-input.md` | Migrate poly examples |
| `web/static/docs/reference/language/arrays.md` | Migrate poly examples |
| `web/static/docs/reference/builtins/stereo.md` | Migrate poly examples |
| `web/static/docs/reference/builtins/soundfonts.md` | Migrate poly examples |
| `CHANGELOG.md` | New `## [Unreleased]` entry (via `/update-changelog`) |

### Example patches
| File | Change |
|---|---|
| `web/static/patches/poly-chords.akk` | Migrate to record-form callback |
| `web/static/patches/chord-stab.akk` | Migrate to record-form callback |
| `web/static/patches/fm-piano.akk` | Migrate to record-form callback |

### Tests
| File | Change |
|---|---|
| `cedar/tests/test_poly.cpp` | Add cases for all callback shapes + per-voice field fill; migrate existing cases to record form |
| `akkado/tests/test_codegen.cpp` | Param-shape classification, `E406`–`E409`, field-buffer table emission |
| `akkado/tests/test_chord.cpp` | Migrate poly callbacks to record form |
| `akkado/tests/test_types.cpp` | Migrate poly callbacks to record form |
| `akkado/tests/test_hot_swap_determinism.cpp` | Migrate poly callbacks; confirm semantic-ID stability across callback shapes |

### Opcode docs regeneration
After `builtins.hpp` changes: `cd web && bun run build:opcodes` and `bun run build:docs`.

---

## 7. Implementation Phases

### Phase 1 — Callback param model (codegen, fixed fields freq/gate/vel/trig)
**Goal:** positional-N, destructure, mixed, and rest forms all compile, limited to the four fields the VM already plumbs (`freq, gate, vel, trig`).
- Rewrite `handle_poly_call` param classification; drop the `==3` check; add `E406`–`E409`; update `E403`/`E404` messages.
- Bind positional/destructure/mixed/rest params to the existing `voice_freq/gate/vel/trig` buffers.
- **Verify:** `(freq) ->`, `(freq, gate, vel) ->`, `({freq, vel}) ->`, `(freq, {vel}) ->`, `(...e) -> e.freq` all compile and produce identical audio to the old 3-param form where equivalent. `akkado_tests`, `cedar_tests` green.

### Phase 2 — All 11 fixed fields (codegen + VM)
**Goal:** any of the 11 fixed fields readable per voice.
- Define `PolyFieldBinding`; add `poly_field_bindings` to `StateInitData`; build the referenced-field set and per-voice buffers in `handle_poly_call`.
- Replace `POLY_BEGIN.inputs[0..3]` wiring with the field-buffer table; add `field_bindings` to `PolyAllocState`.
- `execute_poly_block`: fill each bound field buffer from the voice's `OutputEvent` / cycle position.
- **Verify:** experiment under `experiments/` driving `poly` with `({freq, note, dur, phase}) ->` for ≥300 s of audio; assert per-voice `note`/`dur`/`phase` track the voice's event. WAV written for human listening.

### Phase 3 — Custom fields
**Goal:** mini-notation `c4{cutoff:0.8}` record-suffix fields readable per voice.
- `handle_poly_call` reads `SequenceCompiler::custom_slots_` for the name→slot map; custom destructure names become `PolyFieldBinding` entries with `field_id = CUSTOM`, `prop_index = slot`.
- VM fills the per-voice buffer from `OutputEvent::prop_vals[slot]` for the voice's event.
- Unknown destructure names → constant-`0` buffer; honour explicit `= expr` defaults.
- **Verify:** `pat("c4{cutoff:0.9} e4{cutoff:0.3}") |> poly(%, ({freq, gate, cutoff}) -> …)` — each voice's `cutoff` matches its note; `{cutoff = 0.5}` default applies when the note omits `cutoff`; an undeclared name binds to `0`.

### Phase 4 — mono / legato parity
**Goal:** `mono` and `legato` accept every callback shape.
- Confirm the shared `handle_poly_call` path covers all three; add mono/legato-specific tests.
- **Verify:** mono/legato with positional, destructure, mixed, rest callbacks; retrigger/legato semantics unchanged.

### Phase 5 — Migration sweep + docs
**Goal:** record form is the canonical idiom everywhere in-repo.
- Rewrite all docs, example patches, and test fixtures listed in §6.
- Regenerate opcode + docs indices.
- CHANGELOG entry; `/update-changelog`.
- **Verify:** `bun run check`, `bun run build`; full `akkado_tests` + `cedar_tests`; `experiments/run_all.sh`.

---

## 8. Open Questions

1. **Canonical tail order.** `freq, gate, vel` must lead (back-compat). The tail order chosen here is `trig, type, note, dur, chance, time, phase, sample_id` (matches `PatternPayload` field-constant order after the leading three). Low-stakes — confirm or reorder during Phase 1 review.
2. **Rest param + field plumbing.** §3.3 plumbs only statically-seen `e.field` accesses for a rest-bound `e`. Confirm the field-access codegen already rejects/handles non-static field access on a rest-bound record, so no field is silently missed.

---

## 9. Edge Cases

| Input | Expected behaviour |
|---|---|
| `(v) -> v` | `v` is **freq** (positional position 1), not the event record. Documented footgun. |
| `() -> osc("sin", 220)` | Legal — voice ignores the event entirely. |
| `({frqe}) -> …` (typo) | `frqe` binds to `0` silently (decision #4). No diagnostic. |
| `(a, b, c) -> …` | Identical to `(freq, gate, vel) -> …` — positional names are ignored. |
| `(freq, {freq}) -> …` | `E407` — `freq` bound both positionally and in the destructure. |
| 12+ positional params | `E406` — exceeds the 11 canonical fields. |
| `(...e, freq) -> …` | `E408` — rest param must be trailing. |
| `({freq}, ...e) -> …` | `E409` — cannot combine a destructure and a rest param. |
| `({pitch}) -> …` | `pitch` resolves to `freq` via the alias table. |
| `({cutoff = 0.5}) ->`, pattern never sets `cutoff` | `cutoff` binds to `0.5` (explicit default beats the `0` fallback). |
| `({cutoff}) ->`, pattern never sets `cutoff` | `cutoff` binds to `0`. |
| `c4{cutoff:0.8} e4{cutoff:0.3}` + `({cutoff}) ->` | Each voice sees its own note's `cutoff` (per-voice, from `prop_vals[]`). |
| 5+ distinct custom fields on one pattern | Already capped at `MAX_PROPS_PER_EVENT` (4) by the existing sequencer — not introduced by this PRD. |
| Chord note with no per-voice velocity override | `vel` falls back to the event-wide velocity (existing `OutputEvent::velocities[]` default). |
| Callback closure captures + new param forms | Captures still bound after params (unchanged from today). |
| Hot-swap: callback shape changes between compiles | Semantic-ID path for `poly#N` is unchanged by callback shape; voice state rebinds as today. Covered by `test_hot_swap_determinism.cpp`. |

---

## 10. Testing / Verification Strategy

### Codegen (`akkado/tests/test_codegen.cpp`)
- Each shape compiles: `(freq)`, `(freq,gate,vel)`, all 11 positional, `({freq,vel})`, `(freq,{cutoff})`, `(...e)`, `()`.
- `E406` on 12 positionals; `E407` on `(freq,{freq})`; `E408` on non-trailing/duplicate rest; `E409` on destructure+rest.
- Field-buffer table: emitted entries match the referenced-field set; signal-valued custom field produces *no* per-voice entry.
- `E403`/`E404` message text updated (no "exactly 3").

### VM (`cedar/tests/test_poly.cpp`)
- `(freq, gate, vel)` callback produces bit-identical output to the pre-PRD build (regression guard).
- Per-voice `note` / `dur` / `phase` / `type` / `sample_id` match the voice's originating event.
- Per-event-scalar custom field is per-voice; signal-valued custom field is identical across voices.
- `mono` / `legato` with each callback shape; retrigger + legato gate behaviour unchanged.

### Experiments (`experiments/test_op_poly.py` — new or extended)
- Drive `poly` with `({freq, note, dur, phase}) ->` and a 4+ note pattern for **≥300 s** simulated audio; assert no field drift, no voice-leak, stable allocation. Write a short WAV separately for human listening ("each voice's filter should track its own note's cutoff").

### Manual / web
- `bun run check` + `bun run build` clean.
- Load each migrated patch in the web app; confirm audio unchanged from before migration.
- F1 help on `poly` shows the new callback documentation.

### Build commands
```bash
cmake --build build
./build/akkado/tests/akkado_tests "[poly]"
./build/cedar/tests/cedar_tests "*poly*"
cd experiments && uv run python test_op_poly.py
cd web && bun run check && bun run build:opcodes && bun run build:docs
```

---

## 11. Related Work

- `docs/prd-polyphony-system.md` — the original unified `POLY` opcode design (IMPLEMENTED).
- `docs/prd-records-and-field-access.md` — the record + field-access system this PRD makes the poly callback consistent with.
- `docs/agent-guide-userspace-functions.md` — user functions, closures, param destructuring, rest params.
- `web/static/docs/reference/language/records.md` — pattern-events-as-records, the 11 fixed fields + aliases, function-parameter destructure with defaults.
- `web/static/docs/concepts/record-as-options.md` — the record-as-options convention (related but distinct: that is for *builtin* options, this is for *callback parameters*).
