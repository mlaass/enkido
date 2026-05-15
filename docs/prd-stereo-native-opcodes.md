> **Status: COMPLETE** — All phases landed (Phases 0–4 by 2026-05-13; Phase 5
> on 2026-05-14). Every audio-signal opcode is stereo-native. Phase 5 retired
> the auto-lift mechanism: the last `auto_lift = true` holdouts (`slew` and the
> `EDGE_OP` family — `sah`/`gateup`/`gatedown`/`counter`) are now stereo-native,
> the `STEREO_INPUT`-only auto-lift dispatch branch and `STEREO_STATE_XOR_R`
> constant are deleted from `vm.cpp`/`instruction.hpp`, and the `auto_lift` field
> is gone from `BuiltinInfo`. The `STEREO_INPUT` flag itself is kept — it is now
> the `11` truth-table bit (stereo-native opcode with stereo primary input);
> only the dispatch branch was removed. The user-tunable `lfo_phase` parameter
> shipped in Phase 4 via ExtendedParams. Successor to `prd-stereo-support.md`
> (COMPLETE 2026-04-21); implements the never-written "Stereo-Native VM Opcodes"
> companion referenced in §13 of that PRD.

# PRD: Stereo-Native DSP Opcodes

## 1. Executive Summary

`prd-stereo-support.md` (now COMPLETE) gave Akkado a real type system for stereo signals (`ChannelCount::Mono | Stereo` on `TypedValue`) plus an auto-lift mechanism that runs mono opcodes twice via the `STEREO_INPUT` instruction flag, producing per-channel independent state. That ships, works, and is the right answer for **channel-independent DSP** (filters, distortion, EQ, plain delays).

For **spatializing DSP** (reverbs, chorus, phaser, flanger) auto-lift is the worst of both worlds. `freeverb(stereo_in)` is "double-mono": two uncorrelated mono reverbs, no cross-coupling, 2× CPU. `dattorro` (`cedar/include/cedar/opcodes/reverbs.hpp:181-253`) is the egregious case — its figure-8 tank topology already cross-couples `tank_feedback[0]` (L) and `tank_feedback[1]` (R), and line 253 collapses them with `out[i] = (L + R) * 0.5f`. Auto-lift then runs that already-stereo algorithm twice to fake stereo back. Mono input never widens, so users have to write `mono |> stereo() |> freeverb` just to get a tail at all — and what they get is double-mono.

This PRD makes every audio-signal opcode stereo-native at the VM level, and resolves the auto-lift design tension by collapsing it onto a single mechanism — the `STEREO_OUTPUT` instruction flag, mirror of `STEREO_INPUT`. Each audio opcode opts in by setting the flag (eventually all of them); the allocator reserves an adjacent buffer pair (`R = L + 1`); the opcode body processes L and R inside one call with shared state. Mono input auto-escalates (silent duplication) so existing programs keep working without `stereo()` wrappers. `STEREO_INPUT` and the auto-lift dispatch retire once every audio opcode is migrated.

Key design decisions made in question rounds:
- **Mechanism**: `STEREO_OUTPUT` flag, mono `float[BLOCK_SIZE]` buffers, adjacency convention (`R = L + 1`). Same pattern as the existing `STEREO_INPUT` flag — no new buffer types, lightest VM diff.
- **Auto-escalation, not errors**: Mono entering a stereo-native opcode silently duplicates to L=R. **All** current E181–E185 mono/stereo mismatch errors are relaxed to **warnings** (W181–W185) — including structurally unusual cases like `left(mono)`, `right(mono)`, `mono(mono)`, `stereo(stereo)`. The compiler logs a warning so users get the diagnostic ("you called `left()` on a mono signal — returning the input as-is"), but compilation succeeds and the program runs.
- **One state struct per opcode**: stateful opcodes carry per-channel fields internally (`z1[2], z2[2]` etc.) rather than maintaining two state IDs per opcode call. State ID derivation reverts to `fnv1a(semantic_path)` — no `/L`, `/R`, or XOR suffixes.
- **Generators stay Mono**: `osc()`, `noise()`, `pulse()`, mono-file `sample()` continue to return `Mono`. Escalation happens at the boundary into a stereo-native opcode.
- **Existing language builtins unchanged**: `mono()`, `stereo()`, `left()`, `right()`, `pan()` (both overloads), `width()`, `ms_encode/decode()`, `pingpong()` keep their signatures and codegen. `stereo()` becomes mostly redundant (auto-escalation does it implicitly) but stays for explicit intent.
- **Phased big-bang migration**: every audio opcode converted, but landed in phased commits per category (reverbs → spatializing FX → channel-independent DSP) for review-friendly diffs.
- **Bit-identity gate**: programs that compile to mono-only bytecode after this change must produce audio identical-within-`1e-6` to today's output, every phase.

---

## 2. Problem Statement

### 2.1 Current Behaviour

| Scenario | Expected | Actual today |
|----------|----------|--------------|
| `osc("saw", 110) \|> dattorro(@, 0.85, 30) \|> out(@)` | Stereo reverb tail (mono in widens) | Mono reverb, no width — Dattorro's L/R tanks are summed and discarded |
| `stereo_sig \|> freeverb(@, 0.9, 0.5)` | Real stereo reverb with cross-coupling | Two independent mono Freeverbs (auto-lift), 2× CPU, no inter-channel coupling |
| `mono_sig \|> chorus(@, 0.5, 0.5)` | Stereo chorus via offset L/R LFO | Mono chorus; user must `\|> stereo()` first to even get auto-lifted double-mono |
| `mono_sig \|> filter_lp(@, ...) \|> out(stereo_wet, mono_dry)` | Sensible compile or auto-mix | E185: stereo and mono mixed on out — defensive `stereo()` wrap required |

