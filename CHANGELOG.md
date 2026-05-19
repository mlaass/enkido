# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### ⚠ BREAKING — mini-notation: cycle = beat, top-level = alternation

The clock unit and the mini-notation top-level grouping rule both
change to a single coherent model:

- **1 cycle = 1 beat.** BPM directly sets the cycle rate.
- **Top-level spaces play one element per cycle.** `"a b c d"` plays
  four cycles in sequence (one element per cycle), exactly equivalent
  to the angle-bracket form `<a b c d>`.
- **`[a b c d]` packs four elements into one cycle** (the explicit
  subdivision form). Use this when you want Strudel/Tidal-style
  in-cycle subdivision.
- **`<a b c>` is a documented synonym of the top-level form.**

This is a **deliberate divergence from Strudel/Tidal**, which treats
top-level as subdivision. We chose per-cycle alternation so long
melodies stay readable as `"c d e f g a b c5"` without anyone having
to count elements to predict playback speed.

Engine-side: `ExecutionContext::samples_per_cycle()` no longer
multiplies by 4; every `cycle_length` default flipped from 4 beats to
1 beat; bar phase collapses to beat phase. The codegen dispatch for
`MiniPattern` routes through `compile_alternate_sequence` (the same
path as `<…>`), with the existing single-child inline guard preserving
`pat("a") ≡ pat("<a>") ≡ pat("[a]")` byte-equivalence.

### ⚠ BREAKING — delay-family default mix changed

The `delay`, `delay_ms`, `delay_smp`, `tap_delay`, `tap_delay_ms`,
`tap_delay_smp`, and `pingpong` builtins migrated from full-wet output
to the new unified Category-A defaults `dry=1, wet=0.5` (parallel mix).
Patches that called `delay(in, time, fb)` or `pingpong(s, time, fb)`
and relied on a fully-wet output now get a balanced parallel mix and
will sound different. Set `wet=1` (or `dry=0, wet=1`) explicitly to
restore the previous behaviour. The `pingpong` opcode previously did
`out = in + delayed` (effectively `dry=1, wet=1`) — pass `wet: 1.0` to
reproduce the prior echo loudness.

### Added

- **Unified `dry`/`wet` convention across every effect builtin** — all 33
  effect builtins (delays incl. `pingpong`, reverbs, modulation, comb,
  filters, distortion, dynamics) now expose `dry` and `wet` as their
  last two parameters and apply the standard mix
  `out = dry_in * dry + processed * wet` per channel. Two category-based
  defaults:
  - **Category A — Additive** (delays, reverbs, modulation, comb):
    `dry=1.0, wet=0.5` (balanced parallel mix out of the box).
  - **Category B — Transform** (filters, distortion, dynamics):
    `dry=0.0, wet=1.0` (back-compat — no audible change; set `dry>0`
    for NY compression / parallel filter / parallel distortion).
- `cedar::drywet::{coeff, mix}` inline helpers
  (`cedar/include/cedar/opcodes/drywet.hpp`) used by every effect
  opcode for the standard resolve + mix line.
- Catch2 dry/wet contract tests in `cedar/tests/test_drywet.cpp`
  covering one slot-based and one ExtendedParams-based example per
  category.

### Changed

- `chorus`, `flanger`, `phaser` extend their existing `ExtendedParams`
  with `dry` / `wet` slots (chorus 1→3, flanger 1→3, phaser 3→5). The
  positional argument order is unchanged for existing call sites.
- `phaser` internal topology: the opcode now emits **just** the
  all-pass cascade output and lets the `dry`/`wet` mix produce the
  canonical Bode/MXR phaser sum. With default `dry=1, wet=0.5` the
  phaser sounds milder than the pre-migration canonical sum; set
  `wet=1.0` to recover the previous +6 dB-peak topology. Math is
  bit-identical to the prior code at `dry=1, wet=1`.
- `freeverb`, `dattorro`, `moog`, `diode`, `formant`, `sallenkey`,
  `tape`, `xfmr`, `excite`, `gate`, `fold`, `pingpong` migrate to
  `ExtendedParams<2>` for `dry`/`wet` (their input slots were full).
  `pingpong`'s custom codegen handler now emits the ExtendedParams init
  manually and accepts `dry:` / `wet:` as kwargs in both overloads
  (`pingpong(stereo, t, fb)` and `pingpong(L, R, t, fb, width)`).
- Documentation: every effect doc page in
  `web/static/docs/reference/builtins/` carries a Category-A/B intro
  paragraph and per-effect `dry`/`wet` rows in the parameter tables.
  CLAUDE.md §"Effect Parameters" rewritten with the full convention.

### Known limitations / deferred work

- `experiments/test_op_phaser.py` notch-depth check now reports
  ~9–10 dB (threshold 12 dB) — investigation shows the algorithm is
  bit-identical pre/post when called with `dry=1, wet=1`, but the
  test's `_find_notch` measurement is sensitive to RNG / FFT window
  alignment that drifts when the StatePool state-id layout changes.
  Pre-existing measurement fragility, not a behavioral regression.
