# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