### 2.2 Root Cause

Two separate issues:

1. **Algorithmic**: spatializing FX (reverbs especially) carry stereo internal state but emit mono. The free stereo information is dropped at the output.
2. **Architectural**: auto-lift is the only mechanism the VM has for "make this stereo." Auto-lift duplicates the *whole algorithm* — perfect for filters (per-channel-independent processing is correct), wrong for reverbs (cross-coupling is the point).

The original PRD §13 acknowledged this gap and pointed at a never-written companion. This PRD is that companion.

### 2.3 Existing Infrastructure to Build On

| Component | Location | Reuse |
|-----------|----------|-------|
| `ChannelCount` type, `TypedValue.channels`, `TypedValue.right_buffer` | `akkado/include/akkado/typed_value.hpp:26-115` | Unchanged; channel count flows as today |
| `STEREO_INPUT` instruction flag, dispatch loop branch | `cedar/include/cedar/vm/instruction.hpp:209-221`, `cedar/src/vm/vm.cpp:467-480` | Mirror the flag pattern (`STEREO_OUTPUT`); retire the dispatch branch in Phase 5 |
| Adjacent-buffer allocation (`R = L + 1`) | `akkado/src/codegen_stereo.cpp:211-215` | Reused for stereo-native output pairs |
| Existing stereo opcodes (`PAN`, `WIDTH`, `MS_ENCODE/DECODE`, `DELAY_PINGPONG`, `MONO_DOWNMIX`, `PAN_STEREO`) | `cedar/include/cedar/opcodes/stereo.hpp` | Re-validated under the new flag model; no algorithmic change |
| Reverb internals with stereo-aware state (Dattorro tanks, FDN Hadamard) | `cedar/include/cedar/opcodes/reverbs.hpp` | Algorithm preserved; output split L/R instead of summed to mono |
| `BuiltinSignature` declarative catalog | `akkado/include/akkado/builtins.hpp:83-99` | Adds a `stereo_native: bool` (or extends `output_channels`) and rolls `auto_lift` toward retirement |

---

## 3. Goals and Non-Goals

### 3.1 Goals

- **G1**: Every audio-signal opcode in Cedar can write a stereo output pair natively, with a single instruction emit and one state struct per call.
- **G2**: Spatializing FX (reverbs, chorus, phaser, flanger) widen mono inputs into stereo outputs — `osc(…) |> dattorro(@, …) |> out(@)` produces a stereo tail with no `stereo()` wrapper.
- **G3**: Auto-escalation replaces stereo/mono mismatch errors at audio-signal slots: when a mono signal flows into a slot that's now stereo-native, codegen silently duplicates L=R. All channel-mismatch error codes E181–E185 become warnings W181–W185 (logged but non-blocking). E186 (non-signal type mismatch) remains an error.
- **G4**: Channel-independent DSP (filters, distortion, EQ, plain delays) processes both channels inside one opcode call with per-channel state in a single state struct, retiring the `STEREO_INPUT`/auto-lift dispatch.
- **G5**: Bit-identity for legacy mono code: every Akkado program that compiled to mono-only bytecode under `prd-stereo-support` produces audio identical-within-`1e-6` per sample after this PRD.
- **G6**: Phased rollout: each phase's commit is independently buildable, testable, and runnable. Auto-lift coexists with stereo-native opcodes during Phases 2–4; only Phase 5 deletes the auto-lift dispatch.

### 3.2 Non-Goals

- **Multichannel beyond stereo**: no 5.1, 7.1, ambisonics, arbitrary channel counts.
- **Stereo control signals**: scalars (frequencies, cutoffs, env outputs, LFOs, params) stay mono. Control buffers remain `float[BLOCK_SIZE]`.
- **New buffer pool type**: this PRD does not introduce a `StereoBuffer` struct. Stereo signals stay as adjacent mono pairs (`R = L + 1`). The cleaner-but-bigger refactor (`AudioBufferPool` of stereo pairs, `ControlBufferPool` of mono) is explicitly deferred — possibly indefinitely if the flag-driven model holds up.
- **Removing `mono()` / `stereo()` builtins**: they stay. `stereo()` is mostly redundant with auto-escalation but remains for explicit-intent code.
- **User-defined function channel polymorphism**: still deferred to joint resolution with `prd-advanced-functions.md` (see prd-stereo-support §10.12, OQ5).
- **Retroactively restating the prd-stereo-support type system**: that PRD's `ChannelCount` model, `out()` semantics, broadcast arithmetic rules, and language-level builtins are foundation, not subject to redesign here.
- **WASM bytecode major-version bump**: the `flags` field already exists post-prd-stereo-support; adding a second bit is a no-op for layout. If bytecode versioning is currently enforced, the version bumps minor-style; full ABI break is unnecessary.

---

## 4. Target Syntax and User Experience

### 4.1 Reverbs Become Stereo

```akkado
// Mono in → stereo reverb tail (no `stereo()` wrapper needed)
osc("saw", 110) |> dattorro(@, 0.85, 30) |> out(@)
// Today: mono tail. After: stereo tail with figure-8 cross-coupling.

// Stereo in → cross-coupled stereo reverb (not double-mono)
in() |> dattorro(@, 0.85, 30) |> out(@)
// Today: two independent mono Dattorros via auto-lift, 2× CPU, no L↔R coupling.
// After: one Dattorro instance, L feeds R-tank and vice-versa, stereo native.

// Reverb wet/dry mix is unchanged
dry = osc("saw", 220)
wet = dry |> freeverb(@, 0.9, 0.5)        // Mono dry → stereo wet (auto-escalates)
dry * 0.3 + wet * 0.7 |> out(@)            // Mono+Stereo broadcast, stereo out
```

