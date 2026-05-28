# Akkado Language Evolution Vision

> A strategic analysis of what language constructs would maximize the amount of DSP expressible in userspace, while respecting Cedar's zero-allocation real-time constraints.

## Context

Akkado currently has **~120 unique builtins** mapping to **~95 Cedar VM opcodes**, plus **~40 aliases** for convenience naming. Many builtins are simple 1:1 opcode wrappers that MUST remain as primitives — oscillators, filters, delays, anything needing per-sample state or arena memory. But a significant portion are convenience functions, compile-time array operations, or compound codegen patterns that could theoretically be expressed in userspace — if the language had the right constructs.

This document classifies every builtin, proposes the minimal set of language extensions to maximize userspace expressiveness, and prioritizes them for a live-coding audio DSL.

---

## 1. The Builtin Audit

### Tier 1 — Irreducible Primitives (~85 builtins)

These map directly to VM opcodes with per-sample state, arena-allocated memory, or fundamental arithmetic. They cannot be moved to userspace without new VM capabilities.

#### Oscillators (13 builtins)
`sine`, `tri`, `saw`, `sqr`, `ramp`, `phasor`, `sqr_minblep`, `sqr_pwm`, `saw_pwm`, `sqr_pwm_minblep`, `sqr_pwm_4x`, `saw_pwm_4x`, `noise`

Why primitive: Phase accumulators, band-limiting (PolyBLEP/MinBLEP), oversampling — all require per-sample state (`OscState`, `MinBLEPOscState`, `OscState4x`, `NoiseState`).

#### Filters (6 builtins)
`lp`, `hp`, `bp`, `moog`, `diode`, `formant`, `sallenkey`

Why primitive: Filter state variables (`SVFState`, `MoogState`, `DiodeState`, `FormantState`, `SallenkeyState`) with coefficient caching and ZDF topology. Cannot be decomposed without per-sample feedback.

#### Envelopes (3 builtins)
`adsr`, `ar`, `env_follower`

Why primitive: Multi-stage state machines (`EnvState`) with gate edge detection and exponential coefficient caching.

#### Delays & Reverbs (9 builtins)
`delay`, `delay_ms`, `delay_smp`, `freeverb`, `dattorro`, `fdn`, `comb`, `pingpong`, `tap_delay` (+ms/smp variants)

Why primitive: Arena-allocated ring buffers (`DelayState`, `FreeverbState`, `DattorroState`, `FDNState`, `CombFilterState`, `PingPongDelayState`). These need direct access to `AudioArena` for zero-allocation buffer management.

#### Modulation Effects (3 builtins)
`chorus`, `flanger`, `phaser`

Why primitive: Internally use arena-allocated delay lines with LFO modulation (`ChorusState`, `FlangerState`, `PhaserState`). Could *theoretically* be userspace with `state` + delay primitives — see Tier 3.

#### Distortion (8 builtins)
`saturate`, `softclip`, `bitcrush`, `fold`, `tube`, `smooth`, `tape`, `xfmr`, `excite`

Why primitive (most): `tube`, `smooth`, `tape`, `xfmr`, `excite` use 2x oversampling with internal delay lines (`TubeState`, `SmoothSatState`, `TapeState`, `XfmrState`, `ExciterState`). `bitcrush` uses sample-and-hold state (`BitcrushState`).

Exception: `saturate` and `softclip` are stateless — `tanh(x * drive)` and polynomial clipping. These could be userspace today (see Tier 2). `fold` is also stateless at its core, though the ADAA variant needs state.

#### Dynamics (3 builtins)
`comp`, `limiter`, `gate`

Why primitive: Envelope followers, lookahead buffers, hysteresis state machines (`CompressorState`, `LimiterState`, `GateState`).

#### Samplers (3 builtins)
`sample`, `sample_loop`, `soundfont`

Why primitive: Need access to `SampleBank` for audio data and voice management (`SamplerState`, `SoundFontVoiceState`).

