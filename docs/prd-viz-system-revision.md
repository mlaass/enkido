> **Status: REVISION 2 IMPLEMENTED** — Multi-revision viz system overhaul. Revision 1: Spectral Waterfall + FFT/IFFT opcodes. Revision 2: Extended viz parameters for all types.

# PRD: Visualization System Revision

## Executive Summary

Overhaul the visualization system across three revisions: (1) add a spectral waterfall visualization with first-class FFT/IFFT opcodes in Cedar using kissfft, (2) extend all existing visualization types with richer configurable parameters, and (3) add detachable floating window mode for any visualization. Revision 1 is the priority and is fully specified below. Revisions 2 and 3 are outlined for future specification.

## Problem Statement

### Current Limitations

1. **No spectral history**: The spectrum viz shows a single FFT frame. There is no time-frequency display (spectrogram/waterfall) for observing spectral evolution over time.
2. **Naive FFT**: The spectrum renderer uses a 64-bin JavaScript DFT (`computeSpectrum()` in `spectrum.ts`), which is slow and low-resolution. Higher bin counts (512-2048) are impractical with DFT.
3. **No FFT in Cedar**: There is no frequency-domain processing capability in the audio engine. FFT analysis is done entirely in JavaScript on the main thread.
4. **Limited viz parameters**: Only `width` and `height` options are implemented. The original PRD specified `fftSize`, `logScale`, `minDb`, `maxDb`, `beats`, `duration`, `scale`, `color` — none of which are implemented.
5. **Fixed layout only**: Visualizations are locked to inline CodeMirror block decorations. Users cannot detach, move, or resize them independently.

### Goals

1. Add a `waterfall()` visualization type displaying a scrolling spectrogram
2. Add `FFT` and `IFFT` opcodes to Cedar using kissfft, compiled to WASM
3. Replace the JavaScript DFT in `spectrum.ts` with the new WASM FFT
4. Support configurable direction, speed, FFT resolution, color gradient, and sizing
5. Design the FFT opcodes for future user-accessible spectral processing (Revision 2+)

### Non-Goals

1. **User-accessible `fft()`/`ifft()` DSP primitives** — spectral processing as language-level functions is deferred to Revision 2+. Revision 1 only adds the internal `FFT_PROBE` opcode for visualization.
2. **Spectral processing chains** (freeze, cross-synthesis, vocoding) — requires the IFFT opcode and a spectral buffer model, both Revision 2+ scope.
3. **Runtime FFT size switching** — changing the `fft` parameter requires recompile. No dynamic FFT size changes during playback.
4. **Custom user-defined gradient presets** — only the 5 built-in presets (magma, viridis, inferno, thermal, grayscale) are supported. User-defined gradients may be added in a future revision.
5. **Overlap-add windowing** — Revision 1 uses non-overlapping FFT frames (hop = fft_size). Overlapping windows for smoother time resolution may be added later.

---

## Revision 1: Spectral Waterfall + FFT/IFFT Opcodes

### Target Syntax

```akkado
// Basic waterfall with defaults (angle: 180, speed: 40, fft: 1024, gradient: "magma")
osc("saw", 220) |> waterfall(@, "harmonics") |> out(@, @)

// Fully configured waterfall
osc("saw", 220) |> waterfall(@, "harmonics", {
    angle: 270,          // scroll direction in degrees (0=right, 90=up, 180=left, 270=down)
    speed: 60,           // scroll speed in pixels per second
    fft: 2048,           // FFT resolution (power of 2: 256, 512, 1024, 2048)
    gradient: "viridis", // color preset
    width: 400,          // container width in pixels
    height: 200          // container height in pixels
}) |> out(@, @)

// Relative sizing
signal |> waterfall(@, "full-width", {width: "100%", height: 150})

// Multiple waterfalls with different configs
dry = osc("saw", 220)
dry |> waterfall(@, "pre-filter", {gradient: "thermal", fft: 512})
dry |> filter_lp(@, 2000, 0.7) |> waterfall(@, "post-filter", {gradient: "magma", fft: 512})
|> out(@, @)

// Spectrum now also benefits from WASM FFT (replaces JS DFT)
signal |> spectrum(@, "fft", {fft: 1024}) |> out(@, @)
```

