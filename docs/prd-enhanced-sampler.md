> **Status: NOT STARTED** — Sampler is mono-only, single-format, no bitdepth/SRC reduction, no loop points, minimal playback params (trigger, pitch, id).

# PRD: Enhanced Sampler — MPC-Style Degradation, Loop Points, and Extended Playback

## 1. Executive Summary

The current sampler in nkido plays back samples at full fidelity with only three parameters: trigger, pitch, and sample ID. This PRD proposes extending the sampler system into a full-featured sample engine that can:

1. **Reduce bitdepth and sample rate on load** — emulate the character of classic samplers (SP-1200, MPC60, S950, SP-404) or specify custom reduction parameters.
2. **Support multiple pitch/time algorithms** — varispeed (current), granular synthesis, and phase vocoder, selectable per-bank at load time. Granular parameters (grain size, overlap, window type, position jitter) are fully configurable.
3. **Add extended playback parameters** — pan, velocity/gain, start offset, and length/duration to the `sample()` builtin.
4. **Support loop points** — auto-detected from WAV cue/sampler chunks, user-overridable in code, with auto-looping and gate-controlled sustain loop modes.
5. **Store reduced variants alongside originals** — both the full-quality and reduced versions are kept as separate variant samples, selectable per-bank.

Key design decisions:
- **Keyword arguments on `samples()`**: `samples("uri", preset: "sp1200", bits: 12, sr: 26040)`
- **Built-in presets + user-extensible via dict spread**: once the `...spread` operator lands, users can do `preset = {sr: 8000, bits: 10}; samples("uri", ...preset)`
- **Extended `sample()` builtin** rather than a new function: `sample(trig, pitch, id, pan, vel, start, len)`
- **Reduced variants stored as separate samples** — original is never discarded, both coexist in the bank

---

## 2. Problem Statement

### 2.1 Current Behaviour vs Proposed

| Aspect | Current | Proposed |
|--------|---------|----------|
| **Bitdepth/SRC** | Full quality float, no reduction | Load-time reduction with named presets (SP-1200, MPC60, etc.) or custom params |
| **Sample storage** | Single version per sample | Original + reduced variants stored separately |
| **Pitch/time** | Varispeed only (pitch and time always linked) | Varispeed, granular, phase vocoder — selectable per-bank |
| **Playback params** | trigger, pitch, id | + pan, velocity/gain, start offset, length/duration |
| **Loop points** | `sample_loop()` loops entire sample | Loop points from WAV metadata, user-defined, auto/gate-controlled modes |
| **Granular config** | N/A | Grain size, overlap, window type, position jitter |
| **Preset extensibility** | N/A | Built-in presets + user-defined presets via dict spread |

### 2.2 Root Cause

The sampler was designed for drum machine use with minimal parameters. `SampleData` holds only raw float audio, `SamplerVoice` has no pan or velocity state, and `sample_play` has no length control. The `samples()` builtin only accepts a URI string with no load-time configuration.

### 2.3 Existing Infrastructure to Build On

| Component | Location | Reuse |
|-----------|----------|-------|
| `SampleData` struct | `cedar/include/cedar/vm/sample_bank.hpp:18` | Extend with loop points and variant metadata |
| `SampleBank` | `cedar/include/cedar/vm/sample_bank.hpp:82` | Add variant loading, WAV loop metadata parsing |
| `op_sample_play` | `cedar/include/cedar/opcodes/samplers.hpp:37` | Extend with pan, velocity, start, length, loop logic |
| `op_sample_play_loop` | `cedar/include/cedar/opcodes/samplers.hpp:165` | Merge into unified sampler or extend separately |
| `sample()` builtin | `akkado/include/akkado/builtins.hpp:315` | Extend parameter count and names |
| `samples()` codegen handler | `akkado/src/codegen_patterns.cpp:5545` | Extend to parse keyword args, build SampleLoadParams |
| `UriRequest` / `CodeGenResult` | `akkado/include/akkado/codegen.hpp:226` | Add `SampleLoadParams` to `UriRequest` or `CodeGenResult` |
| `SamplerVoice` / `SamplerState` | `cedar/include/cedar/opcodes/dsp_state.hpp:466` | Extend voice with pan, velocity, start, end, loop mode |

---

## 3. Goals and Non-Goals

### 3.1 Goals

- **G1**: `samples()` accepts keyword arguments: `preset`, `bits`, `sr`, `pitch_algo`, `granular_params`, `loop_mode`, `loop_start`, `loop_end`.
- **G2**: Named presets emulate classic samplers: `sp1200`, `mpc60`, `s950`, `sp404`. Each preset defines bitdepth, sample rate, and optional algorithm settings.
- **G3**: Both original and reduced samples are stored as separate variants in the bank. Original is never discarded.
- **G4**: `sample()` builtin accepts 7 parameters: `trig`, `pitch`, `id`, `pan`, `vel`, `start`, `len`.
- **G5**: Loop points are auto-detected from WAV cue/sampler chunks on load. Users can override loop points per-sample via keyword args.
- **G6**: Three pitch/time algorithms supported: varispeed (default), granular, phase vocoder. Algorithm selectable per-bank at load time.
- **G7**: Granular synthesis is fully configurable: grain size, overlap/spacing, window type (sine, hann, triangle, rect), position jitter.
- **G8**: Once the dict spread operator (`...spread`) lands, users can define custom presets as dicts and spread them into `samples()`.

