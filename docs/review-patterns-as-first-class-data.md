# Review: Patterns as First-Class Data in Akkado

> A comprehensive analysis of the gap between Akkado's pattern system and its typed data primitives, with approaches to bridge them — focusing on runtime manipulation within Cedar's zero-allocation constraints.

## Context

Akkado has a powerful mini-notation system for defining musical patterns (`n"c4 e4 g4"`), and a rich set of typed primitives (records, arrays, numbers, signals). However, these two worlds are largely disconnected. Patterns are opaque compile-time artifacts — you can create them from strings, access their output fields, and apply a fixed set of transforms, but you cannot construct, decompose, inspect, or manipulate them using the same record/array/number primitives that the rest of the language provides.

More critically, pattern *content* is entirely static at runtime. Once compiled, the events in a pattern cannot be changed by signals, parameters, or external input. This rules out dynamic arpeggiators, evolving melodies, reactive compositions, and any use case where pattern data needs to respond to the live state of the system.

This document catalogs what's already possible, identifies the gaps, and proposes approaches that leverage Cedar's existing runtime infrastructure to close them — without violating the zero-allocation audio thread constraint.

---

## 1. What Already Works

### Pattern Creation
- **Mini-notation strings**: `n"c4 e4 g4"`, `s"bd sd [hh hh] cp"`
- **Chord patterns**: `chord("Am C7 F G")` with Strudel-compatible symbol parsing
- **Algorithmic**: `euclid(k, n, rot)` as a Cedar opcode (runtime), Euclidean syntax `bd(3,8)` in mini-notation
- **Timeline curves**: `timeline("0 0.5 1 0.5")` for automation breakpoints

### Pattern Field Access (5 fixed fields)
- `%.freq` / `%.pitch` / `%.f` — frequency (Hz)
- `%.vel` / `%.velocity` / `%.v` — velocity (0–1)
- `%.trig` / `%.trigger` / `%.t` — trigger pulse
- `%.gate` / `%.g` — gate signal
- `%.type` — event type ID (for sample routing)

These are extracted into audio-rate buffers by `SEQPAT_STEP`/`SEQPAT_GATE`/`SEQPAT_TYPE` opcodes at block boundaries.

### Named Bindings and Destructuring
```akkado
n"c4 e4 g4" as e |> osc("sin", e.freq) |> % * e.vel
n"c4 e4 g4" as {freq, vel} |> osc("sin", freq) |> % * vel
```

### Compile-Time Pattern Transforms
`slow`, `fast`, `rev`, `transpose`, `velocity`, `bank`, `variant` — all modify the compiled `Sequence` event list before bytecode emission. Callable via both functional and dot-call syntax:
```akkado
n"c4 e4 g4".slow(2).transpose(12).velocity(0.8)
```

### Type System Foundation
`TypedValue` tracks 8 types: Signal, Number, Pattern, Record, Array, String, Function, Void. The compiler knows when something is a Pattern and can type-check field access, transforms, and polyphonic dispatch.

### Records and Arrays (the "other world")
```akkado
// Records — fully typed, spread, field access
rec = {freq: 440, vel: 0.8}
modified = {..rec, freq: 880}

// Arrays — compile-time unrolled, functional ops
[1, 2, 3] |> map(%, x -> x * 2)
sum([1, 2, 3])
zip(freqs, vels)
```

### Existing Runtime Infrastructure (relevant to this review)
- **Arena allocator**: 32MB bump allocator. Sequence events already live here as mutable structs (`Event*` pointers into arena memory). No heap allocation needed for in-place mutation.
- **`query_pattern()`**: Re-expands source `Sequence` events into `OutputEvents` every cycle. Mutations to source events propagate automatically on the next cycle boundary.
- **EnvMap**: 256-slot lock-free parameter system with per-sample interpolation. Already bridges host thread → audio thread for `param()`, `toggle()`, `button()`.
- **Sample-and-hold**: `SAHState` captures a signal value on trigger rising edge — an established "sample at event time" pattern.
- **Ring buffer writes**: Delay lines write into pre-allocated circular buffers via `write_pos` advancement. Same pattern could apply to event data.
- **Array opcodes**: `ARRAY_PACK`, `ARRAY_INDEX`, `ARRAY_FILL` etc. operate on 128-element buffers. Usable as lookup tables.

