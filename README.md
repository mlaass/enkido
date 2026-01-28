# NKIDO

[![Build and Deploy](https://github.com/mlaass/enkido/actions/workflows/deploy.yml/badge.svg)](https://github.com/mlaass/enkido/actions/workflows/deploy.yml)

A high-performance audio synthesis system for live coding, combining a domain-specific language with a real-time audio engine.

## Components

- **Akkado** - A DSL for live-coding musical patterns and modular synthesis, combining Strudel/Tidal-style mini-notation with functional DAG-based audio processing
- **Cedar** - A graph-based audio synthesis engine with a stack-based bytecode VM, designed for real-time DSP with zero allocations
- **Web IDE** - A browser-based live coding environment built with SvelteKit

## Quick Start

### Native Build

```bash
# Configure and build
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Run tests
./build/cedar/tests/cedar_tests
./build/akkado/tests/akkado_tests

# Use CLI tools
./build/tools/akkado-cli/akkado-cli --help
./build/tools/cedar-cli/cedar-cli --help
```

### Web IDE

```bash
cd web
bun install
bun run dev
```

## Requirements

- C++20 compiler (GCC 10+, Clang 10+, MSVC 2019+)
- CMake 3.21+
- Bun (for web app)
- Emscripten (for WASM builds)

## Example

```akkado
// Simple oscillator patch
osc("sin", 440) |> out(%, %)

// Pattern-based sequence with filter
note("c4 e4 g4 c5") |> osc("saw", %) |> lp(%, 1000, 0.7) |> out(%, %)

// Chord with envelope
C4' |> osc("tri", %) |> adsr(trigger(1), 0.01, 0.2) |> out(%, %)
```

## Architecture

```
Source Code -> Lexer -> Parser -> AST -> DAG -> Topological Sort -> Bytecode -> VM
     ^                                                                   |
     |___________________ Hot-swap state preservation ___________________|
```

The system supports glitch-free live coding through semantic ID tracking and micro-crossfading between program versions.

## Development

### Code Generation

After adding new opcodes to Cedar, regenerate the opcode metadata:

```bash
cd web && bun run build:opcodes
```

This generates `cedar/include/cedar/generated/opcode_metadata.hpp` from the source files.

## Documentation

See `docs/` for design documents and `web/static/docs/` for user-facing documentation.

## License

MIT