### 3.2 Non-Goals

- **Real-time bitdepth reduction**: Bitdepth/SRC reduction is a load-time operation, not a per-sample-playback effect.
- **Per-trigger algorithm selection**: Algorithm is selected per-bank at load time, not per-sample call.
- **Per-trigger preset switching**: Preset is applied at load time; individual `sample()` calls reference the variant that was loaded.
- **Sample editing/recording**: No sample capture, trimming, or editing features.
- **Multi-sample zones / key mapping**: This is covered by the SoundFont system. This PRD focuses on single-sample playback enhancement.
- **Streaming from disk**: All samples are RAM-resident. No streaming or memory-mapped playback.
- **Multichannel beyond stereo**: Samples remain mono or stereo. No 5.1, Ambisonics, etc.
- **Dict spread operator implementation**: The `...spread` syntax is a separate language feature PRD. This PRD defines how presets will use it once it exists, but does not implement it.

---

## 4. Target Syntax and User Experience

### 4.1 Named Presets

```akkado
// SP-1200 character: 12-bit, 26.04kHz, varispeed pitch
samples("github:tidalcycles/Dirt-Samples", preset: "sp1200")
s"bd sd hh*8" |> out(@, @)

// MPC60: 12-bit, 40kHz
samples("github:tidalcycles/Dirt-Samples", preset: "mpc60")

// SP-404: 16-bit, 44.1kHz
samples("github:tidalcycles/Dirt-Samples", preset: "sp404")

// S950: 12-bit, 32kHz
samples("github:tidalcycles/Dirt-Samples", preset: "s950")
```

### 4.2 Manual Parameter Specification

```akkado
// Custom bitdepth and sample rate reduction
samples("my-bank", bits: 10, sr: 16000)

// Choose a different pitch/time algorithm
samples("my-bank", bits: 12, sr: 26040, pitch_algo: "granular")

// Granular synthesis with custom grain parameters
samples("my-bank",
  pitch_algo: "granular",
  grain_size: 0.050,      // 50ms grains
  grain_overlap: 4,       // 4x overlap
  grain_window: "hann",   // window function
  grain_jitter: 0.02      // 2% position jitter
)

// Phase vocoder (best quality, higher CPU)
samples("my-bank", pitch_algo: "phase_vocoder")

// Loop configuration
samples("my-bank", loop_mode: "auto")           // use WAV loop points if present
samples("my-bank", loop_mode: "sustain", loop_start: 1000, loop_end: 5000)

// Combined
samples("amen",
  preset: "sp1200",
  pitch_algo: "granular",
  grain_size: 0.030,
  loop_mode: "auto"
)
```

### 4.3 Extended sample() Playback

```akkado
// Current: sample(trig, pitch, id)
sample(trigger(4), 1.0, 1)

// Extended: sample(trig, pitch, id, pan, vel, start, len)
// All additional params are optional (default to full-range/whole-sample)
sample(trigger(4), 1.0, 1, pan: 0.5)          // pan: -1 (left) to 1 (right)
sample(trigger(4), 1.0, 1, vel: 0.8)          // velocity/gain: 0.0 to 1.0
sample(trigger(4), 1.0, 1, start: 0.25)       // start offset in seconds
sample(trigger(4), 1.0, 1, len: 0.5)          // max length in seconds

// All parameters combined
sample(trigger(4), 1.5, 1,
  pan: -0.3,
  vel: seq([0.5, 0.8, 1.0, 0.7]),
  start: 0.1,
  len: 0.2
)

// With pattern events (velocity from event, pan from pattern)
s"bd sd hh cp".pan("<-1 0 1 0>") |> sample(@, @, @, pan: @.pan)
```

### 4.4 User-Defined Presets (Future: requires dict spread operator)

```akkado
// Define a custom preset as a dict
my_gritty = {
  bits: 8,
  sr: 12000,
  pitch_algo: "granular",
  grain_size: 0.020,
  grain_overlap: 2,
  grain_window: "rect",
  grain_jitter: 0.05,
}

// Spread into samples()
samples("my-bank", ...my_gritty)

// Combine named preset with overrides
samples("my-bank", preset: "sp1200", bits: 8, grain_jitter: 0.1)
```

### 4.5 Loop Modes