---

## 2. The Gaps

### Gap 1: No programmatic pattern construction

You cannot build a pattern from arrays of records:
```akkado
// IMPOSSIBLE today:
events = [{freq: 440, time: 0.0, dur: 0.5}, {freq: 880, time: 0.5, dur: 0.5}]
my_pat = pattern(events)  // does not exist
```

Patterns can *only* originate from string literals parsed by the mini-notation parser, or from the `euclid` opcode.

### Gap 2: No decomposition of patterns into language values

You cannot get a pattern's events out as an array:
```akkado
// IMPOSSIBLE today:
p = n"c4 e4 g4"
events = p.events      // does not exist
first = events[0]
first.freq
len(events)
```

The only way to "read" a pattern is through its 5 audio-rate field buffers.

### Gap 3: No event-level manipulation

You cannot map, filter, or transform individual events using the language's functional tools:
```akkado
// IMPOSSIBLE today:
n"c4 e4 g4 b4"
  |> filter_events(%, e -> e.freq > 400)
  |> map_events(%, e -> {..e, vel: e.vel * 0.5})
```

The existing transforms (`slow`, `transpose`, etc.) are a closed set. Users cannot define their own.

### Gap 4: No pattern composition from parts

You cannot merge, concatenate, or interleave patterns:
```akkado
// IMPOSSIBLE today:
kicks = s"bd ~ bd ~"
snares = s"~ sd ~ sd"
combined = stack(kicks, snares)
appended = cat(kicks, snares)
```

### Gap 5: Limited event fields

`PatternPayload` exposes exactly 5 fields. The underlying `Event` struct also has `time`, `duration`, `chance`, `source_offset` — but these are not accessible from the language. User-defined fields (e.g., `pan`, `cutoff`, `detune`) are not possible.

### Gap 6: No runtime pattern modification (the critical gap)

All pattern data is baked into `StateInitData` at compile time. There is no mechanism to:
- Change which notes an arpeggiator plays based on a live chord input
- Evolve a melody over time using randomness or algorithmic mutation
- React to streaming event data (MIDI, OSC, sensor input)
- Build dynamic compositions where pattern content responds to the system's state

This is the gap that most limits Akkado's expressiveness for live performance and generative music. The compile-time approach handles static patterns well, but music is fundamentally dynamic.

**The key insight**: Cedar's zero-allocation constraint means "no malloc on the audio thread," not "immutable data." Events are arena-allocated structs with stable pointers. They can be mutated in-place. `query_pattern()` re-reads them every cycle. The infrastructure for runtime mutation already exists — it just isn't exposed to the language.

---

## 3. What Existing Work Covers (and Doesn't)

### prd-pattern-array-note-extensions
- **Covers**: Array type, map(), chord system, dot-call, polymeter, time/structure modifiers
- **Doesn't cover**: Runtime event mutation, programmatic event construction, event-level map/filter, user-defined event fields

### Vision-Language-Evolution
- **Covers**: `state` keyword, delay lines, module system, type system, const fn
- **Doesn't cover**: Pattern ↔ record/array bridging. The vision document classifies pattern transforms as "needs Pattern as first-class type" but defines "first-class" as "method chaining + type checking", not "constructible/decomposable/mutable"

### prd-compiler-type-system
- **Covers**: TypedValue with Pattern variant, field access, type checking
- **Doesn't cover**: Pattern internals as structured data accessible to the language

### prd-records-and-field-access
- **Covers**: Record literals, field access, pipe binding, pattern event fields via `%`
- **Doesn't cover**: Events-as-records, constructing patterns from records, mutating event data at runtime

---

## 4. Approaches to Close the Gaps

### Approach A: Mutable Event Slots (Recommended Starting Point)

**Core idea**: Events in `SequenceState.sequences[].events[]` are arena-allocated, mutable structs. New opcodes write into event fields in-place. `query_pattern()` re-reads source events every cycle, so mutations propagate automatically.