- `experiments/test_op_diode.py` failures are pre-existing (unrelated
  to this PRD).

## [0.4.0] - 2026-05-14

### Added
- Record destructuring: statement-level (`{a, b} = rec`) and function-parameter destructuring with defaults
- Record-valued state cells, plus `cell.field` read/write sugar over them
- Extended pattern event fields exposed via the `%.field` accessor
- Builtin record-parameter option schemas, with editor autocomplete driven from them
- Argument spread: `..expr` in call arguments and array literals, for both user functions and builtins
- Microtonal tuning: `d` / `\` accidental aliases, plus just-intonation and Bohlen-Pierce tuning systems
- `nkido-cli serve` mode — SDL window with live waveform, level meter, interactive controls, and scroll/scope views; `.akk` extension recognition
- `nkido-cli` auto-loads the default 808 sample kit, honors patch BPM, and supports named banks
- `beat(n)` clock helper and `spb` (seconds-per-beat) read-only builtin in the stdlib
- Full builtin signatures emitted as JSON for editor integrations
- `ParamsPanel` in `/embed` mode, with an autoplay flag on patch switch
- `dnb-amen` demo patch
- Unified URI resolver (`cedar::UriResolver` / TS `uriResolver`) for all asset loading. Handlers cover `file://`, `http://`, `https://`, `github:`, `bundled://`, plus `blob:nkido:` and `idb:` on the web side. Disk cache under the platform's user cache directory (Linux `$XDG_CACHE_HOME/nkido/`, macOS `~/Library/Caches/nkido/`, Windows `%LOCALAPPDATA%/nkido/cache/`), 500 MB cap with mtime-based LRU eviction.
- `samples("uri")` top-level akkado directive that records sample-bank URIs into `CompileResult.required_uris`. The host fetches and registers each before bytecode swap. Mirrors `wt_load` end-to-end (NOP opcode + special-cased codegen handler).
- `nkido-cli --bank/--soundfont/--sample` URI flags. Each accepts any registered scheme; bare paths are treated as `file://`. Banks accumulate as default banks searched in order; `--sample` admits a `name=uri` shorthand.
- `akkado-cli --uris` lists URI declarations from the program (text + JSON output).
- `docs/uri-schemes.md` documents the scheme list, `samples()` syntax, CLI flags, and caching layout. Indexed in F1 help.
- `web/tests/bank-registry.test.ts` regression test pinning the single-fetch invariant for `bankRegistry.loadBank('github:...')`.

### Changed
- All DSP opcodes are now stereo-native — filters, distortion, delays, comb, tap_delay, dynamics, env_follower, modulation FX, sampler, Freeverb, FDN, and Dattorro process stereo directly instead of relying on automatic mono lifting
- `ExtendedParams<N>`: unified mechanism for builtins needing more than 5 runtime-tunable params, replacing the old `inst.rate` bit-packing hacks
- `sample()` / `sample_loop()` accept a sample-name string as the third argument
- WASM bridge: standardized all four asset loaders (`cedar_load_sample`, `cedar_load_audio_data`, `cedar_load_soundfont`, `cedar_load_wavetable_wav`) on `int32_t` return with `-1` on failure. `cedar_load_soundfont` parameter order is now `(name, data, size)` matching the others.
- `BankRegistry.loadBank` flows `github:` URIs through the resolver; the dedicated `loadFromGitHub` path is gone, eliminating the prior double-fetch on cold loads.
- `audio.svelte.ts` exposes one `loadAsset(uri, kind, name)` method covering samples, SoundFonts, wavetables, and sample banks. Discriminated return per kind.
- `SampleBank::load_audio_data`, `SoundFontRegistry::load_from_memory`, `WavetableBankRegistry::load_from_memory` all take `MemoryView` uniformly.

### Removed
- Automatic mono→stereo lifting (auto-lift) — opcodes are stereo-native now
- Unused `post()` statement syntax
- `BankRegistry.loadFromGitHub` (web) and `audioEngine.loadBankFromGitHub` wrapper.
- `audioEngine.loadSampleFromUrl` / `loadSoundFontFromUrl` / `loadWavetableFromUrl` (web). Replaced by `loadAsset`.
- `cedar_load_sample_wav` WASM export (already dead) and the `_cedar_load_sample_wav` entry in the export list.
- `SampleBank::load_wav_file` / `load_wav_memory` callers; the orphan `SamplePack::load_samples` reference was ported to the resolver path.
- TS `FileSource` discriminated union in favor of URI-string-only `loadFile(uri, options)`.

