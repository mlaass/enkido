# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**NKIDO** is a high-performance audio synthesis system with three main components:

- **Akkado**: A domain-specific language (DSL) for live-coding musical patterns and modular synthesis, combining Strudel/Tidal-style mini-notation with functional DAG-based audio processing
- **Cedar**: A graph-based audio synthesis engine with a stack-based bytecode VM, designed for real-time DSP with zero allocations

## Architecture

### Compiler Pipeline (Akkado → Cedar)

```
Source Code → Lexer → Parser (Pratt/Precedence Climbing) → AST → DAG → Topological Sort → Bytecode
```

Key design decisions:
- **String interning** with FNV-1a hashing for fast identifier comparison
- **Arena-allocated AST** using indices instead of pointers for cache locality
- **Semantic ID path tracking** for hot-swap state preservation (e.g., `main/track1/osc` → stable hash)

### Cedar VM Architecture

- **Stack-based bytecode interpreter** with 95+ opcodes
- **Dual-channel A/B architecture** for glitch-free crossfading between programs
- **Block processing**: 128 samples per block at 48kHz (2.67ms latency)
- **Pre-allocated memory pools** - no runtime allocations in audio path

Memory constants:
- `MAX_ARENA_SIZE`: 128MB for audio buffers
- `MAX_STACK_SIZE`: 64 values
- `MAX_VARS`: 4096 variable slots
- `MAX_DSP_ID`: 4096 concurrent DSP blocks

### Audio Graph Model

Cedar uses a DAG processed via DFS post-order traversal:
1. Traverse from destination node backwards through inputs
2. Process nodes only after all dependencies are ready
3. Buffers are fixed-size arrays (typically 128 samples)

### Hot-Swapping (Live Coding)

State preservation during code updates:
1. Match nodes by semantic ID hash
2. Rebind matching IDs to existing state in StatePool
3. Apply 5-10ms micro-crossfade for structural changes
4. Garbage collect untouched states after N blocks

## Akkado Language Concepts

### Core Operators
- `|>` (pipe): Defines signal flow through the DAG
- `%` (hole): Explicit input port for signal injection
- Mini-notation patterns: `pat()`, `seq()`, `timeline()`, `note()`

### Chord Expansion
Chords are signal arrays that auto-expand UGens:
- `'c4:maj'` → `[261.6, 329.6, 392.0]` Hz
- `'c3e3g3'` → inline chord notation

### Clock System
- 1 cycle = 4 beats by default
- `co`: cycle offset (0-1 ramp)
- `beat(n)`: phasor completing every n beats

## Key DSP Opcodes

Categories: Oscillators (SIN/TRI/SAW/SQR), Filters (biquad, SVF, Moog, diode ladder), Envelopes (ADSR, AR), Delays/Reverbs (Dattorro, Freeverb, Lexicon, Velvet), Sequencers (step, euclidean, timeline), Sample playback (granular, Karplus-Strong), Effects (chorus, flanger, vocoder, bitcrusher)

## Build Commands

Requires C++20 compiler (GCC 10+, Clang 10+, MSVC 2019+).

```bash
# Configure (debug build with tests)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Build everything
cmake --build build

# Build only cedar
cmake --build build --target cedar

# Build only akkado
cmake --build build --target akkado

# Run all tests
./build/cedar/tests/cedar_tests
./build/akkado/tests/akkado_tests

# Run single test (Catch2 pattern matching)
./build/cedar/tests/cedar_tests "VM executes*"
./build/akkado/tests/akkado_tests "[parser]"  # Run by tag

# Run CLI tools
./build/tools/cedar-cli/cedar-cli --help
./build/tools/akkado-cli/akkado-cli --help

# Using presets
cmake --preset debug       # Debug build
cmake --preset release     # Release build
cmake --preset cedar-only  # Cedar without akkado
cmake --preset wasm        # WebAssembly build (requires Emscripten)

# Build cedar standalone (from cedar/ directory)
cmake -B build-cedar cedar/
cmake --build build-cedar
```

## Project Structure

```
enkido/
├── cedar/          # Synth engine (standalone library)
│   ├── include/cedar/
│   ├── src/
│   └── tests/
├── akkado/         # Language compiler (depends on cedar)
│   ├── include/akkado/
│   ├── src/
│   └── tests/
├── tools/
│   ├── cedar-cli/  # Bytecode player
│   └── akkado-cli/ # Compiler CLI
├── web/            # SvelteKit web app
│   ├── src/
│   ├── static/docs/  # Markdown documentation
│   └── scripts/      # Build scripts
├── cmake/          # CMake modules
└── docs/           # Design documentation
```

## Web App

The web app is a SvelteKit application in the `web/` directory. Always use `bun` (not npm).

```bash
cd web

# Development
bun run dev

# Build (includes docs index generation)
bun run build

# Type checking
bun run check

# Rebuild WASM module (requires Emscripten)
bun run build:wasm
```

### Web Architecture

**State Management**: Uses Svelte 5 runes with singleton store pattern.

Stores in `src/lib/stores/`:
- `audio.svelte.ts` - Audio engine, playback, visualization
- `editor.svelte.ts` - Code state, compile status
- `settings.svelte.ts` - UI preferences (panel position, font size, audio config)
- `theme.svelte.ts` - Theme selection, custom themes, CSS variable application
- `docs.svelte.ts` - Documentation system, F1 help lookup

**Key Components** in `src/lib/components/`:
- `Transport/` - Play/pause, BPM, volume controls
- `Editor/` - CodeMirror 6 integration
- `Panel/` - Resizable sidebar with tabs
- `Theme/` - Theme selector and color editor
- `Logo/` - Inline SVG logo component

**Theme System**:
- CSS variables defined in `app.css`, dynamically set by theme store
- 7 preset themes (GitHub Dark/Light, Monokai, Dracula, Solarized, Nord, High Contrast)
- Custom themes stored in localStorage (`enkido-theme` key)
- All UI elements use CSS variables for consistent theming

### Documentation System

Documentation lives in `web/static/docs/` as markdown files with YAML frontmatter. The F1 help system uses a pre-built lookup index for instant keyword lookup.

**When adding or modifying documentation:**

```bash
# Rebuild the docs lookup index after changing markdown files
bun run build:docs
```

This generates `src/lib/docs/lookup-index.ts` which maps keywords to documentation sections. The index is built from:
- Frontmatter `keywords` arrays
- H2 headings in builtin docs (for function-level anchors)

## Implementation Notes

### Thread Safety
- Triple-buffer approach: compiler writes to "Next", audio reads from "Current"
- Lock-free SPSC queues for parameter updates
- Atomic pointer swap at block boundaries

### Performance
- Use `[[likely]]`/`[[unlikely]]` hints in VM switch
- SIMD (SSE/AVX) for hot loops
- Consider cpp-taskflow for parallel DAG branches