```akkado
// Auto: use WAV loop points if present, otherwise one-shot
samples("loops", loop_mode: "auto")

// Force one-shot (ignore WAV loop points)
samples("loops", loop_mode: "oneshot")

// Sustain: loop while gate is high, release on gate off (uses loop_start/loop_end)
samples("sustained", loop_mode: "sustain", loop_start: 500, loop_end: 4500)

// Continuous: loop forever regardless of gate
samples("drone", loop_mode: "continuous", loop_start: 0, loop_end: 1000)
```

---

## 5. Architecture / Technical Design

### 5.1 Data Structures

#### 5.1.1 SampleLoadParams (compile-time)

Passed from codegen to the host to configure how samples are loaded:

```cpp
enum class PitchAlgo {
    Varispeed,      // Current: resample at different speed (pitch+time linked)
    Granular,       // Granular synthesis (independent pitch/time)
    PhaseVocoder,   // Phase vocoder (highest quality, highest CPU)
};

enum class LoopMode {
    Auto,           // Use WAV loop points if present, else one-shot
    OneShot,        // Never loop
    Sustain,        // Loop while gate high, release on gate off
    Continuous,     // Always loop
};

enum class GrainWindow {
    Sine,
    Hann,
    Triangle,
    Rectangular,
};

struct GranularParams {
    float grain_size   = 0.050f;  // seconds
    int   grain_overlap = 4;
    GrainWindow grain_window = GrainWindow::Hann;
    float grain_jitter = 0.0f;   // 0.0 - 1.0
};

struct SampleLoadParams {
    std::string preset;             // Named preset (empty = manual params)
    int         bits       = 0;     // 0 = no reduction
    float       sr         = 0.0f;  // 0 = no reduction
    PitchAlgo   pitch_algo = PitchAlgo::Varispeed;
    GranularParams granular;
    LoopMode    loop_mode  = LoopMode::Auto;
    uint32_t    loop_start = 0;     // in frames, 0 = auto-detect
    uint32_t    loop_end   = 0;     // in frames, 0 = auto-detect
};
```

#### 5.1.2 SampleVariant (runtime)

A reduced version of a sample stored alongside the original:

```cpp
struct SampleVariant {
    uint32_t variant_id;            // Unique ID for this variant
    SampleLoadParams params;        // How this variant was created
    std::vector<float> data;        // Interleaved audio data
    uint32_t channels = 1;
    float    sample_rate = 48000.0f;
    uint32_t frames = 0;
    uint32_t loop_start = 0;        // 0 = no loop
    uint32_t loop_end   = 0;
};
```

#### 5.1.3 Extended SampleData

The existing `SampleData` struct gains variant and loop metadata:

```cpp
struct SampleData {
    std::vector<float> data;
    uint32_t channels = 1;
    float    sample_rate = 48000.0f;
    uint32_t frames = 0;

    // NEW: Loop points (0 = no loop, auto-detected from WAV)
    uint32_t loop_start = 0;
    uint32_t loop_end   = 0;

    // NEW: Variants (reduced versions stored separately)
    std::vector<SampleVariant> variants;

    // ... existing get(), get_interpolated(), get_interpolated_looped() ...
};
```

#### 5.1.4 Extended SamplerVoice

```cpp
struct SamplerVoice {
    float position = 0.0f;
    float speed = 1.0f;
    uint32_t sample_id = 0;
    bool active = false;

    // NEW: Playback parameters
    float pan = 0.0f;               // -1.0 to 1.0
    float velocity = 1.0f;          // 0.0 to 1.0 (gain multiplier)
    float start_offset = 0.0f;      // in frames, where playback starts
    float end_frame = 0.0f;         // in frames, where playback stops (0 = end of sample)
    float max_frames = 0.0f;        // max frames to play (for length parameter)
    uint32_t variant_id = 0;        // Which variant to use (0 = original)

    // NEW: Loop state
    LoopMode loop_mode = LoopMode::OneShot;
    uint32_t loop_start = 0;
    uint32_t loop_end   = 0;
    bool gate_active = false;       // For sustain mode

    // Anti-click envelope (existing)
    uint8_t attack_counter = 0;
    bool fading_out = false;
    uint8_t fadeout_counter = 0;
};
```

