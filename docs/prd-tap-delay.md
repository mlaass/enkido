> **Status: DONE** — Cedar opcodes (DELAY_TAP/DELAY_WRITE) + full Akkado codegen with closure support.

# Tap Delay System PRD

## Executive Summary

This document specifies a **tap delay** system for Nkido/Akkado that enables:
- Configurable feedback processing chains using any existing DSP opcodes
- Compile-time closure inlining between coordinated TAP/WRITE opcodes
- Full flexibility: filters, distortion, pitch shift, etc. in feedback path
- Hot-swap compatible with per-opcode state preservation
- Zero runtime overhead compared to hand-written feedback chains

---

## 1. Core Concepts

### 1.1 What is a Tap Delay?

A **tap delay** is a delay line where the feedback signal can be processed before being fed back. This enables classic effects like:
- **Tape delay**: Feedback through lowpass filter (high-frequency rolloff per repeat)
- **Dub delay**: Feedback through filter + saturation (warm, degrading repeats)
- **Shimmer delay**: Feedback through pitch shifter (ascending/descending pitch spirals)
- **Filtered ping-pong**: Stereo delay with different feedback processing per channel

### 1.2 Design Approach: Compile-Time Inlining

Rather than runtime subroutines, the feedback chain is **inlined at compile time** between two coordinated opcodes:

```
DELAY_TAP    → Reads from delay line, outputs delayed signal
[user code]  → Any processing (filters, effects, math)
DELAY_WRITE  → Commits input + processed_feedback * fb to delay line
```

The TAP and WRITE share the same `state_id`, operating on the same delay buffer.

**Benefits:**
- Zero runtime overhead (no function calls, no subroutine dispatch)
- Any existing opcode works in feedback chain
- Stateful opcodes (filters, etc.) get proper state management
- Hot-swap preserves all state correctly

---

## 2. Syntax & API

### 2.1 Basic Usage

```akkado
// Simple delay with lowpass filter in feedback (time in seconds)
saw(110) |> tap_delay(@, 0.3, 0.7, (x) -> lp(x, 1000))

// Multi-stage feedback processing
saw(110) |> tap_delay(@, 0.5, 0.6, (x) ->
    lp(x, 2000) |> saturate(@, 0.2) |> hp(@, 80)
)

// Using milliseconds variant for precise timing
saw(110) |> tap_delay_ms(@, 300, 0.7, (x) -> lp(x, 1000))

// Using samples variant for comb filtering effects
noise() |> tap_delay_smp(@, 100, 0.9, (x) -> x)  // ~480Hz resonance at 48kHz
```

### 2.2 Function Signatures

There are multiple variants for different time units:

```akkado
tap_delay(input, time_sec, feedback, processor)     // Time in seconds (default)
tap_delay_ms(input, time_ms, feedback, processor)   // Time in milliseconds
tap_delay_smp(input, time_smp, feedback, processor) // Time in samples (direct control)
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `input` | signal | Input audio signal |
| `time_sec` | signal/number | Delay time in seconds (0-10), can be modulated |
| `time_ms` | signal/number | Delay time in milliseconds (0-10000), can be modulated |
| `time_smp` | signal/number | Delay time in samples (direct, for comb filtering) |
| `feedback` | signal/number | Feedback amount (0.0-0.99) |
| `processor` | closure | `(delayed_signal) -> processed_signal` |

**Note:** The time parameter accepts signal-rate modulation for effects like wow/flutter:

```akkado
// Modulated delay time for tape-style wow/flutter
vocal |> tap_delay(@, 0.4 + sine(0.3) * 0.005, 0.6, (x) -> lp(x, 2000))
```

**Choosing the right variant:**
- `tap_delay` (seconds): Most intuitive for musical delays (e.g., 0.5 = half second)
- `tap_delay_ms` (milliseconds): Precise control for tight timing
- `tap_delay_smp` (samples): Sound design, comb filtering (e.g., 100 samples at 48kHz = 480Hz resonance)

### 2.3 Wet/Dry Mix

Wet/dry mixing is handled externally for flexibility:

```akkado
dry = saw(110)
wet = tap_delay(dry, 300, 0.7, (x) -> lp(x, 1000))
dry * 0.3 + wet * 0.7 |> out(@, @)
```

Or using a mix helper (if available):

```akkado
saw(110) as dry
|> tap_delay(@, 300, 0.7, (x) -> lp(x, 1000))
|> mix(dry, @, 0.7)  // 70% wet
|> out(@, @)
```

### 2.4 Advanced Examples

**Dub Delay** (classic reggae/dub sound):
```akkado
guitar_in |> tap_delay(@, 0.375, 0.75, (x) ->
    lp(x, 1500) |> hp(@, 100) |> saturate(@, 0.15)
) |> out(@, @)
```

**Shimmer Delay** (pitch-shifted feedback):
```akkado
pad |> tap_delay(@, 0.6, 0.5, (x) ->
    pitch_shift(x, 12) * 0.7 + x * 0.3  // Mix octave up with original
) |> out(@, @)
```

**Rhythmic Filter Delay** (modulated feedback filter):
```akkado
lead |> tap_delay(@, beat(0.5), 0.6, (x) ->
    lp(x, 500 + lfo("sin", 0.5) * 1500)  // Sweeping filter
) |> out(@, @)
```

**Degrading Delay** (lo-fi tape simulation):
```akkado
vocal |> tap_delay(@, 0.4, 0.65, (x) ->
    lp(x, 3000)
    |> bitcrush(@, 12, 0.8)  // Reduce bit depth
    |> saturate(@, 0.1)
) |> out(@, @)
```

**Comb Filter Resonance** (using sample-based delay):
```akkado
// Create pitched resonance from noise using short sample-based delay
noise() |> tap_delay_smp(@, 109, 0.95, (x) -> x)  // ~440Hz at 48kHz
|> out(@, @)
```

---

## 3. Compilation Strategy

### 3.1 AST Recognition

The compiler recognizes `tap_delay` calls with a closure as the 4th argument:

```
Call
├── Identifier("tap_delay")
├── args[0]: input expression
├── args[1]: time expression
├── args[2]: feedback expression
└── args[3]: Closure
    ├── Identifier("x")  // parameter
    └── body expression  // processing chain
```

### 3.2 Code Generation

`handle_tap_delay_call()` in codegen:

```cpp
std::uint16_t handle_tap_delay_call(NodeIndex call_node) {
    // 1. Compile input arguments
    std::uint16_t in_buf = visit(args[0]);
    std::uint16_t time_buf = visit(args[1]);
    std::uint16_t fb_buf = visit(args[2]);

    // 2. Compute shared state_id for TAP/WRITE pair
    std::uint32_t delay_state_id = compute_semantic_id("tap_delay");

    // 3. Emit DELAY_TAP
    std::uint16_t tap_out = allocate_buffer();
    emit(Opcode::DELAY_TAP, tap_out, {in_buf, time_buf}, delay_state_id);

    // 4. Inline closure body
    auto closure = get_closure(args[3]);
    symbols_->push_scope();

    // Bind closure parameter to tap output
    std::string param = get_param_name(closure, 0);
    symbols_->define_variable(param, tap_out);

    // Push semantic context for nested state_ids
    push_semantic_context("fb");

    // Compile body - produces instructions for feedback chain
    std::uint16_t processed = visit(closure_body(closure));

    pop_semantic_context();
    symbols_->pop_scope();

    // 5. Emit DELAY_WRITE
    std::uint16_t out_buf = allocate_buffer();
    emit(Opcode::DELAY_WRITE, out_buf, {in_buf, processed, fb_buf}, delay_state_id);

    return out_buf;
}
```

### 3.3 Semantic ID Derivation

State IDs for opcodes in the feedback chain are derived from the parent delay's context:

```
tap_delay semantic path: "main/synth/tap_delay"
  → DELAY_TAP state_id:  hash("main/synth/tap_delay")
  → lp state_id:         hash("main/synth/tap_delay/fb/lp")
  → saturate state_id:   hash("main/synth/tap_delay/fb/saturate")
  → DELAY_WRITE state_id: hash("main/synth/tap_delay")  // same as TAP
