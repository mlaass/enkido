# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.4.3] - 2026-05-29

### Fixed

- **Compile no longer blocks the audio thread.** Recompiling a patch
  used to stall `process()` for the full duration of `akkado_compile`
  (~110 ms median, ~158 ms peak on heavy patches like the unison-pad),
  causing audible silence on every hot-swap. Compile now runs in a
  dedicated Web Worker that owns its own WASM instance; the
  AudioWorklet only receives pre-packed bytecode + state-init buffers
  via the new `loadProgram` message.

### Changed

- **Rapid recompiles supersede-by-newest.** Holding Ctrl+Enter no
  longer queues a long backlog of compiles; every new compile gets a
  monotonic generation tag and stale results are silently dropped, so
  only the latest source ever lands in the worklet.
- **Compile worker recovers from crashes.** If the worker is killed
  (WASM trap, OOM, browser kill), the next `compile()` surfaces a
  "worker crashed — restarting" diagnostic and the call after that
  respawns the worker and succeeds — no page reload required.

## [0.4.2] - 2026-05-28

### Added

- **`_` placeholder in specialized call handlers.** The `_` argument
  placeholder now works inside builtins routed through specialized
  codegen call handlers, not just generic calls.
- **Growable chunked BufferPool.** Cedar's `BufferPool` now backs its
  registers with up to 64 lazily-allocated 256-buffer slabs (default
  cap raised from 256 to 16384 total). Slab pointers are stable across
  growth, so the audio thread's in-flight reads survive a hot-swap
  that armed new slabs on the compile thread. Hosts (nkido CLI play /
  serve / ui / render and the web/wasm worklet) call
  `pool.ensure_capacity(required_buffers)` off-cycle before publishing
  the new bytecode.
- **`CompileResult::required_buffers`.** Codegen now reports the peak
  distinct buffer indices used by the program so hosts can size the
  pool exactly. Backwards compatible: hosts that ignore the field still
  work for programs that fit in the default slab.

### Changed

- **Codegen sum/mix accumulator.** `sum()` and `mean()` over arrays now
  emit one accumulator buffer + N-1 in-place ADD instructions instead
  of an N-1-buffer linear chain. Programs that fan out wide (e.g.
  `unison(..., voices: 8)` summed under `poly`) no longer exhaust the
  pool on the per-voice sum. Bit-identical output.
- **`BufferAllocator` reuses freed indices.** Codegen now has a
  `BufferAllocator::release(idx)` that puts indices back into a LIFO
  free list. `reset_to(mark)` drains free-list entries past the mark.
  Used by the sum/mix accumulator and reserved for future
  refcount-driven release across general opcodes.
- **`cedar::MAX_BUFFERS` semantics.** Previously the size of a single
  flat buffer array (256). Now the total addressable buffer index
  space (16384 by default). Per-slab size lives in
  `cedar::SLAB_BUFFERS`. The `CEDAR_MAX_BUFFERS` build flag still
  works; it must be a positive multiple of `SLAB_BUFFERS`.
- **`BUFFER_ZERO` is now an explicit constant.** Pinned at 255 (last
  slot of slab 0) so it stays in the pre-allocated slab regardless of
  any future cap changes.

### Fixed

- **Live IDE deep links 404'd on `live.nkido.cc`.** Opening a shared
  patch (`/p#code=…`) or any non-prerendered route returned Netlify's
  "Page not found". The deploy never applied the repo's `netlify.toml`,
  so the SPA fallback rewrite, `SharedArrayBuffer` COOP/COEP headers,
  and immutable-asset caching were all missing in production. The deploy
  now applies `netlify.toml` and also ships `_redirects` / `_headers` in
  the build artifact.
- **`scales` dispatcher compile failure on a non-literal scrutinee.**
  Selecting a scale with a runtime (non-literal) argument no longer
  fails to compile.

## [0.4.1] - 2026-05-28

### ⚠ BREAKING — CLI binaries renamed: `nkido-cli` → `nkido`, `akkado-cli` → `akkado`

The bytecode player and compiler CLIs were renamed and their build output
moved to `build/bin/`. Update any wrapper scripts, CI, or shell aliases.
Source folders `tools/nkido-cli/` and `tools/akkado-cli/` were renamed to
`tools/nkido/` and `tools/akkado/` to match.