#### Arithmetic & Math (25 builtins)
`add`, `sub`, `mul`, `div`, `pow`, `neg`, `abs`, `sqrt`, `log`, `exp`, `floor`, `ceil`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`, `min`, `max`, `clamp`, `wrap`

Why primitive: Single-opcode per-sample math. These are the atoms — you can't implement `sin(x)` without `MATH_SIN`.

#### Conditionals & Logic (9 builtins)
`select`, `gt`, `lt`, `gte`, `lte`, `eq`, `neq`, `band`, `bor`, `bnot`

Why primitive: Per-sample signal-rate comparison and selection. Fundamental to runtime branching.

#### Sequencing & Timing (5 builtins)
`clock`, `lfo`, `trigger`, `euclid`, `timeline`

Why primitive: Beat-synced phase accumulators, euclidean rhythm generators, breakpoint automation (`LFOState`, `EuclidState`, `TriggerState`, `TimelineState`).

#### Infrastructure (5 builtins)
`out`, `dc`, `slew`, `sah`, `mtof`

Why primitive: `out` maps to `OUTPUT` (audio system boundary). `slew` and `sah` need per-sample state (`SlewState`, `SAHState`). `dc` and `mtof` are trivially stateless but so fundamental they belong as opcodes.

#### Stereo Operations (6 builtins)
`pan`, `width`, `ms_encode`, `ms_decode`, `stereo`, `left`, `right`

Why primitive: `pan`, `width`, `ms_encode`, `ms_decode` are true stereo opcodes producing dual-buffer output. `stereo`, `left`, `right` are compile-time channel routing handled by codegen — they don't emit opcodes but manipulate the stereo buffer tracking system.

#### Polyphony (4 builtins)
`poly`, `mono`, `legato`, `spread`

Why primitive: `POLY_BEGIN`/`POLY_END` brackets with voice allocation state (`PolyAllocState`). The VM iterates voices and re-executes the body. This is a VM execution model feature, not a DSP function.

### Tier 2 — Userspace Today (~45 items)

These are expressible with the current `fn` system and compile-time evaluation.

#### Aliases (~40 entries)
The entire `BUILTIN_ALIASES` map — `lowpass`→`lp`, `reverb`→`freeverb`, `distort`→`saturate`, etc. These are already trivial to implement as userspace forwarding functions:

```akkado
fn lowpass(sig, cut, q = 0.707) -> lp(sig, cut, q)
fn reverb(sig, room = 0.5, damp = 0.5) -> freeverb(sig, room, damp)
```

The only reason they exist as compiler aliases today is for zero-overhead resolution. A stdlib `fn` definition has exactly the same overhead (full inlining).

#### Stateless Distortions (2-3 builtins)
`saturate` is `tanh(x * drive)`. `softclip` is a polynomial. These are expressible today:

```akkado
fn saturate(sig, drive = 2.0) -> tanh(sig * drive)
fn softclip(sig, thresh = 0.5) -> {
    t = abs(sig)
    match { t < thresh: sig, _: sig * thresh / t }
}
```

#### Pure Math Formulas (2 builtins)
`mtof` = `440 * 2^((note - 69) / 12)`. `dc` = identity function.

```akkado
fn mtof(note) -> 440.0 * pow(2.0, (note - 69.0) / 12.0)
fn dc(x) -> x
```

#### Array Reductions (1 builtin)

> **Status: Realized for `product`** — removed in favour of `reduce(arr, (a, b) -> a * b, 1)`.

`mean` is still a builtin but is decomposable via `sum`/`len`:

```akkado
fn mean(arr) -> sum(arr) / len(arr)
```

The higher-order reducer is `reduce(arr, fn, init)` — named `reduce` because `fold` is the wavefolding distortion builtin.

#### Compile-Time Array Generators (5 builtins)
`linspace`, `random`, `harmonics`, `normalize`, `scale` — these execute at compile time and produce constant arrays. They work today as codegen special-cases but could be stdlib functions if the language had `const fn` (see Phase 1).

#### The `osc()` Pattern
The stdlib already demonstrates the Tier 2 approach: `fn osc(type = "sin", freq = 440) -> match(type) { ... }` dispatches to primitive oscillators via compile-time match.

### Tier 3 — Userspace With New Constructs (~25 builtins)

These need language extensions beyond what `fn` + `match` provide.

#### Needs User-Defined State (~8 builtins)

With a `state` keyword enabling per-sample stateful computation, these could be userspace:

- **`bitcrush`** — sample-and-hold with phase accumulator (2 floats of state)
- **`slew`** — one-pole smoother (`current += (target - current) * rate`)
- **`sah`** — sample-and-hold with trigger edge detection
- **`saturate` (ADAA)** — `smooth` needs previous sample and antiderivative
- **`tube`** — asymmetric waveshaping with oversampling delay lines
- **`tape`** — saturation + high-shelf filter state
- **`xfmr`** — saturation + bass extraction integrator
- **`excite`** — high-pass filter + harmonic generation

Example of what `state` could look like:

```akkado
fn one_pole(sig, coeff) -> {
    state y = 0.0
    y = y + coeff * (sig - y)
    y
}