```

This ensures:
- Hot-swap preserves filter/effect state in feedback chain
- Multiple tap_delays have independent feedback chain states
- Nested structures work correctly

### 3.4 Example Compilation

**Input:**
```akkado
saw(220) |> tap_delay(@, 300, 0.6, (x) -> lp(x, 800) |> saturate(@, 0.3)) |> out(@, @)
```

**Output Bytecode:**
```
0: OSC_SAW      out=0, freq=const[220]
1: DELAY_TAP    out=1, in=[0, const[300]], state=0xA1B2C3D4
2: FILTER_SVF_LP out=2, in=[1, const[800]], state=0xE5F6A7B8
3: DISTORT_TANH out=3, in=[2, const[0.3]], state=0xC9D0E1F2
4: DELAY_WRITE  out=4, in=[0, 3, const[0.6]], state=0xA1B2C3D4
5: OUTPUT       in=[4, 4]
```

Note: DELAY_TAP and DELAY_WRITE share state `0xA1B2C3D4`.

### 3.5 Signal Flow

```
                    ┌─────────────────────────────────────────────────────┐
                    │                  Delay Buffer                        │
                    │                  (shared state)                      │
                    └──────────────────────┬──────────────────────────────┘
                                           │
        ┌──────────────────────────────────┴───────────────────────────────┐
        │                                                                   │
        ▼                                                                   │
   ┌─────────┐     ┌─────────────────────────┐     ┌───────────┐           │
   │DELAY_TAP│────▶│  Closure (fb chain)     │────▶│DELAY_WRITE│───────────┘
   │read only│     │  lp() -> saturate()     │     │write back │
   └────┬────┘     └─────────────────────────┘     └─────┬─────┘
        │                                                │
        │              delayed signal                    │  output = tap_cache
        └────────────────────────────────────────────────┼─▶ (delayed signal)
                                                         │
                    write: input + processed * fb ───────┘
```

---

## 4. Cedar VM Implementation

### 4.1 New Opcodes

Add to `instruction.hpp` in the Delays & Reverbs range (70-79):

```cpp
enum class Opcode : std::uint8_t {
    // ... existing opcodes ...

    // Delays & Reverbs (70-79)
    DELAY = 70,
    REVERB_FREEVERB = 71,
    REVERB_DATTORRO = 72,
    REVERB_FDN = 73,
    DELAY_TAP = 74,    // Read from delay line (coordinated with DELAY_WRITE)
    DELAY_WRITE = 75,  // Write feedback to delay line (coordinated with DELAY_TAP)
};
```

### 4.2 Extend DelayState

Extend existing `DelayState` in `dsp_state.hpp` with tap coordination fields:

```cpp
struct DelayState {
    // Maximum delay time: 10 seconds at 96kHz = 960000 samples
    static constexpr std::size_t MAX_DELAY_SAMPLES = 960000;

    // Ring buffer (allocated from arena)
    float* buffer = nullptr;
    std::size_t buffer_size = 0;    // Allocated size in floats
    std::size_t write_pos = 0;

    // Tap delay coordination (used by DELAY_TAP/DELAY_WRITE pair)
    std::array<float, 128> tap_cache{};  // BLOCK_SIZE cached delayed samples

    // Ensure buffer is allocated with requested size
    // arena: AudioArena to allocate from (from ExecutionContext)
    void ensure_buffer(std::size_t samples, AudioArena* arena) {
        if (buffer && buffer_size >= samples) {
            return;  // Already have enough space
        }
        if (!arena) return;

        std::size_t new_size = std::min(samples, MAX_DELAY_SAMPLES);
        float* new_buffer = arena->allocate(new_size);
        if (new_buffer) {
            buffer = new_buffer;
            buffer_size = new_size;
            write_pos = 0;
        }
    }

