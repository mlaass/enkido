> **Status: DONE** — PROBE opcode, pianoroll/oscilloscope/waveform/spectrum, viz declarations.

# Visualization System PRD

## Overview

Refactor the current inline piano roll and introduce an extensible visualization system that supports multiple visualization types (piano roll, oscilloscope, waveform, spectrum) as first-class language constructs.

## Problem Statement

### Current Issues

1. **Text Interruption**: The current piano roll uses `display: inline-block` and is placed inline after pattern strings, disrupting text flow in the editor
2. **Hardcoded Visualization**: Piano roll is tied to pattern strings, not exposed as a composable function
3. **Limited Types**: Only piano roll exists; no waveform, oscilloscope, or spectrum visualizations
4. **No Signal Tapping**: Cannot visualize intermediate signals in the processing graph

### Goals

1. Visualizations display **below** the source line, not inline
2. Visualizations are **function calls** that can be piped: `signal |> oscilloscope(@, "name", opts)`
3. Support **multiple visualization types**: pianoroll, oscilloscope, waveform, spectrum
4. **Extensible architecture** for future custom visualizations
5. **Zero audio-thread allocations** for signal capture

## Proposed Syntax

```akkado
// Piano roll for pattern events (beat-aligned note display)
n"c4 e4 g4" |> pianoroll(@, "melody")
n"c4 e4 g4" |> pianoroll(@, "melody", {height: 64, scale: "chromatic"})

// Oscilloscope (beat-aligned waveform cycles)
saw(220) |> oscilloscope(@, "osc1")
saw(220) |> oscilloscope(@, "osc1", {width: 400, beats: 2})

// Continuous waveform display (scrolling)
signal |> waveform(@, "output")
signal |> waveform(@, "output", {scale: 1.0, duration: 0.5})

// FFT spectrum analyzer
signal |> spectrum(@, "spectrum")
signal |> spectrum(@, "spectrum", {fftSize: 2048, logScale: true})
```

### Semantics

- All visualization functions **pass through** their input signal unchanged
- Visualizations emit **metadata** during compilation, not audio processing code
- Multiple visualizations can be chained: `sig |> pianoroll(@, "a") |> spectrum(@, "b")`

## Visualization Types

### 1. Piano Roll (`pianoroll`)

Beat-aligned display of pattern events with pitch mapping.

**Data Source**: Compiled pattern events from `queryPatternPreview()`

**Options**:
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| height | number | 48 | Canvas height in pixels |
| scale | string | "chromatic" | Pitch scale: "chromatic", "pentatonic", "octave" |
| beats | number | 4 | Number of beats to display |

### 2. Oscilloscope (`oscilloscope`)

Beat-synchronized waveform display showing complete cycles.

**Data Source**: Probe buffer samples, synchronized to beat position

**Options**:
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| width | number | 300 | Canvas width in pixels |
| height | number | 64 | Canvas height in pixels |
| beats | number | 1 | Cycles to display (beat-aligned trigger) |
| color | string | accent | Waveform color |

### 3. Waveform (`waveform`)

Continuous scrolling waveform display.

**Data Source**: Probe buffer samples, continuous scroll

**Options**:
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| width | number | 300 | Canvas width in pixels |
| height | number | 48 | Canvas height in pixels |
| duration | number | 0.5 | Time window in seconds |
| scale | number | 1.0 | Amplitude scale |

### 4. Spectrum (`spectrum`)

FFT frequency analysis display.

**Data Source**: Web Audio AnalyserNode or custom FFT on probe buffer

**Options**:
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| width | number | 300 | Canvas width in pixels |
| height | number | 64 | Canvas height in pixels |
| fftSize | number | 1024 | FFT bin count |
| logScale | boolean | true | Logarithmic frequency axis |
| minDb | number | -90 | Minimum dB level |
| maxDb | number | 0 | Maximum dB level |

## Architecture

### Compiler Changes

#### 1. VisualizationDecl Structure

```cpp
// akkado/include/akkado/codegen.hpp
enum class VisualizationType : std::uint8_t {
    PianoRoll = 0,
    Oscilloscope = 1,
    Waveform = 2,
    Spectrum = 3
};

struct VisualizationDecl {
    std::string name;                   // Display label
    VisualizationType type;             // Visualization type
    std::uint32_t state_id;             // Unique ID for probe binding
    std::string options_json;           // Serialized options
    std::uint32_t source_offset;        // Source location for click-to-source
    std::uint32_t source_length;
};
```

#### 2. Builtin Registration