### 4.2 Chorus / Phaser / Flanger Widen

```akkado
// Mono synth, stereo modulation FX
osc("saw", 220) |> chorus(@, 0.5, 0.4) |> out(@)
// L-channel and R-channel use 90°-offset LFO phases (true stereo chorus,
// not double-mono). Mono input automatically broadcast.

osc("saw", 220) |> phaser(@, 0.7, 4) |> out(@)
// Same pattern: L/R LFOs offset for stereo width.
```

### 4.3 Channel-Independent DSP — Behaviour Preserved

```akkado
// Filters, distortion, EQ, plain delays: same audio as today's auto-lift.
// What changes: one instruction emit instead of "STEREO_INPUT runs twice."
in() |> filter_lp(@, 800, 0.5)
     |> saturate(@, 2.0)
     |> delay(@, 0.25, 0.5, 1.0, 0.5)
     |> out(@)
// Each opcode body now contains a 2-iteration L/R inner loop with shared state struct.
```

### 4.4 Auto-Escalation Replaces Mismatch Errors

```akkado
// Was E185: out(L, R) with stereo as one of the slots
some_stereo |> out(@, osc("sin", 440))
// After: auto-escalates. The mono sine broadcasts; the stereo signal feeds out().
// Equivalent to today's: out(stereo_to_left, stereo_to_right + mono_sine_broadcast).

// Was E183/E184: extraction on mono
mono_sig |> left(@)
// After: warning W183 logged ("left() on mono signal — returning the input unchanged");
// expression evaluates to the mono input. Same for right(mono) → W184.

// Was E181/E182: identity conversions
mono_sig |> mono(@)        // After: W181 logged, returns the input unchanged
stereo_sig |> stereo(@)    // After: W182 logged, returns the input unchanged

// Errors that REMAIN (truly structural):
freq = stereo_sig          // Audio-rate signal in a control slot of a non-signal builtin: error.
```

The relaxation rule: anything that's a *channel mismatch* becomes a warning + compile-success. Anything that's a *type mismatch* (audio-rate signal flowing into a non-signal slot, scalar entering a signal slot, etc.) stays an error.

### 4.5 Sample Playback

```akkado
// Mono WAV: L = R = sample
sample("kick.wav") |> dattorro(@, 0.7, 20) |> out(@)
// Stereo WAV: L = file_L, R = file_R, channels preserved
sample("loop_stereo.wav") |> width(@, 1.4) |> out(@)
// Sampler opcode reads file metadata internally and writes appropriate buffers.
```

---

## 5. Architecture

### 5.1 The `STEREO_OUTPUT` Instruction Flag

Mirror of the existing `STEREO_INPUT` flag at `cedar/include/cedar/vm/instruction.hpp:209-221`. No struct-size change — the `flags` field already lives in the existing 2-byte alignment padding at offset 14 and has 15 unused bits.

```cpp
namespace InstructionFlag {
    constexpr std::uint16_t STEREO_INPUT  = 1u << 0;  // existing — auto-lift
    constexpr std::uint16_t STEREO_OUTPUT = 1u << 1;  // NEW — stereo-native opcode
}
```

When `STEREO_OUTPUT` is set:

1. `BufferAllocator` reserves adjacent buffer pair for the output (already enforced for stereo today; pattern reused).
2. Codegen marks `TypedValue.channels = Stereo` and `TypedValue.right_buffer = out_buffer + 1`.
3. The opcode implementation reads `inst.out_buffer` and `inst.out_buffer + 1`, writes both.
4. The opcode body decides input handling: mono input is duplicated (read once into both L and R inner-loop variables); stereo input reads `inputs[i]` (L) and `inputs[i] + 1` (R).

The flag is opcode-specific metadata declared on `BuiltinInfo` / `BuiltinSignature` (`akkado/include/akkado/builtins.hpp:83-99`). Codegen sets the flag bit when emitting an instruction for any builtin with `output_channels = Stereo` and `stereo_native = true`.

### 5.2 Stateful Opcode Memory Layout

State structs gain per-channel fields. One state struct per opcode instance, one `state_id` per opcode (no `/L` / `/R` suffix, no XOR rotation).

```cpp
// Before (mono opcode, auto-lifted via STEREO_INPUT):
struct FilterLPState {
    float z1, z2;    // single channel; second instance lives under separate state_id
};

// After (stereo-native single-state):
struct FilterLPState {
    float z1[2], z2[2];   // [0] = L, [1] = R
};
```

For Dattorro, `tank_feedback[0]` and `tank_feedback[1]` already exist — the work at the state level is nil; the change is at the output:

```cpp
// Before (line 253 in reverbs.hpp):
out[i] = (state.tank_feedback[0] + state.tank_feedback[1]) * 0.5f;

// After:
out_l[i] = state.tank_feedback[0];
out_r[i] = state.tank_feedback[1];
```

For Freeverb, the state needs duplication of the comb/allpass arrays per channel (or a topological split — see §11 OQ1).

### 5.3 Auto-Escalation at Codegen

When a mono signal flows into a stereo-native opcode's signal slot, codegen does not emit an error. It either:

- **Reads-once-write-twice** inside the opcode body (preferred — no buffer allocation, the opcode just dereferences `inputs[i]` once per sample for both L and R passes), or
- **Pre-escalates** the mono buffer to a temporary stereo pair (only when downstream needs a real stereo buffer — rare).