### Semantics

- `waterfall()` passes its input signal through unchanged (like all viz functions)
- The compiler emits an `FFT_PROBE` opcode that accumulates samples and computes FFT in the audio thread
- Magnitude data is sent from the AudioWorklet to the main thread for rendering
- All parameters are optional with sensible defaults

### Waterfall Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `angle` | number | 180 | Scroll direction in degrees. Cardinal only: 0=right, 90=up, 180=left, 270=down |
| `speed` | number | 40 | Scroll speed in pixels per second |
| `fft` | number | 1024 | FFT size (power of 2: 256, 512, 1024, 2048) |
| `gradient` | string | "magma" | Color gradient preset name |
| `width` | number\|string | 300 | Width in pixels or "100%" for relative |
| `height` | number\|string | 150 | Height in pixels or "100%" for relative |
| `minDb` | number | -90 | Minimum dB level (floor) |
| `maxDb` | number | 0 | Maximum dB level (ceiling) |

### Gradient Presets

| Name | Description | Color Range |
|------|-------------|-------------|
| `magma` | Perceptually uniform, dark-to-hot | Black -> purple -> orange -> yellow |
| `viridis` | Perceptually uniform, colorblind-safe | Dark purple -> teal -> yellow |
| `inferno` | High contrast, dark-to-bright | Black -> purple -> red -> yellow |
| `thermal` | Heat-map style | Black -> blue -> red -> white |
| `grayscale` | Monochrome | Black -> white |

Each preset is a 256-entry RGBA lookup table (1024 bytes) for direct ImageData pixel writes.

### Scroll Direction Model

Direction is specified as an angle in degrees, mapped internally to a scroll vector:

```
angle=0   -> dx=+1, dy=0  -> time flows right, frequency on Y axis
angle=90  -> dx=0,  dy=-1 -> time flows up, frequency on X axis
angle=180 -> dx=-1, dy=0  -> time flows left, frequency on Y axis (default)
angle=270 -> dx=0,  dy=+1 -> time flows down, frequency on X axis
```

For vertical scroll (90/270): frequency bins map to X axis, new spectral lines are drawn as horizontal rows.
For horizontal scroll (0/180): frequency bins map to Y axis, new spectral lines are drawn as vertical columns.

---

## Architecture

### 1. kissfft Integration

