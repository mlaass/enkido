# Extended Parameter Mechanism

**Audience:** Cedar / Akkado contributors and Claude Code agents adding new
DSP opcodes or migrating existing ones off `inst.rate` bit-packing.

**Status:** canonical mechanism as of 2026-05-13. Replaces the four ad-hoc
techniques previously catalogued in `CLAUDE.md`'s "Extended Parameter
Patterns" section.

---

## 1. Why this exists

Cedar's `Instruction` is a fixed 20-byte record with five 16-bit input
buffer slots. Opcodes that need more than five parameters used to reach
for one of four mutually incompatible workarounds:

| Workaround | Where | Cost |
|---|---|---|
| `inst.rate` bit-packing | compressor, limiter, freeverb, dattorro, phaser, distort_comb, delays | every opcode invents a layout, no validation, no type safety |
| `std::bit_cast<float>(state_id)` | (was) stateless opcodes that need one compile-time float | clobbers `state_id`, prevents stateful upgrade |
| `inputs[3]/inputs[4]` halving | SEQPAT_TRANSPORT (cycle_length) | sacrifices two real signal slots |
| Hardcoded constants | Phase 3 chorus/flanger/phaser `STEREO_LFO_OFFSET_TURNS` | not user-tunable; PRD §5.5 promised tunability and deferred |

These cannot interoperate. A builtin needs cmoplete redesign every time it
crosses the five-input boundary. The unified mechanism replaces all four.

## 2. The mechanism

`ExtendedParams<N>` (`cedar/include/cedar/opcodes/dsp_state.hpp:1187`) holds
N per-opcode parameter slots. Each slot is:

```cpp
struct Slot {
    float constant = 0.0f;              // Compile-time scalar value
    std::uint16_t buffer_idx = 0xFFFF;  // 0xFFFF = use constant
    bool is_constant() const { return buffer_idx == 0xFFFF; }
};
```

A slot is either a baked-in constant or a buffer reference (same model
`inst.inputs[]` uses). `ExtendedParams<N>` is a `DSPState` variant, with
N ∈ {1, 2, 3, 5, 8} registered today. The runtime helper picks the
smallest variant that fits the opcode's `extended_param_count`.

The companion `ExtendedParams<N>` for an opcode lives in the StatePool at
a **sibling state_id**:

```
ext_state_id = opcode_state_id ^ EXT_PARAMS_STATE_XOR     // 0xB9D2A1C7
```

The XOR keeps the opcode's primary DSP state (e.g. `ChorusState`) and
its ExtendedParams in distinct slots, so `get_or_create<ChorusState>`
doesn't overwrite the ExtendedParams. Both sides use the
`cedar::ext_params_state_id(state_id)` helper to compute it — never write
the XOR literal yourself.

### Lifecycle

```
Akkado source
  ↓  ── analyzer reorders named args, inserts `_` placeholders for gaps ──
codegen
  ↓  ── builtin->extended_param_count > 0 triggers emit_extended_params_init() ──
StateInitData { type=ExtendedParams, ext_constants[], ext_buffer_indices[] }
  ↓  ── program_loader::apply_state_inits / cedar_apply_state_inits ──
vm.init_extended_params(state_id, constants, buffer_indices, count)
  ↓  ── StatePool::init_extended_params_runtime picks N variant ──
StatePool slot at ext_params_state_id(opcode_state_id)
  ↓
opcode body:  states->get_if<ExtendedParams<N>>(ext_params_state_id(inst.state_id))
```

The state_id-keyed storage means hot-swap matches ExtendedParams to the
opcode that owns them automatically. `apply_state_inits` re-runs on every
recompile, so users can re-tune extended params at edit time without
restarting playback.

## 3. How to add an extended param to a builtin

Worked example: the chorus `lfo_phase` parameter (1 ext slot).

### 3a. Declare the parameter

`akkado/include/akkado/builtins.hpp`:

