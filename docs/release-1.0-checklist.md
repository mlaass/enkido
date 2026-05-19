> **Status: TRACKING** — Master release checklist.

# NKIDO 1.0 Release Checklist

## Features Completed

### Language (Akkado)

- [x] Pipe operator (`|>`) for signal flow
- [x] Hole operator (`%`) for input injection
- [x] Pipe binding (`as`) for named multi-stage access
- [x] Record literals and field access (`{freq: 440}`, `rec.freq`)
- [x] Pattern event fields (`%.freq`, `%.vel`, `%.trig`, `%.note`, `%.dur`, etc.)
- [x] Field aliases (`%.f`, `%.v`, `%.t`, `%.n`)
- [x] Array literals and indexing
- [x] Function definitions (`fn name(params) = expr`)
- [x] Closures with environment capture
- [x] Docstrings (`///`)
- [x] Binary operators (`+`, `-`, `*`, `/`, `^`, comparisons, logical)
- [x] Variadic function arguments
- [ ] Compile-time and runtime `match`
- [ ] JS-style dictionaries/objects
- [ ] Multiline strings
- [ ] Code import (source modules)
- [ ] Compiled code / library import

### Mini-Notation

- [x] Notes: pitches (`c4`, `f#3`, `Bb5`)
- [x] Chords: `C`, `Am7`, `F#m7_4'` (slash bass)
- [x] Sample names: `bd`, `sd:1`, `kick:2` (with variant)
- [x] Rests (`~`) and elongation (`_`)
- [x] Grouping: `[a b]`, alternation `<a b c>`, polyrhythm `[a b, c d]`, polymeter `{a b c}%4`
- [x] Speed (`*n`), slow (`/n`), weight (`@n`), repeat (`!n`)
- [x] Chance (`?n`), random choice (`a|b|c`)
- [x] Euclidean rhythms in patterns: `bd(3,8)`, `sd(3,8,1)`
- [x] Pattern literals: `n"…"`, `v"…"`, `s"…"`, `c"…"`, `seq()`, `timeline()`
- [x] Pattern transformations: `slow`, `fast`, `rev`, `transpose`, `velocity`
- [x] Sample bank/variant control: `bank()`, `variant()`
- [ ] Microtonal mini-notation

### Sample Loading

- [x] Web (URL / GitHub shortcut)
- [x] Local files
- [x] Builtin samples (default drum kit)
- [x] Soundfonts

### Parameter Exposure

- [x] `param(name, default, min, max)` — continuous slider
- [x] `toggle(name, default)` — boolean on/off
- [x] `button(name)` — momentary trigger
- [x] `dropdown(name, ...)` — selection menu

### Array / Polyphony

- [x] Chord expansion auto-expands UGens
- [x] `spread(n, source)` — force voice count
- [x] Higher-order: `map`, `fold`, `zipWith`, `zip`
- [x] Transforms: `sum`, `product`, `mean`, `reverse`, `rotate`, `shuffle`, `sort`, `normalize`, `scale`
- [x] Slicing: `take`, `drop`, `len`
- [x] Generation: `range`, `repeat`, `linspace`, `random`, `harmonics`

### Audio Engine (Cedar)

- [x] Stack-based bytecode VM (95+ opcodes)
- [x] Dual-channel A/B glitch-free crossfading
- [x] Block processing: 128 samples @ 48kHz (2.67ms)
- [x] Zero-allocation audio path (pre-allocated pools)
- [x] DFS post-order DAG traversal
- [x] Hot-swap with semantic ID state preservation
- [x] 5–10ms micro-crossfade on structural changes
- [x] Garbage collection of unused states
- [x] Lock-free SPSC parameter queues
- [x] Triple-buffer compiler→audio handoff

### Web App — Editor

- [x] CodeMirror 6 with Akkado syntax highlighting
- [x] Mini-notation highlighting inside strings
- [x] Autocomplete (builtins, user functions, named args)
- [x] Signature help with parameter tooltips
- [x] Inline error squiggles and hover messages
- [x] Instruction-to-source click highlighting
- [x] Inline pattern preview widgets (piano rolls)
- [x] Active step highlighting during playback
- [x] Font size control (10–24px)
- [x] Better syntax highlighting in web frontend

### Web App — Transport & Audio

