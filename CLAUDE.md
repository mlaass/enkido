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
- `as` (pipe binding): Named binding for multi-stage access: `expr as name`
- Mini-notation patterns: `pat()`, `seq()`, `timeline()`, `note()` - see [Mini-Notation Reference](docs/mini-notation-reference.md)

### Records and Field Access
Record literals allow grouping related values:
```akkado
rec = {freq: 440, vel: 0.8}
rec.freq  // 440
```

Pattern events are records with fields accessible via `%`:
- `%.freq` / `%.pitch` / `%.f` - Frequency in Hz
- `%.vel` / `%.velocity` / `%.v` - Velocity (0-1)
- `%.trig` / `%.trigger` / `%.t` - Trigger pulse
- `%.note` / `%.midi` / `%.n` - MIDI note number
- `%.dur`, `%.chance`, `%.time`, `%.phase` - Extended fields

Example with pipe binding:
```akkado
pat("c4 e4 g4") as e |> osc("sin", e.freq) |> % * e.vel |> out(%)
```

### Chord Expansion (Strudel-compatible)
Chords are signal arrays that auto-expand UGens:
- `C4'` → major chord → `[261.6, 329.6, 392.0]` Hz
- `Am7'` → A minor 7th chord
- `F#m7_4'` → chord with slash bass

### Parameter Exposure
Runtime controls exposed in the web UI:
```akkado
freq = param("freq", 440, 20, 2000)  // slider with min/max
on = toggle("mute", false)           // on/off toggle
hit = button("trigger")              // momentary button
wave = dropdown("wave", ["sin", "saw", "tri"])
```

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
./build/tools/nkido-cli/nkido-cli --help
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

## Releases

Version bumps go through `scripts/bump-version.sh` — **never edit `VERSION`
or `web/package.json` versions by hand, and never create a `vX.Y.Z` tag
manually.**

```bash
./scripts/bump-version.sh <major|minor|patch>
```

The script reads the current version from `VERSION`, computes the new one,
and refuses to run unless:
- the working tree is clean,
- the tag `vX.Y.Z` does not already exist,
- `CHANGELOG.md` has a matching `## [X.Y.Z]` section (add it first via the
  `/update-changelog` skill).

On success it updates `VERSION` + `web/package.json`, commits `Release vX.Y.Z`,
and creates the `vX.Y.Z` tag. Push with `git push origin master --tags`.

The correct release order is: (1) write the CHANGELOG entry, (2) run
`bump-version.sh`, (3) push. Tagging before the script runs leaves `VERSION`
out of sync with the tag.

## Project Structure

```
nkido/
├── cedar/          # Synth engine (standalone library)
│   ├── include/cedar/
│   ├── src/
│   └── tests/
├── akkado/         # Language compiler (depends on cedar)
│   ├── include/akkado/
│   ├── src/
│   └── tests/
├── tools/
│   ├── nkido-cli/ # Bytecode player with audio engine
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

# Debug WASM build with assertions (for debugging WASM crashes)
cd web/wasm && ./build_debug.sh
```

### Web Architecture

**State Management**: Uses Svelte 5 runes with singleton store pattern.

Stores in `src/lib/stores/`:
- `audio.svelte.ts` - Audio engine, playback, visualization, pattern info, state inspection
- `editor.svelte.ts` - Code state, compile status
- `settings.svelte.ts` - UI preferences (panel position, font size, audio config)
- `theme.svelte.ts` - Theme selection, custom themes, CSS variable application
- `docs.svelte.ts` - Documentation system, F1 help lookup
- `pattern-highlight.svelte.ts` - Pattern preview data and active step highlighting

**Key Components** in `src/lib/components/`:
- `Transport/` - Play/pause, BPM, volume controls
- `Editor/` - CodeMirror 6 integration with instruction-to-source highlighting
- `Panel/` - Resizable sidebar with tabs, debug panels
- `Panel/PatternDebugPanel.svelte` - AST visualization, sequence events, source location mapping
- `Panel/StateInspector.svelte` - Live state inspection for stateful opcodes (20Hz polling)
- `Params/` - Runtime parameter controls (ParamSlider, ParamButton, ParamToggle, ParamSelect)
- `Theme/` - Theme selector and color editor
- `Logo/` - Inline SVG logo component

**Theme System**:
- CSS variables defined in `app.css`, dynamically set by theme store
- 7 preset themes (GitHub Dark/Light, Monokai, Dracula, Solarized, Nord, High Contrast)
- Custom themes stored in localStorage (`nkido-theme` key)
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

## Code Generation

### Opcode Metadata

The opcode metadata (name strings and statefulness flags) is auto-generated from source files to avoid manual synchronization.

**When adding new opcodes:**

```bash
cd web && bun run build:opcodes
```