### Fixed
- `poly` preserves stereo through voice mixing — voice callbacks that pan no longer collapse to mono
- Hot-swap preserves pattern playback across recompile; chord RMS normalization
- Chord quality table unified, voicings expanded, close/open voicings decoupled
- Chord patterns pipe natively into SoundFont playback
- Pattern transforms on identifier-bound patterns
- Duplicate diagnostics from pattern-transform handlers
- `button()` not firing as a trigger
- Per-voice velocity in sample-pattern polyrhythms; sample-velocity attenuation in `s"..."` patterns
- Mini-notation record suffix in `s"..."` sample mode and chord atoms
- `samples()` bank registration ordering during compile
- `nkido-cli` serve/play/ui silence for sample and poly patches; serve audio dropout after stop
- SoundFont URL-fallback silent-audio bug
- `/embed` patch loading errors, and the `/embed` 404 on production
- `n"..."` bare-MIDI note lexing

## [0.3.0] - 2026-05-01

### Added
- Smooch wavetable oscillator (`OSC_WAVETABLE`) with multi-bank support and band-limited mipmap tables
- Userspace state primitives: `state`, `get`, `set` cells backed by a new `STATE_OP` opcode for stateful patches in Akkado source
- Edge-triggered operators via the new `EDGE_OP` (replaces `SAH`) with mode-dispatched edge primitives
- UFCS method-call syntax: `x.foo(y)` falls through to `foo(x, y)`
- Pattern event arrays with typed prefixes and auto-coercion of patterns to scalar values
- Pattern transforms: `swing`, `swingBy`, `ply`, `linger`, `zoom`, `segment`, `early`, `late`, `palindrome`, `compress`, `iter`, `iterBack`
- Pattern generators: `run`, `binary`, `binaryN`
- Mini-notation record suffix for per-event fields, e.g. `c4{vel:0.8, dur:0.5}`
- Custom-property accessor with `bend`, `aftertouch`, `dur` transforms
- Voicing system: `anchor`, `mode`, `voicing`, `addVoicings` builtins
- Live audio input: `in()` builtin + `INPUT` opcode wired through the host
- Optional `step` parameter on `range()`; extended optional params across array utility builtins
- `nkido-cli render` mode for offline rendering, plus a Python polyphony experiment harness
- `/embed` route with a patches system and 10 landing-page demo patches
- `web-v*` tag pattern for web-only production deploys
- Hippocratic Code of Conduct

### Changed
- `poly()` signature reordered with a higher voice ceiling; debugger panel and F1 docs polished
- Renamed `fold` → `reduce` in the arrays reference; rebuilt arrays test coverage
- Smooch wavetable position smoothed at audio rate to suppress UI-cadence sidebands
- Hot-swap reliability: Ctrl+Enter is guaranteed to refresh audio even when block topology changes

### Removed
- `product` array builtin (replaced by `reduce`)

### Fixed
- `phaser` dry+wet summing; stages and feedback are now exposed as parameters
- Audio input panel showed "Audio not initialized" before pressing play
- Nested polyrhythms dropped voices; sample polyrhythms (`[bd, hh]`) now play both samples simultaneously
- `poly()` cycle-alignment at rational BPMs and required-input metadata
- `stepper-demo` array-through-closure binding with `STATE_OP`-gated writes
- Velocity-shorthand propagation in mini-notation
- Step-highlighting froze on the edited line and stayed frozen until recompile

## [0.2.0] - 2026-04-23

### Added
- Stereo signal semantics: automatic mono→stereo lifting, `mono()` downmix, stereo-aware builtin catalog with STEREO_INPUT flag
- `bpm` and `sr` builtin variables (desugar through ENV_GET, no new opcodes)
- `>>` and `@` as aliases for `|>` (pipe) and `%` (hole)
- Underscore placeholder (`_`) for skipping positional arguments to use their defaults
- Size-optimized Cedar build configuration and ESP32/Xtensa cross-compile support
- `cedar-size-report.sh` for tracking binary size across feature configurations
- `gm_medium` (FluidR3Mono, 14MB) and `gm_large` (MuseScore, 39MB) soundfont tiers

### Changed
- Rebranded web UI with orange accent and neutral grey palette
- Renamed project from enkido to nkido (canonical name going forward)
- Default soundfont switched from MuseScore_General (39MB) to TimGM6mb (2.6MB SF3)

### Fixed
- Windows and macOS portability blockers across build and runtime
- WASM build failure from missing project version in Cedar compile definitions

## [0.1.1] - 2026-04-03

### Fixed
- Pattern highlight corruption during edits by tracking document offset changes

## [0.1.0] - 2026-04-02

### Added
- Cedar audio synthesis engine with stack-based bytecode VM
- Akkado DSL compiler with Pratt parser and mini-notation support
- SvelteKit web application with CodeMirror editor
- WASM build for browser-based audio synthesis
- Hot-swap live coding with crossfade transitions
- 95+ DSP opcodes including oscillators, filters, delays, and reverbs
- Pattern sequencing with Strudel/Tidal-compatible mini-notation
- Runtime parameter controls (sliders, toggles, buttons, dropdowns)
- Theme system with 7 preset themes and custom theme support
- CI/CD pipeline with Netlify deployment