**Library**: [kissfft](https://github.com/mborgerding/kissfft) (BSD license, single C file + header)

**Files to add**:
```
cedar/third_party/kissfft/
    kiss_fft.h          # Core FFT
    kiss_fft.c          # Core implementation
    kiss_fftr.h         # Real-valued FFT wrapper
    kiss_fftr.c         # Real-valued implementation
    COPYING             # BSD license
```

This follows the existing pattern for third-party deps alongside `dr_flac.h`, `minimp3.h`, `stb_vorbis.c`, `tsf.h`.

**C++ Wrapper** (`cedar/include/cedar/dsp/fft.hpp`):
```cpp
namespace cedar {

// Cached kissfft configs per FFT size (pre-allocated, reused)
// Supported sizes: 256, 512, 1024, 2048

// Forward FFT: time-domain -> frequency-domain
// Applies Hanning window, returns nfft/2+1 complex bins
void compute_fft(const float* time_domain, std::size_t nfft,
                 float* real_out, float* imag_out);

// Inverse FFT: frequency-domain -> time-domain
// Takes nfft/2+1 complex bins, returns nfft time-domain samples
void compute_ifft(const float* real_in, const float* imag_in,
                  std::size_t nfft, float* time_domain_out);

// Convenience: compute magnitude spectrum in dB
// Returns nfft/2+1 values in dB (20*log10(magnitude/nfft))
void compute_magnitude_db(const float* real, const float* imag,
                          std::size_t nfft, float* magnitudes_db_out);

}  // namespace cedar
```

The wrapper caches `kiss_fftr_cfg` instances and pre-computed Hanning window coefficient arrays in static arrays indexed by log2(nfft) for zero-allocation reuse.

### 2. FFT/IFFT Opcodes

Two new opcodes in the Visualization/Debug range (180-189):

```cpp
// cedar/include/cedar/vm/instruction.hpp
FFT_PROBE = 181,   // Forward FFT: accumulate samples, compute FFT, store magnitudes
                    // out=passthrough, in0=signal, rate=fft_size_log2 (8=256..11=2048)

IFFT = 182,        // Inverse FFT (reserved for Revision 2+)
                    // out=time_domain, in0=real_bins, in1=imag_bins, rate=fft_size_log2
```

**FFT_PROBE** is a combined accumulate+analyze opcode:
- Accumulates input samples into an internal ring buffer
- When a full FFT frame is collected (every `fft_size / BLOCK_SIZE` blocks), computes FFT
- Stores magnitude spectrum in state for visualization readout
- Stores complex output (real + imaginary) in state for future IFFT use
- Passes input signal through unchanged

**IFFT** is reserved for Revision 2+. It will synthesize time-domain audio from complex frequency bins, enabling spectral processing chains like:
```akkado
// Future (Revision 2+): spectral freeze, cross-synthesis, etc.
signal |> fft(@) |> spectral_gate(@, threshold) |> ifft(@) |> out(@, @)
```

### 3. FFTProbeState

```cpp
// cedar/include/cedar/opcodes/dsp_state.hpp

struct FFTProbeState {
    static constexpr std::size_t MAX_FFT_SIZE = 2048;
    static constexpr std::size_t MAX_BINS = MAX_FFT_SIZE / 2 + 1;  // 1025

    // Input accumulation ring buffer
    float input_buffer[MAX_FFT_SIZE] = {};
    std::size_t write_pos = 0;
    std::size_t fft_size = 1024;

    // FFT output: magnitude in dB (for visualization)
    float magnitudes_db[MAX_BINS] = {};

    // FFT output: complex bins (for future IFFT / spectral processing)
    float real_bins[MAX_BINS] = {};
    float imag_bins[MAX_BINS] = {};

    // Frame counter — increments each completed FFT, used by viz to detect new data
    std::uint32_t frame_counter = 0;
    bool initialized = false;

    void reset();

    // Write a block of samples. Triggers FFT when accumulation buffer is full.
    // With fft_size=1024 and BLOCK_SIZE=128, triggers every 8 blocks (~21ms at 48kHz).
    void write_block(const float* samples, std::size_t count);
};
```

Add `FFTProbeState` to the `DSPState` variant (after `ProbeState` in the Visualization states section).

> **Design note**: `FFTProbeState` always allocates for `MAX_FFT_SIZE = 2048` regardless of the requested FFT size. This is intentional — fixed-size allocation avoids templating on FFT size or runtime branching in the audio path, keeping the opcode implementation simple. The ~20KB per instance is acceptable given the bounded number of simultaneous viz probes.

**Timing**: At fft_size=1024 and BLOCK_SIZE=128, FFT triggers every 8 blocks = 21.3ms. At 30fps viz update rate (33ms), this means roughly 1-2 new FFT frames per render, which is ideal.

### 4. WASM Exports

Three new exports in `web/wasm/nkido_wasm.cpp`:

```cpp
// Static buffer for FFT magnitude copy
static float g_fft_magnitude_buffer[1025];  // MAX_BINS

// Get number of frequency bins for this FFT probe
WASM_EXPORT uint32_t cedar_get_fft_bin_count(uint32_t state_id);

// Get magnitude spectrum in dB (oldest-to-newest, nfft/2+1 values)
// Returns pointer to static buffer, or nullptr if not found
WASM_EXPORT const float* cedar_get_fft_magnitudes(uint32_t state_id);

// Get frame counter to detect new FFT frames without redundant data copies
WASM_EXPORT uint32_t cedar_get_fft_frame_counter(uint32_t state_id);
```

Add these three functions to `NKIDO_EXPORTED_FUNCTIONS` in `web/wasm/CMakeLists.txt`.

### 5. AudioWorklet Data Pathway

New message handler in `web/static/worklet/cedar-processor.js`:

```
Main thread -> Worklet:  { type: 'getFFTProbeData', stateId: number }
Worklet -> Main thread:  { type: 'fftProbeData', stateId: number,
                           magnitudes: number[] | null,
                           binCount: number,
                           frameCounter: number }
```

The worklet calls `cedar_get_fft_frame_counter()` first. If the frame counter hasn't changed since the last request, it can skip the magnitude copy and return `magnitudes: null` with the same `frameCounter` to signal "no new data."

### 6. Audio Store Extension

New method on the audio engine in `web/src/lib/stores/audio.svelte.ts`:

```typescript
export enum VizType {
    PianoRoll = 0,
    Oscilloscope = 1,
    Waveform = 2,
    Spectrum = 3,
    Waterfall = 4     // NEW
}

interface FFTProbeData {
    magnitudes: Float32Array;
    binCount: number;
    frameCounter: number;
}

// New public method
async function getFFTProbeData(stateId: number): Promise<FFTProbeData | null>
```

### 7. Compiler Changes

**VisualizationType enum** (`akkado/include/akkado/codegen.hpp`):
```cpp
enum class VisualizationType : std::uint8_t {
    PianoRoll = 0,
    Oscilloscope = 1,
    Waveform = 2,
    Spectrum = 3,
    Waterfall = 4    // NEW
};
```

**String options support** (`akkado/src/codegen_viz.cpp`):
Extend `extract_options_json()` to handle `StringLit` values in addition to `NumberLit`. This enables `{gradient: "magma"}` to serialize as `{"gradient":"magma"}` in the options JSON.

**New handler** (`akkado/src/codegen_viz.cpp`):
`handle_waterfall_call(node, n)` — follows the `handle_spectrum_call` pattern:
1. Extract signal argument (required)
2. Extract optional name (default: "Waterfall")
3. Extract optional options record, serialize to JSON
4. Generate state_id via push_path("waterfall")/push_path(name) (matching existing viz handler pattern)
5. Create `VisualizationDecl` with type `Waterfall`
6. Extract `fft` option at compile time to encode in `inst.rate` as log2 (default: 10 = 1024)
7. Emit `FFT_PROBE` opcode (instead of `PROBE`) with `rate = fft_size_log2`
8. Return input signal buffer unchanged

**Builtin registration** (`akkado/src/codegen.cpp`):
Add `{"waterfall", &CodeGenerator::handle_waterfall_call}` to handler dispatch.

> **Follow-up**: All viz handlers (pianoroll, oscilloscope, waveform, spectrum, waterfall) should add a `push_path(source_offset)` step to their state_id generation for disambiguation when multiple viz calls share the same name. This is a separate change that applies to all viz types uniformly.

### 8. Migrate Spectrum to WASM FFT

As part of Revision 1, migrate the existing `spectrum()` viz to use the same FFT_PROBE pathway:

1. Change `handle_spectrum_call` in `codegen_viz.cpp` to emit `FFT_PROBE` instead of `PROBE`
2. Change `spectrum.ts` to call `audioEngine.getFFTProbeData(stateId)` instead of `getProbeData(stateId)`
3. Remove the JavaScript `computeSpectrum()` DFT function and its log-frequency bin spacing (`Math.pow(k/numBins, 1.5)`)
4. Render bars directly from the linear FFT magnitude dB array — the visual appearance will change from log-compressed to linear frequency spacing, which is more accurate for spectral analysis

This validates the full FFT pipeline and eliminates the JS DFT.

### 9. Waterfall Renderer

New file: `web/src/lib/visualizations/waterfall.ts`

**Rendering approach**: Canvas 2D with direct ImageData pixel manipulation.

**State**:
```typescript
interface WaterfallState {
    canvas: HTMLCanvasElement;
    ctx: CanvasRenderingContext2D;
    imageData: ImageData;          // Full canvas pixel buffer
    gradientLUT: Uint8ClampedArray; // 256 * 4 RGBA entries (1024 bytes)
    lastFrameCounter: number;      // Detect new FFT frames
    lastUpdateTime: number;
    scrollAccumulator: number;     // Sub-pixel scroll precision

    // Parsed from options
    angle: number;
    speed: number;
    fftSize: number;
    minDb: number;
    maxDb: number;
}
```

**Scroll algorithm per frame**:
1. `pixelsToScroll = speed * deltaTime` (accumulated for sub-pixel precision)
2. For each integer pixel of scroll:
   - Shift existing pixels using `Uint8ClampedArray.copyWithin()`:
     - Vertical scroll (angle 90/270): single `copyWithin` call shifting by `width * 4` bytes per row
     - Horizontal scroll (angle 0/180): row-by-row `copyWithin` shifting by `4` bytes
   - Draw new spectral line into the exposed edge:
     - Map each FFT bin to a pixel position along the frequency axis
     - Convert dB magnitude to 0-255 index: `floor(clamp((db - minDb) / (maxDb - minDb), 0, 1) * 255)`
     - Look up RGBA from gradient LUT at `index * 4`
     - Write 4 bytes directly into ImageData pixel array
3. `ctx.putImageData(imageData, 0, 0)`

**Retina handling**: Use 1:1 pixel mapping for ImageData (no devicePixelRatio scaling). The waterfall's soft spectral appearance makes doubled resolution unnecessary, and it would quadruple pixel-fill work.

**Gradient LUT module** (`web/src/lib/visualizations/gradients.ts`):
```typescript
export type GradientLUT = Uint8ClampedArray;  // length: 1024 (256 * 4 RGBA)

export const GRADIENT_PRESETS: Record<string, GradientLUT>;
export const DEFAULT_GRADIENT = 'magma';

// Interpolate between color stops to produce 256 RGBA entries
function generateGradient(stops: Array<{pos: number; r: number; g: number; b: number}>): GradientLUT;
```

Color stops for each preset derived from standard colormap definitions (matplotlib/d3).

**Relative sizing**: When `width` or `height` is a string ending in `@`, attach a `ResizeObserver` to the container's parent. On resize, reallocate ImageData and canvas dimensions. Clear the waterfall history on resize (acceptable UX — history is transient).

### 10. Register in Visualization Index

Add `import './waterfall';` to `web/src/lib/visualizations/index.ts`.

---

## Data Flow

```
Source: osc("saw", 220) |> waterfall(@, "spec", {fft: 1024, gradient: "magma"})

                    Compile Time                          Runtime
                    ──────────                            ───────
Akkado Compiler                                Cedar VM (AudioWorklet)
  │                                              │
  ├─ Parse waterfall() call                      ├─ FFT_PROBE opcode executes:
  ├─ Extract options: fft=1024, gradient=magma   │   ├─ Accumulate 128 samples/block
  ├─ Emit FFT_PROBE (rate=10, state_id=hash)     │   ├─ Every 8 blocks: compute FFT via kissfft
  ├─ Create VizDecl {type:4, options:{...}}       │   ├─ Store magnitudes_db[] in FFTProbeState
  │                                              │   └─ Increment frame_counter
  ▼                                              │
CompileResult                                    ▼
  ├─ bytecode[]                          Worklet message handler
  └─ viz_decls[]                           ├─ getFFTProbeData(stateId)
       │                                   ├─ Call cedar_get_fft_frame_counter()
       │                                   ├─ If new frame: cedar_get_fft_magnitudes()
       ▼                                   └─ Post { type: 'fftProbeData', magnitudes, frameCounter }
  Web UI (Main Thread)                              │
  ├─ CodeMirror block decoration                    │
  ├─ waterfall.ts renderer                          │
  │   ├─ requestAnimationFrame loop (~30fps)  ◄─────┘
  │   ├─ Check frameCounter for new data
  │   ├─ Shift ImageData pixels (copyWithin)
  │   ├─ Map magnitudes -> gradient LUT -> pixels
  │   └─ ctx.putImageData()
  └─ Rendered canvas below source line
```

---

## Implementation Phases

### Phase 1: kissfft Integration
- [ ] Add kissfft source files to `cedar/third_party/kissfft/`
- [ ] Create C++ wrapper `cedar/include/cedar/dsp/fft.hpp` + `cedar/src/dsp/fft.cpp`
- [ ] Integrate into `cedar/CMakeLists.txt`
- [ ] Verify native build compiles (WASM inherits cedar linkage automatically)

### Phase 2: FFT_PROBE Opcode + State
- [ ] Add `FFTProbeState` to `cedar/include/cedar/opcodes/dsp_state.hpp`
- [ ] Add `FFTProbeState` to `DSPState` variant
- [ ] Add `FFT_PROBE = 181` to `cedar/include/cedar/vm/instruction.hpp`
- [ ] Implement `op_fft_probe` in `cedar/include/cedar/opcodes/utility.hpp`
- [ ] Wire into VM dispatch in `cedar/src/vm/vm.cpp`
- [ ] Reserve `IFFT = 182` opcode (implementation deferred to Revision 2+)

### Phase 3: WASM Exports + AudioWorklet
- [ ] Add `cedar_get_fft_bin_count`, `cedar_get_fft_magnitudes`, `cedar_get_fft_frame_counter` to `web/wasm/nkido_wasm.cpp`
- [ ] Add exports to `web/wasm/CMakeLists.txt` exported functions list
- [ ] Add `getFFTProbeData` message handler to `web/static/worklet/cedar-processor.js`
- [ ] Add `VizType.Waterfall`, `getFFTProbeData()` method to `web/src/lib/stores/audio.svelte.ts`
- [ ] Run `cd web && bun run build:opcodes` to regenerate opcode metadata

### Phase 4: Compiler Changes
- [ ] Add `VisualizationType::Waterfall = 4` to `akkado/include/akkado/codegen.hpp`
- [ ] Extend `extract_options_json()` in `codegen_viz.cpp` for `StringLit` values
- [ ] Add `handle_waterfall_call` declaration to `codegen.hpp`
- [ ] Implement `handle_waterfall_call` in `codegen_viz.cpp` (emit FFT_PROBE, rate=fft_log2)
- [ ] Register `waterfall` builtin in `codegen.cpp` handler dispatch
- [ ] Migrate `handle_spectrum_call` to emit `FFT_PROBE` instead of `PROBE`

### Phase 5: Waterfall Renderer
- [ ] Create `web/src/lib/visualizations/gradients.ts` with 5 preset LUTs
- [ ] Create `web/src/lib/visualizations/waterfall.ts` with ImageData scroll rendering
- [ ] Implement angle-to-direction mapping and scroll accumulation
- [ ] Implement relative sizing with ResizeObserver
- [ ] Register in `web/src/lib/visualizations/index.ts`

### Phase 6: Spectrum Migration
- [ ] Update `spectrum.ts` to use `getFFTProbeData()` instead of `getProbeData()`
- [ ] Remove `computeSpectrum()` JavaScript DFT function
- [ ] Render spectrum bars directly from magnitude dB array

### Phase 7: Testing + Polish
- [ ] C++ unit tests for kissfft wrapper (known frequency peaks, window correctness)
- [ ] C++ unit tests for FFTProbeState (accumulation, frame triggering)
- [ ] Akkado compiler tests for `waterfall()` builtin (options parsing, FFT_PROBE emission)
- [ ] Manual integration testing with sawtooth wave (visible harmonic series)
- [ ] Verify spectrum migration produces identical visual output

---

## File Changes

### New Files

| File | Purpose |
|------|---------|
| `cedar/third_party/kissfft/kiss_fft.{h,c}` | kissfft core (BSD) |
| `cedar/third_party/kissfft/kiss_fftr.{h,c}` | Real-valued FFT wrapper |
| `cedar/include/cedar/dsp/fft.hpp` | C++ FFT wrapper (cached configs, windowing, magnitude) |
| `cedar/src/dsp/fft.cpp` | FFT wrapper implementation |
| `web/src/lib/visualizations/gradients.ts` | Gradient preset LUTs (magma, viridis, inferno, thermal, grayscale) |
| `web/src/lib/visualizations/waterfall.ts` | Waterfall renderer (ImageData scroll, direction, gradient) |

### Modified Files

| File | Changes |
|------|---------|
| `cedar/CMakeLists.txt` | Add `src/dsp/fft.cpp`, kissfft include path |
| `cedar/include/cedar/vm/instruction.hpp` | Add `FFT_PROBE = 181`, reserve `IFFT = 182` |
| `cedar/include/cedar/opcodes/dsp_state.hpp` | Add `FFTProbeState`, add to `DSPState` variant |
| `cedar/include/cedar/opcodes/utility.hpp` | Add `op_fft_probe` implementation |
| `cedar/src/vm/vm.cpp` | Add FFT_PROBE case to dispatch |
| `akkado/include/akkado/codegen.hpp` | Add `Waterfall = 4` to enum, declare `handle_waterfall_call` |
| `akkado/src/codegen_viz.cpp` | Add `handle_waterfall_call`, extend `extract_options_json` for strings, migrate spectrum to FFT_PROBE |
| `akkado/src/codegen.cpp` | Register `waterfall` builtin |
| `web/wasm/nkido_wasm.cpp` | Add 3 FFT probe WASM exports |
| `web/wasm/CMakeLists.txt` | Add exports to `NKIDO_EXPORTED_FUNCTIONS` |
| `web/static/worklet/cedar-processor.js` | Add `getFFTProbeData` message handler |
| `web/src/lib/stores/audio.svelte.ts` | Add `VizType.Waterfall`, `getFFTProbeData()` |
| `web/src/lib/visualizations/index.ts` | Import waterfall renderer |
| `web/src/lib/visualizations/spectrum.ts` | Migrate to `getFFTProbeData()`, remove JS DFT |

---

## Testing Strategy

### Unit Tests

**Cedar FFT wrapper** (`cedar/tests/`):
- 440Hz sine wave at 48kHz -> FFT -> verify peak bin at expected index
- Verify power-of-2 sizes (256, 512, 1024, 2048) all produce correct bin counts
- Verify Hanning window application (windowed DC input should have expected sidelobe attenuation)
- Round-trip: FFT -> IFFT -> verify reconstruction matches input (within floating-point tolerance)

**FFTProbeState** (`cedar/tests/`):
- Write 8 blocks of 128 samples -> verify frame_counter increments to 1
- Write 16 blocks -> verify frame_counter = 2
- Verify magnitudes_db contains valid dB values (not NaN/Inf)

**Compiler** (`akkado/tests/`):
- `waterfall(signal, "test")` -> verify FFT_PROBE instruction emitted with rate=10 (1024)
- `waterfall(signal, "test", {fft: 512})` -> verify rate=9
- `waterfall(signal, "test", {gradient: "viridis"})` -> verify options JSON contains `"gradient":"viridis"`
- Verify VizDecl has type=Waterfall and correct source offsets

### Manual Integration

Test program:
```akkado
osc("saw", 220) |> waterfall(@, "saw-harmonics", {
    angle: 90, speed: 40, fft: 1024, gradient: "magma", width: 400, height: 150
}) |> out(@, @)
```

Verify:
- Scrolling spectrogram appears below source line
- Visible harmonic series of 220Hz sawtooth (fundamenta + integer harmonics)
- Scrolls upward at ~40px/s
- Magma color gradient (dark -> purple -> orange -> yellow)
- Stops scrolling when playback stops, preserves last image

---

## Open Questions

1. **Overlap-add for FFT**: Revision 1 uses non-overlapping windows (hop = fft_size). At 1024/48kHz this gives ~21ms per frame, adequate for 30fps visual display with 1-2 new frames per render. Overlapping (50% hop) could be added as a future enhancement if smoother time resolution is needed, at the cost of doubling FFT compute.
2. **Logarithmic frequency axis**: Should the waterfall support log-frequency mapping (compress low bins, expand high bins)? Useful for musical content but adds complexity to the pixel mapping.
3. **kissfft licensing**: kissfft is BSD-licensed — confirm this is acceptable for the project.
4. **Max simultaneous waterfalls**: FFTProbeState is ~20KB per instance (1025 × 3 output arrays + 2048-sample input buffer). With MAX_DSP_ID=4096, this is bounded but could be large. Consider a MAX_FFT_PROBES limit?
5. **IFFT opcode buffer model**: When IFFT is implemented in Revision 2+, how does frequency-domain data flow through the DAG? FFT outputs nfft/2+1 complex bins, which don't fit in a single 128-sample buffer. Needs a buffer model design for spectral processing.

---

## Revision 2: Extended Viz Parameters (Outline)

> Detailed PRD to be written after Revision 1 is complete.

### Scope

Implement all originally-specified parameters from `prd-visualization-system.md` plus additional meaningful params for each viz type.

### Per-Type Parameters

**Spectrum** (now using WASM FFT):
- `fft` (256-2048), `logScale` (bool), `minDb`/`maxDb` (number)

**PianoRoll**:
- `beats` (number of beats visible), `scale` ("chromatic"/"pentatonic"/"octave"), `showGrid` (bool)

**Waveform**:
- `duration` (seconds of history), `scale` (amplitude multiplier), `filled` (bool)

**Oscilloscope**:
- `beats` (window in beats), `triggerLevel` (number), `triggerEdge` ("rising"/"falling")

**All types**:
- `width`/`height` with relative sizing ("100%") via ResizeObserver

### Implementation

- Extend `extract_options_json()` to handle `BooleanLit` values
- Each renderer parses its specific options from `viz.options` record
- No new opcodes or WASM changes needed — all parameters are compile-time options

---

## Revision 3: Detachable Floating Windows (Outline)

> Detailed PRD to be written after Revision 2 is complete.

### Scope

Allow any visualization to be "popped out" into a draggable, resizable floating window.

### Triggering

- **Code**: `{detached: true}` in the options record (compile-time default)
- **UI**: Pop-out button on each viz container's label bar (runtime toggle)
- Code takes priority on recompile

### Architecture

1. **FloatingVizManager**: New Svelte component at app root level (sibling to editor). Renders detached viz in `position: fixed` overlays outside CodeMirror.
2. **Drag/resize**: Extend PointerEvent-based resize pattern from `ResizeHandle.svelte` to support 4-corner resize + title bar drag.
3. **Z-index**: Simple stack with "bring to front on click" (global z-counter).
4. **Persistence**: Window positions/sizes stored in localStorage under `nkido-detached-viz`.
5. **Re-attach**: Double-click title bar or click dock button to snap back to inline position.
6. **Data flow unchanged**: Renderer calls same `getFFTProbeData()`/`getProbeData()` regardless of where the DOM element lives.
7. **Placeholder**: CodeMirror block decoration shows a "Detached" placeholder with click-to-reattach when viz is floating.

### Key Consideration

The `VisualizationContainerWidget` needs to coordinate with `FloatingVizManager` on recompile — orphaned floating windows must be cleaned up when viz declarations change.

---

## Success Criteria

### Revision 1
- [ ] `waterfall()` renders a scrolling spectrogram below the source line
- [ ] All 4 scroll directions work (angle: 0, 90, 180, 270)
- [ ] Scroll speed is configurable and visually matches px/s specification
- [ ] 5 gradient presets render correctly
- [ ] FFT resolution is configurable (256, 512, 1024, 2048) with visible quality difference
- [ ] Relative sizing ("100%") works and responds to container resize
- [ ] FFT_PROBE opcode executes with zero audio-thread allocations
- [ ] kissfft compiles into WASM without issues
- [ ] `spectrum()` uses WASM FFT (no more JS DFT)
- [ ] No audio dropouts with 4+ simultaneous waterfalls
- [ ] IFFT opcode number reserved (no implementation needed yet)