The `BuiltinInfo.input_channels` field becomes informational only for stereo-native slots: `Mono` means "broadcast on entry," `Stereo` means "read both halves." Either way, no error.

For broadcast arithmetic (`Mono op Stereo`), today's read-twice pattern (`codegen_arrays.cpp:866-880`) is preserved unchanged.

### 5.4 Channel-Independent DSP — Inner-Loop Pattern

Filters, distortion, EQ, plain delays follow this template:

```cpp
[[gnu::always_inline]]
inline void op_filter_lp(ExecutionContext& ctx, const Instruction& inst) {
    const bool stereo = (inst.flags & InstructionFlag::STEREO_OUTPUT) != 0;
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = stereo ? ctx.buffers->get(inst.out_buffer + 1) : nullptr;

    const float* in_l = ctx.buffers->get(inst.inputs[0]);
    const float* in_r = stereo
        ? ctx.buffers->get(inst.inputs[0] + 1 /* or +0 if mono input auto-broadcast */)
        : nullptr;

    auto& state = ctx.states->get_or_create<FilterLPState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // L channel
        float yL = compute(in_l[i], state.z1[0], state.z2[0], cutoff, q);
        out_l[i] = yL;
        // R channel — only when stereo
        if (stereo) {
            float yR = compute(in_r[i], state.z1[1], state.z2[1], cutoff, q);
            out_r[i] = yR;
        }
    }
}
```

The `if (stereo)` branch is hoistable by the compiler and is the price for bit-identity in the mono path. Pure stereo-native (no mono fallback) is a Phase 5 cleanup once auto-lift is dead.

### 5.5 Spatializing FX — Algorithmic Stereo

These opcodes do not gain a "mono fallback" branch. They are always stereo-native (`STEREO_OUTPUT` always set by codegen for these builtins regardless of input type). Mono input is auto-escalated by reading the same buffer for L and R; the algorithm itself produces decorrelated stereo (LFO offsets for chorus/phaser/flanger; cross-coupled tanks for reverbs).

| Opcode | Mono in handling | L/R decorrelation source |
|--------|------------------|--------------------------|
| `dattorro` | Read once, feed both tanks | Figure-8 cross-coupling (already present) |
| `freeverb` | Read once, feed both comb networks | Comb/allpass length offsets between L and R networks |
| `fdn` | Read once, inject into all delays | Hadamard mixing produces decorrelated taps; emit two diagonal taps as L/R |
| `chorus` | Read once | LFO phase offset between L and R, default 90°, configurable per call |
| `phaser` | Read once | LFO phase offset between L and R, default 90°, configurable per call |
| `flanger` | Read once | LFO phase offset between L and R, default 90°, configurable per call |

**Modulation FX phase parameter.** Chorus, phaser, and flanger gain an additional optional parameter `lfo_phase` (radians, default `π/2` = 90°). The L LFO runs at the opcode's base phase; the R LFO runs at base + `lfo_phase`. 90° is the classic stereo-modulation default; users can dial to 0° (mono-equivalent), π (anti-phase, maximum width with potential mono-summing cancellation), or anywhere in between. The parameter slot uses the existing default-constant mechanism (see CLAUDE.md "Default constants" pattern) — passing nothing keeps 90° behaviour and adds no codegen cost.

### 5.6 Type System Changes — Surface

The language type system (per `prd-stereo-support`) is largely unchanged. Two adjustments:

1. `BuiltinSignature` gains `stereo_native: bool`. Auto-escalation is implicit when this is true and the input channel-count is Mono. The existing `auto_lift` flag becomes "transitional" — true while a builtin is mono-only, flipped to false (and `stereo_native = true`) when the builtin is converted.
2. The error-checking pass converts E181–E185 to **warnings** W181–W185. All five fire only when the user's code is channel-mismatched but otherwise structurally valid; with stereo-native opcodes there is always a sensible interpretation (escalate, no-op, broadcast). W181 (`mono(mono)`) and W182 (`stereo(stereo)`) become silent no-ops — the expression evaluates to the input. W183 (`left(mono)`) and W184 (`right(mono)`) return the input unchanged (the "channel" being extracted is the only one that exists). W185 (`out` arg shape mismatch) auto-escalates. The compiler logs each warning at the source location for discoverability. E186 (non-signal type errors from `BuiltinSignature` catalog) stays as an error — it catches structural type mismatches, not channel mismatches.

---

## 6. Impact Assessment

