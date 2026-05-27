> **Status: SHIPPED (2026-04-27)** — Phases A–C and D1+D2 implemented; D3 (custom-property pipe-binding accessor `e.cutoff`) deferred; coverage closed by audit 2026-04-28 (`docs/audits/prd-pattern-array-note-extensions-phase-2_audit_2026-04-28.md`). Follow-up to `prd-pattern-array-note-extensions.md`. Closes the four unmet goals from `docs/audits/prd-pattern-array-note-extensions_audit_2026-04-24.md`: time/structure modifiers (Phase 7), algorithmic generators (Phase 8), voicing system (Phase 4), and extended note properties (Phase 5).
>
> **Deferred to follow-up:** §5.5a custom-property pipe-binding accessor (`n"c4{cutoff:0.3}" as e |> ... e.cutoff`) — requires new cedar runtime buffer plumbing to expose per-event property values to synth code. Custom keys are parsed and stored on `MiniAtomData.properties` but not yet reachable from runtime. The standalone `bend()` / `aftertouch()` / `dur()` *transforms* (PRD §4.4 first form) are also deferred for the same reason — bend/aftertouch need cedar `Event` fields and SEQPAT plumbing. Only `dur()` and `vel` propagate to runtime today (via the existing velocity buffer and `cedar::Event.duration`).

# PRD: Strudel-Style Pattern System Extensions — Phase 2

## 1. Executive Summary

The original `prd-pattern-array-note-extensions.md` shipped its foundational layers — arrays, `map()`, the chord system, polymeter, and dot-call desugaring — and is in steady use. The 2026-04-24 audit identified four goals that remain unmet:

1. **Phase 7 — Time & structure modifiers**: 12 transforms (`early`, `late`, `palindrome`, `iter`, `iterBack`, `ply`, `linger`, `zoom`, `compress`, `segment`, `swing`, `swingBy`).
2. **Phase 8 — Algorithmic generators**: `run`, `binary`, `binaryN`. (`euclid` already ships.)
3. **Phase 4 — Voicing system**: `anchor`, `mode`, voice leading, `addVoicings`.
4. **Phase 5 — Extended note properties**: `bend`, `aftertouch`, `dur` fields and transforms.

Phase 2 of this PRD ships all four. It does **not** revisit string-as-pattern auto-parsing — that question is closed by the existing `n"..."` prefix and by sampler-vs-note autodetection in the codegen. It does not add new opcodes except for one targeted change to `SEQPAT_STEP` to expose a runtime cycle counter for `iter`/`iterBack`/(optionally) `palindrome`.

### Key Design Decisions

- **Dot-call is free for everything.** Method chaining is pure desugaring at the AST level. No new builtin needs special treatment to gain `.method()` syntax — register the function and chaining works.
- **Voicing is a pattern transform, not a primitive.** `chord("Am C G").anchor("c4").mode("below")` desugars to nested function calls; voicings are computed at compile time from chord intervals + anchor + mode using a greedy nearest-voicing algorithm.
- **`compress` becomes a pattern transform.** The existing `compress` → `comp` alias on the audio compressor is removed to free the name for the Strudel-canonical pattern transform. Users who want the compressor write `comp(...)` or `compressor(...)`.
- **`iter`/`iterBack` get one new runtime opcode field.** `SEQPAT_STEP` gains a cycle counter so `iter` can advance its starting position per cycle without exploding event counts at compile time.
- **`PatternEvent` gains two fixed fields and one extensibility bag.** `bend` and `aftertouch` join `velocity` as first-class fields; arbitrary user properties go in a `std::vector<std::pair<std::string, float>>` that is empty by default (zero overhead when unused).
- **Mini-notation gains a record-style suffix.** `c4{vel:0.8, bend:0.2}` is parsed as a note with named properties. The existing positional `c4:0.8` form keeps working as a velocity shorthand.

---

## 2. Problem Statement

The audit summary:

> Goals met: 4 of 8. Phases 1, 3, 6 fully shipped. Phase 2 resolved via dot-call desugaring. Phases 4, 5, 7, 8 incomplete.

Specifically:

| Area | Audit finding |
|------|---------------|
| Phase 7 transforms | 0 of 12 implemented. PRD listed it "IN PROGRESS" but no work in tree. |
| Phase 8 generators | Only `euclid` present. `run`, `binary`, `binaryN` absent from builtins and codegen. |
| Phase 4 voicing | No `voicing.hpp`/`voicing.cpp`. No `anchor`/`mode`/`addVoicings`. |
| Phase 5 note props | `velocity` real; `bend`, `aftertouch`, `dur` overrides absent from `PatternEvent` and unsurfaced. |

The user-visible consequence: idiomatic Strudel patterns that lean on `early`, `palindrome`, `ply`, `swing`, `run`, etc. do not transfer to Akkado. Composers reach for these constantly in live coding; their absence is the most common rough edge in the pattern system today.

---

## 3. Goals and Non-Goals

### Goals

1. Ship all 12 time/structure modifiers from the original PRD's Phase 7 table.
2. Ship `run(n)`, `binary(n)`, `binaryN(n, bits)` as compile-time pattern constructors.
3. Ship voicing: `anchor`, `mode`, voice leading, `addVoicings` with built-in dictionaries.
4. Extend `PatternEvent` with `bend`, `aftertouch`, and a `properties` bag, plus `.bend()`, `.aftertouch()`, `.dur()` transforms.
5. Add record-style mini-notation suffix syntax: `c4{vel:0.8, bend:0.2}`.
6. Per-modifier unit tests; golden MIDI-note assertions for voicing.
7. Resolve the `compress` naming collision by removing the `compress` → `comp` audio-compressor alias.

### Non-Goals

