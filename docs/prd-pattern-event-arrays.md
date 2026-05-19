# PRD: Pattern Event Arrays — Userspace Access to Chord & Polyphonic Data

> **Status: NOT STARTED** — Surfaces the chord-note array carried inside every pattern event (`OutputEvent.values[]`) as first-class dynamic arrays in Akkado, so userspace closures can write arpeggiators, harmonizers, riff machines, and other polyphonic transforms without new C++ opcodes per use case. Removes the unused `ChordLit` (`C4'`) syntax. Depended on `prd-userspace-state-and-edge-primitives.md` (for `step`, `state`, `counter`, UFCS) — that PRD shipped 2026-04-26, so this prerequisite is satisfied and the PRD is unblocked.

## 1. Overview

Akkado patterns already carry chord/polyphonic data in their internal event format: `OutputEvent.values[MAX_VALUES_PER_EVENT=8]` plus `num_values` (`cedar/include/cedar/opcodes/sequencing.hpp:412,498`). When you write `n"[c4,e4,g4] g3 [a3,c4]"` or `chord("Am G C")`, each event carries up to 8 notes. Today, only the **first** note of any event surfaces in user code — `e.freq` and `e.note` (`akkado/src/codegen.cpp` `pattern_field()` ~line 1865+) extract `values[0]` only. To play the chord polyphonically, the user wraps with `poly(...)` and the runtime voice allocator silently dispatches each chord note to a separate voice. **The user never sees the chord as data they can transform.**

This blocks an entire class of live-coding tools: arpeggiators, harmonizers, riff machines, ostinatos, voicing transformers — anything that wants to *iterate over* or *transform* the notes of a chord rather than just sound them. Each one currently requires a new C++ opcode.

This PRD surfaces the chord array as a first-class **dynamic array** value type — an array whose length varies per pattern event at runtime. Combined with the now-shipped state-and-edge primitives PRD (which provides `step(arr, trig)`, `counter`, `state` cells, and UFCS method calls — shipped 2026-04-26), all four target use cases become small userspace closures.

### Why?

- **One data model unlocks N use cases.** Once chord notes are an array users can index, `step()` (from the state PRD) gives arpeggiation, `map()` gives harmonization, and array slicing gives riff machines — all in 1-3 lines each.
- **No new C++ opcodes per musical operator.** Without this, every live-coding tool that touches chord internals is a C++ change, an opcode allocation, a state struct, and a release.
- **Removes a vestigial language feature.** `ChordLit` (`C4'`) was the original "chord as value" attempt but has been collapsed to the root note in codegen since MVP and is unused in any shipped patch. Patterns are the actual chord-bearing surface; the language should reflect that.

### Major design decisions

- **Chord data is exposed as dynamic arrays from pattern events**, not as static literals. The data lives in `OutputEvent.values[]`; the language gets a way to read it. There is no new chord constructor builtin (`notes("Am7", 4)` was considered and rejected — patterns are the canonical source of chord data).
- **`notes(e)` and `freqs(e)` are added as builtins** that accept a pattern event and return a dynamic array of MIDI numbers / frequencies respectively. With UFCS (from state PRD), `e.notes` and `e.freqs` work as method-style sugar.
- **Existing scalar accessors stay** — `e.freq`, `e.note`, `e.vel`, `e.gate`, `e.trig` continue to return the first chord note's value (or the event's single value), preserving backward compatibility for monophonic patches. Array-form companions (`vels(e)`, `gates(e)`, `trigs(e)`) are scheduled but not load-bearing for v1; v1 ships only `notes(e)` and `freqs(e)`.
- **Dynamic arrays are a new value-type variant.** Their length is a runtime signal, not a compile-time constant. `len()` becomes polymorphic: compile-time on static arrays (existing behavior), runtime on dynamic arrays.
- **Stateful UGens cannot auto-fan-out over dynamic arrays.** `osc("sin", freqs(e))` is a compile error with a directive to use `poly()`. Compile-time fan-out (the existing multi-buffer machinery) requires a static, known-at-compile-time arity. For dynamic-arity polyphonic synthesis, the user composes with the existing `poly()` allocator.
- **`ChordLit` (the `'` suffix) is removed entirely** — token type, AST node, lexer paths, parser branch, codegen branch, and ~30 lexer test cases. It is unused in `web/static/patches/` and adds no value once chord data is array-typed via patterns.
- **One new opcode** is added to fill the array buffer per block from `OutputEvent.values[]`. The exact mechanism (new `SEQPAT_VALUES` opcode vs. mode-extension on `SEQPAT_QUERY`) is captured as Open Question Q1 — both are feasible; pick during implementation.