- [x] Play / pause / stop
- [x] BPM control (20–999)
- [x] Volume slider
- [x] Beat / bar display
- [x] Sample rate selection (44.1k, 48k, 88.2k, 96k)

### Web App — Panel & Debug

- [x] Resizable sidebar (left / right)
- [x] Tabs: Controls, Settings, Docs, Debug
- [x] Parameter controls (slider, toggle, button, dropdown)
- [x] Bytecode disassembly view
- [x] Pattern debug panel (AST tree, events, source mapping)
- [x] State inspector (live 20Hz polling, JSON state)
- [x] Compile diagnostics (bytecode size, sample list, state count)

### Web App — Themes

- [x] 7 preset themes (GitHub Dark/Light, Monokai, Dracula, Solarized, Nord, High Contrast)
- [x] Custom theme editor with live preview
- [x] CSS variable system with localStorage persistence

### Web App — Documentation

- [x] F1 help with instant keyword lookup
- [x] Markdown docs with YAML frontmatter
- [x] Pre-built lookup index (build-time generation)

### Web App — Visualizations

- [x] `pianoroll` — inline piano roll
- [x] `oscilloscope` — scope view
- [x] `waveform` — waveform display
- [x] `spectrum` — frequency analyzer
- [x] PROBE opcode for ring buffer capture
- [x] Pattern-aware scrolling with playhead

### Build & Tooling

- [x] CMake build system with presets (debug, release, cedar-only, wasm)
- [x] Emscripten WASM build
- [x] AudioWorklet integration
- [x] CLI tools: `nkido-cli`, `akkado-cli`
- [x] Python bindings (`cedar_core`) for DSP experiments
- [x] Auto-generated opcode metadata
- [x] Auto-generated docs lookup index
- [x] Catch2 test suites (cedar + akkado)
- [ ] Godot extension

---

## DSP Quality Testing

Test every DSP opcode for correctness. Each item should have an experiment script in `experiments/` producing WAV output for human evaluation.

### Oscillators

- [ ] `osc("sin")` — Sine oscillator
- [ ] `osc("tri")` — Triangle oscillator
- [ ] `osc("saw")` — Band-limited sawtooth
- [ ] `osc("sqr")` — Band-limited square
- [ ] `osc("ramp")` — Rising ramp (0–1)
- [ ] `osc("phasor")` — Phase accumulator
- [ ] `sqr_pwm` — Pulse width modulated square (PolyBLEP)
- [ ] `saw_pwm` — Variable-width sawtooth
- [ ] `sqr_minblep` — MinBLEP anti-aliased square
- [ ] `sqr_pwm_minblep` — MinBLEP PWM square (highest quality)
- [ ] `sqr_pwm_4x` — 4x oversampled PWM square
- [ ] `saw_pwm_4x` — 4x oversampled variable-slope saw
- [ ] Oversampled variants (2x/4x sin, saw, sqr, tri) — alias-free FM

### Filters

- [ ] `lp` — SVF lowpass
- [ ] `hp` — SVF highpass
- [ ] `bp` — SVF bandpass
- [ ] `moog` — 4-pole Moog ladder
- [ ] `diode` — ZDF diode ladder (TB-303)
- [ ] `formant` — 3-band vowel morphing
- [ ] `sallenkey` — MS-20 style Sallen-Key

### Envelopes

- [ ] `adsr` — Attack-decay-sustain-release
- [ ] `ar` — Attack-release (one-shot)
- [ ] `env_follower` — Amplitude envelope follower

### Delays

- [ ] `delay` — Basic delay (seconds)
- [ ] `delay_ms` — Delay (milliseconds)
- [ ] `delay_smp` — Delay (samples)
- [ ] `tap_delay` — Tap delay with feedback chain (seconds)
- [ ] `tap_delay_ms` — Tap delay with feedback chain (ms)
- [ ] `tap_delay_smp` — Tap delay with feedback chain (samples)
- [ ] `pingpong` — Ping-pong stereo delay

### Reverbs

- [ ] `freeverb` — Schroeder-Moorer algorithmic reverb
- [ ] `dattorro` — Plate reverb
- [ ] `fdn` — Feedback Delay Network reverb

### Modulation Effects

- [ ] `chorus` — Multi-voice chorus
- [ ] `flanger` — Classic flanger
- [ ] `phaser` — Multi-stage phaser (cascaded allpass)
- [ ] `comb` — Feedback comb filter