This parses:
- `cedar/include/cedar/vm/instruction.hpp` - extracts Opcode enum values
- `akkado/include/akkado/builtins.hpp` - extracts `requires_state` flags

And generates:
- `cedar/include/cedar/generated/opcode_metadata.hpp` - provides `cedar::opcode_to_string()` and `cedar::opcode_is_stateful()`

The generated header is used by:
- `web/wasm/nkido_wasm.cpp` - for debug disassembly in web UI
- `tools/nkido-cli/bytecode_dump.cpp` - for CLI bytecode inspection

### Pattern Debug Serialization

The pattern debugging system serializes AST and events as JSON:
- `akkado::serialize_mini_ast_json()` - Converts mini-notation AST to JSON for web UI
- `akkado::serialize_sequences_json()` - Exports compiled sequences and events
- `cedar::StatePool::inspect_state_json()` - JSON representation of all DSP state types

## Python Experiments

The `experiments/` directory contains Python scripts for testing Cedar opcodes via the `cedar_core` bindings. Each opcode has its own test file following the pattern `test_op_<codename>.py`. For the full testing philosophy, see [DSP Experiment Methodology](docs/dsp-experiment-methodology.md).

The Python bindings (`cedar_core`) are built to `experiments/cedar_core.cpython-*.so` by the `cedar_core` CMake target.

### Running Experiments

```bash
cd experiments

# Run a single opcode test
uv run python test_op_lp.py
uv run python test_op_chorus.py

# Run all tests
./run_all.sh
./run_all.sh --stop-on-error
```

### File Structure

- `test_op_<codename>.py` — one file per opcode (e.g., `test_op_lp.py`, `test_op_fold.py`, `test_op_adsr.py`)
- `cedar_testing.py` — `CedarTestHost` class and `output_dir()` helper
- `filter_helpers.py` — `analyze_filter()`, `get_bode_data()`, `get_impulse()` for filter tests
- `visualize.py` — `save_figure()` for consistent PNG output
- `utils.py` — general utilities
- `run_all.sh` — runs all `test_op_*.py` files and reports pass/fail summary
- `output/op_<codename>/` — WAV and PNG output per opcode (gitignored)

### Creating Opcode Experiments

**Critical Guidelines**:

1. **Tests verify expected behavior** - Design tests based on documented/expected algorithm behavior, NOT observed behavior
2. **Never adjust tests to fit data** - If a test fails, investigate the implementation, don't change the test to pass
3. **Always output WAV files** - Human ears are the ultimate judge of audio quality. Save WAV files for every test:
   ```python
   wav_path = os.path.join(OUT, "test_something.wav")
   scipy.io.wavfile.write(wav_path, host.sr, output)
   print(f"  Saved {wav_path} - Listen for [describe what to listen for]")
   ```
4. **Report pass/fail clearly** - Use ✓/✗/⚠ symbols and explain what the expected vs actual behavior is
5. **Document acceptance criteria** - Each test should have clear, measurable criteria in the docstring
6. **Run for ≥ 300 seconds of simulated audio** - Bugs that show up "every few bars" need a long window to surface. Any opcode test that drives a sequence/pattern/poly/sampler over time MUST simulate at least 300 seconds of audio (per the `--seconds` flag for `nkido-cli render`, or per the block count in `CedarTestHost`). Trace-only checks (no WAV write) are fine for the long part — render a shorter WAV separately for human listening if file size matters. If a test fails: report the failure block/time, do NOT shorten the duration to make it pass.

**Test Structure**:
```python
from cedar_testing import CedarTestHost, output_dir

OUT = output_dir("op_<codename>")

def test_something():
    """
    Test OPCODE for [behavior].

    Expected behavior (per implementation):
    - [specific measurable criterion 1]
    - [specific measurable criterion 2]

    If this test fails, check the implementation in cedar/include/cedar/opcodes/<file>.hpp
    """
    host = CedarTestHost()
    # ... set up instructions, process blocks ...

    # Save WAV for human evaluation
    wav_path = os.path.join(OUT, "test_something.wav")
    scipy.io.wavfile.write(wav_path, host.sr, output)
    print(f"  Saved {wav_path} - Listen for [specific thing]")

    # Report results with clear pass/fail
    if meets_criteria:
        print(f"  ✓ PASS: [what passed]")
    else:
        print(f"  ✗ FAIL: [what failed] - Check implementation")
```

**When a test fails**:
1. Do NOT modify the test to make it pass
2. Investigate the C++ implementation
3. Discuss with user whether the algorithm needs fixing
4. If the expected behavior was wrong, update both test AND documentation

**Update checklist**: After adding tests, update `docs/dsp-quality-checklist.md` to reflect test coverage.

## Implementation Notes