### ⚠ BREAKING — `pat` builtin and `p"…"` literal removed

The untyped `pat("…")` builtin and the `p"…"` raw-pattern literal were
removed in favor of typed prefixes (`n"…"`, `s"…"`, etc.). The typed
forms carry full event semantics; the raw form only ever surfaced step
indices and is unused by any shipped patch.

### ⚠ BREAKING — `euclid()` default span changed from 1 cycle to 4 cycles (1 bar)

The runtime `euclid(hits, steps)` builtin previously packed all `steps` into a
single cycle (= 1 beat under cycle=beat). At common BPMs this ran at near-32nd-
note rate — far from the "tresillo" feel the docs claimed. We added an explicit
`dur` parameter (audio-rate signal, default **4 cycles**) so `euclid(3, 8)` now
spans 1 bar at 4/4 by default, matching the classic Strudel/Tidal feel.

Existing patches using `euclid(3, 8)` will now sound 4× slower. To preserve the
old feel pass `dur=1` explicitly: `euclid(3, 8, 0, 1)`. To stretch further, raise
`dur`: `euclid(5, 16, 0, 8)` spans 2 bars.

`.fast()` and `.slow()` remain pattern-only and still do **not** apply to
`euclid()` (it returns a signal, not a pattern). Trying to use them now emits a
targeted hint pointing at the `dur` parameter instead of the generic
`E133 first argument must be a pattern`. For pattern-style rate scaling over an
Euclidean rhythm, use mini-notation Euclidean syntax: `n"c4(3,8)".slow(2)`.

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

### ⚠ BREAKING — `ChordLit` (`C4'`) syntax removed

The Strudel-style chord literal — an identifier-shaped token with a
trailing apostrophe, e.g. `C4'`, `F#m7_4'` — has been removed from the
language. It was an MVP stub that only ever emitted the chord's root
note, was unused in any shipped patch, and is superseded by pattern
events carrying real chord data. Write chords as patterns instead
(`n"[c4,e4,g4]"`, `chord("Am G C")`); `C4'` now lexes as the identifier
`C4` followed by a quote. The internal `chord_parser` API (used by
mini-notation) is unaffected.

### ⚠ BREAKING — `scale()` array builtin removed

The `scale(array, lo, hi)` array builtin has been removed. It was
functionally identical to `normalize(array, lo, hi)`, which already
maps an array's value range to an arbitrary `[lo, hi]` range (its
`lo`/`hi` arguments are optional, defaulting to `0`/`1`) and emits
identical bytecode. Replace any `scale(arr, lo, hi)` call with
`normalize(arr, lo, hi)`. Removing the builtin frees the `scale` name
for the planned Strudel-style scale-quantize transform
(`prd-runtime-event-transforms.md`).

### Added

- **Flexible `poly()` / `mono()` / `legato()` instrument callbacks** — the
  instrument is no longer fixed to a 3-parameter `(freq, gate, vel)`
  signature. It can read *any* pattern event field: by record destructure
  (`({freq, note, dur, cutoff}) -> …`), by positional prefix (canonical
  order `freq, gate, vel, trig, type, note, dur, chance, time, phase,
  sample_id`), by a mix of the two, or by a rest param binding the whole
  event (`(...e) -> e.freq`). Custom mini-notation record-suffix fields
  (`c4{cutoff:0.8}`) are readable per voice; an absent field binds to `0`
  or to an explicit `{cutoff = 0.5}` destructure default. The historical
  `(freq, gate, vel)` positional form stays valid; `mono` and `legato`
  accept every callback shape identically. Record form is now the
  canonical idiom across all docs and example patches.
- Destructure-param closures compile in every context — a closure
  assigned to a name (`stab = ({freq, gate, vel}) -> …`) and used as a
  `poly`/`mono`/`legato` instrument, or called directly, not only when
  passed inline as a direct `poly()` argument.