```akkado
// Arpeggiator: chord input writes into a pattern's event slots
chord_notes = chord_detect(midi_in)  // produces array of frequencies
p = n"x x x x"  // 4-slot rhythmic scaffold

// Each event's pitch comes from the live chord
p.set_freq(0, chord_notes[0])
p.set_freq(1, chord_notes[1])
p.set_freq(2, chord_notes[2])
p.set_freq(3, chord_notes[3])

p as e |> osc("sin", e.freq) |> % * e.vel |> out(%, %)
```

**How it works at the VM level**:
1. Compiler emits pattern as usual — arena-allocated `Event` structs with placeholder values
2. New opcode `SEQ_EVENT_SET` takes: `state_id` (which pattern), `event_index`, `field_id` (freq/vel/chance/duration), `value` (from input buffer)
3. Opcode writes directly into `state.sequences[0].events[idx].values[0] = value` (or `.velocity`, `.chance`, etc.)
4. Next `SEQPAT_QUERY` call runs `query_pattern()` which re-expands from source events — mutations are picked up
5. No allocation. Just a store to an existing memory location.

**What this unlocks**:
- **Dynamic arpeggiators**: Write chord tones into event slots, pattern plays them rhythmically
- **Evolving melodies**: Noise/LFO → `SEQ_EVENT_SET` → pitch drift over time
- **Reactive patterns**: EnvMap params or MIDI → event field mutation
- **Probabilistic composition**: Modulate `event.chance` to fade events in/out

**Implementation cost**: One new opcode. No new state types. No arena changes. Compiler needs to track event count per pattern for bounds checking.

**Limitations**: Can only mutate events that exist at compile time. Cannot add/remove events or change timing structure. This is a feature, not a bug — it keeps the rhythmic skeleton stable while content evolves.

### Approach B: Indirection Tables

**Core idea**: Separate *what* plays from *when* it plays. Events store indices into a lookup table. The lookup table is a signal-rate buffer that can be written from anywhere.

```akkado
// Pre-allocate a 8-slot pitch table
pitch_table = table(8)

// Fill it with a scale
pitch_table.set(0, 261.6)  // C4
pitch_table.set(1, 293.7)  // D4
pitch_table.set(2, 329.6)  // E4
// ... etc

// Pattern references table indices, not literal pitches
n"0 2 4 7".lookup(pitch_table) as e
  |> osc("sin", e.freq)
  |> out(%, %)

// Later: mutate the table to change what the pattern plays
// (e.g., modulate by LFO, switch scales, react to input)
pitch_table.set(0, 440.0)  // now slot 0 plays A4 instead of C4
```

**How it works at the VM level**:
1. A "table" is a pre-allocated buffer in the arena (like delay line memory) — fixed size, stable pointer
2. New opcode `TABLE_SET(state_id, index, value)` writes to the table at a given index
3. New opcode `TABLE_GET(state_id, index)` reads from the table — usable as a `SEQPAT_STEP` post-process
4. Or: modify `SEQPAT_STEP` to treat `evt.values[]` as indices and dereference through a table buffer

**What this unlocks**:
- **Scale switching**: Swap all 7 table entries = instant key change
- **Chord-aware arpeggiation**: External process fills table with current chord tones
- **Stochastic melodies**: Random process mutates table slots independently
- **Decoupled rhythm and melody**: One pattern defines when, the table defines what

**Implementation cost**: Two new opcodes + a simple `TableState` (pointer + size, arena-allocated). The `SEQPAT_STEP` lookup variant is a small modification to existing code.

**Relationship to Approach A**: Complementary. Approach A mutates events directly (good for per-event control). Approach B provides shared lookup (good when multiple patterns should reference the same pitch/velocity vocabulary).

### Approach C: Signal-Sampled Events

**Core idea**: Instead of hardcoded values in events, an event "samples" a live signal at its trigger time. The event defines *when* to sample; the signal defines *what* to capture.

```akkado
// Pattern provides triggers only (rhythm)
p = n"x x x [x x]"

// Signal provides values (melody)
melody = lfo("sah", 1) * 500 + 200  // random S&H melody

// At each trigger, SEQPAT_STEP captures the current value of melody
p.sample_freq(melody) as e
  |> osc("sin", e.freq) |> % * e.vel |> out(%, %)
```