    // Reset buffer to silence (for seek)
    void reset() {
        if (buffer && buffer_size > 0) {
            std::memset(buffer, 0, buffer_size * sizeof(float));
            write_pos = 0;
        }
        tap_cache.fill(0.0f);
    }
};
```

**Note:** No new DSPState variant entry is needed since we're extending the existing `DelayState`.

### 4.3 DELAY_TAP Implementation

```cpp
[[gnu::always_inline]]
inline void op_delay_tap(ExecutionContext& ctx, const Instruction& inst) {
    auto& state = ctx.states->get_or_create<DelayState>(inst.state_id);

    // Calculate buffer size based on sample rate (same pattern as op_delay)
    float max_delay_ms = 10000.0f;
    std::size_t max_samples = static_cast<std::size_t>(max_delay_ms * 0.001f * ctx.sample_rate) + 1;
    max_samples = std::min(max_samples, DelayState::MAX_DELAY_SAMPLES);
    state.ensure_buffer(max_samples, ctx.arena);

    if (!state.buffer) {
        // Buffer allocation failed, output silence
        float* output = ctx.buffers->get(inst.out_buffer);
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            output[i] = 0.0f;
        }
        return;
    }

    const float* time_ms = ctx.buffers->get(inst.inputs[1]);
    float* output = ctx.buffers->get(inst.out_buffer);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Convert delay time to samples
        float delay_samples = time_ms[i] * 0.001f * ctx.sample_rate;
        delay_samples = std::clamp(delay_samples, 0.0f, static_cast<float>(state.buffer_size - 2));

        // Integer and fractional parts for linear interpolation
        std::size_t delay_int = static_cast<std::size_t>(delay_samples);
        float delay_frac = delay_samples - static_cast<float>(delay_int);

        // Calculate read positions (circular buffer)
        std::size_t read_pos1 = (state.write_pos + state.buffer_size - delay_int) % state.buffer_size;
        std::size_t read_pos2 = (read_pos1 + state.buffer_size - 1) % state.buffer_size;

        // Linear interpolation for fractional delay
        float delayed = state.buffer[read_pos1] * (1.0f - delay_frac) +
                       state.buffer[read_pos2] * delay_frac;

        output[i] = delayed;
        state.tap_cache[i] = delayed;  // Cache for DELAY_WRITE output
    }
}
```

### 4.4 DELAY_WRITE Implementation

```cpp
[[gnu::always_inline]]
inline void op_delay_write(ExecutionContext& ctx, const Instruction& inst) {
    auto& state = ctx.states->get_or_create<DelayState>(inst.state_id);

    // State should already be initialized by DELAY_TAP
    if (!state.buffer) {
        float* output = ctx.buffers->get(inst.out_buffer);
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            output[i] = 0.0f;
        }
        return;
    }

    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* feedback_in = ctx.buffers->get(inst.inputs[1]);
    const float* feedback_amt = ctx.buffers->get(inst.inputs[2]);
    float* output = ctx.buffers->get(inst.out_buffer);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Clamp feedback to prevent runaway
        float fb = std::clamp(feedback_amt[i], 0.0f, 0.99f);

        // Output: delayed signal (from tap cache)
        output[i] = state.tap_cache[i];

        // Write to delay: input + processed_feedback * fb
        state.buffer[state.write_pos] = input[i] + feedback_in[i] * fb;
        state.write_pos = (state.write_pos + 1) % state.buffer_size;
    }
}
```

### 4.5 VM Dispatch

Add cases in `vm.cpp`:

```cpp
case Opcode::DELAY_TAP:
    op_delay_tap(ctx, inst);
    break;
case Opcode::DELAY_WRITE:
    op_delay_write(ctx, inst);
    break;
```

---

## 5. Akkado Compiler Changes

### 5.1 Builtins Registration

Add to `builtins.hpp`:

```cpp
// Tap delay with configurable feedback processing
// Note: This is a pseudo-entry for signature help. Actual handling is in codegen
// via special_handlers, not a simple opcode emit.
{"tap_delay", {cedar::Opcode::DELAY_TAP,  // Primary opcode (for requires_state check)
               4,                          // Required args: in, time, fb, processor
               0,                          // Optional args
               true,                       // requires_state
               {"in", "time", "fb", "processor", "", ""},
               {NAN, NAN, NAN, NAN, NAN},
               "Tap delay with configurable feedback processing"}},