### 5.2 System Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Akkado Source Code                           │
│                                                                     │
│  samples("uri", preset: "sp1200", pitch_algo: "granular")           │
│  s"bd sd" |> sample(@, @, @, pan: 0.5, vel: 0.8)                │
└──────────────────────────────┬──────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────────┐
│                     Code Generator (C++)                             │
│                                                                     │
│  handle_samples_call() ──► Parse keyword args                       │
│    └─► Build SampleLoadParams                                       │
│        └─► Attach to UriRequest or CodeGenResult                    │
│                                                                     │
│  handle_sample_call() ──► Wire 7 params to SAMPLE_PLAY              │
│    └─► trig, pitch, id, pan, vel, start, len                        │
└──────────────────────────────┬──────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────────┐
│                       Host (Web / CLI)                               │
│                                                                     │
│  1. Fetch bank via URI resolver                                     │
│  2. For each sample:                                                │
│     a. Decode to full-quality float (original)                      │
│     b. Parse WAV cue/sampler chunks for loop points                 │
│     c. Apply bitdepth/SRC reduction → create SampleVariant          │
│     d. Store original + variant(s) in SampleBank                    │
│  3. Register bank, swap bytecode                                    │
└──────────────────────────────┬──────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────────┐
│                    Cedar VM (Audio Thread)                           │
│                                                                     │
│  SAMPLE_PLAY:                                                       │
│    └─► Read 7 input buffers (trig, pitch, id, pan, vel, start, len) │
│        ├─► On trigger: allocate voice with params                   │
│        │   ├─► Apply start offset                                   │
│        │   ├─► Apply velocity as gain                               │
│        │   ├─► Apply pan (constant-power panning)                   │
│        │   ├─► Select variant (original or reduced)                 │
│        │   └─► Configure loop mode/points                           │
│        ├─► Per-sample:                                              │
│        │   ├─► Pitch algorithm (varispeed/granular/phase vocoder)   │
│        │   ├─► Loop point enforcement                               │
│        │   └─► Length enforcement (stop at max_frames)              │
│        └─► Mix voices to stereo output                              │
└─────────────────────────────────────────────────────────────────────┘
```

### 5.3 Bitdepth/SRC Reduction Pipeline

```
Original WAV (24-bit, 48kHz)
    │
    ▼
┌─────────────────────────┐
│ 1. Dither + Truncate    │  bits > 0: quantize to N bits with TPDF dither
│    (if bits > 0)        │  Presets: SP-1200=12, MPC60=12, S950=12, SP-404=16
└───────────┬─────────────┘
            ▼
┌─────────────────────────┐
│ 2. Resample             │  sr > 0: resample to target rate
│    (if sr > 0)          │  Linear interpolation for downsampling
│                         │  (sufficient for intentional lo-fi character)
└───────────┬─────────────┘
            ▼
┌─────────────────────────┐
│ 3. Store as Variant     │  New SampleVariant with reduced params
│    (never discard orig) │  Original SampleData unchanged
└─────────────────────────┘
```

### 5.4 Pitch/Time Algorithm Selection

#### 5.4.1 Varispeed (default, current)
- Pitch and time are linked: `speed = pitch_ratio`
- Linear interpolation resampling
- Zero additional CPU beyond current implementation
- `sample_rate / ctx.sample_rate` ratio applied to position advancement

#### 5.4.2 Granular Synthesis
- Break audio into overlapping grains, play at original pitch, rearrange for time-stretch
- Parameters per-bank at load time:
  - `grain_size`: 0.005 - 0.200 seconds (default 0.050)
  - `grain_overlap`: 1 - 8x (default 4)
  - `grain_window`: sine, hann, triangle, rect (default hann)
  - `grain_jitter`: 0.0 - 1.0 random position offset (default 0.0)
- Granular state stored in `GranularState` (extended params pool)

#### 5.4.3 Phase Vocoder
- FFT-based time-stretching with phase correction
- Highest quality for large time-stretch ratios
- Significantly higher CPU cost
- Window size and hop size derived from granular params

### 5.5 Pan Algorithm

Constant-power panning:

```cpp
float pan_angle = (pan + 1.0f) * PI / 4.0f;  // -1→0, 0→π/4, 1→π/2
float left_gain  = cosf(pan_angle);
float right_gain = sinf(pan_angle);
```

### 5.6 Loop Point Detection

On WAV load, parse the following chunks in order:

1. **`smpl` chunk** (standard sampler metadata): loop start, end, type (forward, pingpong, etc.)
2. **`cue` chunk + `plist` chunk**: cue points that may mark loop boundaries
3. **`inst` chunk**: simple root note and gain, less commonly has loop points

If multiple sources exist, `smpl` chunk takes priority.

### 5.7 Extended sample() Builtin Signature

```cpp
{"sample", {cedar::Opcode::SAMPLE_PLAY, 3, 4, true,
             {"trig", "pitch", "id", "pan", "vel", "start", "len"},
             {NAN, NAN, NAN, 0.0f, 1.0f, 0.0f, 0.0f},  // defaults
             "Sample playback with pan, velocity, start offset, length"}},