```cpp
{"chorus", {.opcode = cedar::Opcode::EFFECT_CHORUS,
            .input_count = 1, .optional_count = 4, .requires_state = true,
            .param_names = {"in", "rate", "depth", "base_delay", "depth_range", ""},
            .defaults = {0.5f, 0.5f, 20.0f, 10.0f, NAN},
            .description = "Stereo-native chorus (...) lfo_phase tunes the R-LFO offset.",
            .extended_param_count = 1,
            .output_channels = ChannelCount::Stereo,
            .auto_lift = false,
            .stereo_native = true,
            .extended_param_names = {"lfo_phase"},
            .extended_defaults = {0.25f}}},
```

Use designated-init syntax — positional init for `BuiltinInfo` ends at
`consumes_polyphonic_pattern`, before the extended-param fields.

### 3b. Read the slot in the opcode body

`cedar/include/cedar/opcodes/modulation.hpp`:

```cpp
auto& state = ctx.states->get_or_create<ChorusState>(inst.state_id);

// Resolve the ExtendedParams<1> slot once outside the sample loop.
const auto* ext = ctx.states->get_if<ExtendedParams<1>>(
    ext_params_state_id(inst.state_id));
const float* lfo_phase_buf = nullptr;
float lfo_phase_const = STEREO_LFO_OFFSET_DEFAULT_TURNS;
if (ext) {
    const auto& slot = ext->params[0];
    if (slot.is_constant()) {
        lfo_phase_const = slot.constant;
    } else {
        lfo_phase_buf = ctx.buffers->get(slot.buffer_idx);
    }
}

for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
    // ...
    const float r_offset = lfo_phase_buf ? lfo_phase_buf[i] : lfo_phase_const;
    // use r_offset...
}
```

**Fall back to a hardcoded default** when `ext == nullptr` (an old program
that pre-dates the migration may still load — the opcode keeps working).

### 3c. Tests

- Add an Akkado test in `akkado/tests/test_types.cpp` under tag
  `[extended-params]` that asserts the `StateInitData::Type::ExtendedParams`
  entry is emitted with the right slot count and default.
- Add a Python audio-level test in `experiments/test_op_<codename>.py`
  driving the new param across its range and confirming the audible
  behaviour matches the docs.

### 3d. User-facing docs

Add the new param to `web/static/docs/builtins/<name>.md` and run
`cd web && bun run build:docs`.

## 4. Migration recipe: moving an `inst.rate`-packed param to ExtendedParams

Worked example: phaser's `feedback` and `stages` (the proof migration
shipped alongside the mechanism).

### Before

```cpp
// In codegen.cpp — special-case bit-packing block.
if (func_name == "phaser") {
    // walk AST, extract literals at positions 5 & 6, pack into inst.rate
}

// In modulation.hpp.
float feedback = static_cast<float>((inst.rate >> 4) & 0x0F) / 15.0f * 0.99f;
std::size_t num_stages = std::clamp(static_cast<std::size_t>(inst.rate & 0x0F),
                                     std::size_t{2}, PhaserState::NUM_STAGES);
```

### After

`builtins.hpp`:

```cpp
{"phaser", {.opcode = cedar::Opcode::EFFECT_PHASER,
            .input_count = 1, .optional_count = 4, .requires_state = true,
            .param_names = {"in", "rate", "depth", "min_freq", "max_freq", ""},
            .defaults = {0.5f, 0.8f, 200.0f, 4000.0f, NAN},
            .description = "Stereo-native multi-stage phaser. "
                           "feedback (0-0.99), stages (2-12), lfo_phase (turns).",
            .extended_param_count = 3,
            .output_channels = ChannelCount::Stereo,
            .auto_lift = false,
            .stereo_native = true,
            .extended_param_names = {"feedback", "stages", "lfo_phase"},
            .extended_defaults = {0.5f, 4.0f, 0.25f}}},
```

`codegen.cpp` — delete the phaser-specific bit-pack block. The
`emit_extended_params_init` helper already runs for any builtin with
`extended_param_count > 0`.

`modulation.hpp` — replace the rate-field reads:

```cpp
const auto* ext = ctx.states->get_if<ExtendedParams<3>>(
    ext_params_state_id(inst.state_id));
// resolve_slot helper: see op_effect_phaser for the full pattern.

for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
    const float feedback = std::clamp(
        feedback_buf ? feedback_buf[i] : feedback_const, 0.0f, 0.99f);
    const std::size_t num_stages = std::clamp(
        static_cast<std::size_t>((stages_buf ? stages_buf[i] : stages_const) + 0.5f),
        std::size_t{2}, PhaserState::NUM_STAGES);
    // ...
}
```