**How it works at the VM level**:
1. `SEQPAT_STEP` gains an optional input buffer: `inputs[4]` = "value override signal"
2. When `inputs[4] != BUFFER_UNUSED` and a trigger fires (crossing an event time), the opcode reads `override_signal[i]` instead of `evt.values[voice_index]`
3. The captured value is held until the next trigger (sample-and-hold behavior, reusing the established pattern from `SAHState`)
4. No new state types needed — just a `float held_value` field added to `SequenceState`

**What this unlocks**:
- **Signal-driven melodies**: Any audio-rate signal becomes a pitch source, quantized to the pattern's rhythm
- **Modular patching idiom**: "Trigger is the clock, signal is the data" — familiar to modular synth users
- **Layered randomness**: Different signals for freq, vel, type → each dimension evolves independently

**Implementation cost**: Minimal — one extra input check in `SEQPAT_STEP`, one extra float in `SequenceState`. The hardest part is the Akkado syntax design, not the VM work.

**Limitation**: All events in the pattern sample the *same* signal. For per-event differentiation, combine with Approach A or B.

### Approach D: Compile-Time Event Arrays (Complementary)

**Core idea**: Let patterns be constructed from and decomposed into arrays of event records at compile time. This bridges the record/array world with the pattern world, enabling programmatic pattern generation.

```akkado
// Construction: array of event records -> pattern
my_pat = pattern([
  {note: 60, time: 0.0, dur: 0.5, vel: 1.0},
  {note: 64, time: 0.5, dur: 0.5, vel: 0.8},
  {note: 67, time: 1.0, dur: 0.5, vel: 0.6}
])

// Decomposition: pattern -> array of event records
events = n"c4 e4 g4".events

// Manipulation: use existing array tools
loud = events |> map(%, e -> {..e, vel: 1.0})
high_only = events |> filter(%, e -> e.note > 62)

// Recompose
pattern(loud) |> osc("sin", %.freq) |> out(%, %)
```

**Why this is complementary, not primary**: All operations are compile-time — no runtime dynamism. But it's the foundation for programmatic pattern *generation*. You'd use this to algorithmically construct the initial pattern structure, then use Approaches A/B/C to make it dynamic at runtime.

**Implementation**: Define a canonical event record shape. Add `pattern(events_array)` that feeds into `SequenceCompiler`. Add `.events` accessor that returns compiled events as an Array of Records. Round-trip guarantee: `p.events |> pattern(%)` ≡ `n`.

### Approach E: Pattern Combinators

**Core idea**: Composition operators that work on Pattern-typed values.

```akkado
stack(s"bd ~ bd ~", s"~ sd ~ sd")  // layer (simultaneous)
cat(n"c4 e4", n"g4 b4")            // sequence (consecutive)
interleave(n"c4 e4 g4", n"d4 f4 a4")  // alternate events
```

**If patterns are decomposable** (Approach D), these are array operations: `stack` = concat events + time-scale, `cat` = concat with time offset, etc. Dedicated combinators would be more ergonomic and could produce optimized compiled output.

**Implementation cost**: Low-Medium if built on Approach D. Each combinator is a compile-time function that manipulates event arrays before feeding them into `SequenceCompiler`.

---

## 5. Recommended Phasing

### Phase 1: Signal-Sampled Events (Approach C) — Quick Win

**Why first**: Minimal implementation effort (one input check in `SEQPAT_STEP`), massive expressiveness gain. Immediately enables signal-driven melodies, modular-style patching, and layered randomness. No new opcodes, no new state types. The VM change is ~20 lines.

**Delivers**: Dynamic melodies, signal-to-rhythm quantization, LFO/noise-driven pitch.

### Phase 2: Mutable Event Slots (Approach A) — Core Runtime Capability

**Why second**: One new opcode unlocks per-event mutation. This is the foundation for arpeggiators, evolving sequences, and reactive patterns. Combined with Phase 1, covers the vast majority of "dynamic pattern" use cases.

**Delivers**: Arpeggiators, per-event pitch/velocity/chance mutation, MIDI-driven pattern content.