```

- `pan`: -1.0 (full left) to 1.0 (full right), default 0.0 (center)
- `vel`: 0.0 to 1.0 (gain multiplier), default 1.0
- `start`: offset in seconds from sample start, default 0.0
- `len`: max playback duration in seconds, default 0.0 (no limit)

---

## 6. Impact Assessment

| Component | Status | Notes |
|-----------|--------|-------|
| `SampleData` | **Modified** | Add `loop_start`, `loop_end`, `variants` vector |
| `SampleBank` | **Modified** | Add variant loading, WAV loop metadata parsing |
| `SamplerVoice` | **Modified** | Add pan, velocity, start, end, variant_id, loop state |
| `SamplerState` | **Modified** | Granular state pool allocation |
| `op_sample_play` | **Modified** | Stereo output, 7 params, loop logic, algorithm dispatch |
| `op_sample_play_loop` | **Modified** or **Merged** | Unified with one-shot or extended separately |
| `sample()` builtin | **Modified** | 3 → 7 parameters with defaults |
| `samples()` codegen | **Modified** | Parse keyword args, build `SampleLoadParams` |
| `UriRequest` | **Modified** | Add `SampleLoadParams*` optional field |
| `BuiltinSignature` | **Modified** | Update `sample()` param names and defaults |
| `audio.svelte.ts` | **Modified** | Pass `SampleLoadParams` when loading banks |
| `nkido_wasm.cpp` | **Modified** | WASM bridge for extended sample loading |
| `nkido` | **Modified** | CLI support for extended sample params |
| `test_sample_registry.cpp` | **Modified** | Tests for variant loading |
| `test_codegen.cpp` | **Modified** | Tests for extended sample() builtin |
| `samplers.hpp` tests | **New** | Tests for pan, velocity, loops, algorithms |
| WAV loader | **Modified** | Parse `smpl`, `cue`, `inst` chunks for loop points |

### 6.1 Components with NO Changes

| Component | Reason |
|-----------|--------|
| `MiniSampleData` | Pattern-level sample references unchanged |
| `SampleRegistry` | Name→ID mapping unchanged |
| `SeqPat` / pattern system | Pattern compilation unchanged, just wires more params |
| `SamplePack` | Drum kit loading unchanged |
| `SoundFont` system | Independent sample engine |

---

## 7. File-Level Changes

| File | Change |
|------|--------|
| `cedar/include/cedar/vm/sample_bank.hpp` | Add `SampleVariant` struct, `SampleData::variants`, `load_variant()`, WAV loop metadata parsing (`parse_smpl_chunk()`, `parse_cue_chunk()`) |
| `cedar/include/cedar/opcodes/dsp_state.hpp` | Extend `SamplerVoice` with pan, velocity, start, end, variant_id, loop state |
| `cedar/include/cedar/opcodes/samplers.hpp` | Rewrite `op_sample_play` with 7 inputs, stereo output, loop logic, algorithm dispatch. Add granular synthesis inner loop. Extend or merge `op_sample_play_loop`. |
| `cedar/include/cedar/vm/instruction.hpp` | Add `GRANULAR` opcode (if granular needs dedicated state/opcodes) |
| `cedar/include/cedar/vm/context.hpp` | May need granular state pool access |
| `cedar/include/cedar/audio/wav_loader.hpp` | Add loop point extraction: `parse_smpl_chunk()`, `parse_cue_chunk()`, `parse_inst_chunk()` |
| `cedar/src/audio/wav_loader.cpp` | Implement loop point parsing |
| `cedar/bindings/bindings.cpp` | Add new opcodes/enums to Python bindings |
| `akkado/include/akkado/builtins.hpp` | Extend `sample()` signature to 7 params with defaults |
| `akkado/include/akkado/codegen.hpp` | Add `SampleLoadParams`, `PitchAlgo`, `LoopMode`, `GranularParams`, `GranularWindow` enums/structs. Add to `UriRequest` or `CodeGenResult`. |
| `akkado/src/codegen_patterns.cpp` | Extend `handle_samples_call()` to parse keyword args. Wire 7 params in `sample()` codegen path. |
| `akkado/src/codegen.cpp` | Wire extended params in special handlers |
| `akkado/tests/test_codegen.cpp` | Add tests for extended `sample()` codegen, `samples()` with keyword args |
| `akkado/tests/test_sample_registry.cpp` | Tests for variant loading and loop point detection |
| `web/src/lib/stores/audio.svelte.ts` | Pass `SampleLoadParams` from compile result to sample loading |
| `web/wasm/nkido_wasm.cpp` | Extended sample loading API, variant registration |
| `tools/nkido/asset_loader.cpp` | CLI support for extended sample params |
| `tools/nkido/main.cpp` | Parse extended sample CLI options |
| `web/static/docs/reference/builtins/samples-loading.md` | Document extended `samples()` parameters |
| `docs/sampler_usage.md` | Update with new features |

---

## 8. Implementation Phases

### Phase 1 — Data Structure Extensions and WAV Loop Parsing (3-4 days)

**Goal**: Extend core data structures and add WAV loop metadata parsing.

**Files to modify**:
- `cedar/include/cedar/vm/sample_bank.hpp` — `SampleVariant`, `SampleData::variants`, `load_variant()`
- `cedar/include/cedar/vm/sample_bank.hpp` — `loop_start`, `loop_end` on `SampleData`
- `cedar/include/cedar/audio/wav_loader.hpp` — loop parsing function declarations
- `cedar/src/audio/wav_loader.cpp` — `parse_smpl_chunk()`, `parse_cue_chunk()`, `parse_inst_chunk()`
- `cedar/include/cedar/opcodes/dsp_state.hpp` — extend `SamplerVoice`

**Verification**:
- Unit tests: load a WAV with `smpl` chunk → `loop_start`/`loop_end` populated
- Unit tests: load a WAV without loop data → `loop_start == loop_end == 0`
- `SampleVariant` can be created and stored alongside original

### Phase 2 — Bitdepth/SRC Reduction Pipeline (3-4 days)

**Goal**: Implement load-time reduction and variant storage.

**Files to modify**:
- `akkado/include/akkado/codegen.hpp` — `SampleLoadParams`, enums
- `akkado/include/akkado/codegen.hpp` — extend `UriRequest` with `SampleLoadParams`
- `akkado/src/codegen_patterns.cpp` — extend `handle_samples_call()` for keyword args
- `cedar/include/cedar/vm/sample_bank.hpp` — `reduce_sample()` function
- `web/src/lib/stores/audio.svelte.ts` — pass params to loading
- `web/wasm/nkido_wasm.cpp` — extended loading API

**Verification**:
- `samples("uri", bits: 12, sr: 26040)` → bank has original + 12-bit/26kHz variant
- `samples("uri", preset: "sp1200")` → same reduction as above
- Memory: original sample data unchanged, variant data separate
- WASM: host receives params, applies reduction

### Phase 3 — Extended sample() Builtin (2-3 days)

**Goal**: Extend `sample()` to 7 parameters with defaults.

**Files to modify**:
- `akkado/include/akkado/builtins.hpp` — extend `sample()` signature
- `akkado/src/codegen_patterns.cpp` — wire 7 params in sample codegen
- `cedar/include/cedar/opcodes/samplers.hpp` — read 7 inputs in `op_sample_play`
- `cedar/include/cedar/opcodes/dsp_state.hpp` — voice carries pan/vel/start/end

**Verification**:
- `sample(trig, pitch, id)` still works (defaults applied)
- `sample(trig, pitch, id, pan: 0.5)` → constant-power panning
- `sample(trig, pitch, id, vel: 0.5)` → 50% gain
- `sample(trig, pitch, id, start: 0.5)` → skips first 0.5s
- `sample(trig, pitch, id, len: 0.2)` → stops after 0.2s
- All params combined work together

### Phase 4 — Loop Point Playback (2-3 days)

**Goal**: Loop modes enforced in sample playback.

**Files to modify**:
- `cedar/include/cedar/opcodes/samplers.hpp` — loop logic in voice processing
- `cedar/include/cedar/opcodes/dsp_state.hpp` — loop state on `SamplerVoice`
- `akkado/include/akkado/codegen.hpp` — `LoopMode` enum, wire to load params

**Verification**:
- `loop_mode: "auto"` with WAV loop points → loops between start/end
- `loop_mode: "sustain"` → loops while gate high, releases on gate off
- `loop_mode: "oneshot"` → ignores loop points, plays to end
- Loop boundaries enforced with crossfade (no clicks at loop point)

### Phase 5 — Granular Synthesis (5-7 days)

**Goal**: Granular pitch/time algorithm with configurable parameters.

**Files to modify**:
- `cedar/include/cedar/opcodes/samplers.hpp` — granular inner loop
- `cedar/include/cedar/opcodes/dsp_state.hpp` — `GranularState`
- `akkado/include/akkado/codegen.hpp` — `GranularParams`, `GrainWindow`
- `akkado/src/codegen_patterns.cpp` — wire granular params to load request

**Verification**:
- `pitch_algo: "granular"` → time-stretch without pitch change
- Grain size changes audible character
- Grain overlap changes smoothness
- Window type affects grain crossfade character
- Position jitter adds randomness
- No clicks or artifacts at normal settings

### Phase 6 — Phase Vocoder (4-5 days)

**Goal**: FFT-based phase vocoder algorithm.

**Files to modify**:
- `cedar/include/cedar/opcodes/samplers.hpp` — phase vocoder inner loop
- `cedar/include/cedar/vm/instruction.hpp` — new opcode if needed
- Algorithm selection dispatch in `op_sample_play`

**Verification**:
- `pitch_algo: "phase_vocoder"` → higher quality time-stretch
- Phase coherence maintained across grains
- CPU usage documented and acceptable

### Phase 7 — Stereo Output (1-2 days)

**Goal**: `sample()` outputs stereo with panning.

**Files to modify**:
- `cedar/include/cedar/opcodes/samplers.hpp` — stereo output buffer pair
- `akkado/src/codegen_patterns.cpp` — stereo buffer allocation for sample()
- `akkado/src/codegen_stereo.cpp` — mark sample as stereo-producing

**Verification**:
- `sample(...)` produces stereo output
- Pan at center: equal L/R
- Pan at extremes: full signal on one side
- Mono samples duplicated correctly to stereo

### Phase 8 — Integration, Docs, and Polish (2-3 days)

**Files to modify**:
- All test files — integration tests
- `web/static/docs/reference/builtins/samples-loading.md`
- `docs/sampler_usage.md`
- `docs/mini-notation-reference.md` (if sample patterns gain new params)

**Verification**:
- End-to-end: `samples("uri", preset: "sp1200")` + `sample(@, @, @, pan: ...)` → correct audio
- CLI and web both support all features
- DSP experiments in `experiments/` validate audio quality

---

## 9. Edge Cases

### 9.1 Sample Loading

| Input | Expected Behavior |
|-------|-------------------|
| `samples("uri", bits: 0, sr: 0)` | No reduction. Original only, no variant created. |
| `samples("uri", bits: 24)` | Bitdepth reduction only, no SRC. |
| `samples("uri", sr: 22050)` | SRC only, no bitdepth reduction. |
| `samples("uri", preset: "unknown")` | Compile error E233: "unknown preset 'unknown'. Valid: sp1200, mpc60, s950, sp404" |
| `samples("uri", bits: 3)` | Compile error: bitdepth must be 8-32. |
| `samples("uri", sr: 100)` | Compile error: sample rate must be ≥ 1000 Hz. |
| `samples("uri", bits: 12, preset: "sp1200")` | Preset applied, then `bits: 12` overrides preset's bitdepth. |
| Same URI loaded twice with different params | Two separate banks loaded (different variant sets). Second bank gets a unique name or replaces first. **[OPEN QUESTION]** |

### 9.2 Playback Parameters

| Input | Expected Behavior |
|-------|-------------------|
| `pan: 2.0` | Clamped to 1.0. |
| `vel: -0.5` | Clamped to 0.0 (silence). |
| `start: 10.0` on a 2s sample | No output (start beyond end). |
| `len: 0.0` (default) | No length limit — plays to natural end or loop. |
| `len: 0.1` on a looping sample | Plays 0.1s then stops (length overrides loop). |
| `start: 0.5, len: 0.3` | Plays frames from 0.5s to 0.8s only. |
| `start + len` exceeds sample duration | Clamped to sample end. |
| `start` inside loop region | Loop region still enforced; wraps to `loop_start`. |

### 9.3 Loop Points

| Input | Expected Behavior |
|-------|-------------------|
| WAV with `smpl` chunk loops | `loop_mode: "auto"` uses them. `loop_mode: "oneshot"` ignores them. |
| WAV with `cue` points but no `smpl` | `cue` points used as fallback for `auto` mode. |
| No loop metadata, `loop_mode: "auto"` | Behaves as `oneshot`. |
| `loop_start >= loop_end` | Loop disabled, behaves as `oneshot`. Warning W234. |
| `loop_mode: "sustain"` with one-shot trigger | Gate is a single-sample pulse → plays once through loop region then stops. |
| Gate goes off during sustain loop | Fadeout applied (existing anti-click), voice deactivates. |
| Loop at very start of sample (loop_start = 0) | Valid. Loops from beginning to `loop_end`. |

### 9.4 Granular Synthesis

| Input | Expected Behavior |
|-------|-------------------|
| `grain_size: 0.001` (1ms) | Very gritty, may produce artifacts. Valid but warned. |
| `grain_size: 0.500` (500ms) | Large grains, minimal time-stretch character. Valid. |
| `grain_overlap: 1` | No overlap, gaps between grains. Audible clicking expected. |
| `grain_overlap: 16` | Heavy overlap, smooth but CPU-intensive. Clamped to 8. |
| `grain_jitter: 1.0` | Maximum randomization. Extreme granular texture. |
| `grain_window: "rect"` | Rectangular window. Audible clicks between grains expected. |
| Short sample (< grain_size) | Grain clipped to sample length. No error. |

### 9.5 Algorithm Selection

| Input | Expected Behavior |
|-------|-------------------|
| `pitch_algo: "granular"` with varispeed pitch input | Granular algorithm ignores speed-based pitch. Pitch from separate param (future). **[OPEN QUESTION]** |
| `pitch_algo: "phase_vocoder"` with extreme time-stretch | Phase artifacts may appear. No error, best-effort rendering. |
| Algorithm not yet implemented | Compile error: "pitch_algo 'X' not supported in this build" |

---

## 10. Testing / Verification Strategy

### 10.1 Unit Tests (C++)

| Test | Input | Expected |
|------|-------|----------|
| WAV `smpl` chunk parsing | WAV with known loop points at 1000/5000 | `loop_start == 1000`, `loop_end == 5000` |
| WAV `cue` chunk fallback | WAV with cue points, no `smpl` | Loop points extracted from `cue` |
| Bitdepth reduction | 24-bit/48kHz → 12-bit | Variant data has reduced bit depth, original unchanged |
| SRC reduction | 48kHz → 22050 | Variant has fewer frames, correct sample_rate |
| Preset loading | `preset: "sp1200"` | bits=12, sr=26040 |
| Pan center | `pan: 0.0` | L = R = input * 0.707 (constant-power) |
| Pan full left | `pan: -1.0` | L = input, R = 0 |
| Pan full right | `pan: 1.0` | L = 0, R = input |
| Velocity | `vel: 0.5` | Output amplitude halved |
| Start offset | `start: 0.5s` on 2s sample | First 0.5s skipped |
| Length limit | `len: 0.2s` | Stops at 0.2s regardless of sample length |
| Loop one-shot | `loop_mode: "oneshot"` | Plays to end, no loop |
| Loop sustain | `loop_mode: "sustain"`, gate high → low | Loops while high, fades out on low |
| Granular time-stretch | 2x stretch, grain_size=50ms | Duration doubled, pitch preserved |
| Granular window types | sine vs hann vs rect | Audible differences in grain blending |
| Granular jitter | jitter=0.0 vs jitter=0.5 | Random variation in grain positions |
| Varispeed unchanged | `pitch_algo: "varispeed"` | Identical to current behavior |

### 10.2 DSP Experiments (Python)

Create `experiments/test_op_sample_enhanced.py`:

```python
# Test pan law
#   Output: WAV with pan sweep from -1 to 1
#   Verify: Constant power across pan positions