- **String-as-pattern auto-parsing of bare strings.** Already addressed via `n"..."` prefix and sampler-vs-note autodetection in `codegen_patterns.cpp:337,731`. Bare strings remain bare strings outside pattern slots.
- **MIDI export of `bend` / `aftertouch`.** Phase 2 plumbs the values through; consuming them in a MIDI-out path is a separate PRD.
- **`inside` / `outside` higher-order transforms.** Listed as deferred in the original PRD §8; remains deferred.
- **Optimal-path voice leading (DP).** Greedy nearest-voicing only.
- **New audio opcodes.** Only `SEQPAT_STEP` gains a cycle-counter field; everything else is compile-time event-list manipulation.

---

## 4. Target Syntax

### 4.1 Time & Structure Modifiers

```akkado
// Functional and dot-call forms work identically (existing desugaring)
early(n"c4 e4 g4", 0.25)
n"c4 e4 g4".early(0.25)

// Time shifts (wrap around cycle)
s"bd sd hh cp".early(0.125)   // shift earlier by 1/8 cycle
s"bd sd hh cp".late(0.125)    // shift later by 1/8 cycle

// Reversal modifiers
n"c4 e4 g4 b4".palindrome()   // forward, then reverse, every 2 cycles

// Subdivision-based
n"c4 e4 g4 b4".iter(4)        // start subdivision rotates per cycle
n"c4 e4 g4 b4".iterBack(4)    // same, opposite direction

// Density
s"bd sd".ply(3)               // each event repeats 3x in its slot
n"c4 e4 g4 b4".linger(0.5)    // keep first half, repeat to fill cycle

// Time-window
s"bd sd hh cp".zoom(0.25, 0.75)   // play middle 50%, stretched to fill cycle
s"bd sd hh cp".compress(0.25, 0.75) // squash whole pattern into [0.25, 0.75)

// Discretization
n"c4 e4 g4 b4".segment(8)    // sample at 8 evenly-spaced points

// Swing
s"bd hh sd hh".swing()        // shorthand for swingBy(1/3, 4)
s"bd hh sd hh".swing(8)       // 8-slice swing
s"bd hh sd hh".swingBy(0.4, 4) // custom delay amount
```

### 4.2 Algorithmic Generators

```akkado
// run(n): integer sequence 0..n-1 as evenly-spaced events
run(8)                            // 0,1,2,3,4,5,6,7
run(8) |> mtof(@ + 60) |> osc("saw", @)   // ascending chromatic

// binary(n): trigger pattern from binary representation of n
binary(0b1010)                    // 4 steps: 1, 0, 1, 0
binary(170) |> sampler(@, "kick") // 170 = 0b10101010, 8 steps

// binaryN(n, bits): zero-padded fixed-width
binaryN(5, 8)                     // 0b00000101 = 0,0,0,0,0,1,0,1

// Generators chain via dot-call (free, via desugaring)
run(16).fast(2).rev() |> mtof(@ + 48) |> osc("sin", @)
binary(0b11010010).slow(2).ply(2) |> sampler(@, "hh")
```

### 4.3 Voicing

```akkado
// Built-in voicing modes
chord("Am C G F").anchor("c4").mode("below")   // all notes below c4
chord("Am C G F").anchor("c5").mode("above")   // all notes above c5
chord("Am C G F").anchor("c4").mode("duck")    // closest avoiding c4
chord("Am C G F").anchor("c4").mode("root")    // root in bass, rest near c4

// Composes with audio
chord("Am C G F")
  .anchor("c4")
  .mode("below")
  |> mtof(@)
  |> poly(@, lead, 4)
  |> out(@, @)

// Voice leading is automatic when mode is set (greedy nearest)
// Each chord change picks the inversion minimizing total semitone movement.

// Built-in voicing dictionary
chord("Am").voicing("drop2")   // standard drop-2 voicing
chord("Am").voicing("open")    // open position
chord("Am").voicing("closed")  // close position

// Custom voicing dictionaries (string-keyed record; quality names may include
// non-identifier chars like "m7b5" so record literal accepts string keys here)
addVoicings("piano-jazz", {
  "M":     [0, 4, 7, 11, 14],   // intervals from root
  "m":     [0, 3, 7, 10, 14],
  "7":     [0, 4, 10, 13, 17],
  "m7b5":  [0, 3, 6, 10]
})
chord("CM Am7 Dm7 G7").voicing("piano-jazz")
```

### 4.4 Extended Note Properties

```akkado
// Transforms (mirror velocity() pattern)
n"c4 e4 g4".bend("<0 0.5 -0.5>")
n"c4 e4 g4".aftertouch(0.7)
n"c4 e4 g4".dur(0.5)         // override per-event duration

// Mini-notation record suffix (new)
n"c4{vel:0.8, bend:0.2} e4{vel:1.0} g4{vel:0.5, bend:-0.3}"

// Custom user properties via the same record suffix
n"c4{vel:0.8, cutoff:0.3} e4{cutoff:0.7}"
// The `cutoff` value lands in PatternEvent.properties as ("cutoff", 0.3).
// Reachable from synth code via record field access on the bound event:
n"c4{cutoff:0.3} e4{cutoff:0.7}" as e
  |> osc("saw", e.freq)
  |> lp(@, 200 + e.cutoff * 4000)
  |> out(@, @)

// Existing positional shorthand still works
n"c4:0.8 e4:1.0"              // colon = velocity (unchanged)
```

> **Note:** `bend` is per-event in Phase 2 — it sets a value on each event, like `velocity`. Continuous (signal-rate) pitch-bend is a richer model and is out of scope here; see §11.

---

## 5. Architecture

### 5.1 Time & Structure Modifiers — Implementation Pattern

All 12 transforms follow the existing `slow`/`fast`/`rev` pattern (see `akkado/src/codegen_patterns.cpp:1618` `compile_pattern_for_transform`):