---

## 2. Problem Statement

### What exists today

| Capability | Status | Reference |
|---|---|---|
| `OutputEvent.values[8]` carries up to 8 notes per event | ✅ | `cedar/include/cedar/opcodes/sequencing.hpp:412,498` |
| `n"[c4,e4,g4]"` mini-notation chord events | ✅ | `akkado/src/mini_lexer.cpp:449`, `pattern_eval.cpp:198` |
| `chord("Am G C")` builtin (pattern-of-chords) | ✅ | `akkado/src/codegen_patterns.cpp:1173` |
| `e.freq`, `e.note` scalar field access on pattern events | ✅ (first-note only) | `akkado/src/codegen.cpp` `pattern_field()` ~1865+ |
| `poly(pattern, instrument, voices)` runtime voice allocator | ✅ | `akkado/src/codegen_functions.cpp:1437` |
| `ChordLit` (`C4'`) syntax → root frequency only | ⚠️ MVP stub, unused in any shipped patch | `akkado/src/codegen.cpp:206` ("For MVP, emit first note (root) of chord") |
| Static array literals `[60, 64, 67]` + `arr[i]` indexing | ✅ | `akkado/src/codegen.cpp:220-262`, `cedar/include/cedar/opcodes/arrays.hpp:37` |
| `len(arr)` compile-time constant | ✅ | `arrays.hpp:77`, `codegen.cpp` (named-builtin special case) |
| `state(init)` cells, `step(arr, trig)`, `counter`, UFCS | ✅ | Shipped by `prd-userspace-state-and-edge-primitives.md` (2026-04-26) |
| Userspace access to chord notes as transformable data | ❌ | — |
| Dynamic-length arrays (length as a runtime signal) | ❌ | — |

### What's missing

To write an arpeggiator that walks the current chord on each trigger, the user must (1) get the chord notes of the current pattern event as data, (2) advance an index on each trigger, (3) wrap the index by the per-event chord length. None of (1) or (3) are currently expressible. (2) lands with the state PRD.

The closest existing surface — `e.freq` — is locked to the first chord note. The internal `OutputEvent.values[]` array is fully populated but never reachable from user code. `poly()` consumes the array internally (one voice per chord note) but is *opaque* — the user's instrument closure receives `(freq, gate, vel)` for one voice at a time and cannot see the chord as a whole.

### Current vs proposed

| Today | After this PRD |
|---|---|
| Chord notes hidden inside `OutputEvent.values[]` | `notes(e)` / `freqs(e)` return them as a dynamic array |
| `e.freq` is scalar (first note) | `e.freq` unchanged + `e.freqs` (UFCS sugar for `freqs(e)`) returns array |
| Stateful chord transforms require new C++ opcodes | Userspace closures over `notes(e)` + `step()` + `map()` |
| `ChordLit` (`C4'`) is dead syntax (unused, root-only) | Removed |
| `len(arr)` is always compile-time | `len()` polymorphic: const for static arrays, signal for dynamic |
| `osc("sin", chord_data)` not expressible (chord data not exposed) | `osc("sin", freqs(e))` is a compile error pointing at `poly()` |

---

## 3. Goals and Non-Goals

### Goals

1. **Surface pattern-event chord notes as dynamic arrays.** Add `notes(e)` (MIDI) and `freqs(e)` (Hz) builtins that return arrays whose length is `e.num_values` per event. With UFCS (state PRD), `e.notes` and `e.freqs` work as sugar.
2. **Add a dynamic-array value-type variant.** Distinct from static `Array` TypedValues, dynamic arrays carry a per-block runtime length signal alongside the data buffer.
3. **Make `len()` polymorphic.** `len(static_arr)` continues to lower to `PUSH_CONST` (compile-time). `len(dynamic_arr)` lowers to a signal read of the array's length buffer (runtime).
4. **Make `arr[i]` work on dynamic arrays.** `ARRAY_INDEX` already supports a runtime length input (`inputs[2][0]` in `arrays.hpp:37`); wire dynamic arrays to use it. Wrap-by-default behavior preserved.
5. **Reject auto-fan-out over dynamic arrays with a clear error.** `osc("sin", freqs(e))` errors at compile time with `E???: Stateful operators cannot auto-expand over dynamic arrays. Wrap with poly() for runtime polyphony.`
6. **Remove `ChordLit` entirely.** Lexer, parser, AST node, codegen branch, ~30 test cases in `test_lexer.cpp`. Sweep `web/static/patches/` (currently zero usages) and docs for any remaining references.
7. **Demo patches.** Ship `web/static/patches/arpeggio-demo.akk` (chord arpeggiator) and `web/static/patches/harmonizer-demo.akk` (interval harmonization) using only the new primitives + state PRD.
8. **Zero regressions on existing scalar accessors.** `e.freq`, `e.note`, `e.vel`, `e.gate`, `e.trig` and existing patches like `web/static/patches/rock-groove.akk` (the only patch using these accessors) compile and produce bit-identical output.

