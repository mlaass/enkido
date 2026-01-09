# Enkido Web IDE - Product Requirements Document

## Executive Summary

Enkido Web IDE is a browser-based live-coding environment for writing and hot-swapping Akkado programs running in the Cedar synth engine. The IDE emphasizes elegance, minimal footprint, and real-time audio-visual feedback, inspired by [Strudel.cc](https://strudel.cc/) and the Tidal Cycles ecosystem.

## Goals

1. **Live Coding**: Edit Akkado code with instant hot-swap to running Cedar synth
2. **Rich Visualization**: Waveforms, spectrograms, oscilloscope, piano roll, pattern highlighting
3. **Interactive Documentation**: Browsable docs with runnable code examples as widgets
4. **Lightweight & Elegant**: Small bundle size, fast load times, minimal dependencies
5. **Educational**: Lower the barrier to entry for modular synthesis and live coding

---

## Technical Architecture

### System Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                         Browser                                  │
│                                                                  │
│  ┌──────────────┐    ┌─────────────────┐    ┌────────────────┐  │
│  │   Editor     │    │   Visualization │    │   Control      │  │
│  │  (CodeMirror │◄──►│   Canvas/WebGL  │◄──►│   Panel        │  │
│  │   + Akkado   │    │   - Waveform    │    │   - Faders     │  │
│  │   syntax)    │    │   - Spectrum    │    │   - Knobs      │  │
│  │              │    │   - Piano Roll  │    │   - Settings   │  │
│  └──────┬───────┘    │   - Oscilloscope│    └────────────────┘  │
│         │            └────────▲────────┘                         │
│         │                     │                                  │
│         ▼                     │ AnalyserNode data               │
│  ┌──────────────┐    ┌────────┴────────┐                        │
│  │   Akkado     │    │   Web Audio     │                        │
│  │   Compiler   │───►│   Worklet       │───► Audio Output       │
│  │   (WASM)     │    │   + Cedar VM    │                        │
│  │              │    │   (WASM)        │                        │
│  └──────────────┘    └─────────────────┘                        │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### WASM Modules

Two WASM modules compiled from existing C++ code:

1. **akkado.wasm** (~50-100KB estimated)
   - Entry: `akkado::compile(source) → CompileResult`
   - Returns: bytecode + diagnostics with source locations
   - Files: `akkado/src/lexer.cpp`, `akkado/src/parser.cpp`, `akkado/src/akkado.cpp` (codegen TBD)

2. **cedar.wasm** (~200-400KB estimated)
   - Entry: `cedar::VM` class with `load_program()`, `process_block()`
   - Runs in AudioWorklet for real-time audio
   - Files: `cedar/src/vm/vm.cpp`, opcodes in `cedar/include/cedar/vm/instruction.hpp`

**Build**: Uses existing CMake preset `wasm` with Emscripten:
```bash
cmake --preset wasm && cmake --build build/wasm
```

### Audio Pipeline

```
Main Thread                    AudioWorklet Thread
─────────────                  ────────────────────
┌─────────┐                    ┌─────────────────┐
│ Akkado  │                    │   Cedar VM      │
│Compiler │──bytecode──────────►│                 │
│ (WASM)  │                    │ process_block() │
└─────────┘                    │   128 samples   │
                               │   @ 48kHz       │
┌─────────┐   SharedArrayBuffer│                 │
│Visualize│◄──────────────────◄│ AnalyserNode    │
│ Canvas  │  waveform/FFT data │                 │
└─────────┘                    └─────────────────┘
```

- **Crossfade**: Cedar's triple-buffer architecture enables glitch-free hot-swap (2-5 blocks ≈ 5-13ms)
- **Latency**: 128 samples @ 48kHz = 2.67ms per block

---

## Framework Decision

### Comparison

| Framework | Bundle | Performance | Ecosystem | Learning Curve |
|-----------|--------|-------------|-----------|----------------|
| **Svelte 5** | ~2KB runtime | Excellent (compile-time) | Mature | Medium |
| **SolidJS** | ~7KB | Fastest (no VDOM) | Growing | Low (React-like) |
| **Preact** | ~3KB | Good | React compatible | Very low |
| **Vanilla JS** | 0KB | Manual optimization | N/A | High maintenance |

### Recommendation: **Svelte 5**

Reasons:
1. **Compile-time optimization**: Generates minimal vanilla JS, no runtime framework overhead
2. **Smallest bundle**: ~2KB runtime gzipped, closest to Strudel's vanilla philosophy
3. **Runes model**: Svelte 5's `$state`, `$derived`, `$effect` provide fine-grained reactivity
4. **HTML-first syntax**: Clean, readable component structure
5. **Mature ecosystem**: SvelteKit available if routing needed, excellent tooling
6. **First-class TypeScript**: Strong typing support with `<script lang="ts">`
7. **No synthetic events**: Direct DOM access, ideal for Canvas/WebGL visualizations

Strudel refactored from React to vanilla JS for performance - Svelte 5 gives us that compiled-to-vanilla approach with excellent developer experience.

### Supporting Libraries

| Purpose | Library | Size |
|---------|---------|------|
| Editor | CodeMirror 6 | ~150KB (with language support) |
| Build | Vite + vite-plugin-svelte | Dev only |
| Styling | Scoped CSS (built-in) | 0KB runtime |
| Icons | Lucide Svelte (tree-shakeable) | ~0.5KB per icon |
| State | Svelte runes (built-in) | 0KB |
| Routing | SvelteKit (optional) | ~15KB if needed |

**Total estimated bundle**: ~180KB gzipped (excluding WASM)

---

## UI/UX Design

### Layout

The editor is the primary view with inline visualizations. A collapsible side panel provides controls, settings, and documentation.

```
┌───────────────────────────────────────────────────────────────┐
│ ▶ Play  ⏸ Pause  │ BPM: [120] │ Vol: ▬▬▬▬░░ │ 👁 Viz │ ⚙     │
├───────────────────────────────────────────────────────────────┤
│                    │                                          │
│  ┌─────────────┐   │  EDITOR (with inline visualizations)     │
│  │ Controls    │   │  ─────────────────────────────────────   │
│  │             │   │                                          │
│  │ [Knob 1]    │   │  bpm = 120                               │
│  │ [Fader 2]   │   │                                          │
│  │ [Toggle 3]  │   │  osc = sin(440)                          │
│  │             │   │        ▁▂▃▄▅▆▇█▇▆▅▄▃▂▁▁▂▃▄▅▆▇█▇▆▅▄▃▂▁   │
│  ├─────────────┤   │                                          │
│  │ Settings    │   │  |> lp(%, 800 + 300 * co, 0.7)           │
│  │             │   │     ━━━━━━━━━▂▄▆█▓░░░░░░░░░░░░░░░░░     │
│  │ Theme       │   │                                          │
│  │ Audio Out   │   │  pad = seq("c3 e3 g3 b3:4", ...)         │
│  │ Buffer Size │   │        ┃ ┃ ┃ █████████████████████       │
│  │             │   │                                          │
│  ├─────────────┤   │  |> out(L: %, R: %)                      │
│  │ Docs        │   │     L ▁▂▄▆█▆▄▂  R ▁▂▄▆█▆▄▂              │
│  │ (quick ref) │   │                                          │
│  │             │   │                                          │
│  └─────────────┘   │                                          │
│                    │                                          │
└───────────────────────────────────────────────────────────────┘
```

**Key UI elements**:
- **👁 Viz toggle**: Show/hide all inline visualizations globally
- **Inline widgets**: Small canvases (~200x20px) below each DSP node
- **Side panel**: Collapsible, position configurable (left/right)
- **Full-width mode**: Panel can collapse to maximize editor space

### Design Principles (Strudel-inspired)

1. **Dark theme default**: Easy on eyes for extended sessions
2. **Monospace typography**: Code-first aesthetic (Fira Code, JetBrains Mono)
3. **Minimal chrome**: Editor is the star, controls are secondary
4. **Responsive**: Works on tablets, adapts panel positions
5. **Keyboard-first**: Ctrl+Enter to evaluate, shortcuts for transport

### Panel Configuration

- Side panel position: Left or Right (user preference, persisted)
- Panel can collapse to icon-only mode
- Tabs: Controls | Settings | Docs

---

## Editor Features

### CodeMirror 6 Configuration

1. **Custom Akkado Language Mode**
   - Syntax highlighting for: keywords, operators (`|>`, `%`), numbers, strings, pitches (`'c4'`), chords
   - Mini-notation parsing inside `pat()`, `seq()`, `timeline()` strings
   - Bracket matching for `()`, `[]`, `{}`

2. **Live Pattern Highlighting**
   - As patterns play, highlight current events in the mini-notation
   - Uses source locations from compiler diagnostics
   - Sync with Cedar's beat clock via SharedArrayBuffer

3. **Error Underlines**
   - Red squiggles for syntax errors
   - Warning highlights for semantic issues
   - Tooltip with error message on hover

4. **Autocomplete**
   - Built-in function completions with signatures
   - Snippet expansion for common patterns

### Inline Visualizations

Instead of a separate visualization panel, visualizations appear **inline** beneath code elements - small pixel graphics that render contextually based on the function/node type. This creates a live, reactive view of the signal flow directly in the editor.

```
┌─────────────────────────────────────────────────────────────┐
│  bpm = 120                                                  │
│                                                             │
│  osc = sin(440)                                             │
│        ▁▂▃▄▅▆▇█▇▆▅▄▃▂▁▁▂▃▄▅▆▇█▇▆▅▄▃▂▁  ← waveform         │
│                                                             │
│  |> lp(%, 800, 0.7)                                         │
│     ━━━━━━━━━━━━▂▄▆█▓░░░░░░░░░░░░░░░░  ← freq response     │
│                                                             │
│  pad = seq("c3 e3 g3 b3:4", (t, v, p) -> {                  │
│        ┃ ┃ ┃ ████████████████████████  ← pattern timeline   │
│        └─┴─┴──────────────────────────                      │
│     ...                                                     │
│  })                                                         │
│                                                             │
│  |> out(L: %, R: %)                                         │
│     L ▁▂▃▄▅▆▇█▇▆▅▄ R ▁▂▃▄▅▆▇█▇▆▅▄  ← stereo output         │
└─────────────────────────────────────────────────────────────┘
```

**Visualization Types by Node**:

| Node Type | Inline Graphic | Size |
|-----------|----------------|------|
| Oscillator (`sin`, `saw`, `tri`, `sqr`) | Waveform | ~200x20px |
| Filter (`lp`, `hp`, `bp`, `svf*`) | Frequency response curve | ~200x20px |
| Envelope (`adsr`, `ar`) | Envelope shape | ~150x20px |
| Pattern (`pat`, `seq`, `timeline`) | Pattern timeline with beat markers | ~300x20px |
| Delay/Reverb | Impulse response | ~150x20px |
| `out()` | Stereo waveform/level meter | ~200x20px |
| Math operations | Value indicator | ~50x15px |

**Implementation**:
- Use CodeMirror 6 **widgets** (block decorations inserted between lines)
- Each widget is a small Canvas element (~200x20px)
- Widgets subscribe to state from the audio engine via semantic ID
- Update at 30-60fps using requestAnimationFrame
- Widgets are collapsible (click to toggle) and can be globally disabled

**Data Flow**:
```
Cedar VM state_id → SharedArrayBuffer → Widget lookup → Canvas render
```

The semantic ID from each bytecode instruction maps to its corresponding inline widget, enabling precise per-node visualization.

Reference: [MDN Web Audio Visualizations](https://developer.mozilla.org/en-US/docs/Web/API/Web_Audio_API/Visualizations_with_Web_Audio_API), [CodeMirror Decorations](https://codemirror.net/docs/ref/#view.Decoration)

---

## Control Panel

### Configurable Controls

Users can add/remove controls that map to Akkado variables:

```akkado
// In user code:
cutoff = 800  // Exposed as fader
resonance = 0.7  // Exposed as knob
```

Control types:
- **Knob**: Circular, good for 0-1 values
- **Fader**: Vertical slider, good for frequency/amplitude
- **Toggle**: Boolean on/off
- **XY Pad**: 2D control for dual parameters
- **Trigger**: Momentary button for manual triggers

### Settings Tab

- Audio output device selection
- Buffer size (128/256/512/1024)
- Sample rate (44.1k/48k)
- Theme (Dark/Light/System)
- Editor font size
- Panel position (Left/Right)

---

## Interactive Documentation

### Structure

Markdown files with embedded Enkido widgets:

```markdown
# Oscillators

The `sin()` function generates a sine wave oscillator.

:::enkido
sin(440) |> out(%, %)
:::

Parameters:
- `freq`: Frequency in Hz (default: 440)

Try modifying the frequency:

:::enkido {interactive}
freq = 440  // Try changing this!
sin(freq) |> out(%, %)
:::
```

### Widget Features

1. **Run/Stop button**: Each widget is an independent mini-REPL
2. **Edit in place**: Code is editable, changes hot-swap
3. **Minimal visualization**: Small waveform display
4. **Copy to main editor**: Button to import example

### Documentation Sections

1. **Getting Started**
   - Hello World (simple sine)
   - Transport controls
   - Hot-swapping basics

2. **Language Reference**
   - Operators (`|>`, `%`, arithmetic)
   - Closures and patterns
   - Mini-notation grammar

3. **Built-in Functions** (auto-generated from opcodes)

   | Category | Functions |
   |----------|-----------|
   | Oscillators | `sin`, `tri`, `saw`, `sqr`, `phasor`, `noise` |
   | Filters | `lp`, `hp`, `bp`, `svflp`, `svfhp`, `svfbp`, `onepole` |
   | Envelopes | `adsr`, `ar`, `perc` |
   | Effects | `delay`, `reverb`, `freeverb`, `chorus`, `flanger` |
   | Sequencing | `seq`, `pat`, `timeline`, `euclid`, `lfo` |
   | Math | `add`, `sub`, `mul`, `div`, `pow`, `abs`, `sqrt`, `log`, `exp`, `min`, `max`, `clamp` |
   | Utility | `mtof`, `slew`, `sah`, `out` |

4. **Examples Gallery**
   - Acid bassline
   - Ambient pad
   - Drum machine
   - Generative sequences

### Docs as Standalone Page

- Route: `/docs` or `/learn`
- Full-page documentation browser
- Table of contents sidebar
- Search functionality
- Same interactive widgets work standalone

---

## Project Structure

```
enkido/
├── web/                          # NEW: Web IDE (this project)
│   ├── src/
│   │   ├── app.html              # HTML shell
│   │   ├── app.css               # Global styles
│   │   ├── lib/
│   │   │   ├── components/
│   │   │   │   ├── Editor/           # CodeMirror wrapper
│   │   │   │   │   ├── Editor.svelte
│   │   │   │   │   ├── akkado-lang.ts  # Language mode
│   │   │   │   │   └── widgets/        # Inline visualization widgets
│   │   │   │   │       ├── widget-manager.ts  # Widget lifecycle, CM integration
│   │   │   │   │       ├── OscillatorWidget.ts  # Waveform display
│   │   │   │   │       ├── FilterWidget.ts      # Freq response curve
│   │   │   │   │       ├── EnvelopeWidget.ts    # ADSR shape
│   │   │   │   │       ├── PatternWidget.ts     # Timeline + beat markers
│   │   │   │   │       └── OutputWidget.ts      # Stereo meters
│   │   │   │   ├── Transport/        # Play/Pause/BPM
│   │   │   │   │   └── Transport.svelte
│   │   │   │   ├── Controls/         # Faders, knobs
│   │   │   │   │   ├── Knob.svelte
│   │   │   │   │   ├── Fader.svelte
│   │   │   │   │   └── XYPad.svelte
│   │   │   │   └── Panel/            # Side panel container
│   │   │   │       └── SidePanel.svelte
│   │   │   ├── audio/
│   │   │   │   ├── worklet.ts        # AudioWorklet processor
│   │   │   │   ├── engine.ts         # WASM loading, scheduling
│   │   │   │   └── analyzer.ts       # Visualization data extraction
│   │   │   ├── compiler/
│   │   │   │   └── akkado.ts         # WASM compiler wrapper
│   │   │   ├── stores/
│   │   │   │   ├── audio.svelte.ts   # Audio engine state ($state runes)
│   │   │   │   ├── editor.svelte.ts  # Editor state
│   │   │   │   └── settings.svelte.ts # User preferences
│   │   │   └── docs/
│   │   │       ├── content/          # Markdown documentation
│   │   │       └── EnkidoWidget.svelte  # Interactive code blocks
│   │   ├── routes/
│   │   │   ├── +page.svelte          # Main IDE page
│   │   │   ├── +layout.svelte        # App shell
│   │   │   └── docs/
│   │   │       └── +page.svelte      # Standalone docs page
│   │   └── +error.svelte
│   ├── static/
│   │   ├── wasm/
│   │   │   ├── akkado.wasm
│   │   │   └── cedar.wasm
│   │   └── samples/              # Demo audio samples
│   ├── package.json
│   ├── svelte.config.js
│   ├── vite.config.ts
│   └── tsconfig.json
├── cedar/                        # Existing synth engine
├── akkado/                       # Existing compiler
└── docs/                         # Existing design docs
```

**Note**: Using SvelteKit structure for routing (`/docs` page) and SSR capabilities (useful for docs SEO). Can simplify to vanilla Vite + Svelte if routing not needed.

---

## Implementation Phases

**Note**: Akkado compiler (semantic analysis + codegen) is being developed in parallel and will be ready for web IDE integration.

### Phase 1: Foundation
- [ ] Set up SvelteKit project in `web/` (`npm create svelte@latest`)
- [ ] Configure WASM build for browser (Emscripten bindings)
- [ ] Basic AudioWorklet with Cedar VM
- [ ] Minimal CodeMirror editor (no Akkado syntax yet)
- [ ] Play/Pause/BPM transport controls
- [ ] Test with mock bytecode program initially

### Phase 2: Compiler Integration
- [ ] WASM bindings for `akkado::compile()` (compiler ready by this phase)
- [ ] Error display in editor (underlines, messages)
- [ ] Hot-swap on Ctrl+Enter
- [ ] Source mapping for error locations

### Phase 3: Inline Visualizations
- [ ] **CodeMirror widget system**: Block decorations for inline canvases
- [ ] **Semantic ID → Widget mapping**: Route VM state to correct widget
- [ ] **Oscillator widgets** (waveform display, ~200x20px canvas)
- [ ] **Filter widgets** (frequency response curve)
- [ ] **Envelope widgets** (ADSR shape visualization)
- [ ] **Pattern widgets** (timeline with beat markers, highlight active events)
- [ ] **Output widget** (stereo level meters/waveform)
- [ ] **Global viz toggle** (show/hide all inline widgets)

### Phase 4: Editor Polish
- [ ] Akkado language mode (full syntax highlighting)
- [ ] Mini-notation syntax highlighting inside strings
- [ ] Autocomplete for built-in functions with signatures
- [ ] Bracket matching and auto-indent

### Phase 5: Control Panel
- [ ] Fader and knob components (Canvas-based)
- [ ] Control binding to Akkado variables
- [ ] XY pad for dual-parameter control
- [ ] Settings persistence (localStorage)
- [ ] Panel position configuration (left/right)

### Phase 6: Documentation
- [ ] Markdown renderer with Enkido widgets
- [ ] Auto-generate function reference from opcodes
- [ ] Example gallery with categories
- [ ] Standalone docs route (`/docs`)
- [ ] Search functionality

### Phase 7: Polish & Launch
- [ ] PWA support (offline caching with service worker)
- [ ] Share URLs (base64 encoded programs)
- [ ] Comprehensive keyboard shortcuts
- [ ] Mobile/tablet responsive design
- [ ] Performance profiling and optimization

---

## Critical Dependencies

1. **Akkado Codegen** (in parallel development):
   - Being developed separately, expected ready before Phase 2
   - Will provide: `compile(source) → { bytecode, diagnostics }`
   - Key files: `akkado/src/akkado.cpp` (semantic analysis, DAG, codegen)

2. **WASM Bindings**: Create JavaScript API for:
   - `akkado.wasm`: `compile(source: string) → { bytecode: Uint8Array, diagnostics: Diagnostic[] }`
   - `cedar.wasm`: `VM.loadProgram(bytecode)`, `VM.processBlock(left, right)`
   - Use Emscripten `embind` or raw exports

3. **AudioWorklet Bridge**: SharedArrayBuffer for:
   - Bytecode transfer (main → worklet)
   - Visualization data (worklet → main)
   - Beat clock synchronization (phase, beat number)
   - Pattern event notifications for highlighting

---

## Verification Plan

### Testing Strategy

1. **Unit Tests**: Akkado compiler output, Cedar VM opcodes
2. **Integration Tests**: Full compile → run → output pipeline
3. **Visual Tests**: Screenshot comparisons for visualizations
4. **Audio Tests**: Reference recordings for DSP accuracy

### Manual Verification Steps

1. Load IDE in browser, verify WASM modules load
2. Type simple program: `sin(440) |> out(%, %)`
3. Press Ctrl+Enter, verify audio output
4. Modify frequency, verify hot-swap (no click)
5. Check waveform visualization matches audio
6. Open docs, run interactive example
7. Test on Chrome, Firefox, Safari

---

## References

- [Strudel.cc REPL](https://strudel.cc/technical-manual/repl/) - Inspiration for architecture
- [Strudel Codeberg](https://codeberg.org/uzu/strudel) - Tech stack reference
- [MDN Web Audio Visualizations](https://developer.mozilla.org/en-US/docs/Web/API/Web_Audio_API/Visualizations_with_Web_Audio_API)
- [CodeMirror 6](https://codemirror.net/) - Editor framework
- [Svelte 5](https://svelte.dev/) - UI framework
- [wavesurfer.js](https://wavesurfer.xyz/) - Waveform library reference