```

### 5.2 Special Call Handling

Register in `special_handlers` map in `codegen.cpp`:

```cpp
static const std::unordered_map<std::string_view, Handler> special_handlers = {
    // ... existing handlers ...
    {"tap_delay", &CodeGenerator::handle_tap_delay_call},
};
```

Add handler declaration to `codegen.hpp`:

```cpp
std::uint16_t handle_tap_delay_call(NodeIndex node, const Node& n);
```

### 5.3 Analyzer Validation

In `analyzer.cpp`, validate tap_delay calls:

```cpp
void Analyzer::check_tap_delay_call(NodeIndex node) {
    auto& args = get_call_args(node);

    if (args.size() != 4) {
        error("E301", "tap_delay requires 4 arguments: input, time, feedback, processor", node);
        return;
    }

    // Check 4th argument is a closure
    if (ast_.type(args[3]) != NodeType::Closure) {
        error("E302", "tap_delay processor must be a closure: (x) -> ...", args[3]);
        return;
    }

    // Check closure has exactly 1 parameter
    auto params = get_closure_params(args[3]);
    if (params.size() != 1) {
        error("E303", "tap_delay processor closure must have exactly 1 parameter", args[3]);
        return;
    }
}
```

---

## 6. Files to Modify

### Cedar (VM)
| File | Changes |
|------|---------|
| `cedar/include/cedar/vm/instruction.hpp` | Add `DELAY_TAP = 74`, `DELAY_WRITE = 75` opcodes |
| `cedar/include/cedar/opcodes/dsp_state.hpp` | Add `tap_cache` field to `DelayState` |
| `cedar/include/cedar/opcodes/delays.hpp` | Implement `op_delay_tap()`, `op_delay_write()` |
| `cedar/src/vm/vm.cpp` | Add opcode dispatch cases |
| `cedar/tests/test_vm.cpp` | Add unit tests |

### Akkado (Compiler)
| File | Changes |
|------|---------|
| `akkado/include/akkado/builtins.hpp` | Add tap_delay entry |
| `akkado/include/akkado/codegen.hpp` | Declare `handle_tap_delay_call()` |
| `akkado/src/codegen.cpp` | Register in `special_handlers`, implement handler |
| `akkado/src/analyzer.cpp` | Add validation for tap_delay calls |
| `akkado/tests/test_codegen.cpp` | Add compilation tests |

### Web & Metadata
| File | Changes |
|------|---------|
| `web/static/docs/reference.md` | Document tap_delay |
| `web/scripts/build-docs.ts` | Include tap_delay in lookup index |
| Run `bun run build:opcodes` | Regenerate opcode metadata after adding opcodes |

---

## 7. Testing Strategy

### 7.1 Unit Tests (Cedar)

```cpp
TEST_CASE("DELAY_TAP and DELAY_WRITE coordination") {
    // Test that TAP reads correctly
    // Test that WRITE commits feedback
    // Test shared state between TAP/WRITE
}

TEST_CASE("Tap delay with filter in feedback") {
    // Compile and run: tap_delay(noise, 100, 0.8, (x) -> lp(x, 500))
    // Verify high frequencies decay faster than low
}

TEST_CASE("Tap delay state isolation") {
    // Two tap_delays should have independent states
    // Feedback chains should have independent filter states
}
```

### 7.2 Integration Tests (Akkado)

```cpp
TEST_CASE("tap_delay compilation") {
    auto code = R"(
        saw(220) |> tap_delay(@, 300, 0.6, (x) -> lp(x, 1000)) |> out(@, @)
    )";

    auto program = compile(code);

    // Verify instruction sequence
    REQUIRE(has_opcode(program, Opcode::OSC_SAW));
    REQUIRE(has_opcode(program, Opcode::DELAY_TAP));
    REQUIRE(has_opcode(program, Opcode::FILTER_SVF_LP));
    REQUIRE(has_opcode(program, Opcode::DELAY_WRITE));
}