### Non-Goals

- **No `notes("Am7", 4)` builtin** for constructing chord arrays from strings. Patterns are the canonical source of chord data; for static arrays users write `[60, 64, 67]` directly.
- **No per-note `vels(e)` / `gates(e)` / `trigs(e)` array accessors in v1.** The internal `OutputEvent` carries a single velocity / gate / trig per event — a per-note version requires extending `OutputEvent` and the mini-notation. Documented as future work; v1 ships only `notes(e)` and `freqs(e)`.
- **No auto-fan-out of stateful UGens over dynamic arrays.** Use `poly()`. (Static arrays continue to fan out at compile time as today.) The eventual runtime mechanism for this is `EVENT_FANOUT` in `prd-runtime-event-transforms.md` — see Future Work and §10 Q3.
- **No new pattern-syntax features** for per-note attributes (e.g. `[c4!.8, e4!.5]` for per-note velocity). Future work.
- **No new chord-construction builtins**, including `voicing()`, `expand_chord()`, etc.
- **No backward-compatibility shims for `ChordLit`.** Hard removal; sweep done in this PRD.
- **No change to `poly()` semantics.** It continues to consume pattern events and dispatch per-voice; this PRD's array accessors are an *alternative* surface for cases where the user wants to transform the chord, not just sound it.
- **No reactive observers on dynamic arrays.** They're values that change per block, like any other signal.

---

## 4. Target Syntax

### 4.1 Read chord notes from a pattern event

```akkado
// the pattern emits chord events; notes(e) gives the array; len() tells us its size
n"[c4, e4, g4] g3 [a3, c4]" as e
e.notes        // dynamic array of MIDI [60, 64, 67] then [55] then [57, 60] per event
e.freqs        // same as Hz: [261.6, 329.6, 392.0] then [196.0] then ...
len(e.notes)   // dynamic signal: 3, then 1, then 2
```

### 4.2 Arpeggiator (chord step on each trigger)

```akkado
// Walk the current chord on every 8th note, sound through a sine
n"[c4, e4, g4] [a3, c4, e4] g3 [b3, d4, f#4]" as e
  |> e.notes.step(trigger(8))   // UFCS from state PRD: step(notes(e), trigger(8))
  |> note(@)                    // MIDI → Hz
  |> osc("sin", @)
  |> out(@, @)
```

`step()` is provided by the state PRD; it composes with dynamic arrays automatically because `len()` (used internally by `step` to wrap the counter) is polymorphic.

### 4.3 Harmonizer (add intervals on top)

```akkado
// Add a 3rd and 5th to every melody note; play the chord with poly
n"c4 e4 g4 e4" as e
  |> map([0, 4, 7], (i) -> e.note + i)   // [n, n+4, n+7] — a static array of 3 elements
  |> poly(@, (f, g, v) -> osc("sin", note(f)) * ar(g, 0.01, 0.3) * v)
  |> out(@, @)
```

This composes the existing `e.note` (scalar, unchanged) with `map()` over a *static* array of intervals, then sounds with `poly()`. No dynamic array needed for this case.

### 4.4 Polymorphic `len()` examples

```akkado
// Static array — compile-time length
intervals = [0, 4, 7]
len(intervals)         // PUSH_CONST 3 — known at compile time

// Dynamic array — runtime length signal
n"[c4, e4, g4] [a3, c4]" as e
len(e.notes)           // signal: 3 for first event, 2 for second event
```

### 4.5 Compile error on auto-fan-out of dynamic arrays

```akkado
n"[c4, e4, g4]" as e
osc("sin", e.freqs)
// E???: Stateful operators cannot auto-expand over dynamic arrays
//       (chord size varies per pattern event). Wrap with poly() for runtime polyphony:
//       e |> poly(@, (f, g, v) -> osc("sin", f) * ar(g, 0.01, 0.3) * v)
```

---

## 5. Architecture

### 5.1 Dynamic array value type

A new variant on `TypedValue` representing a runtime-varying-length array:

```cpp
// in akkado/include/akkado/typed_value.hpp (extended)
enum class ValueType : std::uint8_t {
    Signal, Number, Pattern, Record, Array, String, Function, Void,
    StateCell,        // from state PRD
    DynArray,         // new: array data buffer + per-block length signal
};

struct DynArrayPayload {
    std::uint16_t data_buffer;   // Holds up to MAX_VALUES_PER_EVENT (=8) values; layout TBD per Q1
    std::uint16_t len_buffer;    // Per-block signal carrying num_values for the current event
    ValueType element_type;      // Number (for notes/freqs); reserved for future expansion
};

struct TypedValue {
    ValueType type = ValueType::Void;
    std::uint16_t buffer = 0xFFFF;
    bool error = false;
    // ...existing fields...
    std::uint32_t cell_state_id = 0;          // from state PRD
    std::shared_ptr<DynArrayPayload> dyn;     // populated when type == DynArray
};
```

### 5.2 Pattern → array opcode

Per Q1, this PRD specifies the *semantics* and leaves the opcode mechanism choice (new `SEQPAT_VALUES` opcode vs. mode-extension on existing `SEQPAT_QUERY`) to implementation. Either way, the mechanism must produce, per block:

- A **data buffer** containing the current event's `values[]` content (in MIDI numbers for `notes(e)`, in Hz for `freqs(e)`).
- A **length buffer** containing `num_values` (broadcast across the block, since the value is constant within a block).

Both buffers are wrapped into a `DynArrayPayload` returned from `notes(e)` / `freqs(e)` codegen.

### 5.3 `notes(e)` and `freqs(e)` builtins

These are special-cased by name in `codegen.cpp`'s call dispatcher (same pattern as `len`):

1. Type-check: argument must be a `Pattern` TypedValue.
2. Allocate `data_buffer` (sized for up to 8 notes; exact storage layout per Q1) and `len_buffer`.
3. Emit the pattern-array opcode (per Q1) bound to the pattern's `state_id` and field selector (raw `values[]` for `notes`, MTOF-converted for `freqs`).
4. Return `TypedValue { type = DynArray, dyn = make_shared<DynArrayPayload>(data_buffer, len_buffer, Number) }`.

UFCS desugar (from state PRD) makes `e.notes` ≡ `notes(e)` automatically.

### 5.4 Polymorphic `len()`

In `codegen.cpp`'s `len` special case:

```cpp
TypedValue arg = visit(args[0]);
switch (arg.type) {
    case ValueType::Array:
        // Existing behavior: emit PUSH_CONST with arg.array_payload->elements.size()
        return emit_push_const(elements.size());
    case ValueType::DynArray:
        // New: return the length buffer as a Signal — no instruction needed,
        // it's already populated by the SEQPAT_VALUES opcode each block
        return TypedValue::signal(arg.dyn->len_buffer);
    default:
        error("E???: len() expects an array");
}
```

### 5.5 `ARRAY_INDEX` on dynamic arrays

`ARRAY_INDEX` (`cedar/include/cedar/opcodes/arrays.hpp:37`) already accepts the array length as a runtime input via `inputs[2][0]`. For dynamic arrays, codegen wires `inputs[0] = data_buffer`, `inputs[1] = index`, `inputs[2] = len_buffer`. No opcode change.

Wrap-by-default (`inst.rate == 0`) is preserved; the wrap math `((j % length) + length) % length` already handles per-sample varying length correctly.

### 5.6 Compile-time error for stateful UGens over dynamic arrays

In the auto-expansion path at `akkado/src/codegen.cpp:1129-1143` (`is_multi_buffer` / `get_multi_buffers`), add a check: if `requires_state` is true and the argument is a `DynArray`, emit the directive error pointing at `poly()`. Static arrays continue to fan out at compile time as today.

### 5.7 ChordLit removal

Pure deletion sweep. No new code; just removal:

| Component | Action |
|---|---|
| `TokenType::ChordLit`, `ChordValue` token data | Delete |
| `NodeType::ChordLit` | Delete |
| `Parser::parse_chord()` (`parser.cpp:532-540`) | Delete |
| `case TokenType::ChordLit` in `parser.cpp:451` | Delete |
| `case NodeType::ChordLit` in `codegen.cpp:206-218` | Delete |
| Lexer paths in `lexer.cpp:419, 554, 672` | Delete |
| ~30 ChordLit test cases in `akkado/tests/test_lexer.cpp` | Delete |
| `akkado/include/akkado/chord_parser.hpp` | **Stays** — still used by `mini_lexer.cpp` and `pattern_eval.cpp` for parsing chord symbols inside pattern-literal strings |

---