| Component | Status | Notes |
|-----------|--------|-------|
| `Instruction` struct, `flags` field | **Modified** | Add `STEREO_OUTPUT` bit constant; size unchanged |
| `InstructionFlag` namespace | **Modified** | New constant alongside `STEREO_INPUT` |
| Auto-lift dispatch loop in `vm.cpp` | **Stays** Phases 2–4, **deleted** Phase 5 | Coexists with stereo-native during migration; retires when last channel-independent opcode is converted |
| `STEREO_INPUT` flag | **Stays** Phases 2–4, **retired** Phase 5 | Replaced by `STEREO_OUTPUT` semantics |
| Reverb opcodes (`reverbs.hpp`) | **Modified** | Output split L/R; per-channel state arrays where needed; no auto-lift |
| Spatializing FX (chorus/phaser/flanger) | **Modified** | Stereo-native; LFO offsets for L/R decorrelation |
| Channel-independent DSP (filters, distortion, EQ, plain delays) | **Modified** | Inner-loop becomes 2-iteration; state struct gains `[2]` arrays |
| Existing stereo opcodes (`PAN`, `WIDTH`, `MS_ENCODE/DECODE`, `PINGPONG`, `MONO_DOWNMIX`, `PAN_STEREO`) | **Stays (re-validated)** | Already stereo-native; verify they conform to the new flag convention |
| Generators (`osc`, `noise`, `pulse`, mono-file `sample`) | **Stays** | Continue to emit `Mono`. Escalation happens at the boundary |
| `stereo()` / `mono()` / `left()` / `right()` / `pan()` builtins | **Stays** | Same signatures and codegen |
| `out()` semantics | **Stays** | Three forms (`out(s)`, `out(m)`, `out(L, R)`) preserved; auto-escalation makes mixed forms newly tolerant |
| `BuiltinSignature.auto_lift` | **Modified** Phases 2–4, **deleted** Phase 5 | True for unconverted opcodes; retires with auto-lift |
| `BuiltinSignature` gains `stereo_native: bool` | **New** | Drives codegen to emit `STEREO_OUTPUT` flag |
| `TypedValue.channels` / `right_buffer` | **Stays** | No type-system shape change |
| Type errors E181–E185 | **Modified** → warnings W181–W185 | All channel-mismatch errors relax to warnings; logged at source location, compilation continues |
| Error E186 (non-signal type mismatch on builtin) | **Stays** | Structural type mismatches (audio-rate signal into control slot, etc.) keep erroring |
| `BufferAllocator` adjacency invariant | **Stays** | Same constraint, more frequently invoked |
| `BufferPool` (`buffer_pool.hpp`) | **Stays** | No new buffer types this PRD |
| State pool / `state_id` derivation | **Modified** | Stereo-native state IDs revert to `fnv1a(semantic_path)` — no `/L`/`/R` suffix, no XOR for R |
| Sampler builtin (mono vs stereo file) | **Modified** | Reads file channel count; mono → L=R duplicate, stereo → L/R preserve |
| Web WASM bindings | **Stays** | Stereo output bus already exposed |
| Bytecode format | **Stays** | Same layout; new flag bit consumes existing reserved space |
| Existing mono-only programs | **Stays (bit-identical)** | Per-phase regression gate |

---

## 7. File-Level Changes

| File | Change |
|------|--------|
| `cedar/include/cedar/vm/instruction.hpp` (around line 209) | Add `STEREO_OUTPUT = 1u << 1` constant |
| `cedar/src/vm/vm.cpp` (around lines 467–480) | Phases 2–4: leave auto-lift dispatch in place. Phase 5: delete the `STEREO_INPUT` branch. |
| `cedar/include/cedar/opcodes/reverbs.hpp` | Phase 2: split outputs in `op_reverb_dattorro`, `op_reverb_freeverb`, `op_reverb_fdn`; per-channel state where needed |
| `cedar/include/cedar/opcodes/dsp_state.hpp` | Phase 2/3: extend `FreeverbState`, `DattorroState`, `FDNState`, `ChorusState`, `PhaserState`, `FlangerState` with per-channel arrays |
| `cedar/include/cedar/opcodes/modulation.hpp` (or wherever chorus/phaser/flanger live) | Phase 3: stereo-native rewrite; LFO offsets |
| `cedar/include/cedar/opcodes/filters.hpp`, `distortion.hpp`, `delays.hpp`, `eq.hpp`, etc. | Phase 4: inner-loop 2-iteration pattern; state structs gain `[2]` |
| `cedar/include/cedar/opcodes/stereo.hpp` | Re-validate; no algorithmic change expected |
| `akkado/include/akkado/builtins.hpp:83-99` (BuiltinSignature) | Add `stereo_native: bool`; update each builtin entry per phase |
| `akkado/src/codegen.cpp` | Emit `STEREO_OUTPUT` flag for stereo-native builtins; relax E181–E185 in stereo-native slots; update auto-escalation logic |
| `akkado/src/codegen_stereo.cpp` | Reuse adjacency allocator for `STEREO_OUTPUT`; remove `auto_lift`-only paths in Phase 5 |
| `akkado/src/codegen.cpp` (state-id derivation) | Stereo-native opcodes use `fnv1a(semantic_path)` (drop `/L`/`/R` suffix logic; that path remains only for legacy auto-lift during migration) |
| `akkado/include/akkado/sampler.hpp` (or wherever sampler lives) | Phase 3 (with samples): branch on file channel count |
| `cedar/tests/cedar_tests/*` | New tests per phase: stereo-native dispatch, bit-identity gate for mono path |
| `akkado/tests/test_codegen.cpp`, `test_types.cpp` | Update auto-escalation tests; remove the old E185 cases that now pass; add new positive tests |
| `experiments/test_op_dattorro.py`, `test_op_freeverb.py`, `test_op_fdn.py` | Update to verify true stereo output (cross-channel correlation, decorrelation metrics) |
| `experiments/test_op_chorus.py`, `test_op_phaser.py`, `test_op_flanger.py` | Add stereo-output assertions (LFO offset audible as L/R phase diff) |
| `experiments/test_op_stereo_native_filter.py` (new) | Bit-identity verification for filter L channel vs historical mono output |
| `web/static/docs/concepts/signals.md` | Update: auto-escalation replaces mismatch errors at audio-signal slots; stereo-native is the default |
| `web/static/docs/reference/builtins/reverbs.md` | Note that mono-in widens to stereo automatically; no `stereo()` wrapper needed |
| `web/scripts/build-opcodes.ts` (regenerate) | Picks up new flag automatically once enum is updated |
| `web/scripts/build-docs.ts` (regenerate) | After concept doc update |
| `docs/cedar-architecture.md` | Update: describe `STEREO_OUTPUT` flag; note auto-lift retirement timeline |
| `docs/prd-stereo-support.md` (footer) | Add a "See also" line pointing here |