```cpp
// akkado/include/akkado/builtins.hpp
{"pianoroll", {cedar::Opcode::NOP, 1, 2, false, {"signal", "name", "options"}, ...}},
{"oscilloscope", {cedar::Opcode::NOP, 1, 2, false, {"signal", "name", "options"}, ...}},
{"waveform", {cedar::Opcode::NOP, 1, 2, false, {"signal", "name", "options"}, ...}},
{"spectrum", {cedar::Opcode::NOP, 1, 2, false, {"signal", "name", "options"}, ...}},
```

#### 3. Special Handler Pattern

Follow the `param()` pattern in `codegen_params.cpp`:

1. Extract arguments (signal, name, options)
2. Create `VisualizationDecl` with parsed options
3. Append to `visualization_decls_` vector
4. For signal visualizations: emit `PROBE` opcode to capture buffer
5. Return input signal buffer unchanged (pass-through)

### Signal Capture (Cedar VM)

#### Probe Pool

Pre-allocated ring buffers for zero-allocation signal capture:

```cpp
// cedar/include/cedar/vm/probe_pool.hpp
struct ProbeBuffer {
    static constexpr size_t RING_SIZE = 4;  // 4 blocks = 512 samples
    alignas(64) float samples[RING_SIZE][BLOCK_SIZE];
    std::atomic<uint32_t> write_head{0};
};

struct ProbePool {
    static constexpr size_t MAX_PROBES = 16;
    std::array<ProbeBuffer, MAX_PROBES> probes;
    std::array<uint32_t, MAX_PROBES> state_ids;
    uint8_t count = 0;

    void write(uint32_t state_id, const float* samples);
    size_t read(uint32_t state_id, float* out, size_t max_samples);
};
```

#### PROBE Opcode

```cpp
// In VM execution loop
case Opcode::PROBE: {
    uint16_t input_buf = inst.inputs[0];
    probe_pool_.write(inst.state_id, buffers_.get(input_buf));
    // Pass through
    if (inst.out_buffer != input_buf) {
        buffers_.copy(inst.out_buffer, input_buf);
    }
    break;
}
```

### Web UI Architecture

#### CodeMirror Block Decorations

Replace inline widgets with block-level decorations below lines:

```typescript
// web/src/lib/editor/visualization-widgets.ts
export const visualizationField = StateField.define<DecorationSet>({
    create() { return Decoration.none; },
    update(decorations, tr) {
        for (const effect of tr.effects) {
            if (effect.is(setVisualizationsEffect)) {
                return buildBlockDecorations(tr.state, effect.value);
            }
        }
        return decorations.map(tr.changes);
    },
    provide: f => EditorView.decorations.from(f)
});

function buildBlockDecorations(state: EditorState, vizDecls: VizDecl[]): DecorationSet {
    const widgets = [];
    for (const viz of vizDecls) {
        const line = state.doc.lineAt(viz.sourceOffset);
        widgets.push({
            pos: line.to,
            widget: Decoration.widget({
                widget: createVisualizationWidget(viz),
                block: true,
                side: 1
            })
        });
    }
    return Decoration.set(widgets.map(w => w.widget.range(w.pos)));
}
```

#### Visualization Registry

```typescript
// web/src/lib/visualizations/registry.ts
export interface VisualizationPlugin {
    type: string;
    createWidget: (decl: VizDecl) => WidgetType;
    defaultOptions: Record<string, unknown>;
}

export const vizRegistry = {
    plugins: new Map<string, VisualizationPlugin>(),
    register(plugin: VisualizationPlugin) { ... },
    create(decl: VizDecl): WidgetType { ... }
};

// Built-in registrations
vizRegistry.register(pianorollPlugin);
vizRegistry.register(oscilloscopePlugin);
vizRegistry.register(waveformPlugin);
vizRegistry.register(spectrumPlugin);
```

### Data Flow

```
Source Code
    │
    ▼
┌─────────────────┐
│ Akkado Compiler │
│  - Parse call   │
│  - Emit PROBE   │◄── For oscilloscope/waveform/spectrum
│  - Record decl  │
└────────┬────────┘
         │
         ▼
┌─────────────────┐     ┌──────────────────┐
│ CompileResult   │     │  Cedar VM        │
│ - viz_decls[]   │     │  - ProbePool     │
│ - bytecode      │     │  - Ring buffers  │
└────────┬────────┘     └────────┬─────────┘
         │                       │
         ▼                       │ 60fps poll
┌─────────────────┐              │
│ Web UI          │◄─────────────┘
│ - StateField    │
│ - Block widgets │
│ - Canvas render │
└─────────────────┘
```