## 6. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| Dynamic array value type | **New** | `ValueType::DynArray`, `DynArrayPayload` struct in `typed_value.hpp` |
| `notes(e)` / `freqs(e)` builtins | **New** | `builtins.hpp` entries; codegen special-cased by name |
| Pattern→array opcode | **New (1 opcode or 1 mode)** | Per Q1 — either new `SEQPAT_VALUES` or new mode on `SEQPAT_QUERY` |
| `len()` codegen | **Modified** | Polymorphic dispatch on argument type |
| `ARRAY_INDEX` opcode | **Stays** | Already supports runtime length |
| Auto-expansion check (`is_multi_buffer`) | **Modified** | Reject DynArray with helpful error |
| `ChordLit` (token, AST, parser, codegen, lexer paths) | **Removed** | Plus ~30 lexer tests |
| `chord_parser.hpp` (internal API) | **Stays** | Still used by mini-notation lexer |
| Existing scalar field access (`e.freq`, `.note`, `.vel`, `.gate`, `.trig`) | **Stays** | Unchanged behavior — first-note semantics |
| `poly()` semantics | **Stays** | The PRD's accessors are an alternative surface, not a replacement |
| `web/static/patches/rock-groove.akk` | **Stays** | Uses `@.freq` / `@.trig` — scalar accessors unchanged |
| `web/static/patches/*.akk` (ChordLit usage) | **Sweep** | Currently zero `'` usages confirmed; sweep is a no-op |
| `web/static/patches/arpeggio-demo.akk` | **New** | Demonstrates `e.notes.step(trigger(...))` |
| `web/static/patches/harmonizer-demo.akk` | **New** | Demonstrates `map(intervals, ...)` + `poly()` |
| Auto-generated opcode metadata | **Regenerated (if Q1 → new opcode)** | `cd web && bun run build:opcodes` |

---

## 7. File-Level Changes

### Modify

| File | Change |
|---|---|
| `akkado/include/akkado/typed_value.hpp` | Add `ValueType::DynArray`; add `DynArrayPayload` struct; add `dyn` shared_ptr field |
| `akkado/include/akkado/builtins.hpp` | Register `notes` and `freqs` builtins (codegen-special-cased; signature: `(Pattern) -> DynArray`) |
| `akkado/src/codegen.cpp` | (a) Add name-dispatched branches for `notes` / `freqs` in the call handler. (b) Make `len()` dispatch polymorphic on `Array` vs `DynArray`. (c) Update `Index` (`arr[i]`) handler to wire DynArray's `len_buffer` into `ARRAY_INDEX inputs[2]`. (d) Update auto-expansion check (~line 1129-1143) to reject DynArray for stateful UGens with the `poly()` directive error. (e) **Delete** `case NodeType::ChordLit` (~line 206-218). |
| `akkado/src/parser.cpp` | **Delete** `case TokenType::ChordLit` (line 451-452) and `Parser::parse_chord()` (lines 532-540) |
| `akkado/src/lexer.cpp` | **Delete** chord-literal lex paths (lines 293, 419, 554, 672) |
| `akkado/include/akkado/lexer.hpp` (or token defs) | **Delete** `TokenType::ChordLit`, `ChordValue` token-data variant |
| `akkado/include/akkado/ast.hpp` | **Delete** `NodeType::ChordLit` |
| `akkado/tests/test_lexer.cpp` | **Delete** ~30 ChordLit test cases (lines 253, 294-586, 1034, 1121) |
| `cedar/include/cedar/vm/instruction.hpp` | (Q1-dependent) Either add `SEQPAT_VALUES` opcode or document new mode on `SEQPAT_QUERY` |
| `cedar/src/vm/vm.cpp` | (Q1-dependent) Add dispatch case if new opcode |
| `web/static/docs/reference/builtins/` | New entry: `notes.md`, `freqs.md` (under pattern accessors) |

### Create

| File | Purpose |
|---|---|
| `cedar/include/cedar/opcodes/seqpat_values.hpp` | (If Q1 → new opcode) Implementation of pattern-array fill |
| `web/static/patches/arpeggio-demo.akk` | Demo: chord arpeggiator using `e.notes.step(trigger(8))` |
| `web/static/patches/harmonizer-demo.akk` | Demo: interval harmonization with `map` + `poly` |
| `akkado/tests/test_pattern_event_arrays.cpp` | UFCS + dynamic-array semantics |
| `akkado/tests/test_dynarray_len.cpp` | Polymorphic `len()` |
| `cedar/tests/test_seqpat_values.cpp` | Per-block array fill correctness |
| `experiments/test_op_seqpat_values.py` | Python opcode test |

### Stays — explicitly no change