1. Register in `akkado/include/akkado/builtins.hpp` with `Opcode::NOP`.
2. Add to `is_pattern_call()` switch (`akkado/src/codegen_patterns.cpp:1585`).
3. Add to the transform-recognition switch in `compile_pattern_for_transform()` (`:1675`).
4. Add transform logic that mutates `out_events` and/or `out_cycle_length`.
5. Add a `handle_<name>_call()` dispatch handler in `codegen_patterns.cpp` and register it in `codegen.cpp`.

**Per-transform compile-time semantics:**

| Transform | Implementation |
|-----------|----------------|
| `early(pat, n)` | For each `event.time`: `event.time = (event.time - n + 1) mod 1`. |
| `late(pat, n)` | For each `event.time`: `event.time = (event.time + n) mod 1`. |
| `palindrome(pat)` | Double `out_cycle_length`. Append a reversed copy of all events with `event.time' = 1 + (1 - event.time - event.duration)`. Pure compile-time. |
| `ply(pat, n)` | For each event of duration `d` at time `t`: replace with `n` events of duration `d/n` at times `t, t+d/n, ..., t+(n-1)d/n`. |
| `linger(pat, frac)` | Drop events with `time >= frac`. Scale remaining events' time and duration by `1/frac`. Tile to fill cycle by appending shifted copies. |
| `zoom(pat, start, end)` | Drop events outside `[start, end)`. Map remaining: `t' = (t - start) / (end - start)`, `d' = d / (end - start)`. |
| `compress(pat, start, end)` | Map all event times: `t' = start + t * (end - start)`, `d' = d * (end - start)`. |
| `segment(pat, n)` | At each of `n` evenly-spaced sample points, find the active event; emit a new event of duration `1/n` carrying that event's value. |
| `swing(pat, n)` | Equivalent to `swingBy(pat, 1/3, n)`. |
| `swingBy(pat, amount, n)` | Divide cycle into `n` slices; for each slice, events whose offset within the slice is `>= 0.5` get `time += amount * (1/(2n))`. |
| `iter(pat, n)` | **Runtime.** See §5.2. |
| `iterBack(pat, n)` | **Runtime.** See §5.2. |

Defaults: `swing(pat, n=4)`, `swingBy(pat, amount, n=4)`.

### 5.2 Runtime Cycle Counter for `iter` / `iterBack`

`iter(pat, n)` plays the full pattern every cycle, but rotates the pattern's time origin by `1/n` per cycle. After one cycle, the rotation is `1/n`; after two, `2/n`; and so on, wrapping at `n`. `iterBack` rotates in the opposite direction. This requires runtime cycle awareness.

**Semantics (precise):** Let `k = cycle_index mod n` and let `dir = +1` for `iter`, `-1` for `iterBack`. An event at original time `t` plays at effective time `((t - dir * k / n) mod 1)` within the current cycle. The full event list is queried each cycle; only the time origin shifts.

**Approach:** Add three fields to `cedar::SequenceState` (defined at `cedar/include/cedar/opcodes/sequence.hpp:146`, the state struct read by `op_seqpat_step` in `cedar/include/cedar/opcodes/sequencing.hpp:415`).

| Field | Location | Purpose |
|-------|----------|---------|
| `std::uint32_t cycle_index` | `cedar::SequenceState` | Increments at each cycle wrap. |
| `std::uint8_t iter_n` | `cedar::SequenceState` | Number of subdivisions; 0 = no iter. |
| `std::int8_t iter_dir` | `cedar::SequenceState` | +1 for `iter`, -1 for `iterBack`, 0 for none. |

**At query time:** if `iter_n > 0`, the query's local cycle phase is shifted by `-dir * (cycle_index mod iter_n) / iter_n` before event lookup. Compile-time still emits the unrotated event list; the rotation is purely a query-time time-origin transform. `cycle_index` increments at the existing cycle-wrap detection (`beat_pos < state.last_beat_pos - 0.5f`).

**Codegen:** `iter` and `iterBack` are compile-time *configurators* — they set `iter_n` and `iter_dir` on the emitted `SeqInit` payload. They do not multiply event count.

### 5.3 Algorithmic Generators

Generators are pattern *constructors* — they emit a `PatternEventStream` at compile time and then go through the same `SEQPAT_QUERY`/`SEQPAT_STEP` pipeline as any other pattern.

| Function | Compile-time output |
|----------|---------------------|
| `run(n)` | `n` events at times `0, 1/n, 2/n, ..., (n-1)/n`, each carrying value `0, 1, 2, ...` (n-1) and duration `1/n`. |
| `binary(n)` | `bits = floor(log2(n)) + 1` events. Event `i` is a trigger if bit `i` of `n` is set, else a rest. |
| `binaryN(n, bits)` | Same as `binary` but exactly `bits` events; high-order zeros emit rests. |

Each gets a `handle_run_call`/`handle_binary_call`/`handle_binaryN_call` in `codegen_patterns.cpp` that builds the event stream directly and emits the standard SEQPAT instruction sequence.

**Dot-call composition** is automatic: `run(8).fast(2).rev()` desugars to `rev(fast(run(8), 2))` at the AST level (existing analyzer behavior). No special generator-side support needed.

### 5.4 Voicing System

**Files to create:**
- `akkado/include/akkado/voicing.hpp` — public API
- `akkado/src/voicing.cpp` — algorithm implementation

**Public API:**

```cpp
namespace akkado::voicing {

enum class Mode { Below, Above, Duck, Root };

struct VoicingDict {
    std::unordered_map<std::string, std::vector<int>> qualities;
};

// Apply voicing to a chord progression. Returns one MIDI-note vector per chord,
// each already voice-led to minimize movement from the previous.
std::vector<std::vector<int>> voice_chords(
    const std::vector<ChordEventData>& chords,
    int anchor_midi,
    Mode mode,
    const VoicingDict* dict = nullptr  // nullptr = built-in voicings
);

// Global registration (Phase 5 of original PRD §8 carryover)
void register_voicing(std::string_view name, VoicingDict dict);
const VoicingDict* lookup_voicing(std::string_view name);

} // namespace akkado::voicing
```