Files that explicitly **do not change**:
- `cedar/include/cedar/vm/buffer_pool.hpp` — single mono buffer pool stays
- `akkado/include/akkado/typed_value.hpp` — `ChannelCount`, `right_buffer`, `channels` unchanged
- `cedar/include/cedar/opcodes/utility.hpp` (OUTPUT opcode) — unchanged

---

## 8. Implementation Phases

### Phase 0 — Spike + design lock-in

**Goal**: validate the flag mechanism on Dattorro before generalising.

- Implement `STEREO_OUTPUT` flag in `instruction.hpp`.
- Convert `op_reverb_dattorro` to stereo-native (smallest change — tank topology is already there).
- Add codegen path that sets the flag for `dattorro` builtin.
- Spike measurement: per-block CPU time vs current double-mono auto-lift baseline; record actual numbers, no pre-committed target.
- Decide Freeverb topology (OQ1) before Phase 2.

**Verification**:
- `osc("saw", 110) |> dattorro(@, 0.85, 30) |> out(@)` produces stereo output (correlation < 1.0 between L and R).
- `osc("saw", 110) |> stereo() |> dattorro(@, 0.85, 30)` cross-couples (different from naive double-mono — measure with phase-inverted L+R sum).
- Existing mono test cases unchanged.

### Phase 1 — Codegen + builtin signature plumbing

**Goal**: lay the codegen + signature foundation for the rest of the migration.

- Add `stereo_native: bool` to `BuiltinSignature`.
- Update codegen.cpp to emit `STEREO_OUTPUT` for `stereo_native = true` builtins.
- Implement auto-escalation: mono signal → stereo-native slot, codegen marks the input as broadcast (no buffer alloc).
- Convert E181–E185 to warnings W181–W185 globally (not slot-conditional); add a `compiler::emit_warning` path if one doesn't already exist.
- Migrate `dattorro` from spike state to first-class converted builtin.

**Verification**:
- All existing tests pass (auto-lift opcodes still work).
- New test: `mono_sig |> dattorro(@, ...) |> out(@)` compiles, produces stereo.
- New test: bit-identity for any mono-in / mono-out program (regression gate).

### Phase 2 — Reverbs

**Goal**: convert remaining reverbs.

- Convert `op_reverb_freeverb`, `op_reverb_fdn` to stereo-native.
- Per-channel comb arrays in `FreeverbState`; topology decided per OQ1.
- FDN emits two diagonal Hadamard taps as L/R.
- Update Python experiments: assert stereo decorrelation for each reverb.

**Verification**:
- `osc(...) |> freeverb(@, ...) |> out(@)`, `... |> fdn(@, ...)` — stereo tail measured.
- L/R cross-correlation < 1.0; decorrelation grows over reverb time.
- Stereo input through reverb shows audible cross-coupling (write `in() |> dattorro` and listen).

### Phase 3 — Spatializing modulation FX + sample playback

**Goal**: extend stereo-native to chorus/phaser/flanger; handle stereo sample files.

- Convert `op_chorus`, `op_phaser`, `op_flanger`. LFO phase offset (90° default) for L/R decorrelation.
- Update sampler builtin: branch on file channel count (mono → L=R, stereo → L/R preserve).
- Re-validate `WIDTH`, `MS_ENCODE/DECODE`, `PINGPONG`, `PAN`, `PAN_STEREO` under the flag convention.

**Verification**:
- `osc |> chorus(@, ...)` produces audibly stereo output from mono input.
- Stereo sample file plays with original L/R intact.
- All `stereo.hpp` opcodes pass existing `[stereo]` test tag.

### Phase 4 — Channel-independent DSP

**Goal**: convert the long tail.

- Convert filters (LP, HP, BP, BQ, SVF, Moog, diode), distortion (saturate, softclip, fold, bitcrush, distort_*), EQ, plain `delay`, `comb`, ADSR, AR, env_follower, clip ops.
- Each: state struct gains `[2]` per-channel arrays; inner loop 2-iteration with `if (stereo)` hoist.
- Mark all converted builtins with `stereo_native = true`, `auto_lift = false`.
- Bit-identity gate per opcode: legacy mono path produces identical audio.

**Verification**:
- Full Akkado regression suite green; cedar regression suite green.
- New `test_op_stereo_native_filter.py` confirms L-channel bit-identity vs historical mono output.
- Spectrum/Lissajous comparisons in Python experiments show no algorithmic regression.

### Phase 5 — Cleanup

**Goal**: retire the auto-lift mechanism.

- Confirm zero `BuiltinSignature` entries with `auto_lift = true`.
- Delete `STEREO_INPUT` flag constant, dispatch-loop branch in `vm.cpp:467-480`, `auto_lift` field on `BuiltinSignature`.
- Drop `/L` / `/R` state-id suffix logic from codegen.
- Drop legacy `STEREO_STATE_XOR_R` constant from instruction.hpp.
- Optionally: rename `STEREO_OUTPUT` to a shorter or more inclusive name (`STEREO_NATIVE`?) — bikeshed in review.
- Update docs: `cedar-architecture.md`, `concepts/signals.md`, `reverbs.md`.

**Verification**:
- Full suite green after deletion.
- No reference to `STEREO_INPUT`, `auto_lift`, `STEREO_STATE_XOR_R` remains in source.
- Doc index regenerated: `cd web && bun run build:opcodes && bun run build:docs`.