| File | Reason |
|---|---|
| `akkado/include/akkado/chord_parser.hpp` | Internal API, still used by mini-notation lexer/pattern_eval |
| `akkado/src/codegen_functions.cpp` (handle_poly_call) | `poly()` semantics unchanged |
| `cedar/include/cedar/opcodes/arrays.hpp` (`ARRAY_INDEX`) | Already supports runtime length |
| `web/static/patches/rock-groove.akk` | Uses scalar `@.freq` / `@.trig` — preserved |

---

## 8. Implementation Phases

### Phase 0 — Prerequisite: state PRD must be merged

This PRD's most compelling demos rely on `step()`, `state()`, `counter()`, and UFCS from `prd-userspace-state-and-edge-primitives.md`. **Satisfied** — that PRD shipped 2026-04-26 (all four phases; implementation audit at `docs/audits/prd-userspace-state-and-edge-primitives_audit_2026-05-01.md`). Phase 1 may open immediately.

### Phase 1 — Remove ChordLit

**Status**: TODO  
**Goal**: Clean removal of unused `'`-literal syntax. Independent of array work; can land first.

- [ ] Delete `TokenType::ChordLit`, `ChordValue`, `NodeType::ChordLit`, `parse_chord`, codegen branch, lexer paths
- [ ] Delete ~30 ChordLit test cases in `test_lexer.cpp`
- [ ] Search-and-confirm zero `'`-suffix usages in `web/static/patches/`, `web/static/docs/`
- [ ] Build clean; existing tests pass
- [ ] Update CHANGELOG (breaking change entry)

### Phase 2 — Dynamic array value type + polymorphic `len()`

**Status**: TODO  
**Goal**: Land the new `DynArray` TypedValue and make `len()` dispatch on it. No pattern integration yet — wire up via a synthetic test fixture that constructs a DynArray manually.

- [ ] Add `ValueType::DynArray` and `DynArrayPayload`
- [ ] Make `len()` polymorphic
- [ ] Make `arr[i]` accept DynArray and wire `len_buffer` to `ARRAY_INDEX inputs[2]`
- [ ] Reject DynArray in the stateful-UGen auto-expansion path with the `poly()` directive error
- [ ] Tests: synthetic DynArray indexing, length read, error on stateful-fan-out

### Phase 3 — Pattern → array opcode + `notes(e)` / `freqs(e)` builtins

**Status**: TODO  
**Goal**: Wire pattern events to dynamic arrays end-to-end.

- [ ] Resolve Q1 (new opcode vs mode-extension); implement chosen mechanism
- [ ] Register `notes` and `freqs` builtins; special-case codegen by name
- [ ] Regenerate opcode metadata if Q1 chose new opcode (`bun run build:opcodes`)
- [ ] Tests: `notes(e)` returns expected MIDI array per event; `freqs(e)` returns Hz; UFCS sugar (`e.notes`) works
- [ ] Verify: `len(e.notes)` returns 3 for chord events, 1 for monophonic, varying per event
- [ ] Verify: `osc("sin", freqs(e))` errors at compile time with correct message

### Phase 4 — Demos + docs

**Status**: TODO  
**Goal**: Ship the user-visible end-to-end story.

- [ ] `web/static/patches/arpeggio-demo.akk` — pattern chord arpeggiator
- [ ] `web/static/patches/harmonizer-demo.akk` — interval harmonization
- [ ] Reference docs: `notes.md`, `freqs.md`, polymorphic-`len()` note in `len.md`
- [ ] Tutorial: "Building a userspace arpeggiator" linking the state PRD's `step()` and this PRD's `notes()`
- [ ] Audition both demos in web dev server; verify by ear

### Future Work (out of scope)

- Per-note `vels(e)` / `gates(e)` / `trigs(e)` array accessors — requires extending `OutputEvent` and mini-notation syntax for per-note attributes
- Mini-notation per-note attributes (`[c4!.8, e4!.5]`)
- Static-array form for chord construction (e.g. transpose-aware chord builders) — only if a use case emerges that patterns can't address
- Promotion to a `chord_arp` opcode if the userspace composition is too costly in practice
- Allow auto-fan-out over dynamic arrays via runtime voice slot allocation (effectively a `poly`-like opcode triggered by the auto-expansion path). **`prd-runtime-event-transforms.md`'s `EVENT_FANOUT` opcode is the intended runtime mechanism** — once it lands, the §4.5 compile error can soften into runtime polyphony for chord events instead of forcing `poly()`.

---

## 9. Edge Cases