**Algorithm — greedy nearest voicing per transition:**

1. Generate candidate inversions for each chord (root position + N-1 inversions).
2. For each candidate, apply `mode` constraint:
   - `below`: shift by octaves until all notes ≤ anchor.
   - `above`: shift by octaves until all notes ≥ anchor.
   - `duck`: pick the inversion+octave with smallest sum of `|note - anchor|` that does not include `anchor` itself.
   - `root`: keep root note as-is in bass octave; voice remaining notes near anchor.
3. For chord *i+1*, pick the candidate that minimizes total semitone distance from chord *i*'s chosen voicing.

Built-in voicing dictionaries available out of the box: `"close"`, `"open"`, `"drop2"`, `"drop3"`. Users register custom ones via `addVoicings()`.

**Codegen integration:** `anchor`, `mode`, `voicing` are pattern transforms (NOP builtins). `compile_pattern_for_transform()` recognizes them as "voicing transforms" — when present on a chord pattern, the chord events are routed through `voice_chords()` before being emitted as ordinary multi-value pattern events.

### 5.5 Extended Note Properties

**`PatternEvent` changes** (`akkado/include/akkado/pattern_event.hpp:34`):

```cpp
struct PatternEvent {
    PatternEventType type = PatternEventType::Rest;

    // Timing
    float time = 0.0f;
    float duration = 1.0f;
    bool dur_override = false;   // NEW: true if .dur() set duration explicitly

    // Dynamics
    float velocity = 1.0f;
    float chance = 1.0f;
    float bend = 0.0f;           // NEW: pitch bend, -1..1; default 0.0 = neutral
    float aftertouch = 0.0f;     // NEW: aftertouch, 0..1; default 0.0 = none

    // Existing fields...
    std::uint8_t midi_note = 60;
    // ...

    // NEW: extensible custom properties
    std::vector<std::pair<std::string, float>> properties;
};
```

**Override-flag asymmetry:** `dur_override` is a separate bool because `duration = 1.0` is a meaningful explicit value (one-cycle event). For `bend` and `aftertouch`, the default `0.0` already denotes "neutral / no message", and we deliberately do **not** distinguish "unset" from "explicitly 0" — a per-event override flag would only matter for a future MIDI-out path, and that path can re-derive the distinction from whether the pattern was wrapped in a `.bend()` or `.aftertouch()` transform call (out of Phase 2 scope).

**Transforms** mirror `velocity()` once velocity is unblocked (see Phase D step 0):

| Transform | Effect |
|-----------|--------|
| `bend(pat, value_or_pat)` | Sets `event.bend` on each event. Pattern arg cycles values like `velocity()` does. |
| `aftertouch(pat, value_or_pat)` | Sets `event.aftertouch`. |
| `dur(pat, value_or_pat)` | Sets `event.duration` and `event.dur_override = true`. |

All three follow the existing handler pattern in `codegen_patterns.cpp` and use `compile_pattern_for_transform()` recursion.

**Recognized field-name aliases (record suffix):** Only short forms populate the fixed `PatternEvent` fields. Long forms go into the properties bag. Specifically:

| Recognized key | Fixed field |
|----------------|-------------|
| `vel` | `velocity` |
| `bend` | `bend` |
| `aftertouch` | `aftertouch` |
| `dur` | `duration` (sets `dur_override = true`) |

Anything else — `velocity`, `cutoff`, `freq`, etc. — lands in `event.properties`. This is intentional: the short forms are the canonical mini-notation surface; long forms (and any user-defined keys) are user-property bag values.

### 5.5a Custom-Property Field Access (`as e |> ... e.<key>`)

Custom properties from `c4{cutoff:0.3}` need to be reachable from synth code. AST scaffolding for `as` pipe-binding exists (`PipeBindingData` in `akkado/include/akkado/ast.hpp:271`), but the codegen path that resolves `e.<key>` against a `properties` vector is **new work** — not a small wiring step.

**Resolution rule:** when an expression `e.X` is encountered and `e` is bound to a pattern event:

1. If `X` matches a fixed `PatternEvent` field (`time`, `duration`, `velocity`, `bend`, `aftertouch`, `chance`, `midi_note`, `freq`/`pitch`/`f`, `note`/`midi`/`n`, `vel`/`v`, `trig`/`trigger`/`t`), emit existing fixed-field codegen.
2. Otherwise, look up `X` in the *static* set of property keys observed in the bound pattern's `MiniAtomData.properties` at compile time. If found, emit a load from the property slot.
3. If not found anywhere in the pattern's events, emit error E15x: `"unknown property '<key>' on pattern event 'e'"` with a list of the keys that were defined.

**Compile-time vs runtime:** because property keys are static (set in source), the lookup is resolved at codegen time to a stable per-key buffer index — no runtime string lookup. The `properties` vector on `PatternEvent` exists for compile-time AST→codegen plumbing, not for runtime hash-map access.

**Missing-key behavior:** if event A defines `cutoff:0.3` and event B does not, reading `e.cutoff` on B yields `0.0`. Document this as the explicit semantics. (Alternative: error on missing-per-event keys — rejected because it would force users to set every property on every event.)

### 5.6 Mini-Notation Record Suffix

**Lexer** (`akkado/src/mini_lexer.cpp`): when a `{` follows a note token, switch into a "note record" sub-mode that lexes `key:value` pairs separated by `,` until `}`.

**Parser** (`akkado/src/mini_parser.cpp`): a `MiniNote` AST node gains an optional `properties` field (vector of `(name, value)` pairs).

**Evaluator** (`akkado/src/pattern_eval.cpp`): when emitting a `PatternEvent`, copy `properties` from the AST node. Recognized names (`vel`, `bend`, `aftertouch`, `dur`) populate the corresponding fixed fields; unrecognized names go into `event.properties`.