---

## 9. Edge Cases

### 9.1 Mono signal entering a stereo-native slot
**Behaviour**: silent escalation, no warning. Codegen marks the input as broadcast (read same buffer for both L and R inner-loop iterations). Zero buffer allocation, zero copy. Was E181/E185 in some cases pre-PRD; now silent because the operation is unambiguously well-defined.

### 9.2 Stereo signal entering a non-spatializing channel-independent stereo-native opcode
**Behaviour**: per-channel processing with per-channel state. Same as today's auto-lift but in one instruction emit.

### 9.3 Stereo signal entering a slot that demanded mono pre-PRD (e.g. parameter slot of a builtin without stereo support)
**Behaviour**: still an error. Audio-rate signals into control slots remain a structural mismatch (E186 from prd-stereo-support).

### 9.4 `out(L, R)` with mismatched channel types (e.g. one slot stereo, one mono)
**Behaviour**: auto-escalates. The mono slot is broadcast, the stereo slot drives both channels of out(); the two contributions sum into the output bus per existing OUTPUT opcode semantics. Was E185.

### 9.5 `mono(mono_sig)` and `stereo(stereo_sig)`
**Behaviour**: warnings W181 / W182, expression evaluates to the input unchanged. The compiler logs the warning at the source location so the user notices the redundant call, but the program runs.

### 9.6 `left(mono)` / `right(mono)`
**Behaviour**: warnings W183 / W184, expression evaluates to the input unchanged (extracting "the only channel that exists"). Logged at source location.

### 9.7 Hot-swap inside a single build (no migration concern)
**Behaviour**: same as `prd-stereo-support` §10.6. Within a build, swapping a chain that adds a stereo-native opcode is a structural change; state drops; crossfade applies. Migrations across builds (e.g. user upgrades nkido) are not hot-swap territory — bytecode is recompiled, state restarts cleanly.

### 9.8 Bit-identity boundary: floating-point reorderings
**Behaviour**: per-opcode review. The 2-iteration inner loop must not reorder operations that affect the L-channel result. Compiler ordering (`-ffp-contract`, FMA fusion) should be neutral for the L pass since the body is structurally identical to the mono code.

### 9.9 Chord expansion + stereo-native opcode
**Behaviour**: chord voices are mono-summed (per `prd-stereo-support` §10.5). The summed mono result auto-escalates into a stereo-native opcode just like any other mono signal.

### 9.10 Pattern events into stereo-native opcode
**Behaviour**: pattern events are mono control signals (not audio), so they enter parameter slots, not signal slots. Stereo-native processing of audio is unaffected.

### 9.11 Param (slider/knob) into a stereo-native opcode
**Behaviour**: param is mono control signal; shared between L and R inner-loop iterations. Today's behaviour, unchanged.

### 9.12 Phase coexistence: Phase 2-converted opcode chained into Phase 4-unconverted opcode
**Behaviour**: stereo-native opcode produces stereo pair → unconverted opcode reads it via auto-lift (`STEREO_INPUT`). Both mechanisms valid simultaneously through Phases 2–4. Phase 5 retires auto-lift.

### 9.13 SIMD / vectorisation
**Behaviour**: 2-iteration inner loop is friendly to SSE/AVX (process L and R as a 2-lane vector). Optimisation deferred to a follow-up; bit-identity gate stays scalar for now.

### 9.14 ESP32 / cedar-only builds
**Behaviour**: this PRD touches only Cedar opcode bodies and Akkado codegen. No platform-specific code. ESP32 builds (per `prd-cedar-esp32.md`) inherit the change automatically. CPU saving (1× reverb instead of 2× auto-lifted) is a net win on resource-constrained targets.

---

## 10. Testing and Verification

### 10.1 Per-Phase Regression Gate

Every phase must pass:
```bash
cmake --build build
./build/cedar/tests/cedar_tests
./build/akkado/tests/akkado_tests
./build/akkado/tests/akkado_tests "[stereo]"
./build/akkado/tests/akkado_tests "[types]"
cd experiments && ./run_all.sh
```

Plus the **bit-identity gate**: a curated set of mono-only Akkado programs (oscillators, filters, simple delay chains, no `stereo()`) must produce audio matching the pre-phase build within `1e-6` per sample. Implemented as `experiments/test_bit_identity_mono.py` reading reference WAVs committed at PRD start.

### 10.2 Unit tests (cedar_tests)

- **Stereo-native dispatch**: hand-construct an `Instruction` with `STEREO_OUTPUT` flag set, verify the opcode body writes both `out_buffer` and `out_buffer + 1`.
- **Per-channel state**: filter with different `cutoff` per channel (impossible via the public API but constructible at the test level) — verify L and R produce independent results.
- **Mono input broadcast**: instruction with `STEREO_OUTPUT` set but inputs[0] points to a mono buffer; verify L and R both equal the mono input post-processing.
- **Reverb cross-coupling** (Phase 2): Dattorro with stereo input, swap L and R inputs, observe the cross-coupling produces non-trivially different output (not just channel-swapped output).

### 10.3 Akkado tests (akkado_tests)

- **Auto-escalation positive cases**: `osc(...) |> dattorro(@, ...)`, `osc(...) |> chorus(@, ...)` — compile and execute without error, produce stereo `TypedValue`.
- **Channel-mismatch warnings emitted, not errors**: `mono_sig |> left(@)`, `mono(mono_sig)`, `stereo(stereo_sig)` compile, log W183/W181/W182, and evaluate to the input unchanged.
- **`out()` auto-escalation**: `out(stereo_sig, mono_sig)` compiles (relaxed E185).

