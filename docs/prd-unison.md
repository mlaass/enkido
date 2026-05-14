# PRD: Unison — Userspace Voice Multiplication with Detune, Width, and Phase Variation

> **Status: PHASE 1 DONE — `unison` ships.** Adds a `unison(...)` userspace stdlib function that multiplies a single instrument across N detuned, panned, phase-shifted voices. All four compiler prerequisites landed earlier: stereo support in `poly()` (Phase 0b, commit `6b44b4b`), generalized N-arity closures (Phase 0c), `map(arr, fn)` dispatching on closure arity (Phase 0d), and a stereo-preserving variadic `sum(...)` (Phase 0a). Phase 1 (the `unison` stdlib fn + the compiler glue it turned out to need — see the Phase 1 notes) is now done with an `[unison]` test case. Remaining: Phase 2 (docs/demo), Phase 3 (Python smoke test).

## 1. Overview

Akkado today has two voice-related primitives:

- **`poly(input, instrument, voices=64)`** — a *runtime voice allocator*. Different incoming notes are assigned to different voices via oldest-voice stealing. Lowered to `POLY_BEGIN` / `POLY_END` opcodes with state-pool allocation. `voices` is a compile-time integer literal (the maximum); the active count is runtime via voice stealing.
- **`stereo(mono)`** / **`stereo(L, R)`** — pure signal duplication. No voice allocation, just `COPY` instructions.

What's missing is the third common primitive: **unison** — multiply a *single* note across N voices with detune, stereo width, and phase variation. Classic supersaw, fattened pads, doubled leads. Unlike `poly`, all unison voices play *the same note simultaneously*; they differ only in detune offset, stereo position, and phase.

This PRD ships `unison` as a **userspace stdlib function**, not a new opcode. Akkado already has most of the building blocks: first-class closures, `linspace`, `pan`, oscillators with `phase` parameters, record literals, and arithmetic on signals. A handful of compiler prerequisites land first so the userspace expansion has the right primitives:

1. A new variadic, stereo-preserving `sum(...)` (replaces today's array-summing `sum`).
2. Stereo instrument support in `poly()` (currently mono-only) so a unisoned wrapper is poly-compatible.
3. Generalized N-arity closures: a single `apply_function_ref` taking N argument buffers, replacing the split `apply_function_ref` / `apply_binary_function_ref` pair.
4. `map(arr, fn)` dispatches on closure arity. `(v) -> ...` keeps current behavior; `(v, idx) -> ...` receives a per-element index buffer.

If userspace expansion turns out to be too expensive in practice (e.g. for `voices >= 8` with complex instruments), a follow-up PRD can promote it to a builtin opcode mirroring `poly`, ideally inside a unified "function call with state stack" abstraction shared with `POLY_BEGIN/END`.

### Why?

- **Live-coding ergonomics**: writing `unison(freq, gate, vel, voice, voices=5, detune=0.3)` is dramatically faster than hand-rolling five oscillators with offsets and pans.
- **Composability with `poly`**: once `poly` accepts stereo-returning instruments, a unisoned wrapper is itself a 3-arg instrument that can be passed to `poly(...)`, giving polyphonic clusters for free without a special `poly + unison` combined opcode.
- **Foundation for more**: the generalized N-arity closure machinery and `map`-with-index extension are broadly useful (sequencers, harmonics, indexed envelopes, multi-arg reduce), not specific to unison.

### Major design decisions

- **Userspace, not a builtin opcode.** `unison` is a function in `STDLIB_SOURCE` (`stdlib.hpp`). All voice slots are statically expanded at compile time via `map` over a compile-time `linspace(-1, +1, voices)`. Trade-off: `voices` must be a compile-time integer literal (matches `poly`'s `max_voices` constraint) and the AST grows linearly with `voices`. This is acceptable for `voices ≤ 16`.
- **`detune`, `width`, `phase` are runtime audio-rate signals.** Per-voice offsets are computed at runtime by multiplying the compile-time unit spread by the runtime parameter — no `linspace(-detune, detune, voices)` (which would force compile-time detune).
- **`map(arr, fn)` dispatches on closure arity.** Built on top of generalized N-arity closures: a closure declaring `(v)` gets just the element; `(v, idx)` gets value + index const buffer; longer arities are well-defined and reject only when the closure declares more params than `map` supplies (single value + index, so >2 errors).
- **Instrument signature is `(freq, gate, vel, ext)`** where `ext` is a record `{idx, count, detune_st, pan, phase}`. Positional with `poly`'s `(freq, gate, vel)` for the first three params; the 4th `ext` is a record so future fields don't break existing code.
- **Monophonic semantics for `unison` itself.** Unison plays one note at a time. Polyphonic unison is achieved by *wrapping* — define a unisoned instrument, then pass it to `poly`. No new `unison` × `poly` cross-product opcode is required.
- **Symmetric linear detune/width spread for v1.** Per-voice multipliers come from `linspace(-1, +1, voices)`; random and array-driven distributions are deferred to a future PRD.
- **`voices = 1` is special-cased in the stdlib** to emit a single centered, undetuned, in-phase voice (linspace's `[0]` collapse doesn't naturally happen at n=1).

---

## 2. Problem Statement

### What exists today

| Capability | Status | Reference |
|---|---|---|
| `poly(input, instrument, voices=64)` runtime allocator | ✅ Works | `handle_poly_call` in `codegen_functions.cpp` |
| `stereo(mono)` / `stereo(L, R)` | ✅ Works | `handle_stereo_call` in `codegen_stereo.cpp` |
| `pan(mono, position)` and `pan(stereo, position)` | ✅ Works | `handle_pan_call` in `codegen_stereo.cpp` (mono → equal-power stereo, stereo → balance) |
| First-class closures `(p, …) -> body` | ✅ Works | `parse_closure` in `parser.cpp` |
| Default closure params (`p = 0.5`) | ✅ Works | stdlib's `osc(type, freq, pwm = 0.5, …)` in `stdlib.hpp` |
| Variadic rest params `(...args)` on closures | ✅ Works | `parse_closure` |
| `linspace(start, end, n)` (compile-time const array) | ✅ Works | `handle_linspace_call` in `codegen_arrays.cpp` (E173 if args aren't compile-time) |
| `random(n)` deterministic per-call-site | ✅ Works | `handle_random_call` (LCG seeded by semantic-ID) |
| Oscillator `phase` parameter on `sin`/`tri`/`saw`/`sqr` | ✅ Works | `BUILTIN_FUNCTIONS` table in `builtins.hpp` |
| Record literals `{a: 1, b}` + field access | ✅ Works | `parse_record_literal`, `handle_record_literal` |
| Array indexing `arr[i]` (compile-time and runtime) | ✅ Works | `parse_index` + `ARRAY_INDEX` opcode |
| `pow(base, exp)` builtin and `^` operator | ✅ Works | `pow` in `BUILTIN_FUNCTIONS`; `^` parses to `pow` |
| `map(arr, fn)` with 1-arg closure | ✅ Works | `handle_map_call` in `codegen_arrays.cpp` |
| `apply_function_ref` (1 param), `apply_binary_function_ref` (2 params) | ✅ Works | both in `codegen_arrays.cpp` |
| `sum(array)` (mono-only — collapses arrays of stereo to mono) | ⚠️ Exists, will be replaced | `handle_sum_call` returns `TypedValue::signal(...)` (mono) |
| `poly` accepts stereo-returning instruments | ❌ Not supported | `handle_poly_call` allocates a single mono `voice_out_buf` |
| Generalized N-arg closure helper | ❌ Not supported | only unary and binary helpers exist |
| `map(arr, fn)` dispatches on closure arity | ❌ Not supported | call site always passes one buffer |
| Stdlib as `.akk` file on disk | ❌ Embedded C++ string | `STDLIB_SOURCE` in `stdlib.hpp` |
| `unison(...)` builtin or stdlib function | ❌ Doesn't exist | — |

### What's missing

To write `unison` in user code today, you must construct N parallel voices that each take a *different* detune offset, *different* pan position, and *different* phase. The natural expression is `map(unit, (u, idx) -> ...)` so the closure can multiply a unit-spread value by the runtime `detune`/`width`/`phase` and use `idx` for per-voice variation. But `map` only passes the value, not the index. Then you want to fold the resulting array of stereo voices back into one stereo signal — but today's `sum()` collapses to mono. And if the unisoned wrapper is to be polyphonic via `poly`, `poly` itself must accept stereo-returning instruments. All three gaps are addressed as prerequisites in this PRD.

### Current vs proposed

| Today | After this PRD |
|---|---|
| Fattening a voice = hand-write N oscillators with offsets | `unison(freq, gate, vel, voice, voices=5, detune=0.3)` |
| `map(arr, (v) -> ...)` only | `map(arr, (v) -> ...)` *or* `map(arr, (v, idx) -> ...)`; arity dispatch generalizes beyond map |
| `sum(array)` collapses to mono | `sum(...)` is variadic and stereo-preserving |
| `poly` is mono-only | `poly` accepts stereo-returning instruments |
| Polyphonic unison requires a custom builtin | `poly(%, fat_voice, 4)` where `fat_voice` is a unisoned wrapper |
| Per-voice phase variation requires manually offsetting each oscillator | `instrument(freq, gate, vel, ext)` receives `ext.phase` per voice |

---

## 3. Goals and Non-Goals

### Goals

1. **Ship `unison` as userspace Akkado.** A single function in `STDLIB_SOURCE` produces a stereo signal with N detuned, panned, phase-shifted copies of a passed instrument. No new opcodes.
2. **Variadic stereo-preserving `sum(...)` builtin.** Replace today's array-summing `sum(array)` with a variadic helper that sums signals (mono or stereo) preserving channel count. Required for unison's voice fold-down and broadly useful elsewhere.
3. **Stereo support in `poly`.** Extend `handle_poly_call` to accept stereo-returning instruments so a unisoned wrapper can be passed directly to `poly`.
4. **Generalized N-arity closures.** Replace the split `apply_function_ref` (1 param) and `apply_binary_function_ref` (2 params) with a single helper that takes N argument buffers (capped at, e.g., 32). All existing call sites — `map`, `reduce`, `zipWith` — thread through unchanged.
5. **`map(arr, fn)` dispatches on closure arity.** `(v) -> ...` keeps current bit-identical behavior; `(v, idx) -> ...` receives a per-element index buffer (`emit_push_const(float(i))`); a closure declaring more params than `map` supplies errors clearly.
6. **Compositional with `poly`.** Once goals 3 and 4 land, a unisoned wrapper `(f, g, v) -> unison(f, g, v, voice, voices=N)` is a valid `poly` instrument. No special-case codegen.
7. **Predictable defaults.** `unison(freq, gate, vel, voice)` with no other args produces a sane fat sound (`voices=2`, `detune=0.5`, `width=0.5`, `phase=0`).
8. **Documentation + one demo patch.** Reference page in `web/static/docs/builtins/unison.md`, plus an example in `web/static/patches/unison-demo.akk` showing both standalone and `poly`-wrapped use.
9. **Zero regressions.** Existing patches that use `map`, `poly`, `stereo`, `pan`, oscillator `phase` continue to compile. The `sum(array)` replacement is a deliberate breaking change; existing patches must migrate to the variadic form.

### Non-Goals

- **No `UNISON_BEGIN` / `UNISON_END` opcode.** Voice expansion is static, not runtime. (May be revisited if perf demands; a future PRD should consider unifying poly's BEGIN/END and any future unison opcodes into a shared "function call with state stack" abstraction rather than two disjoint opcode pairs.)
- **No random or array-driven detune distribution in v1.** Symmetric linear spread only. Future work.
- **No `voices` as a runtime/audio-rate value.** Voice count must be a compile-time integer literal — same constraint as `poly`'s `voices` arg today. The AST is unrolled at codegen. (`detune`, `width`, `phase` *are* runtime audio-rate signals.)
- **No automatic wrapping of 3-arg `poly` instruments.** Unison instruments must declare 4 params. A 1-line user wrapper bridges to `poly`-style instruments: `(f,g,v,_) -> existing(f,g,v)`. (Auto-arity dispatch could be a follow-up.)
- **No record mutation / per-block extras updates.** The `ext` record passed to the instrument is constructed once per voice at codegen time; the per-voice scalar fields (`idx`, `count`) are compile-time constants, while `detune_st` / `pan` / `phase` are audio-rate signals derived from runtime inputs.
- **No combined `poly_unison` builtin.** Composition via wrapping is the supported path.
- **No detune LFO modulation built into unison.** Users add their own `lfo(...)` to the voice's freq inside the instrument if desired.

---

## 4. Target Syntax

### 4.1 Minimal usage

```akkado
// Define a 4-arg unison-compatible instrument
fn voice(freq, gate, vel, ext) -> 
    saw(freq) * ar(gate, 0.05, 0.4) * vel

// Drive it with a single note source
e = pat("c4") as evt
unison(evt.freq, evt.gate, evt.vel, voice) |> out(%, %)
```

Defaults: `voices=2, detune=0.5, width=0.5, phase=0`.

### 4.2 Full parameter set

```akkado
unison(
    freq, gate, vel, voice,
    voices = 5,        // 5 detuned copies
    detune = 0.3,      // ±0.3 semitones spread
    width = 0.8,       // ±0.8 stereo pan spread
    phase = 0.25       // up to 0.25 cycles of initial phase variation
) |> out(%, %)
```

### 4.3 Instrument that uses the `ext` record

```akkado
fn rich_voice(freq, gate, vel, ext) -> 
    let detuned = saw(freq, ext.phase)               // initial phase from unison
    let env = ar(gate, 0.05 + ext.idx * 0.005, 0.4)  // tiny per-voice attack stagger
    detuned * env * vel
```

The `ext` record contains:
- `ext.idx` — voice index, 0..voices-1
- `ext.count` — total voice count (= `voices` arg)
- `ext.detune_st` — this voice's detune offset in semitones
- `ext.pan` — this voice's pan position, -1..+1
- `ext.phase` — this voice's initial phase offset, 0..1

### 4.4 Bridging a 3-arg `poly` instrument

```akkado
fn lead(freq, gate, vel) -> sqr(freq) * adsr(gate, 0.01, 0.1, 0.7, 0.3)

// Wrap to ignore the ext record
fn fat_lead(f, g, v, _) -> lead(f, g, v)

unison(440, 1, 1, fat_lead, voices = 5, detune = 0.4) |> out(%, %)
```

### 4.5 Composition with `poly` (polyphonic clusters)

```akkado
// fat_voice itself is a 3-arg instrument compatible with poly()
fn fat_voice(freq, gate, vel) -> 
    unison(freq, gate, vel, (f, g, v, ext) -> saw(f) * v, voices = 4, detune = 0.2)

pat("c4 e4 g4 b4") |> poly(%, fat_voice, 4) |> out(%, %)
```

Note `poly`'s actual signature is `poly(input, instrument, voices=64)` — instrument second, max-voices third (compile-time literal, default 64). Each note triggers a 4-voice unison cluster; up to 4 notes ring simultaneously → up to 16 oscillators alive in the state pool at once. CPU scales with `poly_voices × unison_voices`.

This composition requires Phase 0b (stereo poly) since `unison` always returns stereo.

### 4.6 `map` with index (the supporting change)

```akkado
// Generate harmonics with per-element phase staggering
freqs = [220, 440, 660, 880]
phases = [0, 0.25, 0.5, 0.75]
voices = map(freqs, (f, idx) -> sine(f, phases[idx]))
sum(voices) |> out(%, %)
```

Existing 1-arg form is unchanged:

```akkado
sigs = map([220, 440], (f) -> sine(f))   // still works
```

---

## 5. Architecture

### 5.1 Userspace `unison` definition

Added to `STDLIB_SOURCE` in `stdlib.hpp`. `voices` is a compile-time literal; `detune`, `width`, `phase` are runtime audio-rate signals. The unit-spread array `linspace(-1, +1, voices)` is compile-time; multiplying by the runtime params produces per-voice offsets without forcing the params to be constants.

```akkado
fn unison(freq, gate, vel, instrument,
          voices = 2, detune = 0.5, width = 0.5, phase = 0) -> {
    if voices == 1 {
        // Special-case: single centered, undetuned, in-phase voice.
        // (linspace(-1, +1, 1) returns [-1] not [0], so the general path would
        //  produce an off-center voice — handle n=1 explicitly.)
        let ext = { idx: 0, count: 1, detune_st: 0, pan: 0, phase: 0 }
        pan(instrument(freq, gate, vel, ext), 0)
    } else {
        let unit = linspace(-1, 1, voices)   // compile-time const array, symmetric for voices >= 2
        map(unit, (u, idx) -> {
            let d_st     = u * detune                  // runtime offset, semitones
            let v_freq   = freq * pow(2, d_st / 12)
            let v_pan    = u * width                    // runtime, -width..+width
            let v_phase  = (idx / voices) * phase       // runtime spread
            let ext = {
                idx,
                count:      voices,
                detune_st:  d_st,
                pan:        v_pan,
                phase:      v_phase
            }
            pan(instrument(v_freq, gate, vel, ext), v_pan)
        }) |> sum(%)
    }
}
```

`sum(...)` here is the new stereo-preserving variadic builtin from Phase 0a. Given an array of stereo voices (each from `pan(...)`), it sums L and R channels independently and returns a stereo signal. With Phase 0a in place there is no need for a `sum_stereo` companion.

### 5.2 Compiler changes

#### 5.2.a Generalized N-arity closure helper (Phase 0c)

Today `codegen_arrays.cpp` has two near-duplicate helpers — `apply_function_ref` (1 param) and `apply_binary_function_ref` (2 params). Replace them with a single helper:

```cpp
std::uint16_t CodeGenerator::apply_function_ref(
    const FunctionRef& ref,
    std::span<const std::uint16_t> arg_bufs,
    SourceLocation loc);
```

The helper pushes a scope, binds captures, binds `ref.params[i].name` to `arg_bufs[i]` for `i < arg_bufs.size()`, and visits the body. Errors:

- E132 if `ref.params.size() < arg_bufs.size()` ("Closure has fewer parameters than arguments supplied").
- The existing minimum-arity check is replaced by an exact-or-greater check at each call site.

All current callers (`handle_map_call`, `handle_reduce_call`, `handle_zipwith_call`) update to pass an `std::array` or `std::span` of arg buffers. Cap the supported arity at 32 for safety in case a closure body is recursive or pathological.

#### 5.2.b `map` dispatches on closure arity (Phase 0d)

In `handle_map_call`, both the single-buffer and multi-buffer branches dispatch on `func_ref->params.size()`:

```cpp
const auto arity = func_ref->params.size();
if (arity == 0) {
    error("E132", "map closure must take at least one parameter (the element)", n.location);
    return TypedValue::void_val();
}
if (arity > 2) {
    error("E141", "map closure takes 1 or 2 parameters: (val) or (val, idx)", n.location);
    return TypedValue::void_val();
}

// Per element i:
std::uint16_t idx_buf = (arity == 2)
    ? emit_push_const(buffers_, instructions_, static_cast<float>(i))
    : 0xFFFF;

std::array<std::uint16_t, 2> arg_bufs_storage{element_buffers[i], idx_buf};
std::span<const std::uint16_t> arg_bufs{arg_bufs_storage.data(), arity};

result_buffers.push_back(apply_function_ref(*func_ref, arg_bufs, n.location));
```

`emit_push_const(buffers_, instructions_, value)` is the existing helper from `akkado/include/akkado/codegen/helpers.hpp` — same primitive used by `random()` and `linspace()`. The index buffer is a single `PUSH_CONST` emitting `float(i)` into a fresh buffer; no new opcode needed.

The single-arg path is bit-identical to today: when `arity == 1`, `arg_bufs` is a one-element span and the new helper produces the same instruction stream as the legacy `apply_function_ref`.

### 5.3 Voice expansion (compile-time structure, runtime params)

```
unison(freq, gate, vel, voice, voices=4, detune=<rt>, width=<rt>, phase=<rt>)
                            │
                            ▼  (stdlib expansion)
let unit = linspace(-1, 1, 4)   // compile-time: [-1, -1/3, 1/3, 1]
                            │
                            ▼  (map with idx, generalized N-arity dispatch)
map([-1, -1/3, 1/3, 1], (u, idx) -> ...)
                            │
                            ▼  (codegen unrolls map over multi-buffer, params remain runtime)
voice0_buf = pan(voice(freq * pow(2, (-1     * detune)/12), gate, vel, ext0), -1     * width)
voice1_buf = pan(voice(freq * pow(2, (-1/3   * detune)/12), gate, vel, ext1), -1/3   * width)
voice2_buf = pan(voice(freq * pow(2, ( 1/3   * detune)/12), gate, vel, ext2),  1/3   * width)
voice3_buf = pan(voice(freq * pow(2, ( 1     * detune)/12), gate, vel, ext3),  1     * width)
                            │
                            ▼  (variadic stereo-preserving sum, Phase 0a)
stereo(L0+L1+L2+L3, R0+R1+R2+R3)
```

`detune`, `width`, `phase` enter as audio-rate buffers and are multiplied in by per-sample arithmetic — modulating any of them at runtime works without recompilation.

The semantic-ID path mechanism gives each voice's stateful sub-nodes (oscillators, envelopes) stable distinct hashes via the `map#N/elemK/...` path scheme already used by `map`, so hot-swap state preservation works automatically.

### 5.4 Constraints

| Limit | Value | Rationale |
|---|---|---|
| `voices` range | 1..16 | Compile-time AST unrolling. >16 risks compile time blow-up and arena pressure. Matches the spirit of `poly`'s 1..128 range but tighter because every voice exists statically rather than via state-pool allocation. |
| `voices` form | Compile-time integer literal | Required because the unit-spread `linspace(-1, +1, voices)` and the `map` unrolling are both compile-time. |
| `detune` form | Runtime audio-rate signal (or const) | Multiplied in at runtime via per-voice arithmetic; not constrained at codegen. |
| `width` form | Runtime audio-rate signal (or const) | Same — runtime per-voice multiplication. |
| `phase` form | Runtime audio-rate signal (or const) | Same. |
| Instrument arity | Exactly 4 | `(freq, gate, vel, ext)`. 3-arg `poly` instruments require a wrapper. |
| Output | Stereo signal | Always; users wanting mono can do `0.5 * (left(u) + right(u))`. |

---

## 6. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| `POLY_BEGIN` / `POLY_END` opcodes | **Modified** | Phase 0b extends them to carry a stereo voice-output buffer pair when the instrument body returns stereo. Mono-instrument bytecode unchanged. |
| `STEREO_*` / `PAN` opcodes | **Stays** | No changes — `pan` already accepts stereo input. |
| `OSC_*` opcodes | **Stays** | Phase parameter already exists and is used directly. |
| `handle_sum_call` (`sum`) | **Replaced** | Phase 0a: dropped array-summing semantics; new `sum(...)` is variadic and stereo-preserving. Existing patches that called `sum(array)` migrate to `sum(...arr)` or fold via `reduce`. |
| `apply_function_ref` / `apply_binary_function_ref` | **Refactored** | Phase 0c: replaced by a single N-arity `apply_function_ref(ref, std::span<const std::uint16_t>, loc)`. All current callers update; semantics for arity 1 and 2 are bit-identical. |
| `map(arr, 1-arg-fn)` semantics | **Stays** | Bit-identical bytecode after Phase 0d when `arity == 1`. |
| `map(arr, 2-arg-fn)` | **New** | Phase 0d: closure receives `(val, idx)`; `idx` is a fresh PUSH_CONST buffer per element. |
| `handle_poly_call` | **Modified** | Phase 0b: detect stereo instrument body, allocate stereo voice-out buffers, route per-channel mix. |
| `STDLIB_SOURCE` | **Modified** | Phase 1: adds `fn unison(...)`. |
| `STDLIB_LINE_COUNT` | **Auto-adjusts** | constexpr lambda; recomputes. |
| Hot-swap state preservation | **Stays** | Existing semantic-ID path scheme covers `map#N/elemK/...` already; new prereqs don't change path semantics. |
| Web docs index | **Modified** | New entry for `unison` builtin; `map` doc gains 2-arg form; `sum` doc rewritten for variadic stereo behavior. |
| F1 help lookup | **Modified** | Built from frontmatter keywords; rebuild via `bun run build:docs`. |

---

## 7. File-Level Changes

### Modify

| File | Change |
|---|---|
| `akkado/src/codegen_arrays.cpp` | Phase 0a: rewrite `handle_sum_call` to be variadic and stereo-preserving (sum L and R independently when any input is stereo). Phase 0c: replace `apply_function_ref` and `apply_binary_function_ref` with a single N-arity helper. Phase 0d: `handle_map_call` dispatches on closure arity (errors on >2). |
| `akkado/include/akkado/builtins.hpp` | Update `sum` entry to be variadic. Update `map` description to mention `(val, idx)` form. Phase 0c: any internal callers using `apply_*_function_ref` update to the new helper. |
| `akkado/src/codegen_functions.cpp` | Phase 0b: `handle_poly_call` detects stereo instrument body and routes per-channel through `POLY_BEGIN`/`POLY_END`. |
| `akkado/include/akkado/stdlib.hpp` | Phase 1: add `fn unison(...)` to `STDLIB_SOURCE`. |
| `akkado/tests/test_codegen.cpp` | New test cases for variadic stereo `sum`, generalized N-arity closures, `map`-with-index, stereo-poly, and `unison` (see §11). |
| `cedar/include/cedar/vm/instruction.hpp` and related opcode files | If Phase 0b requires extending `POLY_BEGIN` / `POLY_END` payloads to carry a second voice-output buffer, update the opcode definition and VM handler accordingly. |
| `web/static/docs/builtins/sum.md` (if exists) or its index entry | Document new variadic stereo-preserving semantics. |
| `web/static/docs/builtins/map.md` | Document 2-arg closure form with example. |
| `web/static/docs/builtins/poly.md` (if exists) or its index entry | Note stereo-instrument support. |
| `web/static/docs/builtins/index.md` (or per-section index) | Add `unison` entry. |

### Create

| File | Purpose |
|---|---|
| `web/static/docs/builtins/unison.md` | Reference page: signature, params, instrument convention, examples (basic, with `ext`, composed with `poly`). |
| `web/static/patches/unison-demo.akk` | Demo patch showing standalone unison and a `poly(%, unisoned, 4)` cluster. |
| `experiments/test_unison.py` | Python opcode-level smoke test: render unison output for a simple `saw` voice, verify N spectral peaks around target freq, save WAV. |

### Stays

| File | Reason |
|---|---|
| `cedar/include/cedar/opcodes/poly.hpp` | Voice-pool allocator logic unchanged; only the buffer routing extends to stereo if Phase 0b needs it. |
| `akkado/src/codegen_stereo.cpp` | `pan()` and `stereo()` are reused unchanged. |

---

## 8. Implementation Phases

Phases 0a–0d are independent prerequisites that can ship separately. Phases 0a, 0c are pure refactors with no new behavior; 0b adds capability; 0d depends on 0c. Phase 1 depends on all four.

### Phase 0a — Stereo-preserving variadic `sum(...)`

**Status**: DONE  
**Goal**: Replace `sum(array)` with `sum(...)`. The new builtin accepts any number of signal arguments, sums per-channel, and returns stereo if any input is stereo.

**Implementation note**: Shipped as a *backward-compatible* variadic (per user decision) — `sum` accepts both the legacy single-array form (`sum(array)`, still bit-identical bytecode for mono arrays) **and** the new N-arg form (`sum(a, b, ...)`). The only behavior change is stereo preservation. This avoids breaking ~80 internal test sites / the chord-summing code and matches this PRD's own unison examples (§5.1, §4.6), which pass a single array to `sum`. `sum` is special-cased in `analyzer.cpp` (arity ≥ 1, alongside `compose`) and rewritten in `handle_sum_call`.

Files to modify:
- `akkado/src/codegen_arrays.cpp` — rewrite `handle_sum_call`.
- `akkado/include/akkado/builtins.hpp` — update `sum` entry (variadic, returns mono or stereo per input channel count).
- `akkado/tests/test_codegen.cpp` — `[sum][stereo]`, `[sum][variadic]` tests.
- Any existing patches in `web/static/patches/` calling `sum(arr)` migrate to either `sum(...arr)` (if Akkado has spread on call args) or `reduce(arr, (a, b) -> a + b, 0)`.

Tasks:
- [ ] Audit the codebase for `sum(...)` call sites; document migration of any array-form callers.
- [ ] Rewrite `handle_sum_call` to extract per-arg `TypedValue`s and emit per-channel ADD chains.
- [ ] Test: `sum(saw(220), saw(330))` → mono signal that is the sum of both saws.
- [ ] Test: `sum(stereo(saw(220),saw(220)), stereo(saw(330),saw(330)))` → stereo, L sums correctly, R sums correctly.
- [ ] Test: `sum(saw(220), stereo(saw(220), saw(330)))` → stereo (mono input duplicates to L and R, then sums).

### Phase 0b — Stereo support in `poly()`

**Status**: DONE (commit `6b44b4b`)  
**Goal**: `handle_poly_call` accepts a stereo-returning instrument body and routes per-channel through `POLY_BEGIN` / `POLY_END`. Landed as a broader stereo-native migration: `poly` always outputs stereo, `voice_out`/`mix` are adjacent L/R pairs, `POLY_BEGIN` carries `STEREO_OUTPUT`, mono bodies broadcast dual-mono.

Files to modify:
- `akkado/src/codegen_functions.cpp` — `handle_poly_call`.
- `cedar/include/cedar/vm/instruction.hpp` and the POLY opcode handler — extend to carry a second voice-output buffer index for stereo bodies.
- `akkado/tests/test_codegen.cpp` — `[poly][stereo]` tests.

Tasks:
- [ ] Detect stereo instrument body (visit the body once, observe the resulting `TypedValue.channels`, then re-emit with the right buffer wiring — or visit speculatively and patch).
- [ ] Allocate stereo voice-out buffers and a stereo `mix_buf` when the body is stereo.
- [ ] Update `POLY_BEGIN` / `POLY_END` payload to optionally carry the right-channel buffer indices; mono path unchanged.
- [ ] Test: `pat("c4") |> poly(%, (f,g,v) -> stereo(saw(f), saw(f*1.01)))` compiles and produces stereo output.
- [ ] Test: existing mono `poly` patches produce bit-identical bytecode.

### Phase 0c — Generalized N-arity closure helper

**Status**: DONE  
**Goal**: Replace `apply_function_ref` (1 param) and `apply_binary_function_ref` (2 params) with a single `apply_function_ref(ref, std::span<const std::uint16_t>, loc)` that handles N arguments.

**Implementation note**: Single helper with a 32-arity cap; under-arity now errors **E132** at the call site (the old `apply_binary_function_ref` E140 is retired from this path — E140 stays defined, it's used elsewhere). All 5 call sites updated (`map` ×2, `reduce`, `zipWith`, `compose` chain). 1-arg and 2-arg paths produce identical bytecode.

Files to modify:
- `akkado/src/codegen_arrays.cpp` — refactor the helpers; update all current call sites (`handle_map_call`, `handle_reduce_call`, `handle_zipwith_call`, etc.).
- Any other files that call `apply_*_function_ref`.
- `akkado/tests/test_codegen.cpp` — closure-arity tests.

Tasks:
- [ ] Define new helper signature taking `std::span<const std::uint16_t>`.
- [ ] Add an arity cap (e.g. 32) to guard against pathological inputs.
- [ ] Update existing call sites; ensure 1-arg and 2-arg paths produce bit-identical bytecode.
- [ ] Test: `reduce` and `zipWith` semantics unchanged for existing patches.
- [ ] Test: a 3-arg closure can be invoked through the helper from a future caller (sets the stage for `zipWith3`).

### Phase 0d — `map(arr, fn)` dispatches on closure arity

**Status**: DONE  
**Goal**: `map(arr, (v) -> body)` keeps current behavior; `map(arr, (v, idx) -> body)` receives a per-element index buffer; `map(arr, (a, b, c) -> body)` errors clearly.

**Implementation note**: The PRD specified `E141` for the >2-arity error, but `E141` is already in use (compose, `len`, `voicing`, destructure). The ">2 params" error uses **E146** instead; arity 0 uses **E132**. For a single-buffer (scalar-signal) input, a 2-arg closure receives index `0`.

Depends on: Phase 0c.

Files to modify:
- `akkado/src/codegen_arrays.cpp` — both branches of `handle_map_call`.
- `akkado/include/akkado/builtins.hpp` — refresh `map` description.
- `akkado/tests/test_codegen.cpp` — `[map][index]` tests.

Tasks:
- [ ] In `handle_map_call`, read `func_ref->params.size()` and reject 0 (E132) and >2 (E141).
- [ ] Allocate per-element const index buffer via `emit_push_const(buffers_, instructions_, float(i))` only when arity == 2.
- [ ] Dispatch through the unified `apply_function_ref` helper from Phase 0c.
- [ ] Verify single-arg path produces bytecode identical to current `master` (snapshot test).
- [ ] Test: `map([10, 20, 30], (v, i) -> v + i)` returns `[10, 21, 32]`.
- [ ] Test: existing 1-arg map calls in fixtures unchanged.
- [ ] Test: `map([1], (a, b, c) -> a)` errors with E141 mentioning `(val) or (val, idx)`.

### Phase 1 — `unison` stdlib function

**Status**: DONE  
**Goal**: `unison(freq, gate, vel, instrument, voices=2, detune=0.5, width=0.5, phase=0)` compiles and produces a stereo signal matching expected spectral content.

Depends on: Phase 0a, 0b, 0c, 0d.

**Implementation notes / deviations from the PRD draft:**

- **Q2 resolved — semitones.** `v_freq = freq * pow(2, (u * detune) / 12)`.
- **Q5 resolved — kept `phase`** as the arg name (documents as a spread amount).
- **Q6 resolved — `pan()` is equal-power** (verified `codegen_stereo.cpp`); no
  unison-specific panning math needed.
- **`if`/`let` don't exist in Akkado.** The §5.1 draft is pseudocode. The
  shipped stdlib uses `match(voices) { 1: …, _: … }` (scrutinee form, which
  const-folds because `voices` is a compile-time literal) and plain block-local
  `x = expr` assignments instead of `let`.
- **Named call args use `:` not `=`.** `unison(…, voices: 5, detune: 0.3)`.
  The `=` form in the PRD's §4 target syntax does not parse; `:` is Akkado's
  named-argument syntax. (Function *definitions* still use `=` for defaults.)
- **`voices` bounds — soft limits (per user decision).** `unison` is pure
  userspace Akkado and has no `error()` primitive, so `voices = 0` / `voices > 16`
  are *not* hard compile errors. `voices <= 0` collapses `linspace` to a single
  centered voice; large counts compile but grow the AST. The §9 edge-case rows
  for `voices = 0` / `voices = 17` are downgraded to soft behavior; the
  corresponding error tests are dropped. `voices` not a literal still errors
  E173 from `linspace`.
- **Compiler prerequisites beyond Phases 0a–0d** (the userspace expansion
  needed these to actually work — all landed in this phase):
  - `resolve_param_literal()` + `handle_linspace_call` now resolve a `linspace`
    `n` arg that is a function parameter bound to a literal (previously E173).
  - Parser/analyzer/codegen: numeric param defaults (`voices = 2`) now
    materialise a backing `NumberLit` node (`default_node`), so `match(param)`
    const-folds and `linspace` resolves the param even when left at its
    default. Previously only *explicitly-passed* literal args were tracked in
    `param_literals_`.
  - `handle_function_value_call` now binds **record arguments** as records
    (mirrors `handle_user_function_call`) — required for the `ext` record to
    reach the instrument closure. It reuses an existing record symbol's
    `record_type` when the arg is an identifier (pointing `source_node` at the
    identifier would make `visit()` recurse infinitely).
  - Fixed a latent double-remap bug in `hide_namespaced_definitions`: the
    qualified-symbol copy shared the original's `record_type` `shared_ptr`, so
    Pass 2.5 remapped `source_node` twice and corrupted it. Now deep-copies
    `RecordTypeInfo`. (Exposed because `unison` is the first stdlib fn
    containing a `|>` pipe, which enlarges the pipe-rewrite `node_map`.)
- **3-arg instruments compile rather than erroring.** §9 expected an arity
  error for a 3-param instrument; in practice a call to a function-ref
  parameter can't be arity-checked statically, so the 4th `ext` arg is
  silently dropped. This is *more* permissive than the PRD (the §4.4 manual
  wrapper still works but is no longer strictly required). Strict arity
  checking for function-ref-param calls is out of scope.
- **`unison` returns stereo, so `out(%)` is the idiom** — `out(%, %)` (used in
  some PRD examples) triggers a W185 mixed-channel warning.

Files modified:
- `akkado/include/akkado/stdlib.hpp` — added `fn unison` (with the voices=1 special case).
- `akkado/src/parser.cpp`, `akkado/src/analyzer.cpp`, `akkado/src/codegen_functions.cpp`,
  `akkado/src/codegen_arrays.cpp`, `akkado/include/akkado/codegen.hpp` — the
  compiler prerequisites above.
- `akkado/tests/test_codegen.cpp` — `[unison]` test case (8 sections).

Tasks:
- [x] Confirm Q2 (semitone unit) before locking in the formula.
- [x] Add `unison` to `STDLIB_SOURCE`.
- [x] Test: `unison(440, 1, 1, (f,g,v,e) -> sine(f))` compiles with all defaults.
- [x] Test: `unison(..., voices: 5)` compiles; bytecode contains 5 sine instances at distinct state-IDs.
- [x] Test: `unison(..., voices: 1)` produces a single centered voice (the special-case branch).
- [~] ~~Test: error on `voices = 0`~~ — N/A, soft limits (see note above).
- [~] ~~Test: error on `voices > 16`~~ — N/A, soft limits (see note above).
- [x] Test: `voices` not a compile-time literal → error from `linspace` (E173).
- [x] Test: composing with `poly` — `pat("c4 e4") |> poly(%, fat, 2)` where `fat` calls `unison(… voices: 4)` internally — compiles and produces stereo output.
- [x] Extra: `ext` record fields accessible inside the instrument; runtime-modulatable `detune`; defaulted-`voices` const-folding regression test.

### Phase 2 — Documentation and demo

**Status**: TODO  
**Goal**: Web docs explain unison; demo patch sounds great.

Tasks:
- [ ] Write `web/static/docs/builtins/unison.md` with frontmatter keywords (`unison, supersaw, fatten, detune, voices`).
- [ ] Update `web/static/docs/builtins/map.md` with 2-arg form.
- [ ] Update `web/static/docs/builtins/sum.md` (or its index entry) for the variadic stereo-preserving semantics.
- [ ] Update `web/static/docs/builtins/poly.md` (or its index entry) noting stereo instrument support.
- [ ] Run `bun run build:docs` to refresh the F1 lookup index.
- [ ] Write `web/static/patches/unison-demo.akk` — at least two presets (a fat lead, a polyphonic pad).
- [ ] Add a brief mention in `CLAUDE.md` Akkado concepts section if appropriate.

### Phase 3 — Python opcode smoke test

**Status**: TODO  
**Goal**: A Cedar-level test in `experiments/` that renders unison output and saves a WAV for ear evaluation. Per CLAUDE.md, simulate ≥ 300 seconds of audio for any sequenced/poly path.

Tasks:
- [ ] Compile a small unison patch via `akkado-cli` to bytecode.
- [ ] Render the patch via `cedar_core` Python bindings, ≥ 300 seconds for a poly+unison patch (shorter for the standalone smoke test).
- [ ] Verify spectrum has approximately `voices` peaks within ±detune semitones of the fundamental.
- [ ] Save WAVs to `output/op_unison/`.

### Phase 4 (FUTURE — out of scope here)

If perf becomes a problem at large `voices` counts, promote `unison` to a builtin opcode mirroring the `poly()` lowering path. That would be a separate PRD with full opcode design, voice-pool allocation, and runtime `voices` count.

A more ambitious follow-up: design a unified "function call with state stack" abstraction shared between `POLY_BEGIN/END` and any future `UNISON_BEGIN/END`. Both opcodes call a body multiple times with per-call state slots; a single primitive could subsume both and probably also generic stateful function inlining. This deserves its own PRD.

Other future work, also separate PRDs:
- Random and array-driven detune/pan/phase distributions.
- Audio-rate `voices` count (not feasible with userspace expansion).
- `unison` × `poly` combined opcode if composition overhead matters.
- Auto-arity dispatch so 3-arg poly instruments work in unison without a manual wrapper.

---

## 9. Edge Cases

| Situation | Expected behavior | Rationale |
|---|---|---|
| `voices = 1` | Special-cased in the stdlib (see §5.1): single voice with `ext = {idx: 0, count: 1, detune_st: 0, pan: 0, phase: 0}`, panned to center. | `linspace(-1, +1, 1)` returns `[-1]`, not `[0]`, so the general path would produce an off-center detuned voice. Special-casing keeps the semantically-expected centered voice for `voices=1`. |
| `voices = 0` | Compile error from `linspace` (count must be > 0) or an explicit guard in the stdlib. | Otherwise produces silence with no instrument calls — confusing. |
| `voices = 17` (or >16) | Compile error: `unison voices must be <= 16`. | Static AST unrolling; arena/buffer pressure. |
| `voices` not a literal | Compile error from `linspace` (E173: arguments must be compile-time constants). | linspace already needs compile-time `n`. |
| `detune = 0`, `width = 0`, `phase = 0` (runtime zero) | All per-voice multipliers collapse to 0: voices play in unison, centered, in-phase → equivalent to `voices * instrument(...)` (louder single voice). | Mathematically correct; user error to set all three to 0 but harmless. |
| `instrument` has 3 params instead of 4 | E132 (or analogous arity error) raised by the new N-arity helper at the call site inside the unrolled `map`. | Documented; require manual wrapper for 3-arg poly instruments. |
| `instrument` has 5+ params | Same arity error from the helper. | Same. |
| `instrument` returns stereo (multi-buffer) signal | `pan(stereo_sig, p)` does balance panning (codegen_stereo.cpp); the resulting stereo voices are summed by the variadic stereo-preserving `sum(...)`. End-to-end stereo. | Resolved: see §10 closure of Q3. |
| Negative `detune` or `width` (audio-rate) | Sign flip: voice 0 ends up on the right instead of the left. Fully valid. | Just a runtime multiply; no symmetry assumption to break. |
| `phase > 1` | Each voice gets `(idx/voices) * phase`; oscillators wrap modulo 1 internally. | Existing oscillator behavior; no special handling needed. |
| Hot-swap with `voices` change | Existing patch's voices get fresh state (different `map#N/elemK` paths). | Acceptable — voice count change is structural; micro-crossfade applies. |
| Hot-swap with same `voices`, different `detune` | Existing voice states preserved; only the audio-rate `detune` buffer changes. | Standard hot-swap behavior; runtime params don't affect node identity. |
| `freq = 0` | Each oscillator gets `0 * pow(2, d/12) = 0`. Output: silence. | Correct. |

---

## 10. Open Questions

- **Q1 — Does `sum(arr_of_stereo_signals)` work today? — RESOLVED.**
  Verified: `handle_sum_call` returns `TypedValue::signal(...)` (mono) regardless of input channel count, so an array of stereo voices collapses to mono. Resolution: replace `sum` with the new variadic stereo-preserving `sum(...)` in Phase 0a.

- **Q2 — Detune unit: semitones, cents, or normalized?** — *Open.*
  v1 proposal is **semitones** (`detune = 0.3` → ±0.3 semitones = ±30 cents, classic supersaw range). Alternatives: cents (`detune = 30` for ±30 cents), or 0..1 normalized (`detune = 0.3` = 30% of an octave). Recommend semitones for proximity to musical thinking. Confirm before Phase 1.

- **Q3 — Stereo instruments inside unison? — RESOLVED.**
  Verified: `pan()` accepts stereo input (codegen_stereo.cpp branches on `is_stereo`, doing balance for stereo and equal-power for mono). With Phase 0a's stereo-preserving `sum(...)`, an array of stereo voices folds back to a single stereo signal correctly. Stereo instruments inside unison work end-to-end.

- **Q4 — `pow` vs `^`? — RESOLVED.**
  Verified: `pow` exists as a builtin (`BUILTIN_FUNCTIONS` table) and `^` is parsed as the power operator mapping to `pow`. Either form is valid in stdlib code. The §5.1 stdlib uses `pow(2, d_st / 12)` for clarity.

- **Q5 — `phase` arg naming?** — *Open.*
  `unison(..., phase = 0.25)` controls the *spread* of phase across voices, not an absolute phase. Documentation should be clear; possibly rename to `phase_spread` or `phase_jitter`. Recommend keeping `phase` for brevity but documenting prominently.

- **Q6 — Equal-power vs linear panning inside unison?** — *Open.*
  Does `pan()` use equal-power law (cos/sin) or linear? For unison, equal-power preserves perceived volume regardless of `width`. Verify and document; if `pan` is linear, unison may need its own panning math.

- **Q7 — Map closure arity dispatch policy. — RESOLVED.**
  Resolution: with Phase 0c's generalized N-arity helper, `map` accepts arities 1 (`(v) -> ...`) and 2 (`(v, idx) -> ...`); arity 0 errors as E132, arity ≥ 3 errors as E141 with a clear message naming the supported forms. No warning for 1-arg closures.

---

## 11. Testing / Verification Strategy

### Phase 0a — variadic stereo `sum(...)`

**Akkado tests** (`akkado/tests/test_codegen.cpp`, tag `[sum][stereo]`):

```cpp
SECTION("variadic mono sum") {
    auto src = "sum(saw(220), saw(330)) |> out(%, %)";
    REQUIRE_NOTHROW(compile(src));
}

SECTION("sum preserves stereo when any input is stereo") {
    auto src = R"(
        a = stereo(saw(220), saw(220))
        b = stereo(saw(330), saw(330))
        sum(a, b) |> out(%, %)
    )";
    auto bc = compile_to_bytecode(src);
    // Expect ADD chains for both L and R buffers; output is stereo.
    REQUIRE(/* output TypedValue is stereo */);
}

SECTION("sum mixes mono and stereo (mono duplicates)") {
    auto src = R"(
        m = saw(220)
        s = stereo(saw(330), saw(440))
        sum(m, s) |> out(%, %)
    )";
    REQUIRE_NOTHROW(compile(src));
}
```

### Phase 0b — stereo `poly`

```cpp
SECTION("poly accepts stereo instrument") {
    auto src = R"(
        pat("c4 e4") |> poly(%, (f,g,v) -> stereo(saw(f), saw(f*1.01)), 2) |> out(%, %)
    )";
    REQUIRE_NOTHROW(compile(src));
    // Output TypedValue is stereo.
}

SECTION("mono poly bytecode unchanged") {
    auto src = R"(pat("c4") |> poly(%, (f,g,v) -> saw(f), 4) |> out(%, %))";
    auto bc_before = compile_to_bytecode_str_master(src);
    auto bc_after  = compile_to_bytecode_str_branch(src);
    REQUIRE(bc_before == bc_after);
}
```

### Phase 0c — generalized N-arity closures

```cpp
SECTION("reduce semantics unchanged") {
    auto bc_before = compile_to_bytecode_str_master(
        "reduce([1,2,3], (a,b) -> a + b, 0)");
    auto bc_after  = compile_to_bytecode_str_branch(
        "reduce([1,2,3], (a,b) -> a + b, 0)");
    REQUIRE(bc_before == bc_after);
}
```

### Phase 0d — `map` with index

**Akkado tests** (`akkado/tests/test_codegen.cpp`, tag `[map][index]`):

```cpp
SECTION("map with 2-arg closure receives index") {
    // arr = [10, 20, 30]
    // result = map(arr, (v, i) -> v + i) → [10, 21, 32]
    auto result = compile_and_extract_array("map([10,20,30], (v, i) -> v + i)");
    REQUIRE(result == std::vector<float>{10.0f, 21.0f, 32.0f});
}

SECTION("map with 1-arg closure unchanged") {
    auto bc_before = compile_to_bytecode_str_master("map([1,2,3], (v) -> v * 2)");
    auto bc_after  = compile_to_bytecode_str_branch("map([1,2,3], (v) -> v * 2)");
    REQUIRE(bc_before == bc_after);
}

SECTION("map with 3-arg closure errors") {
    REQUIRE_THROWS_WITH(compile("map([1], (a,b,c) -> a)"),
                        Catch::Contains("E141"));
}
```

### Phase 1 — `unison`

**Akkado tests** (`akkado/tests/test_codegen.cpp`, tag `[unison]`):

```cpp
SECTION("unison compiles with default args") {
    auto src = R"(
        fn voice(f, g, v, e) -> sine(f)
        unison(440, 1, 1, voice) |> out(%, %)
    )";
    REQUIRE_NOTHROW(compile(src));
}

SECTION("unison emits N voice instances") {
    // voices=5 → 5 SINE opcodes, each with distinct state_id
    auto bc = compile_to_bytecode("unison(440, 1, 1, (f,g,v,e) -> sine(f), voices=5)");
    auto sine_count = count_opcode(bc, Opcode::SINE);
    REQUIRE(sine_count == 5);
    auto state_ids = collect_state_ids(bc, Opcode::SINE);
    REQUIRE(unique(state_ids).size() == 5);
}

SECTION("unison voices=1 takes the special-case branch") {
    auto bc = compile_to_bytecode(
        "unison(440, 1, 1, (f,g,v,e) -> sine(f), voices=1)");
    REQUIRE(count_opcode(bc, Opcode::SINE) == 1);
    // Voice should be panned to center; no detune applied.
}

SECTION("unison voices=0 errors") {
    REQUIRE_THROWS_WITH(
        compile("unison(440, 1, 1, (f,g,v,e) -> sine(f), voices=0)"),
        Catch::Contains("voices"));
}

SECTION("unison composes with poly (stereo poly required)") {
    auto src = R"(
        fn voice(f, g, v, e) -> saw(f)
        fn fat(f, g, v) -> unison(f, g, v, voice, voices=4)
        pat("c4 e4") |> poly(%, fat, 2) |> out(%, %)
    )";
    REQUIRE_NOTHROW(compile(src));
    // poly is a runtime voice allocator; the unison body is compiled once
    // and runtime-multiplexed across poly voice slots. So the unison body
    // contributes 4 SAW opcodes (one per unison voice).
    auto bc = compile_to_bytecode(src);
    REQUIRE(count_opcode(bc, Opcode::SAW) == 4);
}

SECTION("unison detune is runtime-modulatable") {
    auto src = R"(
        fn voice(f, g, v, e) -> saw(f)
        d = param("detune", 0.3, 0, 1)
        unison(440, 1, 1, voice, voices=4, detune=d) |> out(%, %)
    )";
    REQUIRE_NOTHROW(compile(src));
}
```

### Phase 3 — Python smoke test

**File**: `experiments/test_unison.py`

```python
def test_unison_spectrum():
    """
    unison(440, 1, 1, voice, voices=5, detune=0.3) should produce 5 spectral peaks
    spread approximately ±0.3 semitones around 440Hz (~±7.6Hz).
    """
    src = '''
        fn voice(f, g, v, e) -> saw(f)
        unison(440, 1, 1, voice, voices=5, detune=0.3) |> out(%, %)
    '''
    audio = render(src, seconds=1.0)
    peaks = find_spectral_peaks(audio, min_db=-20)
    # Should see 5 distinct peaks within ±10 Hz of 440
    nearby_peaks = [f for f in peaks if abs(f - 440) < 20]
    assert len(nearby_peaks) == 5, f"Expected 5 peaks, found {len(nearby_peaks)}"

    wav_path = os.path.join(OUT, "unison_5voice.wav")
    scipy.io.wavfile.write(wav_path, SR, audio)
    print(f"  Saved {wav_path} - Listen for fat detuned saw, voices spread across stereo")
```

For poly+unison, render at least 300 seconds per CLAUDE.md to surface long-window bugs in voice stealing.

### Acceptance criteria

**Prerequisites:**
- [ ] New `sum(...)` is variadic and preserves stereo (`sum(stereo, stereo)` → stereo, `sum(mono, stereo)` → stereo).
- [ ] Stereo-returning `poly` instruments compile and play; mono `poly` bytecode is bit-identical to pre-change.
- [ ] N-arg closures (3+) work in callers that opt into them; existing 1-arg and 2-arg sites produce bit-identical bytecode.
- [ ] `map` with 1-arg closure produces byte-identical bytecode to pre-change `master`.
- [ ] `map` with 2-arg closure correctly receives integer indices.
- [ ] `map` with 3+ arg closure errors with E141.

**Unison:**
- [ ] `unison` compiles for `voices` in {1, 2, 4, 8, 16}.
- [ ] `unison` errors clearly for `voices` in {0, 17, non-literal}.
- [ ] `unison` with `voices = 1` produces a centered, undetuned voice (special-case branch).
- [ ] `unison` runtime-modulates detune/width/phase (param sliders work).
- [ ] `unison` composed with `poly` compiles; the unison body emits `unison_voices` instrument instances which are runtime-multiplexed by poly across `poly_voices` slots.
- [ ] Demo patch `unison-demo.akk` plays without glitches at default audio settings (48kHz / 128-block).
- [ ] Hot-swap test: edit a unison patch's `detune` from 0.3 to 0.5 in the web UI; voices retain envelope state across the swap.
- [ ] WAV output from the Python test sounds like a fat detuned chord (human ear evaluation).