### Phase 3: Indirection Tables (Approach B) — Shared Vocabulary

**Why third**: Builds on the mutation concept but adds a shared lookup layer. Enables scale switching, chord-aware arpeggiation, and shared pitch vocabularies across multiple patterns. Useful but less urgent than direct mutation.

**Delivers**: Scale/key switching, shared pitch tables, chord-to-arpeggio workflows.

### Phase 4: Compile-Time Event Arrays + Combinators (Approaches D & E)

**Why fourth**: These are about pattern *construction* and *composition*, not runtime dynamism. Important for algorithmic composition and ergonomics, but the runtime approaches (Phases 1-3) deliver the most impactful new capabilities.

**Delivers**: Programmatic pattern generation, pattern decomposition, stack/cat/interleave combinators.

---

## 6. Key Design Questions

### Runtime Mutation

1. **Granularity of mutation**: Should `SEQ_EVENT_SET` operate per-event-per-field (fine-grained but verbose) or accept a "field mask + values array" (batch update but more complex opcode)? Per-event-per-field is simpler to implement and compose.

2. **Timing of mutation propagation**: Mutations take effect on the next `query_pattern()` call (next cycle boundary). Is per-cycle granularity fast enough? At 120 BPM with 4-beat cycles, that's 2 seconds. For sub-cycle mutation, we could force a re-query — but that adds complexity. Alternatively, mutations could target `OutputEvents` directly for instant effect within the current cycle.

3. **Bounds safety**: `SEQ_EVENT_SET(state_id, event_index, ...)` needs to bounds-check `event_index < num_events`. Out-of-bounds should be a silent no-op (not a crash). The compiler should expose event count so Akkado code can iterate safely.

4. **Multiple voices in events**: Events hold up to 4 values (for chords). Should `set_freq` target a specific voice index, or replace all voices? Probably need `set_freq(event_idx, voice_idx, value)` for chord-aware mutation.

### Signal Sampling

5. **Hold behavior**: When `SEQPAT_STEP` samples a signal at trigger time, what value does it output between triggers? Last sampled value (sample-and-hold) is the natural choice, matching existing `SAHState` semantics. But should there be a "glide" option that interpolates toward the next sample?

6. **Per-field override**: Should signal sampling be available for velocity and type_id too, not just frequency? Likely yes — `p.sample_vel(signal)`, `p.sample_type(signal)`. Each adds one input buffer to `SEQPAT_STEP` (or a separate opcode).

### Indirection Tables

7. **Table size**: Fixed at compile time (like delay line buffer sizes)? Or resizable within a pre-allocated maximum? Fixed is simpler and fits the arena model.

8. **Table ↔ event binding**: How does an event "know" to look up its value in a table? Options: (a) store index in `evt.values[]` and mark the pattern as "uses indirection" via a flag; (b) a post-processing opcode that replaces `SEQPAT_STEP` output with a table lookup; (c) a dedicated `SEQPAT_STEP_TABLE` opcode variant. Option (b) is most composable — it's just `table_get(pitch_table, floor(%.freq))`.

### Compile-Time Event Arrays

9. **Event record shape**: What fields are canonical? Minimal (`{note, time, dur, vel}`) vs. maximal (include `chance`, `type_id`, etc.)? Unknown fields should error at compile time.

10. **Filter semantics**: If you filter events out of a pattern, what happens to timing? Absolute times (leaving gaps) vs. redistribute (closing gaps)? Both are useful — probably need both modes.

11. **Round-trip fidelity**: Should `n"c4 e4".events` return the *same* record shape as what `pattern()` accepts? Perfect round-tripping is valuable but means exposing internal details like `type_id` and `source_offset`.

### Syntax

12. **Naming**: `p.set_freq(idx, val)` vs `set_event(p, idx, "freq", val)` vs `p[idx].freq = val`? The last is most ergonomic but requires assignment semantics the language doesn't have. Method syntax (`.set_freq()`) is consistent with existing pattern transforms.

13. **Table syntax**: `table(8)` vs `[0; 8]` (Rust-style fill) vs something else? Tables are a new concept — the name should make clear they're mutable and fixed-size, distinct from arrays (which are compile-time and immutable).