### Migration checklist

1. Bump `extended_param_count`, add `extended_param_names` /
   `extended_defaults`.
2. Move every `inst.rate` decode out of the opcode body.
3. Delete the codegen special-case (if any).
4. Add `Catch::Approx` assertions on `state_inits` to lock in the
   StateInit emission shape.
5. Verify `inst.rate == 0` post-migration with a test (rate field must
   not silently carry stale bits).
6. Run the Python experiment with old vs new programs producing the same
   audio (impulse response or spectrogram).

## 5. When NOT to use ExtendedParams

`inst.rate` is **not** deprecated. Keep using it for:

- **Audio-rate vs control-rate dispatch.** The original purpose (0 = audio,
  1 = control).
- **Small fixed enum modes** with ≤4 values that the user does not tune
  at runtime: CLOCK phase type (0/1/2), EDGE_OP mode (0/1/2/3), LFO shape
  (0-6), ARRAY count.
- **Compile-time count fields** that have no audio-rate meaning
  (ARRAY_PACK, ARRAY_CONCAT len_a, etc.).

If the user can plausibly automate the parameter, it belongs in
ExtendedParams.

## 6. Capacity table

| `extended_param_count` | Variant chosen | When |
|---|---|---|
| 1 | `ExtendedParams<1>` | chorus, flanger lfo_phase |
| 2 | `ExtendedParams<2>` | future: filter Q + drive, etc. |
| 3 | `ExtendedParams<3>` | phaser (feedback, stages, lfo_phase) |
| 4-5 | `ExtendedParams<5>` | future: complex filter banks |
| 6-8 | `ExtendedParams<8>` | future: large parameter blocks |

Counts above 8 are clamped to 8 and the tail is truncated — bump
`MAX_EXTENDED_PARAMS` and add a larger variant if a real use case appears.

## 7. Hot-swap behaviour

ExtendedParams lives in the StatePool, keyed off `ext_params_state_id(
state_id)`. Because the state_id derives from the semantic path (FNV-1a
of the call's lexical identity), it's stable across recompiles that
don't change the opcode's location in the program graph.

- Edit a default value (e.g. lfo_phase 0.25 → 0.4): the StateInit emits
  a new constants array, `apply_state_inits` runs after load, overwrites
  the same slot. No crossfade needed.
- Wire a buffer instead of a constant (e.g. lfo_phase from an LFO): the
  StateInit changes the slot from constant to buffer-backed; the next
  block reads from the buffer instead. Smoothness is the user's
  responsibility just like for any audio-rate param.
- Move the opcode call to a different location in the source: state_id
  changes, both the primary DSP state AND the ExtendedParams get new
  slots, the old ones GC away after the fade window.

## 8. Reference

- `cedar/include/cedar/opcodes/dsp_state.hpp` — `ExtendedParams<N>` struct,
  `DSPState` variant registration.
- `cedar/include/cedar/vm/state_pool.hpp` — `init_extended_params<N>`,
  `init_extended_params_runtime`.
- `cedar/include/cedar/vm/vm.hpp` — `VM::init_extended_params`.
- `cedar/include/cedar/vm/instruction.hpp` — `EXT_PARAMS_STATE_XOR`,
  `ext_params_state_id`.
- `cedar/bindings/bindings.cpp` — Python binding `vm.init_extended_params`,
  module-level `cedar.ext_params_state_id`.
- `akkado/include/akkado/builtins.hpp` — `BuiltinInfo::extended_param_count`,
  `extended_param_names`, `extended_defaults`.
- `akkado/include/akkado/codegen.hpp` — `StateInitData::Type::ExtendedParams`.
- `akkado/src/codegen.cpp` — `CodeGenerator::emit_extended_params_init`.
- `tools/nkido/program_loader.cpp` — `apply_state_inits`.
- `web/wasm/nkido_wasm.cpp` — `cedar_apply_state_inits`.
