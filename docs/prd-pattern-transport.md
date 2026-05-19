> **Status: SHIPPED (2026-05-20)** — SEQPAT_TRANSPORT opcode, clock overrides,
> and the `transport()` codegen handler shipped in `5cb4eda` (Feb 2026); the
> `transport` builtin entry that makes them reachable from Akkado source, plus
> `akkado/tests/test_transport.cpp` compile-level tests, landed 2026-05-20.

# PRD: Pattern Transport

## Prerequisite

[Compiler Type System](prd-compiler-type-system.md) — the compiler needs to know the first argument to `transport()` is a Pattern to compile the inner sequence and wire up clock override.

## Problem

Patterns are locked to the global clock. The SEQPAT opcodes derive beat position from `ctx.global_sample_counter / ctx.samples_per_beat()`, modulo `cycle_length`. There is no way to:

- Step through a pattern at a different rate than the global BPM
- Advance a pattern driven by external triggers (e.g., another pattern's triggers, LFO zero-crossings, MIDI input)
- Scrub/seek to an arbitrary position in a pattern
- Play a pattern backwards

These are fundamental for polyrhythmic composition, pattern-based sequencing of sequences, and interactive performance.

## Syntax

```akkado
// Basic: step through pattern on each trigger, 1 beat per step
transport(n"c4 e4 g4", trigger(2)) as e |> osc("sin", e.freq)

// Custom step size: advance 0.5 beats per trigger
transport(n"c4 e4 g4", trigger(4), 0.5) as e |> osc("sin", e.freq)

// Cross-pattern triggers: one pattern drives another
transport(n"c4 e4 g4", n"x x x x".trig) as e |> osc("sin", e.freq)

// Pipe style (@ is Pattern thanks to type system)
n"c4 e4 g4" |> transport(@, trigger(2)) as e |> osc("sin", e.freq)

// Negative step: reverse playback
transport(n"c4 e4 g4", trigger(2), -1.0) as e |> osc("sin", e.freq)

// With reset trigger (4th arg)
transport(n"c4 e4 g4", trigger(2), 1.0, trigger(0.5)) as e |> osc("sin", e.freq)
```

### Arguments

| # | Name | Type | Default | Description |
|---|------|------|---------|-------------|
| 0 | pattern | Pattern | required | The pattern to step through |
| 1 | trig | Signal | required | Trigger signal — rising edge advances position |
| 2 | step | Signal | 1.0 | Beat increment per trigger (negative = reverse) |
| 3 | reset | Signal | unused | Rising edge resets position to 0 |

`transport()` returns a Pattern with the same fields as the input (freq, vel, trig, gate, type), driven by the overridden clock.

## Technical Approach

### Overview

The key insight: SEQPAT_STEP and SEQPAT_GATE already compute `beat_pos` per sample from `ctx.global_sample_counter`. If we supply an external `beat_pos` buffer instead, the same event-lookup logic works — but driven by a trigger-accumulated position rather than the wall clock.

Three changes:

1. **New opcode `SEQPAT_TRANSPORT`** — accumulates trigger edges into a beat position buffer
2. **Clock override slot on `SEQPAT_STEP`/`SEQPAT_GATE`** — `inputs[3]` optionally overrides the global clock
3. **Compiler support** — `transport()` builtin compiles the inner pattern normally, then emits SEQPAT_TRANSPORT and wires its output to the clock override slot

### 1. New Opcode: SEQPAT_TRANSPORT

```
SEQPAT_TRANSPORT
  out_buffer:  beat_pos output (audio-rate buffer)
  inputs[0]:   trigger signal
  inputs[1]:   step size (audio-rate or constant, default 1.0)
  inputs[2]:   reset trigger (0xFFFF if unused)
  state_id:    TransportState reference
  rate:        unused (0)
```

**State:**
```cpp
struct TransportState {
    float beat_pos = 0.0f;       // Accumulated position (wraps at cycle_length)
    float cycle_length = 4.0f;   // Set by compiler from pattern's cycle_length
    float last_trig = 0.0f;      // For edge detection
    float last_reset = 0.0f;     // For edge detection
};
```

`sizeof(TransportState) = 16` bytes — fits in state pool.

**Implementation:**
```cpp
inline void op_seqpat_transport(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* trig = ctx.get_input(inst, 0);
    const float* step = ctx.get_input(inst, 1);
    const float* reset = (inst.inputs[2] != BUFFER_UNUSED)
        ? ctx.buffers->get(inst.inputs[2]) : nullptr;
    auto& state = ctx.states->get_or_create<TransportState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Reset on rising edge
        if (reset && reset[i] > 0.5f && state.last_reset <= 0.5f) {
            state.beat_pos = 0.0f;
        }
        if (reset) state.last_reset = reset[i];

        // Advance on trigger rising edge
        if (trig[i] > 0.5f && state.last_trig <= 0.5f) {
            state.beat_pos += step[i];
            // Wrap at cycle boundary
            if (state.beat_pos >= state.cycle_length) {
                state.beat_pos = std::fmod(state.beat_pos, state.cycle_length);
            }
            // Handle negative wrap
            while (state.beat_pos < 0.0f) {
                state.beat_pos += state.cycle_length;
            }
        }
        state.last_trig = trig[i];

        out[i] = state.beat_pos;
    }
}
```

### 2. Clock Override on SEQPAT_STEP and SEQPAT_GATE

Currently `inputs[3]` is unused on SEQPAT_STEP (inputs are: [0]=vel_out, [1]=trig_out, [2]=voice_index). Add:

```
inputs[3]: optional beat_pos override buffer (0xFFFF = use global clock)
```

In `op_seqpat_step`, the per-sample beat_pos calculation changes:

```cpp
// Before:
float beat_pos = std::fmod(
    static_cast<float>(ctx.global_sample_counter + i) / spb,
    state.cycle_length
);

// After:
float beat_pos;
if (ext_beat_pos) {
    beat_pos = ext_beat_pos[i];  // External clock from SEQPAT_TRANSPORT
} else {
    beat_pos = std::fmod(
        static_cast<float>(ctx.global_sample_counter + i) / spb,
        state.cycle_length
    );
}
```

Same change applies to `op_seqpat_gate`:
- Currently `inputs[0]` = voice_index. Add `inputs[1]` = optional beat_pos override.

And `op_seqpat_query`:
- Currently stateless inputs (queries are block-boundary). When clock override is present, query must run every block since the external clock is not monotonic. Add a flag or detect from state.

### 3. SEQPAT_QUERY with External Clock

The current SEQPAT_QUERY caches results per cycle (`last_queried_cycle`). With external clock, the position can jump to any cycle at any time. Two options:

**Option A: Re-query every block when clock override is active.**
Simple but potentially expensive for complex sequences. Since query_pattern() is O(sequence_depth), this is likely acceptable for typical patterns.

**Option B: Query on demand when beat_pos crosses into a new cycle.**
More efficient but requires SEQPAT_STEP to detect cycle boundary crossings and request re-query. Adds complexity.

**Recommendation: Option A** — re-query every block. Mark this in the TransportState or via a flag on SequenceState so the query knows to skip the cache check.

To implement: SEQPAT_QUERY gains an optional input for the beat_pos buffer. When present, it derives the cycle number from the external beat_pos instead of the global clock:

```
SEQPAT_QUERY
  inputs[0]: optional beat_pos override (0xFFFF = use global clock)
```

```cpp
if (ext_beat_pos) {
    // Use start-of-block position from external clock
    float beat_start = ext_beat_pos[0];
    float current_cycle = std::floor(beat_start / state.cycle_length);
    // Always re-query (external clock is non-monotonic)
    state.last_queried_cycle = -1.0f;
    // ... rest of query logic
} else {
    // Existing global clock logic
}
```

### 4. Compiler: transport() Builtin

**Builtin definition:**
```cpp
{"transport", {cedar::Opcode::SEQPAT_TRANSPORT, 2, 2, true,
               {"pattern", "trig", "step", "reset", "", ""},
               {1.0f, NAN, NAN, NAN, NAN},
               "Trigger-driven pattern transport",
               // param_types (from type system PRD):
               // Pattern, Signal, Signal, Signal
              }}
```

**Codegen in `codegen_patterns.cpp` or `codegen_builtins.cpp`:**

When the compiler encounters `transport(pattern_expr, trig, step, reset)`:

1. **Visit the pattern argument** → `TypedValue{Pattern, ...}` with state_id and cycle_length
2. **Visit trig, step, reset** → Signal buffers
3. **Emit SEQPAT_TRANSPORT:**
   ```cpp
   Instruction transport_inst;
   transport_inst.opcode = Opcode::SEQPAT_TRANSPORT;
   transport_inst.out_buffer = alloc_buffer();  // beat_pos output
   transport_inst.inputs[0] = trig_buf;
   transport_inst.inputs[1] = step_buf;
   transport_inst.inputs[2] = reset_buf;  // or 0xFFFF
   transport_inst.state_id = make_state_id("transport");
   emit(transport_inst);
   ```
4. **Patch SEQPAT_QUERY:** set `inputs[0]` = transport_inst.out_buffer
5. **Patch SEQPAT_STEP:** set `inputs[3]` = transport_inst.out_buffer for each voice
6. **Patch SEQPAT_GATE:** set `inputs[1]` = transport_inst.out_buffer for each voice
7. **Set TransportState cycle_length** from the pattern's compiled cycle_length (via StateInitData)
8. **Return the same Pattern TypedValue** — same field buffers, just now driven by the overridden clock

The key: `transport()` doesn't create new field buffers. It modifies the clock source of the existing SEQPAT opcodes that the pattern already emitted.

## Edge Cases

### Reverse Playback (negative step)

`step = -1.0` decrements beat_pos on each trigger. The wrap logic in SEQPAT_TRANSPORT handles negative positions: `while (beat_pos < 0) beat_pos += cycle_length`. Event lookup in SEQPAT_STEP works correctly since it scans forward from `current_index = 0` — but cycle wrap detection needs updating:

Current wrap detection: `beat_pos < last_beat_pos - 0.5f` assumes forward playback. With external clock, replace with:
```cpp
bool wrapped = (ext_beat_pos && beat_pos < last_beat_pos - 0.001f)
            || (!ext_beat_pos && beat_pos < last_beat_pos - 0.5f);
```

Or simpler: when using external clock, always reset `current_index = 0` and scan forward. The scan is O(num_events) which is small.

### ALTERNATE Mode Sequences

ALTERNATE-mode sequences cycle through sub-patterns on consecutive cycles. With external clock, "consecutive cycles" means consecutive wraps of `beat_pos` past `cycle_length`, not global clock cycles. This works naturally if SEQPAT_QUERY uses the external beat_pos for cycle counting — each wrap of the transport clock counts as a cycle for ALTERNATE selection.

### Trigger-Driven Gate Signal

SEQPAT_GATE determines gate high/low by checking if `beat_pos` falls within `[event.time, event.time + event.duration)`. With external clock override, this works automatically — the gate signal reflects the transported position. However, since the transport clock advances discretely (on triggers only), the gate will snap between on/off states at trigger boundaries rather than smoothly tracking wall time. This is the correct behavior for trigger-driven transport.

### Reset Trigger

The reset input (arg 3) snaps `beat_pos` back to 0.0. This also needs to trigger a re-query in SEQPAT_QUERY since the position may jump to a different cycle. Since we already re-query every block with external clock (Option A above), this is handled automatically.

### Initial State

On first block, `TransportState.beat_pos = 0.0` and no triggers have fired. The pattern outputs the event at position 0 (first event). This is the expected behavior — the pattern starts at the beginning and waits for triggers to advance.

### Hot-Swap

TransportState is preserved across hot-swaps via semantic ID matching, like all other states. The accumulated `beat_pos` persists, so the pattern continues from where it left off after a code edit.

## Key Files

| File | Change |
|------|--------|
| `cedar/include/cedar/opcodes/sequencing.hpp` | SEQPAT_TRANSPORT impl, clock override on STEP/QUERY/GATE |
| `cedar/include/cedar/opcodes/dsp_state.hpp` | TransportState struct |
| `cedar/include/cedar/vm/instruction.hpp` | SEQPAT_TRANSPORT in Opcode enum |
| `akkado/include/akkado/builtins.hpp` | transport() builtin definition |
| `akkado/src/codegen_patterns.cpp` | transport() codegen — emit SEQPAT_TRANSPORT, patch clock slots |

## Closed Follow-Up — `transport()` builtin registration (2026-05-20)

For ~3 months after `5cb4eda` the opcode + codegen handler existed but were
dead code: no `BuiltinInfo` entry was registered in
`akkado/include/akkado/builtins.hpp`, so the analyzer rejected `transport(...)`
with `E004 "Unknown function"` before codegen ran. Closed 2026-05-20:

- **Builtin registered** — `{"transport", {SEQPAT_TRANSPORT, 2, 2, true, ...}}`
  added to `BUILTIN_FUNCTIONS`, alongside the other pattern-transform builtins.
- **Codegen path confirmed** — `handle_transport_call`
  (`akkado/src/codegen_patterns.cpp`) fires via the `special_handlers` map and
  is already ExtendedParams-current; no change needed.
- **Compile-level tests** — `akkado/tests/test_transport.cpp` compiles all six
  Syntax examples, asserts `SEQPAT_TRANSPORT`/`SEQPAT_QUERY` emission, and
  covers the arg-count / non-pattern error paths.
- **Autocomplete** — `transport` surfaces automatically via
  `akkado_get_builtins_json()` now that it is in `BUILTIN_FUNCTIONS`.

## Non-Goals

- **Continuous transport (non-trigger).** Smoothly ramping through a pattern at a different BPM is a different feature (clock multiplication/division). This PRD focuses on trigger-driven stepping only.
- **Pattern-level seeks.** Jumping to an arbitrary beat position via `seek(pattern, 2.5)` is out of scope. The reset trigger covers the "go to start" case.
- **Nested transport.** `transport(transport(n"…", trig1), trig2)` — while it should technically work (transport returns a Pattern), optimizing nested clock overrides is not a goal.
- **Sample-accurate trigger quantization.** Triggers advance the position at the exact sample they fire. No quantization to beat grid.