fn bitcrush(sig, bits = 8.0, rate = 0.5) -> {
    state held = 0.0
    state phase = 0.0
    phase = phase + rate
    match { phase >= 1.0: { phase = phase - 1.0; held = quantize(sig, bits) }, _: held }
}
```

This is THE single biggest language unlock. One-pole filters, DC blockers, running averages, zero-crossing detectors, Schmitt triggers — all become expressible.

#### Needs User State + Delay Primitives (~3 builtins)

With `state` + a raw delay-line primitive, the compound modulation effects become userspace:

- **`chorus`** — 3 delay taps modulated by phase-offset LFOs
- **`flanger`** — single modulated delay tap with feedback
- **`phaser`** — cascaded allpass filters (each is a one-pole with delay)

This would require either:
1. A `delay_read`/`delay_write` pair usable from userspace (lower-level than `tap_delay`), or
2. A `ring_buffer` type with `read(pos)` and `write(val)` operations.

#### Needs Type System + Generics (~8 builtins)

The array HOFs are currently codegen special-cases because they need to:
1. Know the array length at compile time
2. Unroll loops over multi-buffer arrays
3. Accept functions of varying arity

- **`map`**, **`reduce`**, **`sum`**, **`zipWith`**, **`zip`** — need typed array parameters
- **`take`**, **`drop`**, **`reverse`** — need compile-time length tracking
- **`rotate`**, **`shuffle`**, **`sort`** — need compile-time array manipulation

With the proposed type system (`TypedValue` returning `Array` types), these could be expressed as typed stdlib functions rather than codegen special-cases.

#### Needs Pattern as First-Class Type (~7 builtins)

Pattern transforms are compile-time AST manipulations:

- **`slow`**, **`fast`** — time-stretch pattern events
- **`rev`** — reverse event order
- **`transpose`** — shift pitches
- **`velocity`**, **`bank`**, **`n`** — modify event metadata
- **`tune`** — apply microtonal tuning context

These operate on the `SequenceCompiler` output, not on audio buffers. They need patterns to be a first-class type with known transform operations, which the type system PRD already proposes (`Pattern` type with field access).

#### Needs Macro System (~4 builtins)

- **`tap_delay`** — emits DELAY_TAP, compiles closure body, emits DELAY_WRITE (a multi-instruction codegen pattern)
- **`pianoroll`**, **`oscilloscope`**, **`waveform`**, **`spectrum`** — emit PROBE + register visualization metadata

These need the ability to emit multiple instructions around user code, which `fn` cannot express.

---

## 2. Proposed Language Constructs

### Phase 1 — High Impact, Compiler-Only (no VM changes)

#### 2.1 Module/Import System

**Motivation:** Enables stdlib-as-akkado, community sharing, and progressive builtin migration.

**Proposed syntax:**
```akkado
import "std/filters"           // imports all exports
import { lp, hp } from "std/filters"  // named imports
import "my-library" as lib     // namespace import: lib.reverb(...)
```

**Compiles to:** Nothing at runtime. The compiler resolves imports at parse time, merging symbol tables. All functions are still fully inlined.

**Unlocks:** All Tier 2 builtins can be moved to `stdlib/*.akkado` files. The compiler ships with a standard library directory.

**Complexity:** Medium. Needs file resolution, cycle detection, symbol namespacing. No VM changes.

#### 2.2 Dot-Call Syntax (Method-Style)

> **Status: Implemented** — `0dc05a3`

**Motivation:** `x.f(args)` → `f(x, args)` enables natural chaining without `|>` and `%`.

**Proposed syntax:**
```akkado
// These are equivalent:
saw(440).lp(800).delay(0.3, 0.5)
delay(lp(saw(440), 800), 0.3, 0.5)
saw(440) |> lp(%, 800) |> delay(%, 0.3, 0.5)
```

**Compiles to:** Desugars to regular function calls at parse time. The parser already has `NodeType::MethodCall` in the AST — it just needs to rewrite to `Call` nodes.

**Unlocks:** Ergonomic chaining for all functions, especially user-defined ones. Makes `|>` optional for simple chains. Enables pattern method syntax: `n"c4 e4".slow(2).transpose(12)`.

**Complexity:** Low. Parser rewrite only. The AST node type already exists.

#### 2.3 `const fn` — Compile-Time Pure Evaluation

> **Status: Implemented** — `bda74f9`

**Motivation:** Array generators (`linspace`, `harmonics`, `random`) and tuning tables need compile-time computation. Currently they're codegen special-cases that only work with literal arguments.

**Proposed syntax:**
```akkado
const fn linspace(start, end, n) -> {
    // Body runs at compile time, produces a constant array
    range(0, n) |> map(%, (i) -> start + (end - start) * i / (n - 1))
}

const fn edo_scale(divisions) -> {
    range(0, divisions) |> map(%, (i) -> pow(2, i / divisions))
}
```

**Compiles to:** The compiler evaluates the function body during compilation and emits the result as constants. No runtime code generated.

**Unlocks:** `linspace`, `harmonics`, `random`, `normalize`, `scale` move to stdlib. User-defined tuning tables, wavetable generators, coefficient precomputation.

**Complexity:** Medium. Needs a compile-time interpreter for a subset of the language (arithmetic, array operations, basic control flow). Does not need to handle stateful operations.

### Phase 2 — High Impact, Requires VM Work

#### 2.4 User-Defined State (`state` keyword)

**Motivation:** THE biggest unlock. Enables one-pole filters, S&H, bitcrushers, DC blockers, running averages, slew limiters, and opens the door to userspace effects.

**Proposed syntax:**
```akkado
fn one_pole(sig, coeff) -> {
    state y = 0.0                  // persistent across blocks
    y = y + coeff * (sig - y)      // per-sample update
    y                              // return current value
}

fn dc_block(sig, coeff = 0.995) -> {
    state x_prev = 0.0
    state y_prev = 0.0
    y = sig - x_prev + coeff * y_prev
    x_prev = sig
    y_prev = y
    y
}

fn schmitt(sig, lo = -0.5, hi = 0.5) -> {
    state out = 0.0
    out = match {
        sig > hi: 1.0
        sig < lo: 0.0
        _: out
    }
    out
}
```

**Compiles to:** Two approaches:

*Option A: STATE_READ/STATE_WRITE opcodes.* The compiler allocates state slots in the `StatePool` (already exists), emits `STATE_READ state_id, slot → buffer` at block start and `STATE_WRITE buffer → state_id, slot` at block end. State is preserved across blocks via the semantic ID system.

*Option B: Reuse ExtendedParams.* State variables map to `ExtendedParams<N>` slots in the `StatePool`, with initial values set via `StateInitData`. Per-sample updates compile to in-place buffer operations. This avoids new opcodes but limits state to float values.

**Unlocks:** `slew`, `sah`, `bitcrush`, `smooth` (ADAA), and all Tier 3 "needs state" builtins. Also enables:
- One-pole filters, DC blockers, running averages
- Edge detectors, zero-crossing counters
- Custom envelope generators
- Simple sequencers and sample-and-hold patterns

**Complexity:** High. Needs new opcodes or `ExtendedParams` reuse, semantic ID assignment for state persistence across hot-swaps, and careful interaction with the polyphony system (each voice needs independent state).

**Design considerations:**
- State must be per-voice in poly blocks (the `POLY_BEGIN`/`POLY_END` system already handles this for built-in stateful opcodes via `state_id` remapping)
- State initialization must participate in hot-swap matching — a `state y = 0.0` with the same semantic path should preserve its value when code is edited
- State updates must be per-sample, not per-block, to enable feedback within a single block
- The number of state variables per function should be bounded (8-16 floats) to keep the state pool manageable

#### 2.5 Raw Delay Line Primitive

**Motivation:** With `state` alone, you can't build chorus/flanger/phaser because they need delay lines (ring buffers with arbitrary read positions). A minimal delay-line primitive bridges this gap.

**Proposed syntax:**
```akkado
fn my_chorus(sig, rate = 0.5, depth = 0.5) -> {
    buf = delay_line(2400)  // 50ms ring buffer at 48kHz
    write(buf, sig)

    // 3-voice chorus with phase-offset LFOs
    d1 = read(buf, 20.0 + depth * 10.0 * sin(rate * 2 * pi * co))
    d2 = read(buf, 20.0 + depth * 10.0 * sin(rate * 2 * pi * co + 2.094))
    d3 = read(buf, 20.0 + depth * 10.0 * sin(rate * 2 * pi * co + 4.189))

    (d1 + d2 + d3) / 3.0
}
```

**Compiles to:** `delay_line(N)` allocates from `AudioArena` and returns a handle. `write(buf, val)` and `read(buf, pos)` are per-sample operations on the ring buffer.

**Unlocks:** `chorus`, `flanger`, `phaser` move to userspace. Also enables:
- Custom reverb topologies (Schroeder, FDN variants)
- Karplus-Strong synthesis
- Physical modeling (waveguide strings, tubes)
- Granular delay effects

**Complexity:** Medium-High. Needs 2-3 new opcodes (`DELAY_LINE_ALLOC`, `DELAY_LINE_WRITE`, `DELAY_LINE_READ`) and arena integration. The existing `DelayState` pattern provides a template.

**Deferred alternative:** Instead of a general ring buffer, expose the existing `tap_delay` as a lower-level API with raw read/write access. Less general but leverages existing infrastructure.

### Phase 3 — Medium Impact, Depends on Type System

#### 2.6 Type System

> **Status: Implemented** — Phase 3A (`c733f66`), Phases 3B+3C (`57bf870`)

**Status:** Already has a PRD (`docs/prd-compiler-type-system.md`). The core proposal: `visit()` returns `TypedValue` instead of `uint16_t`, with 8 built-in types (Signal, Number, Pattern, Record, Array, String, Function, Void).

**What it unlocks for language evolution:**
- **Typed builtin signatures** — catch type errors at compile time instead of silent garbage
- **Overloaded functions** — dispatch on argument type, not just string matching
- **Array HOFs as typed stdlib** — `map`, `reduce`, `sum` become regular typed functions instead of codegen special-cases
- **Pattern methods** — `.slow()`, `.transpose()` become typed method calls on Pattern values
- **Better error messages** — "expected Signal, got Pattern" instead of silent buffer misuse

**Complexity:** High. Touches every codegen path. The PRD estimates 2-3 weeks of focused work.

#### 2.7 Match Destructuring

> **Status: Implemented** — `cfbb699`

**Motivation:** Pattern events are records with fields. Destructuring makes event-driven code more ergonomic.

**Proposed syntax:**
```akkado
// Current
n"c4 e4 g4" as e |> sine(e.freq) |> % * e.vel

// With destructuring
n"c4 e4 g4" as {freq, vel} |> sine(freq) |> % * vel

// In match arms
fn process(event) -> match(event) {
    {freq, vel} if vel > 0.5: sine(freq) * vel
    {freq, vel}: tri(freq) * vel * 0.5
    _: 0.0
}
```

**Compiles to:** Desugars to field access at parse time. `{freq, vel}` in binding position expands to individual `e.freq`, `e.vel` references.

**Unlocks:** Cleaner pattern-driven synthesis code. Works with both records and pattern events.

**Complexity:** Low-Medium. Parser change + symbol table binding. No new opcodes.

#### 2.8 Pattern Methods and Transforms

> **Status: Implemented** — via dot-call + TypedValue type checking (`57bf870`)

**Motivation:** Pattern transforms (`slow`, `fast`, `rev`, `transpose`) are currently free functions with codegen special-cases. With the type system, they can be method-style calls that the compiler recognizes on Pattern values.

**Proposed syntax:**
```akkado
// Current
transpose(slow(n"c4 e4 g4", 2), 12)

// With methods
n"c4 e4 g4".slow(2).transpose(12)

// Chaining
n"c4 e4 g4"
    .slow(2)
    .transpose(12)
    .velocity(0.8)
    |> poly(4, (freq, gate, vel) -> ...)
```

**Compiles to:** With dot-call syntax (2.2), this is just syntactic sugar over the existing pattern transform codegen. The type system ensures these methods are only called on Pattern-typed values.

**Unlocks:** Natural chaining of pattern operations. Discoverable API via autocomplete on Pattern type.

**Complexity:** Low (if dot-call and type system are already implemented).

### Phase 4 — Lower Priority

#### 2.9 Macro System (Minimal)

**Motivation:** Some codegen patterns emit multiple instructions around user code — `tap_delay` (TAP + user closure + WRITE), visualization builtins (PROBE + metadata registration). These can't be expressed as regular functions.

**Proposed approach:** NOT a general macro system. Instead, a limited "template function" mechanism:

```akkado
// Hypothetical — NOT a concrete proposal
template fn feedback_delay(sig, time, fb, process) -> {
    @emit(DELAY_TAP, sig, time)  // compiler intrinsic
    processed = process(@tap_output)
    @emit(DELAY_WRITE, sig, processed, fb)
}
```

**Decision:** Defer. The current codegen special-case approach works and affects only ~4 builtins. A macro system is high complexity for low incremental value. If the pattern becomes more common (e.g., users want custom feedback topologies), revisit.

#### 2.10 Custom Operators

**Motivation:** DSP has a few domain-specific operations that are awkward as named functions.

**Potential operators:**
- `~>` for feedback routing (alternative to `tap_delay` closure)
- `>>` for function composition (alternative to `compose()`)

**Decision:** Defer. Pipe (`|>`) and dot-call cover 95% of chaining needs. Custom operators add parser complexity and reduce readability for newcomers.

### Explicitly Deferred

| Feature | Reason |
|---------|--------|
| Traits/Protocols | Over-engineering for a DSL. Akkado isn't Rust. |
| Iterator/Generator protocol | Compile-time arrays cover the use cases. Runtime iteration conflicts with block processing. |
| Extensible mini-notation | The mini-notation parser is complex enough. User extensions should be userspace functions, not syntax. |
| Mutable variables | Fundamentally conflicts with the hot-swap model. All state goes through `state` keyword with semantic ID tracking. |
| Recursive functions | The full-inlining model makes recursion impossible without a call stack. Array HOFs (`reduce`, `map`) cover recursive patterns. |
| Runtime polymorphism | Zero-allocation constraint. All dispatch is compile-time. |

---

## 3. Improvements to Existing Constructs

### 3.1 `fn` Defaults: Allow Expression Defaults

> **Status: Implemented** — `695269e`

Currently defaults must be literals: `fn f(x, cut = 1000)`. Allow constant expressions:

```akkado
fn f(x, cut = 440 * 2) -> lp(x, cut)           // arithmetic
fn f(x, cut = mtof(60)) -> lp(x, cut)           // pure function call
fn f(x, freqs = harmonics(440, 5)) -> sum(...)   // array generator
```

**Compiles to:** The compiler evaluates the default expression at compile time (must be a `const fn` or pure arithmetic).

### 3.2 `match`: Range Patterns

> **Status: Implemented** — `243d1e3`

```akkado
match(velocity) {
    0.0..0.3: "soft"
    0.3..0.7: "medium"
    0.7..1.0: "loud"
    _: "unknown"
}
```

**Compiles to:** `CMP_GTE`/`CMP_LT` + `LOGIC_AND` for range checks. Sugar over existing comparison opcodes.

### 3.3 Records: Computed Field Access and Spreading

> **Status: Implemented** (record spreading) — `695269e`

Allow constructing records with computed field values and spreading:

```akkado
base = {freq: 440, vel: 0.8}
modified = {..base, freq: 880}  // spread + override
```

**Compiles to:** Copy field buffer bindings from source record, override specified fields.

### 3.4 Arrays: Compile-Time Fusion

When array operations chain, fuse them at compile time to avoid intermediate arrays:

```akkado
// Currently: range(0,8) creates 8 buffers, map creates 8 more
range(0, 8) |> map(%, (i) -> sine(440 * (i + 1)))

// With fusion: directly emit 8 sine oscillators with computed frequencies
```

The compiler already does this implicitly (multi-buffer unrolling), but making it a documented optimization enables more aggressive constant folding.

---

## 4. Impact Summary

| Construct | Tier 2 → stdlib | Tier 3 → stdlib | New patterns enabled | VM changes |
|-----------|----------------|-----------------|---------------------|------------|
| **Module/import** | ~40 aliases | — | Community sharing, stdlib organization | None |
| **Dot-call syntax** | — | — | Ergonomic chaining | None |
| **`const fn`** | 5 array generators | — | Tuning tables, wavetable gen | None |
| **User-defined `state`** | — | ~8 distortions, slew, sah | One-pole filters, DC blockers, custom envelopes, edge detectors | 2-3 opcodes |
| **Raw delay line** | — | ~3 modulation FX | Custom reverbs, Karplus-Strong, waveguides | 2-3 opcodes |
| **Type system** | — | ~8 array HOFs | Type checking, overloading, better errors | None |
| **Match destructuring** | — | — | Cleaner pattern code | None |
| **Pattern methods** | — | ~7 pattern transforms | Chainable pattern API | None |

**Cumulative unlock:** Phases 1-2 move ~55 items to userspace (all of Tier 2 + most of Tier 3). Phases 3-4 complete the picture and improve ergonomics.

---

## 5. Design Principles

### The VM stays minimal — complexity belongs in the compiler

Cedar's VM has ~95 opcodes, each a tight per-sample loop. New VM opcodes should only be added when the operation fundamentally requires per-sample state or arena memory that cannot be expressed through existing primitives. `STATE_READ`/`STATE_WRITE` and `DELAY_LINE_*` are justified because they provide new *capabilities*. Convenience operations (mean, normalize, scale) should always be compiler-level.

### Zero-allocation runtime is non-negotiable

Every proposed construct must compile to fixed-size instructions operating on pre-allocated buffers. No heap allocation, no dynamic dispatch, no garbage collection in the audio path. The `state` keyword must use the existing `StatePool` with semantic ID hashing. Delay lines must use `AudioArena`.

### Live-coding ergonomics > theoretical purity

Akkado is a live-coding language. Constructs should optimize for:
- **Discoverability** — autocomplete, type-directed suggestions
- **Incrementality** — small code changes → small behavior changes
- **State preservation** — hot-swap must preserve user-defined state (via semantic IDs)
- **Error recovery** — partial programs should produce partial audio, not silence

### "Good enough" stdlib > perfect but complex language features

A stdlib `fn saturate(sig, drive) -> tanh(sig * drive)` that's 99% as good as a native opcode is better than a complex language feature that enables a theoretically perfect implementation. The 40+ aliases in the current codebase demonstrate this — they add zero complexity to the language while improving discoverability.

### Faust-style composition over Rust-style abstraction

Akkado's model is closer to Faust (compile-time function composition, no runtime objects) than Rust (ownership, traits, generics). Language extensions should follow this direction:
- Functions compose, they don't inherit
- Types are structural, not nominal
- Dispatch is compile-time, not runtime
- Arrays are fixed-size, not growable
- State is per-node, not per-object

### Backward compatibility is table stakes

Every proposed construct is additive. Existing Akkado programs continue to compile and produce identical output. Builtins can be migrated to stdlib progressively — the compiler falls back to the builtin if no stdlib definition is found.

---

## 6. Recommended Roadmap

```
Phase 1 (compiler-only, no VM changes):
  🟡 1a. Module/import system ─ import statement shipped; URI unification + stdlib migration pending
  ✅ 1b. Dot-call syntax ──────── parser rewrite, low risk
  ✅ 1c. const fn ─────────────── compile-time evaluation

Phase 2 (VM work, high impact):
  2a. User-defined state ───── STATE_READ/STATE_WRITE opcodes
  2b. Raw delay line ───────── DELAY_LINE_* opcodes

Phase 3 (type system foundation):
  ✅ 3a. TypedValue refactor ──── per existing PRD
  ✅ 3b. Match destructuring ──── parser + symbol table
  ✅ 3c. Pattern methods ──────── type-aware dot-call

Phase 4 (polish):
  ✅ 4a. Expression defaults ──── const fn prerequisite
  ✅ 4b. Range patterns ───────── sugar over comparisons
  ✅ 4c. Record spreading ─────── compile-time field merge
```

Phases 1, 3, and 4 are complete except for the module/import system's
later stages: the `import` statement (direct injection + namespaces)
has shipped, but URI-resolver unification, the web virtual filesystem,
and the stdlib migration remain — see [`prd-module-import.md`](prd-module-import.md).
Other remaining work: Phase 2 (user-defined state + raw delay lines).