## Implementation Phases

### Phase 1: Metadata Infrastructure

**Compiler**:
- Add `VisualizationDecl` struct to `codegen.hpp`
- Add visualization handlers to `codegen.cpp`
- Register builtins in `builtins.hpp`
- Serialize decls to JSON for WASM export

**WASM**:
- Add getter functions: `akkado_get_viz_count()`, `akkado_get_viz_json(index)`
- Extend `CompileResult` TypeScript interface

### Phase 2: Block Widget Infrastructure

**CodeMirror**:
- Create `StateField` for visualization decorations
- Implement `buildBlockDecorations()` with `block: true`
- Create base `VisualizationWidget` class
- Refactor piano roll to use new infrastructure

**Styling**:
- Block container with full width, margin below line
- Optional title/label display
- Theme-aware colors

### Phase 3: Signal Capture

**Cedar VM**:
- Implement `ProbePool` with ring buffers
- Add `PROBE` opcode
- WASM export: `cedar_get_probe_data(state_id)`

**Worklet**:
- Poll probe buffers at configurable rate
- Transfer via MessagePort to main thread

**Audio Store**:
- Add `getProbeData(stateId): Float32Array` method

### Phase 4: Visualization Components

- `PianoRollWidget` - Refactor existing, use block layout
- `OscilloscopeWidget` - Beat-triggered waveform display
- `WaveformWidget` - Continuous scrolling display
- `SpectrumWidget` - FFT frequency analysis

### Phase 5: Plugin System

- Finalize `VisualizationPlugin` interface
- Document extension API
- Consider user-defined visualizations (future)

## File Changes

### New Files

| File | Purpose |
|------|---------|
| `cedar/include/cedar/vm/probe_pool.hpp` | Probe buffer pool |
| `akkado/src/codegen_viz.cpp` | Visualization handlers |
| `web/src/lib/editor/visualization-widgets.ts` | Block widget infrastructure |
| `web/src/lib/visualizations/registry.ts` | Plugin registry |
| `web/src/lib/visualizations/pianoroll.ts` | Piano roll plugin |
| `web/src/lib/visualizations/oscilloscope.ts` | Oscilloscope plugin |
| `web/src/lib/visualizations/waveform.ts` | Waveform plugin |
| `web/src/lib/visualizations/spectrum.ts` | Spectrum plugin |

### Modified Files

| File | Changes |
|------|---------|
| `akkado/include/akkado/builtins.hpp` | Add visualization builtins |
| `akkado/include/akkado/codegen.hpp` | Add `VisualizationDecl`, extend `CodeGenResult` |
| `akkado/src/codegen.cpp` | Add handler dispatch entries |
| `cedar/include/cedar/vm/instruction.hpp` | Add `PROBE` opcode |
| `cedar/include/cedar/vm/vm.hpp` | Add `ProbePool` member |
| `web/wasm/nkido_wasm.cpp` | Add visualization WASM exports |
| `web/static/worklet/cedar-processor.js` | Add probe polling |
| `web/src/lib/stores/audio.svelte.ts` | Add probe data API |
| `web/src/lib/components/Editor/Editor.svelte` | Replace patternPreview extension |
| `web/src/lib/editor/pattern-preview.ts` | Deprecate or refactor |

## Testing Strategy

1. **Unit Tests**:
   - `VisualizationDecl` serialization in Akkado tests
   - `ProbePool` ring buffer correctness in Cedar tests

2. **Integration Tests**:
   - Compile code with visualization calls, verify decls emitted
   - Verify WASM exports return correct data

3. **Manual Testing**:
   - Visual verification of block placement (below line)
   - Playback synchronization for oscilloscope
   - FFT accuracy for spectrum

## Open Questions

1. **Probe Buffer Size**: Is 4 blocks (512 samples, ~10ms) sufficient for smooth visualization?
2. **FFT Implementation**: Use Web Audio AnalyserNode or implement FFT on probe data?
3. **User Custom Visualizations**: How to expose safe sandboxed rendering API?
4. **Performance**: Maximum number of simultaneous visualizations before frame drops?

## Success Criteria

- [ ] Visualizations render below source lines without text interruption
- [ ] `|> pianoroll(@, ...)` syntax works in the language
- [ ] All four visualization types functional
- [ ] Playhead synchronized with beat position
- [ ] No audio dropouts with 4+ active visualizations
- [ ] Zero allocations in audio thread for probe capture