# Test granular time-stretch
#   Output: WAV of 2s sample stretched to 4s at original pitch
#   Verify: Duration ≈ 4s, fundamental frequency preserved

# Test bitdepth reduction character
#   Output: WAV of sample at 24-bit, 12-bit, 8-bit
#   Verify: Increasing quantization noise at lower bit depths

# Test loop points from WAV
#   Input: WAV with known smpl chunk loop points
#   Output: WAV of sustained loop playback
#   Verify: Seamless looping at correct points
```

### 10.3 Integration Tests (C++ + Akkado)

```cpp
// test_codegen.cpp
TEST_CASE("sample() with 7 params wires correctly") {
    // Compile: sample(trig, 1.0, 1, pan: 0.5, vel: 0.8, start: 0.1, len: 0.5)
    // Verify: SAMPLE_PLAY has 7 input buffers, correct defaults
}

TEST_CASE("samples() with preset keyword") {
    // Compile: samples("uri", preset: "sp1200")
    // Verify: UriRequest has SampleLoadParams with bits=12, sr=26040
}

TEST_CASE("samples() with granular params") {
    // Compile: samples("uri", pitch_algo: "granular", grain_size: 0.05)
    // Verify: SampleLoadParams has pitch_algo=Granular, grain_size=0.05
}
```

### 10.4 Manual Testing

- Load a drum bank with `preset: "sp1200"`, compare to original — should sound grittier
- Load a loop with `loop_mode: "sustain"`, hold a key — should loop seamlessly
- Pan a sample left/right in a stereo field — should move smoothly
- Apply granular time-stretch to a vocal sample — should stretch without pitch change
- Test all presets against known recordings of the original hardware

---

## 11. Open Questions

### 11.1 Same URI, Different Params

If the user writes:
```akkado
samples("my-bank", preset: "sp1200")
samples("my-bank", preset: "mpc60")
```

Should this:
- **A)** Load two separate banks with different names (e.g., "my-bank-sp1200" and "my-bank-mpc60")?
- **B)** Replace the first bank with the second (source order wins)?
- **C)** Merge variants into a single bank (both preset variants available)?

**Recommendation**: **A)** — append a suffix based on params to create distinct bank names. This is the most predictable and avoids silent replacement.

### 11.2 Granular Pitch Control

With granular synthesis, pitch and time are independent. Currently `sample(trig, pitch, id)` uses the `pitch` param for varispeed. In granular mode:
- Should `pitch` control pitch independently (pitch shift without time change)?
- Should we add a separate `time_stretch` parameter?

**Recommendation**: In varispeed mode, `pitch` controls speed (current behavior). In granular/phase vocoder mode, `pitch` controls pitch shift independently, and a new `time` parameter (or `stretch`) controls time-stretch. This requires adding an 8th parameter to `sample()`. **[OPEN QUESTION — defer to implementation]**

### 11.3 Stereo Sample Playback

Currently `sample()` outputs mono. With the addition of `pan`, stereo output is implied. Should the stereo support be part of this PRD or a follow-up?

**Recommendation**: Include in this PRD (Phase 7) since pan is useless without stereo output. The stereo infrastructure already exists (adjacent buffer pairs, `codegen_stereo.cpp`).

### 11.4 Phase Vocoder State Management

Phase vocoder requires significant state (FFT buffers, phase history). This exceeds the simple `SamplerVoice` model. Should it use `ExtendedParams<N>` in the StatePool, or a dedicated `PhaseVocoderState`?

**Recommendation**: Dedicated `PhaseVocoderState` in `dsp_state.hpp`, similar to `SamplerState`. StatePool already supports arbitrary state types.