### 10.4 Python experiments

- **`test_op_dattorro.py`**: stereo-output assertions — L/R cross-correlation < 1.0; spectral decorrelation; figure-8 cross-coupling visible when L and R inputs are pulse-train delta apart.
- **`test_op_freeverb.py`**: same plus topology validation per OQ1.
- **`test_op_fdn.py`**: Hadamard tap split produces decorrelated L/R.
- **`test_op_chorus.py`**, **`test_op_phaser.py`**, **`test_op_flanger.py`**: LFO offset audible as L/R phase difference; mono input produces non-trivial stereo output.
- **`test_op_stereo_native_filter.py`** (new, Phase 4): filter L channel matches pre-PRD mono output bit-identically.

### 10.5 End-to-end manual listening

```akkado
// L1 — Reverb tail width
osc("saw", 110) |> dattorro(@, 0.85, 30) |> out(@)
// Listen for: stereo width in the tail, not centred mono

// L2 — Chorus on mono synth
osc("saw", 220) |> chorus(@, 0.5, 0.4) |> out(@)
// Listen for: shimmer that moves between L and R

// L3 — Stereo input through reverb (cross-coupling)
in() |> dattorro(@, 0.85, 30) |> out(@)
// Listen for: tail that's denser than two independent mono reverbs

// L4 — Filter chain on stereo (regression check)
in() |> filter_lp(@, 800, 0.5) |> saturate(@, 2.0) |> delay(@, 0.25, 0.5, 1.0, 0.5) |> out(@)
// Listen for: identical tone vs current build (bit-identity gate)
```

### 10.6 Performance verification

Per-phase microbenchmark (BLOCK_SIZE = 128, 48kHz, 1000 blocks). No pre-committed numerical targets — measure first, document after. Baseline captured at PRD start (current auto-lift cost on stereo input); each phase records before/after for the opcodes it touches and commits the result alongside the implementation. A phase regresses if any converted opcode is *slower* than its auto-lift baseline; otherwise the speedup is reported, not gated on.

Benchmarks live in `experiments/bench_op_*.py`, run before each phase commit.

### 10.7 Build commands

```bash
# Build
cmake --build build

# Cedar + Akkado tests
./build/cedar/tests/cedar_tests
./build/akkado/tests/akkado_tests

# Tag-filtered
./build/akkado/tests/akkado_tests "[stereo]"
./build/akkado/tests/akkado_tests "[types]"

# Python suite
cd experiments && ./run_all.sh

# Regenerate metadata after opcode changes
cd web && bun run build:opcodes && bun run build:docs
```

---

## 11. Open Questions

- **OQ1**: **Freeverb topology under stereo-native.** Two candidates:
  - (a) 8 combs total, split 4-L + 4-R (lower memory, lighter density).
  - (b) 8 combs per channel = 16 total (same density per channel as today, classic stereo Freeverb).
  - Decide in Phase 0 spike based on tail-density listening test and memory budget. Default lean: (b), classic stereo Freeverb is what listeners expect.

- **OQ2**: **Naming of the flag.** `STEREO_OUTPUT` reads as "this opcode produces stereo," which is right for spatializing FX but slightly misleading for channel-independent ops where the flag really means "this opcode is stereo-native, it handles both channels in one pass." Alternatives: `STEREO_NATIVE`, `STEREO_OPCODE`. Bikeshed in Phase 5 review.

- **OQ3**: **`auto_lift` field on BuiltinSignature — drop entirely or keep as a debugging hint?** Phase 5 default is to delete. If it stays valuable for diagnostics ("this was once auto-lifted"), keep but mark deprecated.

- **OQ4**: **SIMD lane fusion for the L/R inner loop.** Hand-vectorising filter and EQ inner loops with SSE2 (process L and R as a `__m128` pair) is a follow-up optimisation. This PRD specifies scalar; SIMD lands in a separate PRD if the win is measured.

- **OQ5**: **Channel-type polymorphism through user-defined functions** — still deferred to `prd-advanced-functions.md` (inherited from prd-stereo-support OQ5). Not blocking.

- **OQ6**: **WASM versioning.** Bytecode layout doesn't change (the new flag bit is in existing reserved space), but the *meaning* of the bit is new. Decide in Phase 1 whether to bump a minor version constant in WASM exports for tooling compatibility. Default: no version bump unless a reader actively checks the bit.

---

## 12. Related Work

- **Predecessor**: [`prd-stereo-support.md`](prd-stereo-support.md) — type system, auto-lift mechanism, language-level builtins. This PRD picks up where that one left off; their §13 explicitly named the never-written companion this is.
- **Audit reference**: [`docs/audits/prd-stereo-support_audit_2026-04-21.md`](audits/prd-stereo-support_audit_2026-04-21.md) — confirmed all goals met; the limitations this PRD addresses are noted there as scope explicitly out of the original PRD.
- **Dependent PRDs**:
  - [`prd-audio-input.md`](prd-audio-input.md) — `in()` is a stereo-native generator; its auto-escalation behaviour into stereo-native DSP is exercised by this PRD's reverb test cases.
  - [`prd-enhanced-sampler.md`](prd-enhanced-sampler.md) — Phase 7 of that PRD becomes simpler under this one (sampler stereo handling is folded into Phase 3 here).
- **Hot-swap context**: [`prd-crossfade-audio-fixes.md`](prd-crossfade-audio-fixes.md) — within-build hot-swap behaviour unchanged; cross-build migrations are not hot-swap.