TEST_CASE("tap_delay semantic IDs") {
    // Verify nested opcodes get derived state IDs
    // Verify hot-swap preserves feedback chain state
}
```

### 7.3 Audio Tests (Python Experiments)

```python
def test_tap_delay_frequency_response():
    """
    Verify feedback filter affects frequency content of repeats.

    Test: White noise through tap_delay with lp(1000) in feedback
    Expected: Each repeat should have progressively less high frequency content

    If this test fails, check the implementation in cedar/include/cedar/opcodes/delays.hpp
    """
    import numpy as np
    import scipy.io.wavfile as wavfile

    # Generate test signal (short burst of white noise)
    sr = 48000
    duration = 2.0  # seconds
    noise_burst_len = int(0.1 * sr)  # 100ms burst

    input_signal = np.zeros(int(duration * sr), dtype=np.float32)
    input_signal[:noise_burst_len] = np.random.randn(noise_burst_len).astype(np.float32) * 0.5

    # Run through tap_delay with lowpass feedback
    output = run_tap_delay(input_signal, delay_ms=300, feedback=0.7, cutoff_hz=1000)

    # Save WAV for human evaluation
    wav_path = "output/tap_delay_lpf_feedback.wav"
    wavfile.write(wav_path, sr, output)
    print(f"  Saved {wav_path} - Listen for progressively duller repeats")

    # Analyze frequency content of successive repeats
    repeat_times = [0.3, 0.6, 0.9, 1.2]  # Expected repeat times in seconds
    high_freq_energy = []

    for t in repeat_times:
        start = int(t * sr)
        end = start + noise_burst_len
        if end > len(output):
            break

        segment = output[start:end]
        fft = np.abs(np.fft.rfft(segment))
        freqs = np.fft.rfftfreq(len(segment), 1/sr)

        # Measure energy above 2kHz
        high_mask = freqs > 2000
        high_energy = np.sum(fft[high_mask]**2)
        high_freq_energy.append(high_energy)

    # Verify high frequency content decreases with each repeat
    if len(high_freq_energy) >= 2:
        decreasing = all(high_freq_energy[i] > high_freq_energy[i+1]
                        for i in range(len(high_freq_energy)-1))
        if decreasing:
            print(f"  ✓ PASS: High frequency energy decreases with each repeat")
        else:
            print(f"  ✗ FAIL: High frequency energy not decreasing - Check filter implementation")
            print(f"    Energy values: {high_freq_energy}")
```

### 7.4 Manual Verification

1. **Web app**: Load tap_delay example, verify audio output
2. **Hot-swap**: Modify feedback filter cutoff, verify smooth transition
3. **State inspector**: Verify filter states visible in debug panel

---

## 8. Future Extensions

### 8.1 Multi-Tap Delay

```akkado
// Multiple taps with independent processing
multi_tap(input, [
    {time: 200, fb: 0.5, proc: (x) -> lp(x, 2000)},
    {time: 400, fb: 0.4, proc: (x) -> hp(x, 500)},
    {time: 600, fb: 0.3, proc: (x) -> x}  // Clean tap
])
```

### 8.2 Stereo Tap Delay

```akkado
// Ping-pong with different processing per channel
stereo_tap_delay(input, 300, 0.6,
    (l) -> lp(l, 1000),  // Left feedback
    (r) -> hp(r, 500)    // Right feedback
)
```

### 8.3 Feedback Amount Modulation

```akkado
// Feedback amount from envelope or LFO
tap_delay(input, 300, env(trig, 0.8, 0.2), (x) -> lp(x, 1000))
```

### 8.4 Tempo-Synced Tap Delay

```akkado
// Delay time in beats
tap_delay_sync(input, beat(0.25), 0.6, (x) -> lp(x, 1000))
```

---

## 9. Implementation Checklist

### Phase 1: Cedar VM
- [ ] Add DELAY_TAP = 74, DELAY_WRITE = 75 to Opcode enum
- [ ] Add tap_cache field to DelayState struct
- [ ] Implement op_delay_tap()
- [ ] Implement op_delay_write()
- [ ] Add VM dispatch cases
- [ ] Run `bun run build:opcodes` to regenerate opcode metadata
- [ ] Unit tests for opcodes

### Phase 2: Akkado Compiler
- [ ] Add tap_delay to builtins
- [ ] Register in special_handlers map
- [ ] Implement handle_tap_delay_call()
- [ ] Add analyzer validation
- [ ] Semantic ID derivation for nested opcodes
- [ ] Integration tests

### Phase 3: Documentation & Polish
- [ ] Add to web docs
- [ ] Run `bun run build:docs` to rebuild docs index
- [ ] Add example patterns
- [ ] Python audio verification tests with WAV output
