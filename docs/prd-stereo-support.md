> **Status: COMPLETE** — Implemented and audited 2026-04-21; §5.2 `BuiltinSignature` catalog landed 2026-04-22 as G1 completion. See `docs/audits/prd-stereo-support_audit_2026-04-21.md`. The companion VM-level PRD, [`prd-stereo-native-opcodes.md`](prd-stereo-native-opcodes.md), is now also COMPLETE (2026-05-14): every audio opcode is stereo-native and the auto-lift mechanism is retired.
>
> **Intended final location**: `docs/prd-stereo-support.md` (matches the project's newer lowercase convention).

# PRD: Universal Stereo Signal Semantics in Akkado

## 1. Executive Summary

Akkado today has substantial stereo infrastructure — stereo opcodes (`PAN`, `WIDTH`, `MS_ENCODE`, `MS_DECODE`, `DELAY_PINGPONG`), language functions (`stereo()`, `left()`, `right()`, `pan()`, `width()`, `ms_encode()`, `ms_decode()`, `pingpong()`), and `out()` auto-expansion — but this machinery is **not universal at the signal level**. Most DSP (delays, filters, reverbs, distortion) is mono-only; to apply an effect to a stereo signal the user must manually duplicate the chain. There is no formal signal type, so the compiler cannot warn about mono/stereo mismatches, and users have no reliable way to learn which functions are stereo-aware.

This PRD proposes making stereo a first-class language concept:

1. **Formal signal channel type** (`Mono` | `Stereo`) carried on every `TypedValue` so every expression has a known channel count.
2. **Auto-lift**: when a mono-only DSP function receives a stereo input, the compiler emits a `STEREO_INPUT` flag and the VM runs the opcode twice with independent per-channel state, producing a stereo output. Users get stereo effects transparently.
3. **Explicit conversions**: `stereo(x)` promotes mono → stereo (duplicate channels). A new `mono(x)` demotes stereo → mono (L+R average). These are the canonical, documented ways to move between representations.
4. **Clear compile-time errors** when mono and stereo are mixed in ways auto-lift cannot resolve (e.g. a function that requires mono like `out(L, R)` getting stereo passed to its `L` slot).

Key design decisions made in question rounds:
- Auto-lift is implemented at **VM dispatch** (a `STEREO_INPUT` instruction flag), not via codegen instruction duplication — keeps the instruction stream compact.
- Keep existing **function-call syntax** (`stereo(x)`, `mono(x)`); no method-call `.stereo()` form is added.
- The broader "every opcode becomes stereo-native, mono = degraded stereo" refactor is out of scope here — tracked in a **separate VM-level PRD**.
- Existing stereo functions (`pan`, `width`, `ms_encode`, `ms_decode`, `pingpong`, `left`, `right`) keep their current signatures; this PRD formalises their types rather than changing behaviour.

---

## 2. Problem Statement

### 2.1 Current Behaviour

| Scenario | Expected | Actual |
|----------|----------|--------|
| `osc("saw", 220) \|> stereo() \|> filter_lp(@, 500, 0.7) \|> out(@)` | Stereo lowpass applied, stereo output | Silent failure or incorrect behaviour: `filter_lp` reads `inputs[0]` (left buffer only), right channel is dropped |
| `sig \|> freeverb(@, 0.8, 0.5)` where `sig` is stereo | Stereo reverb tail | Right channel dropped; reverb runs mono on left |
| User wants stereo delay on a mono source | Single call: `sig \|> stereo() \|> delay(@, 0.3, 0.5)` | Must duplicate chain: `sig \|> (delay(@, 0.3, 0.5), delay(@, 0.3, 0.5)) \|> out(@, @)` |
| Mix stereo signal down to mono | Canonical `sig \|> mono(@)` | No function exists; user writes `(left(sig) + right(sig)) * 0.5` manually |
| Mono and stereo mismatch at compile time | Clear error with location | No type checking; silent miscompile or runtime audio confusion |
| Discover which functions are stereo-aware | Docs + compile-time types | Must read source or tests in `akkado/tests/test_codegen.cpp:3422-3540` |

### 2.2 Root Cause

Stereo is tracked only as **codegen-side metadata** (`CodeGenerator::stereo_outputs_` and `stereo_buffer_pairs_` maps in `akkado/src/codegen_stereo.cpp:19`). `TypedValue` carries only a single buffer index with no channel count. Stereo "works" only on the handful of opcodes that were manually wired into `codegen_stereo.cpp` to look up these maps. Everything else silently defaults to "treat input as mono, read left only."

### 2.3 Existing Infrastructure to Build On

| Component | Location | Reuse |
|-----------|----------|-------|
| Stereo opcodes (PAN, WIDTH, MS_ENCODE/DECODE, DELAY_PINGPONG) | `cedar/include/cedar/opcodes/stereo.hpp` | Unchanged; already adjacent-buffer convention |
| `stereo_outputs_`, `stereo_buffer_pairs_` codegen maps | `akkado/src/codegen_stereo.cpp:19-24` | Extend to all stereo-producing nodes |
| Adjacent-buffer allocation (right = left+1) | `akkado/src/codegen_stereo.cpp:211-215` | Reuse invariant for auto-lift outputs |
| Language functions (`stereo`, `left`, `right`, `pan`, `width`, `ms_encode`, `ms_decode`, `pingpong`) | `akkado/include/akkado/builtins.hpp:571-615` | Gain formal channel-type signatures |
| `out()` auto-expand (mono → duplicate, stereo → split) | `akkado/src/codegen.cpp` (around `func_name == "out"`) | Kept; now driven by signal type instead of buffer metadata |
| OUTPUT opcode accumulating into `ctx.output_left/right` | `cedar/include/cedar/opcodes/utility.hpp:39-58` | Unchanged |

---

## 3. Goals and Non-Goals

### 3.1 Goals

- **G1**: Every expression in Akkado has a statically known channel count (`Mono` or `Stereo`).
- **G2**: Applying a mono DSP function to a stereo signal automatically produces a stereo result, with per-channel independent state, without requiring users to duplicate the chain.
- **G3**: `stereo(x)` and `mono(x)` are the single canonical mono↔stereo conversions, documented and discoverable.
- **G4**: Compile-time errors are emitted for unresolvable mono/stereo mismatches, with precise source locations.
- **G5**: All existing programs continue to compile and produce identical audio output (zero user-visible regressions for mono-only code).

### 3.2 Non-Goals

- **Stereo-native VM refactor**: Making every opcode intrinsically stereo (mono = degraded stereo at the VM level) is a separate PRD ("Stereo-Native VM Opcodes") that this PRD depends on in principle but does not require in practice — the current adjacent-buffer-pair representation is sufficient here.
- **Multichannel beyond stereo**: No 5.1, 7.1, Ambisonics, or arbitrary channel counts. Output is always 2 channels.
- **Cross-channel effects beyond what exists**: No new stereo-specific opcodes (e.g. stereo chorus, stereo flanger) — these are follow-ups, not part of this PRD.
- **Method-call syntax**: `sig.stereo()` / `sig.mono()` are out of scope. Function-call syntax only.
- **Signal rate type unification**: This PRD addresses channel count, not audio-rate vs control-rate. The *semantics* of the `rate` field are unchanged; low-level bit layout elsewhere in `Instruction` (e.g. the new `flags` field from §6.1) may change.
- **Parameter stereo**: A parameter (slider, knob) is always a mono control signal. Stereo promotion happens only at DSP-signal boundaries.
- **Implementation of stereo-native generator builtins**: This PRD defines the *category* and *type-system slot* for stereo-native generators (§5.2), but does **not** implement any specific stereo-native generator. `in()` is owned by [`prd-audio-input.md`](prd-audio-input.md); stereo-aware `sample()` playback of multi-channel files, stereo synths, and stereo granular/wavetable are separate follow-ups. References to `in()` in this document (§4.3, §5.2) are illustrative only — consumers of the type system, not work this PRD performs.

---

## 4. Target Syntax and User Experience

### 4.1 Core Conversions

```akkado
// Mono → stereo: duplicate channels (L = R = input)
osc("saw", 220) |> stereo() |> out(@)

// Stereo → mono: sum-to-mono with 0.5 gain  (L+R) * 0.5
stereo_sig |> mono() |> filter_lp(@, 500, 0.7) |> out(@)

// Channel extraction (unchanged)
stereo_sig |> left(@)    // → Mono
stereo_sig |> right(@)   // → Mono

// Explicit stereo construction from two mono signals (unchanged)
stereo(osc("sin", 440), osc("sin", 442))   // → Stereo with distinct L/R
```

### 4.2 Auto-Lift

```akkado
// Mono effect on stereo input: auto-lifts to stereo effect
sig = osc("saw", 220) |> stereo()    // Stereo
sig |> filter_lp(@, 500, 0.7)        // Stereo (per-channel independent filter state)
    |> delay(@, 0.25, 0.5, 1.0, 0.5) // Stereo (per-channel independent delay line)
    |> out(@)                         // Explicit stereo signal: L and R routed
```

Equivalent explicit form users can still write:

```akkado
sig = osc("saw", 220)
left_out  = sig |> filter_lp(@, 500, 0.7) |> delay(@, 0.25, 0.5, 1.0, 0.5)
right_out = sig |> filter_lp(@, 500, 0.7) |> delay(@, 0.25, 0.5, 1.0, 0.5)
out(left_out, right_out)
```

Auto-lift is purely syntactic sugar for this explicit form — identical state handling, identical audio.

### 4.3 Common Patterns

```akkado
// Ping-pong stereo chain
osc("saw", 110)
  |> stereo()
  |> pingpong(@, 0.375, 0.6)
  |> out(@)

// Mono synth, stereo FX bus
synth = osc("saw", 220) |> filter_lp(@, 800, 0.5)    // Mono
synth |> stereo()
      |> width(@, 1.4)                                 // Stereo widen
      |> freeverb(@, 0.85, 0.5)                        // Auto-lifted stereo reverb
      |> out(@)

// Stereo source, sum to mono for sidechain
sc_env = stereo_drums |> mono() |> env_follower(@)     // Mono control

// Dry/wet mix with mixed channel types (see §5.3 rule 4)
dry = osc("saw", 220)                                  // Mono
wet = dry |> stereo() |> freeverb(@, 0.9, 0.5)         // Stereo
dry * 0.3 + wet * 0.7 |> out(@)                        // Mono + Stereo → Stereo
// Equivalent to broadcasting dry across both channels of wet.

// Illustrative: once a stereo-native generator exists (e.g. `in()` from
// prd-audio-input.md — implemented there, not here), its output flows straight
// into auto-lifted DSP with no `stereo()` wrapper needed.
in() |> filter_lp(@, 2000, 0.7) |> out(@)              // Stereo in → stereo filter → stereo out
```

### 4.4 Error Cases

```akkado
// Error: out(L, R) needs two mono signals; one of them is stereo
some_stereo |> out(@, osc("sin", 440))
// → E2xx at col N: 'out' expects Mono for argument 1, got Stereo.
//   Use `out(stereo_sig)` or `out(left(stereo_sig), right(stereo_sig))`.

// Error: left()/right() require stereo
mono_sig |> left(@)
// → E2xx at col N: 'left' expects Stereo, got Mono.
```

---

## 5. Type System Design

### 5.1 Channel Type Extension

Add a channel-count tag to `TypedValue`:

```cpp
enum class ChannelCount : std::uint8_t {
    Mono   = 0,
    Stereo = 1,
};

struct TypedValue {
    std::uint16_t buffer_id;       // existing — left buffer for Stereo
    std::uint16_t right_buffer_id; // NEW — only valid when channels == Stereo
    ChannelCount  channels;        // NEW
    SignalRate    rate;            // existing
    // ... existing fields ...
};
```

Invariant: when `channels == Stereo`, `right_buffer_id == buffer_id + 1` (adjacency invariant already enforced by `BufferAllocator`).

### 5.2 Builtin Signatures

Each builtin in `akkado/include/akkado/builtins.hpp` gains channel-type info per argument and return:

```cpp
struct BuiltinSignature {
    std::vector<ChannelCount> input_channels;  // Mono per arg, or Stereo for stereo inputs
    ChannelCount              output_channels; // or AutoLift sentinel
    bool                      auto_lift;       // true ⇒ Stereo in → Stereo out per-channel
};
```

Classification of existing builtins:

| Category | auto_lift | output_channels | Examples |
|----------|-----------|-----------------|----------|
| Mono generators | false | Mono | `osc`, `noise`, `pulse`, `pat` |
| **Stereo-native generators** | false | Stereo | *Category slot only — no concrete builtin in this category is implemented by this PRD.* Expected first consumer: `in()` from [`prd-audio-input.md`](prd-audio-input.md). Other future candidates: stereo-aware `sample()` for multi-channel files, stereo synths, stereo granular/wavetable. Such builtins emit adjacent L/R buffers directly and do **not** require a `stereo()` wrapper. |
| Mono-in, mono-out DSP | **true** | follows input | `filter_lp`, `filter_hp`, `svf`, `delay`, `comb`, `freeverb`, `dattorro`, `fdn`, `bitcrush`, `fold`, `saturate`, ADSR, etc. |
| Stereo constructors | false | Stereo | `stereo(mono)`, `stereo(L, R)` |
| Mono → Stereo (panning) | false | Stereo | `pan(mono, pos)` (existing equal-power pan law) |
| Stereo → Stereo (balance) | false | Stereo | `pan(stereo, pos)` (new overload — stereo balance / pan law, see §5.5) |
| Stereo-in, stereo-out | false | Stereo | `width`, `pingpong`, `ms_encode`, `ms_decode` |
| Stereo → mono | false | Mono | `mono`, `left`, `right` |
| Sink | false | — | `out(m) → ()` (Mono, duplicated), `out(s) → ()` (Stereo, split), `out(l, r) → ()` (both Mono, explicit) |

A builtin declares itself stereo-native simply by setting `output_channels = Stereo` in its signature — no special codegen path is needed beyond the adjacent-buffer allocation the compiler already performs for `stereo()`. Downstream auto-lift treats a stereo-native generator's output identically to a `stereo()`-wrapped mono signal.

### 5.3 Type-Checking Rules

Argument classification is driven by the existing `BuiltinInfo.param_types[i]` field (see `akkado/include/akkado/builtins.hpp`, `enum class ParamValueType`). An argument is a **signal argument** iff `param_types[i] == ParamValueType::Signal`; anything else (`Scalar`, `Pattern`, `String`, etc.) is a **non-signal argument**. Only signal arguments participate in channel-type inference and auto-lift. Non-signal arguments (including control-rate scalars like frequency and resonance) are read as mono control and broadcast unchanged to both channels in the stereo path.

1. For a call `f(a1, ..., aN)` with builtin `f`:
   - If `f.auto_lift` is false: each signal `ai.channels` must equal `f.input_channels[i]`. Mismatch → compile error `E2XX`.
   - If `f.auto_lift` is true and all signal arguments are Mono: emit opcode as today, result is Mono.
   - If `f.auto_lift` is true and **at least one signal argument** is Stereo: all signal arguments must be coercible to Stereo (Mono is auto-promoted via duplication — this costs 0 extra opcodes, just a dual read). Non-signal arguments are shared between L and R passes. Emit opcode with `STEREO_INPUT` flag; result is Stereo.
2. `out()` is special-cased:
   - `out(s)` with `s: Stereo` → split into left/right
   - `out(m)` with `m: Mono` → duplicate to both channels
   - `out(l, r)` with both Mono → explicit left/right
   - Any other combination → compile error
3. Pipe (`|>`): `a |> f(@, args)` — `@` takes the channel type of `a`; normal call rules then apply.
4. **Binary operators** (`+`, `-`, `*`, `/`) on signal operands follow auto-lift semantics:
   - `Mono op Mono` → Mono (as today)
   - `Stereo op Stereo` → Stereo, per-channel independent (L op L, R op R)
   - `Mono op Stereo` (and symmetric) → Stereo: the Mono operand is broadcast to both channels (zero-cost dual read of the same buffer). Result is Stereo. This is the canonical dry/wet mixing case: `dry + stereo_wet`.
   - Multiplication by a scalar control signal (e.g. an envelope or LFO on a stereo bus) falls under this rule — the scalar is a mono signal, shared L/R.

### 5.4 State ID Derivation for Auto-Lift

When auto-lifting a stateful mono opcode to stereo, the compiler must generate **two distinct state IDs** (one per channel) so left and right have independent filter memory, oscillator phase, delay line, etc. Rule:

```
state_id_L = fnv1a(semantic_path + "/L")
state_id_R = fnv1a(semantic_path + "/R")
```

The mono, non-lifted case keeps the current derivation (`fnv1a(semantic_path)`) for backward hot-swap compatibility — existing programs with mono opcodes keep their current state IDs and preserve live-coding state across recompiles.

**Stateless auto-lifted opcodes** (e.g. `fold`, `saturate`, simple gain/clip ops where `requires_state == false` in `BuiltinInfo`) have no `state_id` consumer. The suffix rule is a no-op for them; codegen may set both `state_id_L` and `state_id_R` to `0` (or derive them anyway — the VM ignores `state_id` for stateless opcodes). No special case is needed in codegen; the rule just collapses to "doesn't matter" for this class.

### 5.5 Stereo `pan()` Overload

`pan(stereo_sig, pos)` is a new overload that panning-balances an already-stereo signal. Using standard equal-power pan law, with pan position `p ∈ [-1, +1]` and angle `θ = (p + 1) * π/4`:

```
L_out = L_in * cos(θ)
R_out = R_in * sin(θ)
```

At `p = -1` → `L_out = L_in`, `R_out = 0` (hard left).
At `p =  0` → both channels scaled by `cos(π/4) ≈ 0.707` (centre, -3 dB equal-power).
At `p = +1` → `L_out = 0`, `R_out = R_in` (hard right).

This is the standard DAW behaviour for panning a stereo track — it's equal-power balance, not a re-pan of each channel independently. A new dedicated opcode `PAN_STEREO` (or the existing `PAN` opcode widened to accept stereo) is added in Cedar; which of the two is an implementation detail decided in Phase 4.

Signatures for `pan`:
- `pan(Mono, Mono) → Stereo` — existing equal-power mono-to-stereo pan
- `pan(Stereo, Mono) → Stereo` — new: equal-power stereo balance

Both share the user-facing name `pan`; the compiler dispatches based on the channel type of the first argument.

---

## 6. VM Support: `STEREO_INPUT` Flag

### 6.1 Instruction Format Change

The current `Instruction` struct (`cedar/include/cedar/vm/instruction.hpp:195-229`) is 20 bytes. Its real memory layout, accounting for `alignas(4)` and the 4-byte alignment of `state_id`, is:

| Offset | Size | Field |
|--------|------|-------|
| 0 | 1 | `opcode` |
| 1 | 1 | `rate` |
| 2 | 2 | `out_buffer` |
| 4 | 2 | `inputs[0]` |
| 6 | 2 | `inputs[1]` |
| 8 | 2 | `inputs[2]` |
| 10 | 2 | `inputs[3]` |
| 12 | 2 | `inputs[4]` |
| **14** | **2** | **implicit padding (free)** |
| 16 | 4 | `state_id` |

There are already **2 free bytes of padding at offset 14–15** that exist solely to align `state_id`. These can be claimed for a new `flags` field without increasing struct size.

**Decision (resolves OQ1)**: add a dedicated `std::uint16_t flags` field in the existing padding slot. `STEREO_INPUT` is one bit; the remaining 15 bits are reserved for future per-instruction attributes (control-rate hints, SIMD hints, etc.).

**New layout:**

| Offset | Size | Field |
|--------|------|-------|
| 0 | 1 | `opcode` |
| 1 | 1 | `rate` (semantics unchanged) |
| 2 | 2 | `out_buffer` |
| 4 | 2 | `inputs[0..4]` (total 10 bytes) |
| 14 | 2 | **`flags` (new)** — bit 0: `STEREO_INPUT` |
| 16 | 4 | `state_id` |

`static_assert(sizeof(Instruction) == 20)` still holds. Cache impact: nil.

**Rejected alternatives:**
- **Steal bits from `rate`** (0 bytes but lossy): `rate` is already overloaded per-opcode — FREEVERB packs wet/dry mix into it, DELAY packs time-unit, COMB packs damping. Stealing bits reduces per-opcode packing headroom and couples an unrelated concern into the field.
- **Paired opcode entries** (`FILTER_LP_STEREO` etc.): doubles opcode-table entries for every auto-liftable op. §6.2's design goal is explicitly "zero opcode-table cost" — this would violate it.

The earlier framing that option (b) "grows the struct to 24 bytes" was incorrect; that assumed no pre-existing padding.

### 6.2 Dispatch Semantics

In the VM dispatch loop (`cedar/src/vm/vm.cpp`):

```cpp
if (inst.flags & STEREO_INPUT) {
    // Run opcode once for left channel
    exec_op(inst_with_left_bufs_and_state_L);
    // Run opcode once for right channel
    exec_op(inst_with_right_bufs_and_state_R);
} else {
    exec_op(inst);
}
```

The exact mechanism (temporary local `Instruction` copy vs dispatch-loop branch) is an implementation detail; the guarantee is: every mono opcode automatically gains a stereo variant at zero opcode-table cost and with independent per-channel state.

### 6.3 Constraints

- Works only for opcodes that read one primary signal input and produce one output (the auto-lift set in §5.2). Opcodes with intrinsically multi-signal inputs (e.g. a sidechain compressor where the detector is stereo and the signal is mono) need explicit signatures and are not auto-lifted.
- Output buffer pair must be allocated adjacent (already enforced by `BufferAllocator`).
- Parameter (scalar) inputs are shared between L and R passes — parameters don't themselves get stereo-lifted.

---

## 7. Impact Assessment

| Component | Status | Notes |
|-----------|--------|-------|
| Existing stereo opcodes (PAN, WIDTH, MS_ENCODE, MS_DECODE, DELAY_PINGPONG) | **Stays** | Kept as-is; type signatures just formalise what they already do |
| Existing language functions `stereo`, `left`, `right`, `pan`, `width`, etc. | **Stays** | Same signatures, now with formal channel types |
| `out()` auto-expansion | **Stays** | Behaviour unchanged; implementation now driven by `channels` field |
| BufferAllocator adjacency invariant | **Stays** | Relied upon for auto-lift output pairs |
| `TypedValue` | **Modified** | Add `right_buffer_id` and `channels` fields |
| Builtin signature table | **Modified** | Add `input_channels`, `output_channels`, `auto_lift` to every builtin entry |
| Codegen type checker | **Modified** | Enforce §5.3 rules; emit errors with source locations |
| Codegen for auto-lift | **New** | Emit `STEREO_INPUT` flag + allocate adjacent output pair + derive dual state IDs |
| `Instruction` flags / `rate` field | **Modified** | Carry `STEREO_INPUT` bit |
| VM dispatch loop | **Modified** | Branch on `STEREO_INPUT` and dispatch opcode twice |
| `mono()` language function | **New** | Sum-to-mono with 0.5 gain; stereo-in mono-out builtin |
| All mono DSP opcodes (filters, delays, reverbs, etc.) | **Stays** | Untouched — auto-lift is transparent to them |
| Existing mono programs | **Stays** | Produce identical bytecode and audio (state IDs unchanged for mono path) |
| Hot-swap state preservation | **Stays** | Mono state IDs preserved; new `/L` and `/R` IDs for stereo-lifted states |

---

## 8. File-Level Changes

| File | Change |
|------|--------|
| `akkado/include/akkado/typed_value.hpp` (struct at line 63) | Add `ChannelCount` enum, extend `TypedValue` with `right_buffer_id` and `channels` |
| `akkado/include/akkado/builtins.hpp` | Extend each builtin entry with `input_channels`, `output_channels`, `auto_lift` |
| `akkado/src/codegen.cpp` | Type-check rules (§5.3); route auto-lift for stereo-in mono ops; populate `channels` on all produced `TypedValue`s |
| `akkado/src/codegen_stereo.cpp` | Simplified: reads `TypedValue.channels` instead of external maps; extended to cover auto-lift outputs |
| `akkado/src/codegen.cpp` (state ID derivation) | Append `/L` and `/R` suffixes for auto-lifted stereo opcodes |
| `cedar/include/cedar/vm/instruction.hpp` | Add `STEREO_INPUT` flag (bit or new `flags` field); update struct comment |
| `cedar/src/vm/vm.cpp` | Dispatch-loop branch: when `STEREO_INPUT` is set, run opcode for left then right with per-channel buffers/state |
| `akkado/src/builtins.cpp` (new builtin) | Add `mono()` builtin — emits opcode that writes `out[i] = (L[i] + R[i]) * 0.5` |
| `cedar/include/cedar/opcodes/stereo.hpp` | Add `MONO_DOWNMIX` opcode (stereo in, mono out); add `PAN_STEREO` opcode or widen existing `PAN` to accept stereo input per §5.5 |
| `cedar/include/cedar/vm/instruction.hpp` (opcode enum) | Add `MONO_DOWNMIX` entry; add `PAN_STEREO` entry if kept as a separate opcode |
| `akkado/src/codegen.cpp` (builtin `pan` dispatch) | Route `pan(Mono, _)` → existing `PAN`; route `pan(Stereo, _)` → `PAN_STEREO` overload |
| `web/scripts/build-opcodes.ts` (or equivalent codegen script) | Picks up new opcode automatically once enum is updated and `bun run build:opcodes` is run |
| `akkado/tests/test_codegen.cpp` | New tests: auto-lift of filters/delays/reverbs, type mismatch errors, `mono()` downmix, state-ID L/R separation |
| `akkado/tests/test_types.cpp` (new) | Unit tests for channel-type inference rules |
| `experiments/test_op_mono_downmix.py` (new) | Python experiment verifying sum-to-mono with 0.5 gain |
| `docs/cedar-architecture.md` | Short update describing `STEREO_INPUT` flag in instruction format section |
| `web/static/docs/reference/builtins/stereo.md` (new or updated) | User-facing docs for `stereo`, `mono`, `left`, `right`, auto-lift behaviour, worked examples |
| `web/static/docs/concepts/signals.md` (new) | Concept page explaining Mono vs Stereo signal types, conversions, auto-lift |
| `web/scripts/build-docs.ts` → `src/lib/docs/lookup-index.ts` | Regenerated after doc updates via `bun run build:docs` |

Files that explicitly **do not change**:
- Any mono DSP opcode implementation (filters, delays, reverbs, distortion)
- `BufferAllocator` (adjacency invariant already correct)
- `OUTPUT` opcode (`utility.hpp:39-58`)
- `ExecutionContext` (`context.hpp:15-88`)
- Web WASM bindings (`web/wasm/nkido_wasm.cpp`) — stereo output already exposed

---

## 9. Implementation Phases

### Phase 1 — Type System Foundation
**Goal**: Every expression knows its channel count; no behaviour change yet.

- Add `ChannelCount` enum and extend `TypedValue`
- Annotate every builtin with channel-type signature
- Implement type checker; emit errors for existing obvious mismatches
- Replace `stereo_outputs_` / `stereo_buffer_pairs_` lookups with `TypedValue.channels` reads in `codegen_stereo.cpp`

Verification:
- All existing Akkado tests still pass byte-identical bytecode
- `bun run check` and `cedar_tests` / `akkado_tests` green
- New failing test case: `mono_sig |> left(@)` emits a compile error

### Phase 2 — `mono()` Builtin
**Goal**: Canonical stereo-to-mono downmix.

- Add `MONO_DOWNMIX` opcode in Cedar (`stereo.hpp`)
- Add `mono()` builtin in Akkado
- Run `bun run build:opcodes` to regenerate metadata
- Add Python experiment `test_op_mono_downmix.py` verifying `(L + R) * 0.5`

Verification:
- Python experiment passes; WAV file sounds correct
- `stereo_sig |> mono() |> left(@)` compiles? (It shouldn't — left requires stereo. Test the error.)
- `stereo(osc("sin", 440), osc("sin", 440)) |> mono()` equals original sine at unity gain

### Phase 3 — VM `STEREO_INPUT` Flag
**Goal**: VM can run any opcode twice for stereo input.

- Add flag bit or `flags` field to `Instruction`
- Implement dispatch-loop branch in `vm.cpp`
- Unit test: manually construct a bytecode program with `STEREO_INPUT` flag set on `FILTER_LP`, verify L/R produce independent results
- Hot-swap verification: mono programs keep identical state IDs (no state drop on recompile)

### Phase 4 — Auto-Lift Codegen
**Goal**: Users write stereo chains without duplication.

- In `codegen.cpp`, when an auto-lift builtin receives a stereo input:
  - Allocate adjacent output buffer pair
  - Emit single instruction with `STEREO_INPUT` flag
  - Derive `state_id_L` and `state_id_R` from path + `/L` / `/R`
  - Produce `TypedValue` with `channels = Stereo`
- Mono input path unchanged
- Add `test_codegen.cpp` cases: `stereo() |> filter_lp`, `stereo() |> freeverb`, `stereo() |> delay`, nested chains

Verification:
- Hand-written explicit-dual-chain program produces same audio as auto-lifted form (within floating-point equality)
- Hot-swap: swap mono version of chain with stereo version — no clicks beyond normal crossfade

### Phase 5 — Documentation & Discovery
**Goal**: Users can find and learn stereo features.

- Write `web/static/docs/reference/builtins/stereo.md` and `docs/concepts/signals.md`
- Update `web/static/docs/reference/builtins/utility.md` (out section)
- Regenerate doc index: `cd web && bun run build:docs`
- Add worked examples to F1 help for `stereo`, `mono`, `pan`, `width`

Verification:
- F1 on `mono` / `stereo` returns the new doc pages
- Example code in docs compiles and produces expected audio

---

## 10. Edge Cases

### 10.1 `pan()` on Stereo Input
`stereo_sig |> pan(@, 0.3)` dispatches to the `pan(Stereo, Mono) → Stereo` overload (§5.5): equal-power stereo balance, standard DAW behaviour. Not auto-lifted — balance is semantically different from applying mono-pan to each channel independently, so it gets a dedicated signature rather than flowing through the auto-lift path.

### 10.2 `mono()` on Mono Input
Compile error: `mono()` requires `Stereo`. Users shouldn't call it defensively. Rationale: silent no-op hides bugs (e.g. user thought something upstream made the signal stereo).

### 10.3 `stereo()` on Stereo Input
Compile error with friendly hint: "value is already stereo; `stereo()` takes a mono signal or two mono signals."

### 10.4 Mixed Pipe with Scalar Parameters
`stereo_sig |> filter_lp(@, freq, 0.7)` where `freq = param("cutoff", 500, 100, 2000)` — `freq` is a mono control signal, shared between L and R passes. Both channels see the same cutoff. This is the expected behaviour; independent per-channel parameters require explicit splitting.

### 10.5 Chord Expansion + Stereo
`C4' |> filter_lp(@, 800, 0.5)` — chord expansion produces a signal array (mono per voice). Stereo does not auto-multiply with chord expansion. If user wants stereo chord synth: `C4' |> filter_lp(@, 800, 0.5) |> stereo() |> ...`. Stereo lifting happens AFTER chord expansion collapses to a summed signal.

### 10.6 Hot-Swap Across Mono/Stereo Structural Changes
Swapping `osc |> filter_lp` → `osc |> stereo() |> filter_lp` is a structural change: old has state ID `fnv1a("filter_lp")`, new has `/L` and `/R` suffixes. State does not transfer. This is correct — the DSP topology changed. Crossfade still applies per existing hot-swap machinery.

### 10.7 Pattern Events and Stereo
Pattern events (`pat`, `seq`, `timeline`) are always mono control signals; they are not auto-lifted. A stereo synth driven by a mono pattern is the normal case.

### 10.8 `out()` Called With Stereo Twice
`out(stereo_a)` then `out(stereo_b)` — both accumulate into `output_left/right`, so the sum mixes down correctly. No change from today.

### 10.9 `left()` / `right()` Followed by Re-Stereo
`stereo_sig |> left(@) |> stereo()` — valid; produces stereo with both channels equal to the original L. Common pattern for processing then re-widening.

### 10.10 Auto-Lift Interaction With `as` Pipe Binding
`stereo_sig |> filter_lp(@, 500, 0.7) as s |> out(s)` — `s` has type `Stereo`. `out(s)` uses stereo path. No special handling needed; type flows through the binding.

### 10.11 Mixed-Channel Arithmetic
`mono_sig + stereo_sig` (and symmetric `*`, `-`, `/`) is valid: the mono operand is broadcast to both channels at zero cost (dual read of the same buffer), result is `Stereo`. See §5.3 rule 4. Canonical use: `dry * 0.3 + stereo_wet * 0.7` for reverb wet/dry mixing where the dry path is mono and the wet path is stereo.

### 10.12 User-Defined Functions and Channel Type
`my_fx = fn(sig) -> sig |> filter_lp(@, 500, 0.7) |> delay(@, 0.2, 0.5)` — the intended semantics is that `my_fx(mono_in)` produces `Mono` and `my_fx(stereo_in)` produces `Stereo`, with channel-type polymorphism inherited from the body's auto-lift behaviour. The exact inference rule (e.g. parametric channel-type variables, monomorphisation-per-callsite, or something simpler) is an **open question (OQ5)** to resolve jointly with [`prd-advanced-functions`](prd-advanced-functions.md). Until OQ5 is resolved, implementations may restrict user-defined functions to a single channel type per definition (error at call site on mismatch).

---

## 11. Testing and Verification

### 11.1 Unit Tests (`akkado_tests`)

- **Type inference**: every builtin produces correct output channel count; mismatches produce errors with correct source locations
- **Auto-lift emission**: `stereo() |> filter_lp` produces a single `FILTER_LP` instruction with `STEREO_INPUT` flag set and adjacent output buffers
- **State ID derivation**: `state_id_L != state_id_R`, and mono-path `state_id == fnv1a(path)` unchanged
- **Error cases**: enumerate §10 edge cases and assert expected compile errors

### 11.2 VM Tests (`cedar_tests`)

- **Dispatch branch**: hand-constructed bytecode with `STEREO_INPUT` flag on `FILTER_LP` produces independent L/R processing
- **Mono path unchanged**: without the flag, audio output is bit-identical to pre-PRD builds on regression suite
- **Stereo path correctness**: `FILTER_LP` with `STEREO_INPUT` matches two independently-constructed mono `FILTER_LP` calls within `< 1e-6` per sample

### 11.3 Python Experiments

- `test_op_mono_downmix.py` — new; verifies `(L + R) * 0.5` with sines, DC, stereo noise
- Update existing filter/delay/reverb experiments to include a stereo-lift variant checking channel independence

### 11.4 End-to-End Manual Tests

Run in the web app with audible listening for each case:

```akkado
// E2E 1: Stereo reverb
osc("saw", 110) |> stereo() |> freeverb(@, 0.9, 0.5) |> out(@)
// Expect: full stereo reverb tail, clearly wider than mono version

// E2E 2: Auto-lifted stereo delay chain
drums = s"bd sn cp hh" |> sample(@)
drums |> stereo() |> delay(@, 0.3, 0.5, 1.0, 0.5) |> out(@)
// Expect: stereo delays with independent per-channel decay, not mono-duplicated

// E2E 3: Downmix for sidechain
src = osc("saw", 220) |> stereo() |> filter_lp(@, 800, 0.5)
env = src |> mono() |> env_follower(@)
src |> @ * env |> out(@)
// Expect: compiles; sidechain ducks the stereo signal via mono-derived env

// E2E 4: Error messages
stereo_sig |> out(@, osc("sin", 440))
// Expect: E2XX with clear message pointing to argument 1
```

### 11.5 Hot-Swap Tests

- Live-coded session: start with mono chain, save state, edit to add `|> stereo()` at a point in the chain, hot-swap, confirm no click beyond normal crossfade and that downstream per-channel state is fresh rather than inheriting mono state incorrectly.

### 11.6 Build Commands

```bash
# Full build
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Run tests
./build/cedar/tests/cedar_tests
./build/akkado/tests/akkado_tests "[stereo]"
./build/akkado/tests/akkado_tests "[types]"

# Regenerate opcode + doc metadata
cd web && bun run build:opcodes && bun run build:docs

# Python experiments
cd experiments && uv run python test_op_mono_downmix.py
```

---

## 12. Open Questions

- **OQ1**: ~~Instruction flag encoding~~ — **resolved**: add `std::uint16_t flags` in the existing 2-byte alignment padding at offset 14. Struct stays 20 bytes. See §6.1 for layout and rationale.
- **OQ2**: ~~`mono()` gain choice~~ — **resolved**: `0.5` (standard sum-to-mono).
- **OQ3**: Should `out()` emit a warning when receiving mono and silently duplicating? Default: no — current behaviour is conventional and expected. Reconsider if users report confusion.
- **OQ4**: Should the companion "Stereo-Native VM Opcodes" PRD land before or after this one? This PRD does not require it; lifting to that architecture later is a codegen-only change (no language-surface impact). Default: this PRD ships first, companion PRD is an independent follow-up optimisation.
- **OQ5**: Channel-type polymorphism through user-defined functions (see §10.12). The intended behaviour is that a user function inherits polymorphism from its body (mono-in ⇒ mono-out; stereo-in ⇒ stereo-out with per-channel independent state). The exact inference rule is deferred to joint resolution with [`prd-advanced-functions`](prd-advanced-functions.md). Until resolved, implementations may restrict user-defined functions to a single channel type per definition.

---

## 13. Related Work

- **Companion PRD (COMPLETE)**: [`prd-stereo-native-opcodes.md`](prd-stereo-native-opcodes.md) — "Stereo-Native VM Opcodes" makes every audio opcode handle a stereo pair in one VM dispatch, treating mono as a widen-on-entry case and retiring the auto-lift mechanism entirely. The language-level type system described here is unchanged by that refactor; the internal codegen path and VM dispatch shifted, and the E181–E185 channel-mismatch errors were relaxed to W181–W185 warnings.
- **Dependent PRD**: [`prd-audio-input.md`](prd-audio-input.md) — adds `in()` as a stereo-native audio source. That PRD's signature declaration (`output_channels = Stereo`) relies on the type system and stereo-native generator category defined here (§5.2).
- **Existing work referenced**:
  - `prd-crossfade-audio-fixes.md` — crossfade machinery, relevant for hot-swap edge cases in §10.6
  - `cedar/include/cedar/opcodes/stereo.hpp` — existing stereo opcode implementations we keep
  - `akkado/src/codegen_stereo.cpp` — existing codegen-side stereo tracking, simplified by this PRD