- **Pattern event arrays — `notes()` / `freqs()`** — surface a pattern
  event's chord notes as a first-class **dynamic array** (an array
  whose length is a runtime signal). `notes(e)` returns MIDI numbers,
  `freqs(e)` returns Hz; the method forms `e.notes` / `e.freqs` are
  equivalent. `len()` is now polymorphic (compile-time constant for
  static arrays, runtime signal for dynamic ones), `arr[i]` indexes
  dynamic arrays with wrap-by-default, and `map()` over a dynamic
  array stays dynamic. Combined with `step()` / `counter()` this makes
  arpeggiators and harmonizers userspace closures — no new C++ opcode
  per musical operator. A stateful UGen cannot auto-fan-out over a
  dynamic array (`sine(e.freqs)` → E181, use `poly()`). New
  `SEQPAT_VALUES` opcode; demo patches `arpeggio-demo` and
  `harmonizer-demo`.
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

- **Bus routing** — diamond `<>` operator (`signal <> 3` routes to bus 3),
  numbered buses with always-safe `master`, per-bus FX via mixer/master
  closures. Three phases shipped: numbered buses + master, per-bus FX,
  and the `<>` operator at pipe precedence.
- **Per-element `*N` / `/N` rate modifiers in mini-notation** —
  `n"c4*2 d4/2"` doubles/halves individual event durations under one
  uniform per-element mechanism (no top-level vs inner split).
- **Runtime event transforms** — closure-taking `event_map` /
  `event_filter`, runtime `fast()` / `slow()` (`EVENT_RATE_SCALE`),
  structural `EVENT_REORDER` / `EVENT_FANOUT`, and stdlib `key` /
  `scale` / `voice` / `invert` / `swing` / `swingBy` / `early` / `late`
  / 5 property modifiers. Chord-array READ/WRITE inside event_map
  closures. New `fmod` builtin + stdlib `.ak` embed mechanism.
- **Block-rate control flow** — `when() { … }` conditional bypass,
  `loop(N) { body }` bounded static iteration, `#inline` annotation
  with recursion rejection, `each()` / `reduce()` over event records,
  `FOREACH_EVENT` + subprogram table (POLY migrated onto it),
  `BLOCK_CALL` shared-block fn dispatch, `BLOCK_BIND` for shareable
  fns with >5 params.
- **Parameter type annotations Phase 2** — `evs` / `sig` / `num` /
  `rec` / `arr` / `str` / `fn` annotations parsed via `name: type`
  grammar, propagated through analyzer, dispatched in
  `handle_user_function_call`. New `E184` type-mismatch diagnostic.
- **Built-in Tidal Drum Machines sample catalog** — TR-808/909/etc.
  packs ship inside the WASM bundle, addressable from `s"…"` patterns.
- **`SF_VOICE` opcode** — single-voice soundfont primitive (poly
  unification Phase 1).
- **`transport()` builtin** is now reachable — previously declared but
  unregistered.
- **Live-editor → embedding parent postMessage** — iframe embeds can
  observe code edits.

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

### Fixed

- `legato()` no longer retriggers on every note. A pre-existing VM bug
  set the gate-on edge unconditionally for both `mono` and `legato`, so
  `legato` re-attacked the envelope identically to `mono`. Overlapping
  legato notes now glide without re-attacking (gate stays high); a note
  arriving after the previous one's release tail still retriggers, as
  documented.
- **Hot-swap robustness** — `ExtendedParams<N>` slots, `SEQPAT_QUERY`
  cycle cache, and `foreach_event` state all survive recompiles; audio
  arena buffers are deep-copied into the crossfade snapshot; state pool
  is snapshotted across crossfade dual-execution; byte-identical
  recompiles skip the crossfade entirely.
- **`SEQPAT_STEP` mid-block cycle wrap** — `state.output` is refreshed
  on the wrap, eliminating a one-block stale-value glitch.
- **Rest as first event** — no longer fires a phantom trigger on the
  cycle wrap.
- **Mixer-closure stereo copy-back** — must not alias the L bus into
  both channels.
- **`fn` arrow body** — no longer swallows the line that follows.
- **`..record` spread** — fields now bind to builtin params by name.
- **`<>` operator** — parses as a pipe-precedence infix, not
  statement-only.
- **Canonical closure-body recovery** — handles bare-identifier bodies.
- **Step-highlighting offsets** — accurate source ranges for pattern
  step highlights in the editor.
- **StatePool tables** are heap-allocated so the VM fits on the default
  thread stack.

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