### Effect Parameters
- **Dry/wet mixing convention**: All delays and filters should have explicit `dry` and `wet` parameters in their function signature. This is the standard interface for mixable effects.
  ```akkado
  delay(input, time, feedback, dry, wet)  // Standard delay signature
  filter_lp(input, freq, q, dry, wet)     // Filters follow same pattern
  ```
- Effects without dry/wet params (chorus, flanger, phaser, reverbs) output 100% wet signal. Users mix manually:
  ```akkado
  dry = osc("saw", 220)
  dry * 0.3 + chorus(dry, 0.5, 0.5) * 0.7 |> out(%)  // 30% dry, 70% wet
  ```
- Never use bit-packing tricks for parameters. Use the 5 input slots and extended params properly.

### Record-as-Options Convention
Builtins that need more than ~3–4 parameters take a record literal as the last positional argument. Declare the option fields via `OptionSchema` on the `BuiltinInfo` (see `akkado/include/akkado/builtins.hpp` — visualizers like `waterfall` are the worked example). Codegen reads the caller's record through `codegen::extract_options(arena, node, schema)` (`akkado/include/akkado/codegen/options.hpp`); the helper validates field names, drops unknown fields silently into `OptionsPayload::unknown_fields` (reserved for a future `W160` warning pass), and emits canonical compact JSON via `to_json()`. Editor autocomplete picks up the schema automatically through `akkado_get_builtins_json()`.

Adopters today: visualizers (`pianoroll`, `oscilloscope`, `waveform`, `spectrum`, `waterfall`). Recommended next: samplers, filters, delays/reverbs — each owned by its own per-family PRD. See `web/static/docs/concepts/record-as-options.md` for the full convention and authoring guide.

### Thread Safety
- Triple-buffer approach: compiler writes to "Next", audio reads from "Current"
- Lock-free SPSC queues for parameter updates
- Atomic pointer swap at block boundaries

### Performance
- Use `[[likely]]`/`[[unlikely]]` hints in VM switch
- SIMD (SSE/AVX) for hot loops
- Consider cpp-taskflow for parallel DAG branches

### Extended Parameter Patterns

The instruction format has 5 input buffer slots (`inst.inputs[0..4]`).
When a builtin needs more than 5 runtime-tunable parameters, use
`ExtendedParams<N>` — the canonical mechanism. Full design + worked
migration: **`docs/extended-params-mechanism.md`**.

#### Quick reference

Declare the parameter on the builtin:

```cpp
{"phaser", {.opcode = cedar::Opcode::EFFECT_PHASER,
            .input_count = 1, .optional_count = 4, .requires_state = true,
            .param_names = {"in", "rate", "depth", "min_freq", "max_freq", ""},
            .defaults = {0.5f, 0.8f, 200.0f, 4000.0f, NAN},
            .description = "...",
            .extended_param_count = 3,
            .output_channels = ChannelCount::Stereo,
            .stereo_native = true,
            .extended_param_names = {"feedback", "stages", "lfo_phase"},
            .extended_defaults = {0.5f, 4.0f, 0.25f}}},
```

Read the slot in the opcode body. The companion `ExtendedParams<N>`
lives at `ext_params_state_id(inst.state_id)` (XOR'd to avoid colliding
with the opcode's primary DSP state):

```cpp
const auto* ext = ctx.states->get_if<ExtendedParams<3>>(
    ext_params_state_id(inst.state_id));
const float* feedback_buf = nullptr; float feedback_const = 0.5f;
if (ext) {
    const auto& slot = ext->params[0];
    if (slot.is_constant()) feedback_const = slot.constant;
    else                    feedback_buf   = ctx.buffers->get(slot.buffer_idx);
}
// Per-sample read inside the BLOCK_SIZE loop:
float feedback = feedback_buf ? feedback_buf[i] : feedback_const;
```

#### `inst.rate` is reserved, NOT deprecated

`inst.rate` is the right tool for:
- audio-rate vs control-rate dispatch (its original purpose)
- small fixed enum modes ≤4 values (CLOCK 0/1/2, EDGE_OP 0-3, LFO shape)
- compile-time count fields with no audio meaning (ARRAY_PACK count, etc.)

**Do not bit-pack runtime-tunable params into `inst.rate` in new
opcodes.** Existing usages (compressor attack/release, limiter, freeverb
damping/mod_depth, dattorro size_mod, distort_comb damp, delays
ping-pong mix) will migrate per-family.

#### Deprecated workarounds — do not use in new code

- `std::bit_cast<float>(state_id)` — clobbers state_id, blocks stateful
  upgrade. Migrate any new use to ExtendedParams.
- `inputs[3]/inputs[4]` halving to encode a 32-bit float — sacrifices
  real signal slots. Migrate to ExtendedParams.

The "default constant" pattern (`defaults[]` array on `BuiltinInfo`) is
still fine and is the right thing for any optional input slot ≤5.