**Backwards compatibility:** the existing positional `c4:0.8` (velocity shorthand) is unchanged. Lexer disambiguates: `:` after a note → positional, `{` after a note → record.

### 5.7 `compress` Naming Resolution

**Decision:** Remove the `compress` → `comp` alias from `BUILTIN_ALIASES` (`akkado/include/akkado/builtins.hpp:983`). `compress` becomes the pattern transform exclusively. Audio-side users write `comp(...)` or `compressor(...)` — both still aliased to the audio compressor.

**Migration impact:** any user code calling `compress(signal, ratio, ...)` for audio must rename to `comp(...)`. This is a breaking change; we accept it because (a) `comp`/`compressor` aliases keep the audio compressor reachable, (b) the pattern transform is more frequently needed than a third spelling of the compressor, and (c) Strudel uses `compress` for the time transform exclusively.

---

## 6. Impact Assessment

| Component | Status | Notes |
|-----------|--------|-------|
| `n"…"`/`v"…"`/`s"…"`/`c"…"`, `chord()`, `seq()`, `timeline()` | **Stays** | Constructors unchanged. |
| Existing transforms (`slow`, `fast`, `rev`, `transpose`, `velocity`, `bank`, `variant`, `tune`) | **Stays** | Implementation pattern unchanged; new transforms follow the same shape. |
| `is_pattern_call()` switch | **Modified** | Add 12 new transform names + 3 new constructors. |
| `compile_pattern_for_transform()` | **Modified** | Add 12 new event-list rewrite branches; `iter`/`iterBack` set state instead of rewriting events. |
| `PatternEvent` struct | **Modified** | Add `bend`, `aftertouch`, `dur_override`, `properties`. |
| `cedar::SequenceState` | **Modified** | Add `cycle_index`, `iter_n`, `iter_dir`. (Defined in `cedar/include/cedar/opcodes/sequence.hpp:146`, not in any `state/` directory.) |
| `BUILTIN_ALIASES` (`compress` → `comp`) | **Removed** | Frees `compress` for the pattern transform. |
| `BUILTINS` map | **Modified** | Add 22 entries: 12 time/structure transforms (`early`, `late`, `palindrome`, `iter`, `iterBack`, `ply`, `linger`, `zoom`, `compress`, `segment`, `swing`, `swingBy`) + 3 generators (`run`, `binary`, `binaryN`) + 4 voicing (`anchor`, `mode`, `voicing`, `addVoicings`) + 3 note-prop transforms (`bend`, `aftertouch`, `dur`). |
| Mini-notation lexer/parser | **Modified** | Add record-suffix sub-mode after notes. |
| `voicing.hpp` / `voicing.cpp` | **New** | Voicing algorithms + dictionary registry. |
| Audio compressor implementation | **Stays** | `comp` and `compressor` aliases unchanged. |
| Existing tests | **Stays** | All current behavior preserved (except removed `compress` alias — must update any test using it). |

---

## 7. File-Level Changes

| File | Change |
|------|--------|
| `akkado/include/akkado/builtins.hpp` | Add 22 new entries (`early`, `late`, `palindrome`, `iter`, `iterBack`, `ply`, `linger`, `zoom`, `compress`, `segment`, `swing`, `swingBy`, `run`, `binary`, `binaryN`, `anchor`, `mode`, `voicing`, `addVoicings`, `bend`, `aftertouch`, `dur`). Remove `{"compress", "comp"}` from `BUILTIN_ALIASES` (line 983). |
| `akkado/include/akkado/codegen.hpp` | Declare 22 new `handle_*_call` functions. |
| `akkado/src/codegen.cpp` | Register 22 new dispatch entries in the call-handler map. |
| `akkado/src/codegen_patterns.cpp` | Implement 22 handlers + extend `compile_pattern_for_transform()` with new transform branches + extend `is_pattern_call()` recognition. Fix existing `handle_velocity_call` (currently a stub — see Phase D step 0). |
| `akkado/include/akkado/pattern_event.hpp` | Add `bend`, `aftertouch`, `dur_override`, `properties` fields to `PatternEvent`. |
| `akkado/src/pattern_eval.cpp` | Copy record-suffix properties from AST into `PatternEvent`. |
| `akkado/include/akkado/mini_token.hpp` | Token-class changes for record-suffix lexing if needed (likely reuse existing brace tokens). |
| `akkado/src/mini_lexer.cpp` | Add note-record sub-mode (lexes `{key:value,...}` after a note). |
| `akkado/include/akkado/ast.hpp` | Add optional `properties` field to `MiniNote` data. |
| `akkado/src/mini_parser.cpp` | Parse record suffix into `MiniNote.properties`. |
| `cedar/include/cedar/opcodes/sequence.hpp` (line ~146, struct `SequenceState`) | Add `cycle_index`, `iter_n`, `iter_dir` fields. |
| `cedar/include/cedar/opcodes/sequencing.hpp` (line ~415, `op_seqpat_step`) | Increment `cycle_index` at the existing cycle-wrap detection; apply iter rotation at query time. |
| `akkado/include/akkado/voicing.hpp` | **New.** Public voicing API. |
| `akkado/src/voicing.cpp` | **New.** Greedy voice-leading algorithm + built-in dictionaries + `addVoicings` registry. |
| `akkado/tests/test_pattern_transforms.cpp` (new or existing) | One unit test per new transform. |
| `akkado/tests/test_pattern_generators.cpp` (new) | Unit tests for `run`, `binary`, `binaryN`. |
| `akkado/tests/test_voicing.cpp` (new) | Golden MIDI-note assertions for chord+anchor+mode combinations. |
| `akkado/tests/test_pattern_event.cpp` (existing) | Tests for `bend`, `aftertouch`, `dur` transforms and record-suffix parsing. |
| `akkado/tests/test_builtins.cpp` | Update `canonical_name("compress") == "comp"` test — either remove or assert it errors after alias removal. `canonical_name("compressor") == "comp"` stays. |
| `web/static/docs/reference/pattern/transforms.md` (existing or new) | Document new transforms; update F1 help index via `bun run build:docs`. |
| `docs/mini-notation-reference.md` | Document `c4{vel:0.8, bend:0.2}` record suffix. |
| `docs/prd-pattern-array-note-extensions.md` | Status line + Phase 4/5/7/8 sections updated to defer to this PRD. |
| `docs/audits/prd-pattern-array-note-extensions_audit_2026-04-24.md` | Note that unmet goals are tracked in Phase 2. |