| Situation | Expected behavior | Rationale |
|---|---|---|
| `notes(e)` on a monophonic pattern event (`num_values == 1`) | Returns DynArray of length 1 | Uniform model: monophony is just "len=1" of the array |
| `len(e.notes)` between events (block boundary) | Length signal updates per event boundary; per-block constant otherwise | Matches the per-block stability of all pattern fields |
| `e.notes[counter(trig)]` where `counter` exceeds chord length | Wraps via `ARRAY_INDEX` default-wrap mode | Existing `((j % len) + len) % len` math works with runtime length |
| Empty event (`num_values == 0`, e.g. silence) | `len(e.notes) == 0`; `arr[i]` returns 0.0 | Matches existing degenerate-array path in `arrays.hpp:44` |
| `osc("sin", freqs(e))` (stateful fan-out over DynArray) | Compile error pointing at `poly()` | Compile-time fan-out requires static arity |
| `osc("sin", [60, 64, 67])` (stateful fan-out over static Array) | Unchanged — fans out to 3 oscillators with stable per-element state IDs | Static arrays preserve existing behavior |
| `map(e.notes, fn)` (dynamic array through a stateless map) | **Open** — does map auto-iterate up to current `len`? Or also reject? | Likely allowed since `map` is stateless; documented in implementation notes |
| `e.freq` on a chord event (existing accessor) | Returns first chord note's frequency (today's behavior, unchanged) | Backward compatibility for monophonic patches |
| Pattern with mixed monophonic + chord events (e.g. `n"c4 [e4,g4] a4"`) | `e.notes` length signal is `1, 2, 1` per event in turn | Naturally falls out of the per-event fill |
| Hot-swap: pattern source code edited | Pattern state preserved per state PRD semantics; DynArray is recreated each block from current event | DynArray has no persistent state of its own |
| Defining a closure named `notes` or `freqs` | Same as state PRD: reserved-builtin error if codegen depends on name resolution; otherwise lexical shadowing wins | Match state PRD's `state`/`get`/`set` precedent if needed |
| `e.notes` as the input to `poly()` | **Future work** — `poly()` consumes a Pattern TypedValue today, not a DynArray. Could be extended to "spawn one voice per dynamic-array element per event" but is out of scope | Composition happens via `poly(e, ...)` (existing) |
| Storing `e.notes` to a state cell | Compile error — state cells hold a single float per state PRD | Documented limitation |

---

## 10. Open Questions

- **Q1: New `SEQPAT_VALUES` opcode vs. mode-extension on `SEQPAT_QUERY`?** Both are feasible. New opcode is cleanest separation of concerns and matches existing per-field opcode pattern (one opcode per pattern field today). Mode-extension keeps opcode count down but introduces a multi-output / different-shape behavior on an opcode that today has uniform single-buffer output. **Recommend deciding during Phase 3 implementation** based on how the storage layout for the array data buffer shakes out — if the array fits one buffer (8 floats packed into the first 8 samples + length buffer), extending `SEQPAT_QUERY` is small; if it needs multiple output buffers (8 separate buffers), a new opcode is cleaner because the instruction format only has one `out_buffer` slot.