### Distortion

- [ ] `saturate` — Tanh saturation
- [ ] `softclip` — Polynomial soft clipping
- [ ] `fold` — Wavefolding
- [ ] `bitcrush` — Bit depth & sample rate reduction
- [ ] `tube` — Asymmetric tube saturation
- [ ] `smooth` — ADAA alias-free saturation
- [ ] `tape` — Tape-style saturation with warmth
- [ ] `xfmr` — Transformer saturation with bass emphasis
- [ ] `excite` — Harmonic exciter (HF enhancement)

### Dynamics

- [ ] `comp` — Feedforward compressor
- [ ] `limiter` — Brick-wall limiter with lookahead
- [ ] `gate` — Noise gate with hysteresis

### Samplers

- [ ] `sample` — One-shot sample playback
- [ ] `sample_loop` — Looping sample playback

### Sequencers & Timing

- [ ] `clock` — Beat/bar/cycle phase output
- [ ] `lfo` — Beat-synced LFO
- [ ] `seq_step` — Step sequencer
- [ ] `euclid` — Euclidean rhythm generator
- [ ] `trigger` — Beat-division impulse generator
- [ ] `timeline` — Breakpoint automation timeline

### Stereo

- [ ] `pan` — Mono-to-stereo panning
- [ ] `width` — Stereo width control
- [ ] `ms_encode` — Stereo to mid/side
- [ ] `ms_decode` — Mid/side to stereo

### Utility

- [ ] `noise` — White / sample-and-hold noise
- [ ] `mtof` — MIDI note to frequency
- [ ] `dc` — DC offset
- [ ] `slew` — Slew rate limiter (portamento)
- [ ] `sah` — Sample and hold

### Arithmetic

- [ ] `add` — Addition
- [ ] `sub` — Subtraction
- [ ] `mul` — Multiplication
- [ ] `div` — Division
- [ ] `pow` — Exponentiation
- [ ] `neg` — Negation

### Math Functions

- [ ] `abs` — Absolute value
- [ ] `sqrt` — Square root
- [ ] `log` — Natural logarithm
- [ ] `exp` — Exponential (e^x)
- [ ] `floor` — Round down
- [ ] `ceil` — Round up
- [ ] `min` — Minimum
- [ ] `max` — Maximum
- [ ] `clamp` — Clamp to range
- [ ] `wrap` — Wrap to range

### Trigonometric

- [ ] `sin` — Sine (radians)
- [ ] `cos` — Cosine
- [ ] `tan` — Tangent
- [ ] `asin` — Arc sine
- [ ] `acos` — Arc cosine
- [ ] `atan` — Arc tangent
- [ ] `atan2` — Two-argument arc tangent

### Hyperbolic

- [ ] `sinh` — Hyperbolic sine
- [ ] `cosh` — Hyperbolic cosine
- [ ] `tanh` — Hyperbolic tangent

### Comparison & Logic

- [ ] `gt` — Greater than
- [ ] `lt` — Less than
- [ ] `gte` — Greater or equal
- [ ] `lte` — Less or equal
- [ ] `eq` — Approximate equality
- [ ] `neq` — Not equal
- [ ] `band` — Logical AND
- [ ] `bor` — Logical OR
- [ ] `bnot` — Logical NOT
- [ ] `select` — Signal mux (ternary)

### Array Operations

- [ ] `map` — Apply function to each element
- [ ] `fold` — Reduce with binary function
- [ ] `zipWith` — Combine arrays element-wise
- [ ] `zip` — Interleave two arrays
- [ ] `sum` — Sum all elements
- [ ] `product` — Multiply all elements
- [ ] `mean` — Average
- [ ] `min` / `max` — Array reduction
- [ ] `reverse` — Reverse order
- [ ] `rotate` — Rotate by n positions
- [ ] `shuffle` — Deterministic permutation
- [ ] `sort` — Ascending sort
- [ ] `normalize` — Scale to 0–1
- [ ] `scale` — Scale to range
- [ ] `take` / `drop` — Slice operations
- [ ] `len` — Array length
- [ ] `range` — Integer range generation
- [ ] `repeat` — Repeat value n times
- [ ] `linspace` — Evenly spaced values
- [ ] `random` — Deterministic random values
- [ ] `harmonics` — Harmonic series generation
- [ ] `spread` — Force polyphony count