---

## 8. Implementation Phases

Staged so each sub-phase ships independently. Earlier phases unblock later ones (most notably, Phase A's `compile_pattern_for_transform()` extensions establish the patterns Phase B and Phase D will reuse).

### Phase A — Time & Structure Modifiers

**Goal:** All 12 transforms ship and chain via dot-call.

**Steps:**
1. Land the easy compile-time-only transforms first: `early`, `late`, `palindrome`, `compress`. (Pure event-list rewrites.) Drop the `compress` → `comp` alias.
2. Add `ply`, `linger`, `zoom`, `segment`. (Slightly more complex rewrites.)
3. Add `swing`, `swingBy`. (Slice-based timing offset.)
4. Add `SEQPAT_STEP` cycle-counter fields and implement `iter`, `iterBack`. (Touches Cedar.)

**Verification per step:**
- Unit test per transform: input pattern + expected output event list.
- Mini-notation/dot-call integration test for each: confirm `n"…".x(args)` and `x(n"…", args)` produce identical output.
- For `iter`/`iterBack`: Python experiment in `experiments/test_op_seqpat_step.py` exercising the cycle-counter rotation across multiple cycles.

**Acceptance:** `n"c4 e4 g4 b4".palindrome().ply(2)` compiles, plays, and event list matches hand-computed expectation.

### Phase B — Algorithmic Generators

**Goal:** `run`, `binary`, `binaryN` ship as pattern constructors.

**Steps:**
1. Implement `run(n)` — simplest, validates the constructor pattern.
2. Implement `binary(n)`.
3. Implement `binaryN(n, bits)`.

**Verification:**
- Unit test per generator: assert event count, times, values match spec.
- Composition test: `run(8).fast(2).rev()` works (validates dot-call free composition).

**Acceptance:** `binary(0b10110010) |> sampler(@, "hh")` produces an audible 8-step rhythm matching the bit pattern.

### Phase C — Voicing System

**Goal:** `anchor`, `mode`, `voicing`, `addVoicings` ship with greedy voice leading.

**Steps:**
1. Create `voicing.hpp` / `voicing.cpp` with `voice_chords()` and the dictionary registry.
2. Implement built-in voicing dictionaries (`close`, `open`, `drop2`, `drop3`).
3. Wire `anchor`, `mode`, `voicing` into `compile_pattern_for_transform()` as voicing transforms on chord patterns.
4. Wire `addVoicings()` as a top-level statement that registers a dictionary at compile time.

**Verification:**
- Golden test fixtures: `(chord, anchor_midi, mode) → expected MIDI notes`.
- Voice-leading distance test: for a chord progression, total semitone movement is ≤ a hand-verified bound.
- Custom dictionary test: `addVoicings("test", {...})` then `chord(...).voicing("test")` round-trips.

**Acceptance:** `chord("Am C G F").anchor("c4").mode("below") |> mtof(@) |> poly(@, lead, 4) |> out(@, @)` plays with audible smooth voice leading.

### Phase D — Extended Note Properties

**Goal:** `bend`, `aftertouch`, `dur` transforms + record-suffix mini-notation.

**Steps:**
0. **Prerequisite — un-stub `velocity()`.** Current `handle_velocity_call` (`codegen_patterns.cpp:2398`) is a stub: its own comment says "currently doesn't modify velocity since velocity is not stored in cedar::Event." Make it write `event.velocity` in the emitted instruction stream so the new transforms can be implemented by genuinely mirroring it (rather than mirroring a stub).
1. Extend `PatternEvent` with `bend`, `aftertouch`, `dur_override`, `properties`.
2. Implement `.bend()`, `.aftertouch()`, `.dur()` transforms (mirrors fixed `.velocity()`).
3. Extend mini-notation lexer/parser for `c4{key:value,...}` suffix.
4. Wire record-suffix property values into `PatternEvent` (recognized short-form keys → fixed field per the §5.5 alias table; everything else → `properties` bag).
5. Wire `properties` bag access through `as e |> ... e.cutoff` pipe-binding per §5.5a (compile-time lookup; new codegen path).

**Verification:**
- Transform tests: input event with default field, apply transform, assert field is set.
- Mini-notation test: parse `n"c4{vel:0.8,bend:0.2}"`, assert resulting `PatternEvent.velocity == 0.8` and `event.bend == 0.2`.
- Custom property test: `n"c4{cutoff:0.3}" as e |> lp(@, 200 + e.cutoff * 4000)` compiles to the expected cutoff.

**Acceptance:** `n"c4 e4 g4".bend("<0 0.5 -0.5>") |> mtof(@ + bend(@) * 12) |> osc("sin", @)` audibly bends.

### Phase E — Cross-Phase Smoke Acceptance

**Goal:** All four areas compose in a single program without regression.

**Smoke test program** — must compile clean and produce audible output:

```akkado
// 1. Voicing-led chord progression (Phase C)
chord("Am C G F").anchor("c4").mode("below") as ch
  |> mtof(ch)
  |> poly(@, lead, 4)
  |> out(@, @)

// 2. Time-modified pattern (Phase A)
n"c4 e4 g4 b4".early(0.125).palindrome() as p
  |> mtof(p)
  |> osc("saw", @)
  |> out(@, @)

// 3. Generator-driven sample pattern (Phase B)
binary(0b10110010) |> sampler(@, "hh") |> out(@, @)

// 4. Bend + aftertouch + custom-property pattern (Phase D)
n"c4{vel:0.8, bend:0.2, cutoff:0.3} e4{vel:1.0, cutoff:0.7}" as e
  |> osc("saw", e.freq)
  |> lp(@, 200 + e.cutoff * 4000)
  |> out(@, @)
```

**Acceptance:** the program compiles with `cmake --build build` clean, runs in `nkido render` for ≥ 30 s without crashes, and an experiment script confirms event-list correctness for blocks 0–N (per §10.3).

This phase blocks PRD closure — no Phase A–D claim is "done" until they all coexist in this program.

---

## 9. Edge Cases

### 9.1 Time Modifiers

- **`early(pat, n)` with `n > 1` or `n < 0`:** Wrap into `[0,1)` via modulo. `early(pat, 1.25)` ≡ `early(pat, 0.25)`.
- **`zoom(pat, 0, 0)` (degenerate range):** Error E13x: "zoom range must have positive width."
- **`zoom(pat, 0.5, 0.25)` (reversed range):** Error: "zoom end must exceed start."
- **`ply(pat, 0)` or `ply(pat, n)` with `n < 1`:** Error: "ply count must be ≥ 1."
- **`linger(pat, 0)` or `linger(pat, n)` with `n <= 0`:** Error.
- **`linger(pat, 1.0)`:** No-op (keep entire pattern).
- **`segment(pat, 1)`:** Single event spanning the cycle, valued at the event active at time 0.
- **`compress(pat, 0, 1)`:** No-op.
- **`compress(pat, 0, 0)`:** Empty pattern (zero duration).

### 9.2 Generators

- **`run(0)`:** Empty pattern.
- **`run(1)`:** Single event at time 0, value 0, duration 1.
- **`binary(0)`:** Single rest event (no bits set).
- **`binaryN(0, 0)`:** Empty pattern.
- **`binaryN(5, 2)`:** `n` exceeds `2^bits` — error or truncate to lower `bits` bits? **Decision: truncate to lower `bits` bits** (matches Strudel; defensible as bitmask semantics).
- **`binary(n)` with `n < 0`:** Error: "binary requires non-negative integer."

### 9.3 Voicing

- **`anchor()` without `mode()`:** Use a default mode of `"below"`.
- **`mode()` without `anchor()`:** Default anchor to `c4` (MIDI 60).
- **Empty chord progression:** Pass through unchanged.
- **Single chord:** No voice-leading happens; just apply mode/anchor.
- **Unknown voicing name:** Error E14x: "voicing dictionary 'X' not registered."
- **`addVoicings` called twice with same name:** Last write wins; no error (consistent with hot-reload workflow).

### 9.4 Extended Note Properties

- **`.bend(pat, val)` with `|val| > 1`:** Clamp to [-1, 1].
- **`.aftertouch(pat, val)` outside [0, 1]:** Clamp.
- **`.dur(pat, val)` with `val <= 0`:** Error.
- **Record suffix conflict with positional:** `c4:0.8{bend:0.2}` — both forms — combine: `:0.8` sets velocity, `{bend:0.2}` sets bend. No error.
- **Duplicate keys in record suffix:** `c4{vel:0.5, vel:0.7}` — last wins. No error (matches JS object semantics).
- **Long-form keys go to properties bag, not fixed fields.** `c4{velocity:0.5}` populates `event.properties` with `("velocity", 0.5)` and leaves `event.velocity` at its default `1.0`. Only the short forms in the §5.5 alias table (`vel`, `bend`, `aftertouch`, `dur`) populate fixed fields. This is intentional: the short forms are the canonical surface; long forms behave like any user-defined key. Surface a lint warning E15y when a long form (`velocity`, `duration`, etc.) is used in record-suffix to nudge users toward the short form.

### 9.5 Compatibility

- **`compress(signal, ratio, ...)`:** Will fail to type-check after the alias removal. Error message must point users to `comp(...)` / `compressor(...)`. Test that this error is emitted with a helpful message.
- **Existing tests using `compress` alias:** Must be updated to use `comp` or `compressor`. Audit `akkado/tests/` and the experiments directory.

---

## 10. Testing & Verification

### 10.1 Unit Tests

- **Per transform** (`akkado/tests/test_pattern_transforms.cpp`): build a pattern, apply transform, assert resulting `cedar::Event` list matches expected.
- **Per generator** (`akkado/tests/test_pattern_generators.cpp`): assert event times, values, durations.
- **Voicing** (`akkado/tests/test_voicing.cpp`): golden assertions per (chord, anchor, mode) tuple. At minimum, embed these worked examples to pin behavior:

  | Chord | Anchor | Mode | Expected MIDI notes |
  |-------|--------|------|---------------------|
  | `Am` | `c4` (60) | `below` | `[57, 60, 64]` (A3, C4, E4 — top note ≤ anchor; root in close position) |
  | `C` | `c5` (72) | `above` | `[60, 64, 67]` shifted up so all notes ≥ 72 → `[72, 76, 79]` (C5, E5, G5) |
  | `Am` | `c4` (60) | `duck` | `[57, 64, 69]` (A3, E4, A4 — closest to anchor avoiding 60 itself) |
  | `Am C G F` | `c4` | `below` | progression notes voice-led: each chord's voicing minimizes total semitone movement from previous (specific values per implementation, but total movement across the four chords ≤ 6 semitones) |
- **PatternEvent extensions**: assert `bend`/`aftertouch`/`dur_override` set correctly.
- **Mini-notation record suffix**: parse → assert AST contains expected `properties` vector.

### 10.2 Integration Tests

- `n"c4 e4 g4 b4".early(0.25).palindrome()` — chained transforms produce expected event list.
- `run(8).fast(2)` — generator + transform composition.
- `chord("Am C G F").anchor("c4").mode("below")` — voicing produces expected MIDI notes.
- `n"c4{vel:0.8,bend:0.2}" as e |> osc("sin", e.freq + e.bend * 100)` — full pipeline compiles and runs.

### 10.3 Experiments (Cedar runtime)

Per `docs/dsp-experiment-methodology.md` and the long-window guidance in `CLAUDE.md`:

- `experiments/test_op_seqpat_step.py` — extend with `iter` rotation tests across **at least 300 seconds** of simulated audio. Verify the pattern rotates correctly cycle-after-cycle, no voice drops, no timing drift.
- `experiments/test_op_palindrome.py` (new) — verify forward/backward alternation over 300+ s.

### 10.4 Manual Audition

- Chord progression with voicing leading: listen for smooth transitions, not octave jumps.
- Swing patterns at various amounts: confirm groove changes audibly.
- `bend` on a sustained note: confirm pitch bends.

### 10.5 Build & Lint

- `cmake --build build` clean.
- `bun run check` in `web/` clean.
- `bun run build:docs` regenerates the F1 lookup index for the new transforms.

---

## 11. Follow-up Work (deferred from Phase 2 ship)

Two pieces from the original Phase 2 scope landed only partially because they
share a common blocker — they need new cedar runtime buffer plumbing (either a
new opcode or new `cedar::Event` fields and corresponding SEQPAT_STEP work) to
surface per-event property values to running synth code. Both should be picked
up as a Phase 2.1 follow-up.

### 11.1 §5.5a custom-property pipe-binding accessor

**Status:** AST + lexer + parser shipped; runtime exposure deferred.

**What works today:**
- `n"c4{cutoff:0.3} e4{cutoff:0.7}"` parses cleanly. The `cutoff` key is
  preserved on `MiniAtomData.properties`. Recognized short-form keys (`vel`,
  `dur`) already populate `cedar::Event` fields and reach runtime.
- `as e |> ... e.freq` (existing fixed-field accessor) still works.

**What's missing:**
- `n"c4{cutoff:0.3}" as e |> lp(@, 200 + e.cutoff * 4000)` — the `e.cutoff`
  member-access path. `pattern_field_index()` returns -1 for unrecognized
  keys; per the PRD §5.5a design, the resolver needs to fall through to
  `tv.pattern->custom_fields` (a `std::unordered_map<std::string, std::uint16_t>`
  added to `PatternPayload`) populated during pattern compile.
- Per-key buffer allocation in `SequenceCompiler` (one buffer per distinct
  non-aliased key collected from `MiniAtomData.properties` across the bound
  pattern's atoms). SEQPAT_STEP needs to write each event's per-key value to
  the corresponding buffer slot — likely via a new opcode or by extending
  `cedar::Event` with a small inline property table (max 4 keys, matching
  `MAX_VALUES_PER_EVENT`).
- Error path E15x: `"unknown property '<key>' on pattern event 'e'"` listing
  defined keys.

**Acceptance test (unblocked by this work):**
```akkado
n"c4{vel:0.8, cutoff:0.3} e4{vel:1.0, cutoff:0.7}" as e
  |> osc("saw", e.freq)
  |> lp(@, 200 + e.cutoff * 4000)
  |> out(@, @)
```

### 11.2 Standalone `bend()` / `aftertouch()` transforms

**Status:** Reserved as builtins but not registered as dispatch handlers; cedar
runtime fields not added.

**What works today:**
- Record-suffix `c4{bend:0.5, aftertouch:0.7}` parses; values are kept on
  `MiniAtomData.properties`. `dur` propagates to runtime via existing
  `cedar::Event.duration`.

**What's missing:**
- `cedar::Event` needs `float bend` and `float aftertouch` fields, OR (cleaner)
  a generic property bag on the event (see 11.1 — same plumbing).
- Per-key buffer slots in `PatternPayload` (e.g., `BEND` and `AFTERTOUCH`
  beyond the existing `FREQ`/`VEL`/`TRIG`/`GATE`/`TYPE`).
- `pattern_field_index()` recognizes `"bend"` and `"aftertouch"` as fixed
  fields once buffers exist.
- Top-level handlers `handle_bend_call` / `handle_aftertouch_call` /
  `handle_dur_call` that mirror `handle_velocity_call`'s shape (compile inner
  pattern → emit runtime MUL/SET on the new buffer). The `dur` transform can
  ship before the others since `cedar::Event.duration` already exists.

**Acceptance test (unblocked by this work):**
```akkado
n"c4 e4 g4".bend("<0 0.5 -0.5>") |> mtof(@ + bend(@) * 12) |> osc("sin", @) |> out(@, @)
```

### 11.3 Implementation sequencing

The two follow-ups overlap at the runtime side: one shared design decision
(generic `cedar::Event` property table vs. fixed `bend`/`aftertouch` fields)
unblocks both. Recommended order:

1. Pick the runtime model (probably: extend `cedar::Event` with a small
   `prop_keys[N] / prop_vals[N]` array and a key→buffer mapping carried on
   `SequenceState` for SEQPAT_STEP to dispatch).
2. Wire §5.5a (custom-key accessor): more general, demonstrates the new
   plumbing under user-provided keys.
3. Build standalone `bend()` / `aftertouch()` / `dur()` transforms on top:
   they're a special case of §5.5a where the key is one of a few known
   short-form names, with optional MIDI-out semantics layered on later.

---

## 12. Open Questions

- **Should `bend` be a per-event step value, or a continuous time-varying signal?** Phase 2 ships per-event (matches `velocity`). Continuous bend is a richer model that may want a separate signal-rate path; deferred.
- **Voicing dictionary sharing across compile units:** `addVoicings()` registers globally; should hot-reload preserve registrations? **Tentative:** yes — registry survives across recompiles, like `param()` values. Open until implementation.
- **`segment(pat, n)` on continuous (signal-rate) patterns:** the existing pattern eval is event-based; what does sampling a continuous source mean? **Tentative:** error for non-discrete patterns; revisit if/when continuous-pattern types land.