- **Q2: Storage layout for the array data buffer.** Three candidates:
  1. **Packed-in-buffer**: First 8 samples of a single buffer hold the chord notes; the rest is unused. `ARRAY_INDEX` reads `arr[index_at_sample_j]` from positions 0-7. Fits the existing single-buffer-per-output instruction format.
  2. **Multi-buffer**: 8 separate buffers, each broadcast-filled with one chord-slot value. Matches static array layout but needs 8 output buffers from the fill opcode (favors a new opcode with extended output slots).
  3. **Scalar-broadcast-per-slot**: Each slot is constant within a block (chord doesn't change mid-block); use existing buffer broadcast.
  
  Likely answer: option 1 (packed) — minimal allocation, fits existing instruction format, `ARRAY_INDEX` already consumes single-buffer arrays this way.

- **Q3: Should `map(dyn_arr, fn)` work?** `map` over a runtime-varying-length array is a useful primitive (per-note transforms inside a chord event). Specifying it requires deciding whether `map` outputs a same-shape DynArray or fans out at compile time. **Recommend: allow `map` over DynArray, output is same-length DynArray, executed via element-wise loop with runtime length.** Carries through to Phase 3. **This decision is also consumed by `prd-runtime-event-transforms.md` OQ-1**, whose chord-wide closure form `(e) -> {notes: map(e.notes, (v) -> v + 7)}` is only well-defined once `map`-over-`DynArray` is specified here — that PRD treats this one as a soft dependency (its §0.6).

- **Q4: Should `notes(e)` / `freqs(e)` accept a Pattern variable that is the result of a pattern-literal *expression* (not a `pipe-bound` variable)?** I.e. is `notes(n"...")` allowed, or must it be `n"..." as e |> notes(e) ...`? Likely answer: both — `notes(e)` accepts any TypedValue::Pattern. Confirm during Phase 3.

- **Q5: Per-note vels/gates** — when do we promote `vels(e)` / `gates(e)` / `trigs(e)` from "future work" to a real PRD? Trigger: when a real use case (e.g. accent patterns within a chord) is requested.

---

## 11. Testing / Verification Strategy

### Phase 1 — ChordLit removal

- All existing tests pass after deletion (compilation succeeds; lexer tests for ChordLit are removed, not adjusted).
- `grep -rE "[A-G][#b]?[0-9]+'" web/static/patches/ web/static/docs/ akkado/tests/` returns zero results.
- Build cleanly: `cmake --build build` and `./build/akkado/tests/akkado_tests`.

### Phase 2 — Dynamic arrays + polymorphic len()

Akkado tests in `akkado/tests/test_dynarray_len.cpp`:

```
- len(static_array) compiles to PUSH_CONST (no instruction beyond const)
- len(dyn_array) returns a Signal whose buffer is the DynArray's len_buffer
- arr[i] on DynArray uses dyn_array's len_buffer for wrap
- osc("sin", dyn_array) raises compile error E??? with poly() directive in message
- osc("sin", static_array) continues to fan out (unchanged behavior)
```

Use a synthetic-DynArray test fixture (no pattern integration yet) to exercise the type system in isolation.

### Phase 3 — Pattern → arrays end-to-end

Akkado tests in `akkado/tests/test_pattern_event_arrays.cpp`:

```
- notes(n"[c4,e4,g4]") yields a DynArray; sample 0 = 60.0, sample 1 = 64.0, sample 2 = 67.0; len = 3
- freqs(n"[c4,e4,g4]") yields the MTOF-converted equivalents; len = 3
- n"c4 [e4,g4] a4" drives len signal through 1, 2, 1 across events
- e.notes UFCS sugar produces identical bytecode to notes(e) (requires state PRD's UFCS)
- e.notes[counter(trig)] (with state PRD primitives) compiles and steps correctly
- e.freq (existing scalar) continues to return first-note value, bit-identical to today
```

Cedar tests in `cedar/tests/test_seqpat_values.cpp` (or extension of existing pattern tests):

```
- Per-block fill: data buffer matches OutputEvent.values[0..num_values-1]
- Length buffer broadcast across BLOCK_SIZE
- Per-event update: length and data change at event boundaries
- Stateful: hot-swap preserves pattern state, DynArray output reflects current event
```

Python in `experiments/test_op_seqpat_values.py`: drive a synthetic pattern with chord events, dump the output buffers, assert chord-by-chord array contents.

### Phase 4 — Demos

- Compile `web/static/patches/arpeggio-demo.akk` and `harmonizer-demo.akk` via akkado-cli; confirm no errors.
- Audition in web dev server; verify arpeggio walks the chord notes, harmonizer adds intervals correctly.
- Listen for click/discontinuity at chord-event boundaries (should be inaudible — wrap is sample-accurate).
- `nkido-cli render --seconds 16 -o /tmp/arp.wav arpeggio-demo.akk` and inspect WAV by ear.

### Build / lint commands

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/akkado/tests/akkado_tests "[pattern_event_arrays]"
./build/akkado/tests/akkado_tests "[dynarray_len]"
./build/cedar/tests/cedar_tests "[seqpat_values]"

cd experiments && uv run python test_op_seqpat_values.py

cd web
bun run build:opcodes      # if Q1 chose new opcode
bun run build:docs         # rebuild docs index
bun run dev                # audition demos
bun run check
```

### Acceptance criteria

- All four target use cases (arp on chord pattern, harmonizer, plus monophonic pattern as `len=1` chord, plus per-event len-varying chord) demonstrated as userspace closures in shipped demo patches.
- `e.freq`, `e.note`, `e.vel`, `e.gate`, `e.trig` produce bit-identical output to pre-PRD on existing patches (`web/static/patches/rock-groove.akk` is the canonical test patch).
- Zero `ChordLit` (`'`-suffix) syntax remains in code, tests, patches, or docs.
- Exactly one new opcode (or one mode-extension on `SEQPAT_QUERY`) added — Q1 documents the chosen mechanism in the implementation PR.
- `len()` is polymorphic with documented compile-time-vs-runtime dispatch.
- `osc("sin", freqs(e))` errors at compile time with the `poly()` directive message.
- The `web/wasm/nkido_wasm.cpp` and `tools/nkido-cli/bytecode_dump.cpp` builds pass after `bun run build:opcodes`.
