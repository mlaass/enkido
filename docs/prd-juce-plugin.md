> **Status: NOT STARTED — DRAFT** — Architecture plan for a JUCE 8 audio plugin (`nkido-juce-plugin`, closed-source) that runs the Cedar/Akkado engine natively as both an instrument and an effect, embedding the existing web UI via a JUCE WebView. Drafted 2026-06-07 from web research + a coupling assessment of `web/`. Implementation, signing/notarization, and licensing are out of scope for this doc (deferred to follow-up PRDs).

# PRD: JUCE Plugin (Nkido as VST3 / CLAP / AU / Standalone)

**Date:** 2026-06-07

---

## 1. Overview

### 1.1 Problem Statement

Nkido (Akkado + Cedar) today runs in two places: the open-source CLI tools
(`tools/nkido`, `tools/akkado`) and the SvelteKit web IDE (`web/`, which drives
a WASM build of the engine through an `AudioWorklet`). Musicians who work in a
DAW cannot use it where their music actually happens. There is no way to drop
Akkado live-coding into Ableton, Bitwig, Reaper, Logic, or any host, sync it to
the session tempo, automate its parameters, or save it inside a project.

A native plugin also unlocks a funding model: the plugin is a **closed-source,
paid** product whose revenue supports the open-source engine. The engine itself
(Cedar/Akkado) and the web UI stay open; only the JUCE wrapper that turns them
into a sellable plugin is closed.

### 1.2 Proposed Solution

A JUCE 8 plugin in a **separate, closed-source sibling repository**
(`nkido-juce-plugin`) that:

- Links the open-source Cedar + Akkado engine **natively** (no WASM) via a
  pinned git submodule of this repo.
- Ships as **two build targets from one codebase** — an **instrument/generator**
  (MIDI in → audio out) and an **effect** (audio in → audio out) — sharing the
  engine, diverging only in JUCE bus/MIDI wiring.
- Builds **VST3, CLAP, Standalone, and AU** formats. **Linux is the first
  working target**; Windows and macOS codepaths are added now with local smoke
  testing; full signing/notarization is deferred to per-platform follow-up PRDs.
- **Reuses the existing web UI** as the plugin editor via a **JUCE 8 WebView**
  (`juce::WebBrowserComponent`), so the live site and the plugin share one UI
  codebase instead of two parallel implementations.
- **Syncs to host transport** (tempo + playhead), runs the engine off a
  ring-buffer adapter (the engine's fixed 128-sample blocks ↔ the DAW's
  arbitrary block sizes), exposes a **fixed pool of 64 macro slots**
  (host-automatable) remapped to the code-defined `param()` set, and persists
  the Akkado source + parameter values in the DAW session.

### 1.3 Key Design Decisions (settled during planning)

| Decision | Choice | Rationale |
|----------|--------|-----------|
| v1 variants | **Synth + Effect** (both) | Engine is identical; only the JUCE wrapper's bus/MIDI contract differs. |
| Editor UI | **Reuse web UI via JUCE 8 WebView** | Single UI codebase across site + plugin; JUCE 8's relay/resource-provider bridge is mature. |
| Web reuse strategy | **Extract shared UI core + host adapters** | The backend is already centralized behind one `audioEngine` singleton; extraction is only marginally more work than branching and keeps one source of truth. |
| Platforms | **Linux first**; Win + macOS codepaths now, smoke-tested locally | De-risk the audio core on Linux; defer signing/notarization. |
| Formats | **VST3 + CLAP + Standalone + AU** | VST3 is MIT (Oct 2025); CLAP is MIT and Linux-friendly; AU rides the macOS follow-up. |
| Host automation | **Fixed pool of 64 macro slots** remapped by semantic-id | DAWs want a stable param list; dynamic param counts are fragile. |
| Engine link | **Git submodule of `nkido`, pinned to a release tag** | Reproducible, version-locked, easy to bump; no drift. |
| Transport | **Full host sync** (tempo + ppqPosition, loop-aware) | Expected behavior for a generative plugin. |
| Block size | **Accumulator + reported latency** | Reuses the engine's zero-alloc path unchanged; revisit variable-block only if it bites. |
| Presets | **Session state + user preset browser + factory presets** | A paid plugin needs first-class preset management. |
| Parity testing | **Web Playwright baseline + plugin validation** (pluginval + audio-render equivalence) | Lock site behavior before refactor; validate the plugin path independently. |
| Licensing/DRM | **Out of scope for v1** | Architecture first; selling/activation is a separate concern. |
| Open/closed line | **Only the JUCE wrapper is closed** | Shared UI core stays open in `nkido/web` (benefits the site too); the closed repo holds only the JUCE C++ plugin. |
| Engine license | **MIT** (per `nkido/README.md`) | Permissive — embedding the open engine + UI bundle inside a closed paid binary is compatible. Housekeeping: add a top-level `LICENSE` file mirroring the README statement. |

### 1.4 Goals (v1)

- A loadable **Linux VST3/CLAP/Standalone** plugin, in both synth and effect
  variants, that compiles Akkado source natively and produces audio in a host.
- **Host-synced** transport: tempo + playhead drive Akkado's cycle/beat clock.
- **64 macro slots** (host-automatable) mapped to the user's `param()`
  declarations, with names shown to the host and automation that survives
  code edits.
- **Web UI in a WebView** as the editor: code editor, params, visualizations,
  state inspector, preset browser.
- **Shared UI core** extracted in `nkido/web` with a `WasmAudioBackend` (site)
  and a `JuceAudioBackend` (plugin) behind one interface; **Playwright e2e
  baseline** proving the site did not regress.
- **Windows + macOS build codepaths** present and locally smoke-tested.
- **Session persistence**: source + params round-trip through the DAW project.

### 1.5 Non-Goals (v1 — deferred to future PRDs)

- **Code signing / notarization / installers** for any platform (per-platform
  follow-up PRDs). v1 ships unsigned local builds.
- **Licensing, activation, copy-protection, trial mode, storefront.**
- **AAX** (Pro Tools) — requires Avid approval, iLok, and PACE signing.
- **AUv3 / iOS** — mobile/App-Store sandbox is a separate effort.
- **Variable-length engine blocks** — the accumulator handles the mismatch;
  changing Cedar's block model is its own engine-side PRD.
- **MPE, microtonal host integration, multi-out routing** beyond stereo.
- **Browser-only UI panels that don't apply in a plugin** (see §7): media
  capture input, the Cloudflare share backend.

---

## 2. User Experience

### 2.1 Instrument (generator) variant

The user adds **Nkido** as an instrument on a MIDI track. The editor opens with
the familiar web IDE. They type Akkado, hit compile, and the engine plays;
incoming MIDI notes drive `poly()` / pattern triggers; the DAW's tempo and
transport drive the clock.

```akkado
// Instrument: MIDI note in → synth voice out
cutoff = param("cutoff", 1200, 200, 8000)
poly(@, (freq, gate, vel) =>
    saw(freq) |> lp(@, cutoff) |> @ * adsr(gate, 0.01, 0.2, 0.7, 0.3) * vel,
    8
) |> out(@)
```

- Pressing **Play** in the DAW starts the pattern clock at song position 0.
- Moving the **cutoff** automation lane in the host moves the slider in the UI,
  and vice versa.
- Saving the project stores the source + the cutoff value; reopening restores
  both.

### 2.2 Effect variant

The user adds **Nkido FX** on an audio track. Incoming audio is available to the
Akkado code as the input signal; the code processes and returns it.

```akkado
// Effect: process the DAW's audio bus through Akkado
mix = param("wet", 0.4, 0, 1)
in() |> chorus(@, 0.5, 0.5, wet: mix) |> out(@)
```

(`in()` is the shipped audio-input builtin from `prd-audio-input.md`; whether
the plugin reuses it as-is or adds a dedicated host-bus variant is
**[OPEN QUESTION 11.1]**.)

### 2.3 Editor (shared web UI in a WebView)

```
+--------------------------------------------------------------+
|  Nkido            [host: 128.0 BPM ▸ playing]   [≡ presets]   |
+----------------------------+---------------------------------+
|  saw(220) |> lp(@, cutoff) |  Params                          |
|       |> out(@)            |   cutoff  [======|---] 1200      |
|                            |   wet     [===|------] 0.40      |
|  cutoff = param("cutoff",  |  ----------------------------    |
|      1200, 200, 8000)      |  ▸ Oscilloscope   ▸ Spectrum     |
|                            |  ▸ State Inspector              |
|  [Compile ✓]               |  ▸ Pattern Debug                |
+----------------------------+---------------------------------+
```

Transport controls in the plugin UI are **host-driven indicators** (play state,
BPM, beat position reflect the DAW) rather than an owned clock (§6.3).

### 2.4 Presets

A **preset browser** lets the user save the current source + parameter snapshot
as a named user preset on disk, browse and load presets, and pick from bundled
**factory presets**. The DAW's own preset menu also works via plugin state.

---

## 3. Repository & Open/Closed Boundary

> **Amendment (2026-07-04, nkido studio planning):** the standalone
> `nkido-juce-plugin` repo below is **superseded** by a single closed repo
> **`nkido-studio`** holding a shared `host-core/` library (compile thread,
> native bridge, WebView host, block adapter, param pool) plus thin
> `plugin/` (these two targets) and `studio/` (`juce_add_gui_app`) targets.
> The plugin still ships first; only the repo layout changes. See
> `prd-studio-foundation.md` and `docs/research/studio-overview.md`.
> §3.1's structure remains accurate if `nkido-juce-plugin/` is read as the
> `plugin/` + `host-core/` subtrees of `nkido-studio`. Resolves [11.7].

### 3.1 Two repositories

```
nkido/                         (OPEN, this repo)
├── cedar/  akkado/            engine — consumed natively by the plugin
└── web/                       shared UI core + WasmAudioBackend (site)
                               + JuceAudioBackend (plugin host adapter)

nkido-juce-plugin/             (CLOSED, paid, sibling repo)
├── external/nkido/            git submodule → nkido @ vX.Y.Z (pinned tag)
├── external/JUCE/             git submodule → JUCE @ 8.0.x (pinned tag)
├── external/clap-juce-extensions/  submodule (CLAP wrapper)
├── src/                       PluginProcessor, PluginEditor, bridge, adapter
├── ui/                        built web bundle (BinaryData) from nkido/web
└── CMakeLists.txt             juce_add_plugin × 2 (synth + effect)
```

| Code | Lives in | License |
|------|----------|---------|
| Cedar/Akkado engine | `nkido` (open) | Existing OSS license |
| Shared web UI core + backend interface + `WasmAudioBackend` + `JuceAudioBackend` (JS) | `nkido/web` (open) | Existing OSS license |
| JUCE `AudioProcessor`, `AudioProcessorEditor`, WebView host, **native** bridge (C++), block-adapter, param pool, build/packaging | `nkido-juce-plugin` (closed) | Proprietary / paid |

**Rationale:** the UI is open and shared by both the site and the plugin (the
don't-diverge goal). The paid product is the native integration — the part that
makes it a real plugin. The `JuceAudioBackend` *JavaScript* adapter is open (it
runs inside the open web UI); the *C++* native bridge it talks to is closed.

The engine is **MIT-licensed** (per `nkido/README.md`), so embedding the open
UI bundle inside a closed paid binary is compatible. Housekeeping: add a
top-level `LICENSE` file mirroring the README statement.

### 3.2 Engine consumption

`nkido-juce-plugin` includes `nkido` as a **git submodule pinned to a release
tag** and builds `cedar` + `akkado` static libraries from source via
`add_subdirectory`, exactly as the CLI tools do. Bumping the engine = moving the
submodule to a new tag. No vendoring, no prebuilt-artifact pipeline to maintain.

---

## 4. What Already Exists vs What's New

The C++ engine surface the plugin needs is **largely already built** — it is the
same surface `tools/nkido` and the WASM module (`web/wasm/nkido_wasm.cpp`) use.

| Capability | Exists today | Used by | New work for plugin |
|------------|--------------|---------|---------------------|
| `akkado_compile` (source → bytecode) | ✅ | CLI, WASM | Call from a background thread |
| Cedar VM block processing (128 samples) | ✅ | CLI, WASM | Drive from `processBlock` via adapter |
| Hot-swap / atomic A/B program swap | ✅ | WASM worklet | Reuse; trigger from off-thread compile |
| State-init / sample-id / MIDI-source apply paths | ✅ | WASM worklet | Call natively (no wire buffers needed) |
| Asset resolution (`cedar::UriResolver`) | ✅ | CLI | Wire to plugin's asset paths |
| State inspection JSON (`inspect_state_json`) | ✅ | WASM | Expose over bridge |
| Pattern/sequence JSON (`serialize_*_json`) | ✅ | WASM | Expose over bridge |
| Probe / FFT data for visualizations | ✅ (worklet) | WASM | Native equivalent over bridge |
| Builtins metadata (`akkado_get_builtins_json`) | ✅ | WASM | Expose over bridge |

**Net:** the plugin does **not** reimplement the engine. The new work is (a) the
JUCE wrapper (processor/editor/bus/param/state), (b) the **native bridge** that
re-exposes the worklet's RPC surface as `getNativeFunction` calls + events, and
(c) the **shared-UI extraction** in `web/`.

---

## 5. Web UI Refactor (Shared Core Extraction)

### 5.1 Current state (assessed)

Everything audio funnels through **one singleton**:
`web/src/lib/stores/audio.svelte.ts` (~2943 lines), exported as `audioEngine`.
It directly owns the Web Audio graph, the `AudioWorklet`, and the compile
`Worker`. Its public return object (lines ~2825–2940) is **~28 reactive getters +
~56 methods** and is the *only* thing the rest of the app sees: **~33 files**
import `audioEngine`, and **all call high-level methods only**. No component
touches `AudioContext` / `AudioWorkletNode` / `Worker` / `port.postMessage`
directly (those primitives appear only inside `audio.svelte.ts` and its helpers
`web/src/lib/audio/input-source.ts` and `web/src/lib/midi/midi-input.svelte.ts`).

**Implication:** the public `audioEngine` shape is already a de-facto interface.
Extraction touches **~1 production file** (split it); the ~33 consumers are
untouched.

### 5.2 Target architecture

```
              ┌─────────────────────────────────────────┐
  ~33 UI      │  audio.svelte.ts (store SHELL)           │
  consumers ─▶│  reactive $state + getters + delegating  │
  (unchanged) │  methods → backend.<method>()            │
              └───────────────┬─────────────────────────┘
                              │ implements
                    ┌─────────▼──────────┐  AudioBackend (interface)
                    │  audio-backend.ts  │  ~35 methods:
                    └─────────┬──────────┘  compile/play/stop/setParam/
              ┌───────────────┴───────────────┐ setBpm/loadAsset/inspect/
              ▼                                ▼ getPatternInfo/probe/...
   ┌────────────────────┐         ┌──────────────────────────┐
   │ WasmAudioBackend   │         │ JuceAudioBackend          │
   │ (web — moved code) │         │ (plugin)                  │
   │ AudioContext +     │         │ window.__JUCE__.backend:  │
   │ AudioWorkletNode + │         │ getNativeFunction(...)    │
   │ compile.worker +   │         │ getSliderState(...) relay │
   │ input-source +     │         │ event listeners           │
   │ Web MIDI           │         │ (no AudioContext/Worker)  │
   └────────────────────┘         └──────────────────────────┘
```

### 5.3 `AudioBackend` interface, extraction & host reconciliation — see `prd-web-audio-backend.md`

**Superseded 2026-07-07 by
[`prd-web-audio-backend.md`](prd-web-audio-backend.md).** That PRD owns the formal
`AudioBackend` interface (the grouped `audioEngine` surface), the split of
`audio.svelte.ts` into a backend-agnostic shell + a transport backend,
`WasmAudioBackend`, `JuceAudioBackend`, the host-capability reconciliation (the
transport/clock-ownership inversion, bidirectional params, native asset
resolution, the web-only analyser leak), and the `IS_NATIVE` build.

The plugin is a **consumer** of that abstraction: it provides the
`JuceAudioBackend`'s runtime target — the C++ native-function/event bridge (§6.8)
— and the plugin-only UI panels (§5.6).

### 5.6 Plugin-only UI panels

Some UI surfaces don't exist on the site at all — they're plugin-specific by
nature: a **drag-and-drop sample loader** that targets the host filesystem and
a JUCE `FileChooser`, the **preset browser** (§6.7), host-driven transport
indicators (§6.3), and (eventually) license-activation UI. The shared core
(§5.2) covers the editor, params, visualizations, and inspectors; these
plugin-only panels need a home that doesn't pollute the site bundle.

Per [`prd-web-audio-backend.md`](prd-web-audio-backend.md) §5.5 (resolving
OQ11.6): one Svelte codebase under `nkido/web`; host-only components live under
`web/src/lib/native/`, gated behind an **`IS_NATIVE`** build flag (Vite
`define`) and tree-shaken out of the site build. The plugin's preset browser,
drag-and-drop sample loader, and license UI live there. `JuceAudioBackend` runs
only when `window.__JUCE__` exists, so importing it never pulls native code into
the site bundle. No separate UI package.

> **[RESOLVED 11.6 — 2026-07-07]** One shared Svelte codebase with an
> `IS_NATIVE` build flag; host-only panels under `web/src/lib/native/`,
> tree-shaken from the site build; **no** separate `plugin-ui/` package. Both
> native products (plugin + studio) share the gating. Specified in
> [`prd-web-audio-backend.md`](prd-web-audio-backend.md) §5.5.

---

## 6. Plugin Architecture (C++ / JUCE)

### 6.1 High-level

```
            DAW (host)
   ┌──────────┼───────────────────────────────────────────┐
   │ transport/tempo   MIDI / audio bus   automation       │
   ▼          ▼                ▼              ▼             │
┌─────────────────────────────────────────────────────────┴──┐
│ NkidoAudioProcessor : juce::AudioProcessor                  │
│  prepareToPlay() → alloc ring buffers (one window)          │
│  processBlock():                                            │
│   1. read AudioPlayHead (bpm, ppq, isPlaying, loop)         │
│   2. push host MIDI/input + transport into engine timeline  │
│   3. run Cedar in fixed 128-blocks via accumulator          │
│   4. pull N samples to host output buffer                   │
│   5. report setLatencySamples() (≤127)                      │
│  getStateInformation()/setStateInformation() (source+params)│
│                                                             │
│  ┌─ off audio thread ────────────────────────────────────┐ │
│  │ CompileThread: akkado_compile → immutable program      │ │
│  │  → SPSC FIFO → atomic pointer swap in processBlock      │ │
│  │  → retired program returned to GC thread (no audio-     │ │
│  │     thread free)                                        │ │
│  └────────────────────────────────────────────────────────┘ │
│  ParamPool: 64 AudioParameterFloat (APVTS), remapped by      │
│   semantic-id, names pushed via updateHostDisplay()          │
└──────────────────────────────┬───────────────────────────────┘
                               │ owns
            ┌──────────────────▼───────────────────────┐
            │ NkidoEditor : AudioProcessorEditor        │
            │  juce::WebBrowserComponent (lazy create)  │
            │   .withNativeIntegrationEnabled()         │
            │   .withResourceProvider(uiBundle)         │
            │   .withNativeFunction(...) × bridge RPC   │
            │   .withOptionsFrom(WebSliderRelay × 64)   │
            │   (Win: .withWinWebView2Options(userDir)) │
            └───────────────────────────────────────────┘
```

### 6.2 Synth vs Effect (two targets, one codebase)

The identity is fixed at **build time** by `juce_add_plugin` flags → `JucePlugin_*`
macros. Two targets are declared from the same sources:

| | Synth target | Effect target |
|---|---|---|
| `IS_SYNTH` | `TRUE` | `FALSE` |
| `NEEDS_MIDI_INPUT` | `TRUE` | `FALSE` (or `TRUE` for MIDI-controlled FX — **[OPEN 11.2]**) |
| Bus layout | output-only (stereo) | input + output (stereo == stereo) |
| `isBusesLayoutSupported` | accept mono/stereo out | require in-layout == out-layout |
| AU main type | `MusicDevice` | `Effect` |

Synth-only paths (e.g. ignoring an input bus) are guarded with
`#if JucePlugin_IsSynth`. The engine and the WebView editor are identical across
both.

### 6.3 Transport / host sync (full sync)

`processBlock` reads `AudioPlayHead::getPosition()` each block:

- **Tempo** (`bpm`) → feeds Akkado's BPM (= cycle rate).
- **`ppqPosition`** → maps song position to Akkado cycle/beat phase so patterns
  sit on the DAW grid and **re-align on loop/locate/seek**.
- **`isPlaying`** → starts/stops the pattern clock with host transport.
- Plugin UI transport controls become **read-only host indicators**; `play()`/
  `setBpm()` from the UI are no-ops or best-effort requests (the host is truth).

> **[OPEN QUESTION 11.3]** Phase-alignment policy on loop/seek: hard re-anchor
> Akkado phase to `ppqPosition` every block (sample-accurate grid lock) vs.
> free-run with re-anchor only on transport state change. Affects feel of
> long/evolving patterns. Default proposed: re-anchor to `ppq` continuously.

### 6.4 Block-size adapter (128 ↔ arbitrary N)

In `prepareToPlay(sampleRate, maxBlock)` allocate a ring/accumulator sized to a
power-of-two ≥ `maxBlock + 128`. In `processBlock(N)`:

1. Translate each `MidiBuffer` event's `samplePosition` into the engine timeline.
2. While the output ring has `< N` ready samples, run one engine 128-block.
3. Copy `N` samples out; leftover stays for the next callback.

Worst-case latency = 127 samples; **zero** when `N` is a multiple of 128
(typical: 128/256/512). Report via `setLatencySamples()` for host PDC. Reuses
the engine's zero-alloc audio path unchanged. If a buggy host exceeds
`maxBlock`, reallocate once (one glitch) rather than crash.

### 6.5 Background compile + lock-free swap

Mirrors the engine's existing triple-buffer model — **the same off-audio-thread
compile + atomic swap pattern shipped for the web in `prd-compile-off-audio-thread.md`
(v0.4.3, 2026-05-29)**, run natively here on a `juce::Thread` instead of a
web Worker. `akkado_compile` runs off-thread; the result (an immutable program:
bytecode + block table + state-init buffers) is published to the audio thread
by **atomic pointer swap** via a lock-free SPSC FIFO. The audio thread
`acquire`-loads the program at the top of `processBlock`; the retired program
pointer is pushed back to a GC thread so the **audio thread never frees memory**.
The existing 5–10 ms micro-crossfade on structural change runs on the audio
thread across a few blocks after the swap.
`static_assert(std::atomic<Program*>::is_always_lock_free)`.

### 6.6 Parameter system (fixed pool of 64 macro slots)

- At construction, register **64 anonymous `AudioParameterFloat` slots** in an
  `AudioProcessorValueTreeState` (e.g. `Param 1..64`, normalized 0–1).
- On compile, bind each code-defined `param()` to the next free slot **by the
  engine's semantic-id hash**, so the same named param re-binds to the same slot
  across code edits → automation lanes survive edits.
- Push the human label/range to the host via `updateHostDisplay()` /
  `kParamTitlesChanged`. The param's `min`/`max` come from the `param()` decl.
- `button()`/`toggle()`/`dropdown()` map onto slots too (toggle → 0/1 range,
  dropdown → stepped). **[OPEN 11.4]:** are 64 slots enough; what happens when a
  patch declares >64 params (proposed: bind first 64, surface a UI warning for
  the rest, still controllable in-UI but not host-automatable).
- Host automation writing a slot → updates the engine param **and** the WebView
  slider (via `WebSliderRelay`/`WebSliderParameterAttachment`). UI drag → updates
  the slot → host sees it as a gesture.

### 6.7 State & presets

- **Session state** (`getStateInformation`/`setStateInformation`): store the
  Akkado source string + the 64 slot values + the semantic-id→slot map in an
  APVTS `ValueTree` (XML→binary). On load, recompile the stored source off-thread
  before swapping; re-bind params by semantic-id.
- **User preset browser**: save/load named presets (source + param snapshot) to
  disk in a standard per-OS preset directory; browse/load from the WebView UI.
- **Factory presets**: a bundled set of example patches selectable in the UI.

### 6.8 The native bridge (re-exposing the worklet RPC surface)

The web worklet today exposes ~24 inbound / ~18 outbound message types plus a
separate compile-worker channel. The plugin re-exposes the **same logical
surface** natively:

- **Request/response** (e.g. `compile`, `getPatternInfo`, `inspectState`,
  `getBuiltins`, `getProbeData`) → `WebBrowserComponent::Options::withNativeFunction(name, lambda)`
  returning a Promise; `JuceAudioBackend` calls `Juce.getNativeFunction(name)`.
- **Pushed events** (e.g. beat position, probe frames, compile diagnostics,
  param changes from host) → `emitEventIfBrowserIsVisible(eventId, payload)`;
  `JuceAudioBackend` registers listeners.
- **Params** → `WebSliderRelay`/`WebToggleButtonRelay`/`WebComboBoxRelay` +
  attachments (the blessed APVTS↔web binding).

The C++ handlers mostly forward to **already-existing** engine functions (§4):
`akkado_compile`, `inspect_state_json`, `serialize_*_json`,
`akkado_get_builtins_json`, the apply paths, the resolver.

### 6.9 WebView asset serving

Bundle the built UI (HTML/JS/CSS from `nkido/web`) into `BinaryData` and serve
via `.withResourceProvider(...)`, navigating to `getResourceProviderRoot()` (the
JUCE 8 virtual-scheme model — `juce://juce.backend/...` on mac/Linux,
`https://juce.backend/...` on Windows). **No localhost HTTP server.** Dev builds
may point at a Vite dev server for hot reload. **Windows requires** a writable
user-data folder via `.withWinWebView2Options(...withUserDataFolder(...))` or
WebView2 silently fails. **Lazy-create** the WebView on editor open and tear it
down on close to bound per-instance RAM (each instance spins its own renderer
process).

---

## 7. Editor Panel Inventory (v1 in/out — resolving "decide in PRD")

| Panel / feature | v1 | Notes |
|-----------------|----|-------|
| Code editor (CodeMirror) | **IN** | Core. |
| Params controls | **IN** | Bidirectional with host automation (§6.6). |
| Visualizations (oscilloscope, spectrum, waterfall, …) | **IN** | Use `getProbeData`/`getFFTProbeData` over the bridge. |
| State Inspector | **IN** | `inspect_state_json` already exists. |
| Pattern Debug Panel | **IN** | `serialize_*_json` already exists. |
| Transport bar | **MODIFIED** | Host-driven indicators, not an owned clock (§6.3). |
| Sample / SoundFont / wavetable loading | **MODIFIED** | Asset resolution via the **native engine resolver**; file selection via JUCE `FileChooser` bridged. Browser upload/IndexedDB UI not reused as-is. |
| Preset browser | **IN (new)** | §6.7. Plugin-specific UI, in the shared codebase behind a host flag. |
| Theme / Settings | **IN** | WebView keeps `localStorage`; drop site-only settings. |
| Audio input panel (`getUserMedia`/`getDisplayMedia`) | **OUT** | Effect variant takes input from the DAW bus, not browser capture. |
| Cloudflare share backend | **OUT** | Browser-only; not applicable in a plugin. |
| Standalone "owns transport" controls | **OUT** (in plugin) | DAW owns transport; the **Standalone format** keeps a minimal owned transport. |

> **[OPEN QUESTION 11.5]** Sample/SF2 loading UX in the plugin: reuse the web
> file-router with a native resolver behind it, or a dedicated JUCE-native asset
> picker bridged into the UI? Affects how much of `web/src/lib/io/` and
> `file-router.ts` is shared vs plugin-specific.

---

## 8. Build, Formats & Distribution

### 8.1 Build system

- **CMake + `juce_add_plugin`** (no Projucer). JUCE and `clap-juce-extensions`
  as **pinned git submodules**. `nkido` as a pinned submodule providing `cedar`
  + `akkado` via `add_subdirectory`.
- **Two plugin targets** (synth + effect) differing only in flags/bus setup.
- UI bundle embedded via `juce_add_binary_data`; WebView2 backend enabled with
  `NEEDS_WEBVIEW2 TRUE`.
- **GitHub Actions** matrix (Linux/Win/mac) building all formats headless.

### 8.2 Formats & licensing reality

| Format | v1 | Licensing | Platforms |
|--------|----|-----------|-----------|
| **VST3** | ✅ | **MIT since Oct 2025** (SDK ≥ 3.8.0 — verify the pinned JUCE vendors it; bump if not) | Linux/Win/mac |
| **CLAP** | ✅ | MIT via `clap-juce-extensions` | Linux/Win/mac |
| **Standalone** | ✅ | Free (JUCE) | All |
| **AU** | ✅ codepath | Free (Apple); **macOS-only**, validated in the macOS follow-up | macOS |
| AAX | ❌ future | Avid approval + iLok + PACE signing | Win/mac |
| AUv3/iOS | ❌ future | App-Store sandbox | mac/iOS |

### 8.3 Platform rollout

1. **Linux first** — VST3/CLAP/Standalone fully working; `webkit2gtk` declared
   as a soft dependency for the WebView editor.
2. **Windows + macOS codepaths now** — build green, **local smoke testing**
   only (Win: WebView2 runtime + user-data folder; mac: WKWebView + AU).
3. **Signing / notarization / installers** — **deferred to per-platform
   follow-up PRDs** (macOS: Developer ID + hardened runtime + notarize the
   pkg/dmg; Windows: Azure Trusted Signing + Inno Setup). v1 ships unsigned
   local builds.

> **macOS WebView risk (flagged for the macOS follow-up):** documented
> JUCE-point-release-sensitive WKWebView crashes (Logic right-click, Ableton
> escape-key on 8.0.6, repeated open/close under pluginval, `ProcessThrottler`).
> Pin a specific JUCE 8.0.x and re-verify against pluginval + Logic/Ableton.

---

## 9. Implementation Phases

Each phase ends with a verification step. Phases 0–5 target **Linux**; Phase 6
adds the other platforms' codepaths.

### Phase 0 — Shared UI core extraction — see `prd-web-audio-backend.md`
**Superseded 2026-07-07.** The `AudioBackend` interface + the `audio.svelte.ts`
store split + `WasmAudioBackend` + `JuceAudioBackend` + the `IS_NATIVE` build are
specified and delivered by
[`prd-web-audio-backend.md`](prd-web-audio-backend.md) (single effort, in
`nkido/web`). The plugin **consumes** that work; its plugin-specific pieces are
Phases 1–6 below (the C++ native bridge the `JuceAudioBackend` calls is Phase 4).

### Phase 1 — Plugin skeleton (Linux)
**Goal:** an empty plugin that builds and loads.
- New repo `nkido-juce-plugin`; submodules (JUCE, clap-juce-extensions, nkido);
  CMake with two `juce_add_plugin` targets; link `cedar`+`akkado`.
- **Verify:** VST3/CLAP/Standalone load in a Linux host (Reaper/Bitwig);
  `pluginval` basic level passes (no audio yet).

### Phase 2 — Engine integration & audio (Linux)
**Goal:** native compile + audio out, host-synced, both variants.
- `prepareToPlay` ring buffers; `processBlock` 128-block accumulator;
  `setLatencySamples()`; MIDI from `MidiBuffer`; `AudioPlayHead` transport sync;
  synth output-only + effect in/out bus layouts; off-thread compile + atomic swap.
- **Verify:** **audio-render equivalence** — same source through the plugin
  (offline render) vs `tools/nkido` render produces matching audio (per
  `dsp-experiment-methodology.md`, ≥300 s where a sequence is involved);
  pluginval mid level; manual play in a host with tempo changes/loop.

### Phase 3 — Parameter system & state
**Goal:** 64-slot automation pool + session persistence.
- APVTS 64 slots; semantic-id remap; `updateHostDisplay()`; button/toggle/
  dropdown mapping; `getStateInformation`/`setStateInformation` (source+params).
- **Verify:** automate a slot in a host → engine param moves; save/reopen project
  → source + values restored; edit code → automation lane survives (semantic-id
  rebind). Regression test for the rebind mapping.

### Phase 4 — WebView editor & native bridge
**Goal:** the shared web UI as the editor, fully wired.
- Bundle UI into BinaryData; resource provider + virtual scheme; lazy WebView;
  implement `JuceAudioBackend` (JS) against `window.__JUCE__`; implement the C++
  native-function/event bridge forwarding to existing engine functions; param
  relays.
- **Verify:** edit+compile+play from the WebView; params relay both directions;
  visualizations + state inspector + pattern debug render; **the same Playwright
  flows that pass on the site pass against the plugin's WebView** where
  applicable (parity).

### Phase 5 — Preset browser
**Goal:** save/load user presets + factory presets.
- Disk preset storage; browser UI (shared, host-flagged); bundled factory set.
- **Verify:** round-trip a user preset; factory presets load and play.

### Phase 6 — Windows + macOS codepaths (smoke only)
**Goal:** build green + locally smoke-tested on Win + mac.
- WebView2 backend + user-data folder (Win); WKWebView + AU target (mac);
  per-OS asset/preset paths; CI matrix builds all formats.
- **Verify:** plugin loads + makes sound + WebView opens on each OS locally;
  pluginval per OS. (Signing/notarization explicitly **not** in scope.)

### Future (separate PRDs)
- Per-platform **signing/notarization + installers**.
- **Licensing / activation**.
- **AAX**, **AUv3/iOS**, variable-length engine blocks, MPE.

---

## 10. Edge Cases

| Situation | Expected behavior |
|-----------|-------------------|
| Host block size not a multiple of 128 | Accumulator buffers; up to 127-sample latency reported via `setLatencySamples()`; no glitch. |
| Host block exceeds `maximumExpectedSamplesPerBlock` | One-time reallocation (single glitch) rather than crash; never allocate in steady state. |
| Compile error in user source | Keep last good program playing; surface diagnostics in the WebView; do **not** drop audio. |
| Code edit while playing | Off-thread compile → atomic swap → 5–10 ms micro-crossfade; automation lanes survive via semantic-id rebind. |
| Patch declares > 64 params | Bind first 64 to slots; remaining controllable in-UI but not host-automatable; UI warning (**[OPEN 11.4]**). |
| Host loop / seek / locate | Re-anchor Akkado phase to `ppqPosition` so patterns stay on grid (**[OPEN 11.3]**). |
| Host transport stopped | Pattern clock stops; engine continues rendering tails/releases until silent. |
| WebView2 runtime missing (Windows) | Editor fails to load; processor still runs audio. Installer should ship/detect the Evergreen runtime (signing-PRD scope). v1: documented limitation. |
| `webkit2gtk` missing (Linux) | Editor unavailable; audio still works. Declared soft dependency. |
| Multiple plugin instances | Each lazy-creates its own WebView (RAM cost); tear down on editor close. Audio engines are independent. |
| DAW offline/bounce render | Deterministic: host drives block-by-block; accumulator + latency reporting keep render sample-accurate. |
| Project saved by old plugin version, opened by new | State is versioned in the ValueTree; unknown fields ignored; source recompiles. |
| Standalone format (no host) | Minimal owned transport + audio/MIDI device setup; otherwise identical engine + UI. |

---

## 11. Open Questions

- **11.1** Effect-variant input builtin: reuse the shipped `in()` from
  `prd-audio-input.md`, or add a dedicated host-bus variant with explicit
  L/R access? Confirm naming/semantics.
- **11.2** Does the **effect** variant accept MIDI (MIDI-controlled FX) in v1, or
  audio-only?
- **11.3** Loop/seek phase-alignment policy (continuous re-anchor to `ppq` vs.
  re-anchor on transport change only).
- **11.4** Is 64 macro slots the right pool size? Overflow UX for patches
  with more params.
- **11.5** Sample/SF2 loading UX: shared web file-router + native resolver vs.
  dedicated JUCE-native picker; how much of `web/src/lib/io/` is shared.
- **11.6** Plugin-only UI panels architecture (see §5.6): single shared
  codebase with `IS_PLUGIN` build-time flag, separate `plugin-ui/` package
  consuming the shared core, or runtime feature detection?
- **11.7** ~~Repo name: `nkido-juce-plugin` (assumed) — confirm.~~
  **Resolved 2026-07-04:** single closed repo `nkido-studio` with
  `host-core/` + `plugin/` + `studio/` targets (see §3 amendment).
- **11.8** Final marketing/product names beyond the working convention
  "Nkido" (instrument) / "Nkido FX" (effect), plus distinct VST3/CLAP/AU
  plugin codes.

---

## 12. Testing / Verification Strategy

### 12.1 Web parity (lock before refactor)
- **Playwright e2e baseline** in `nkido/web` capturing current behavior
  (compile, play/stop, param drag, sample load, visualization presence, state
  inspector) **before** Phase 0. Must stay green through the extraction. This is
  the "stable feature parity" guarantee for the shared-core refactor.

### 12.2 Plugin validation
- **`pluginval`** at increasing strictness per phase (and per OS in Phase 6).
- **Audio-render equivalence**: a harness renders the same Akkado source through
  (a) the plugin offline and (b) `tools/nkido`, asserting matching output
  (sample-exact where deterministic; tolerance-bounded otherwise). For any patch
  driving a sequence/pattern/poly, render **≥ 300 s** of simulated audio per the
  project's DSP methodology; report the failing block/time, never shorten to pass.
- **Parameter/state regression**: automate→engine, save→restore, code-edit→
  semantic-id rebind survival, >64-param overflow.

### 12.3 Bridge parity
- Where applicable, the **same Playwright flows** that pass against the site run
  against the plugin's WebView (driving `JuceAudioBackend`), proving the shared
  UI behaves identically on both backends.

### 12.4 Manual / host matrix
- Load synth + effect in Reaper/Bitwig (Linux), plus local smoke on Win
  (WebView2) and mac (WKWebView + AU): tempo change, loop, automation, project
  save/reopen, editor open/close × N (WebView lifecycle), CPU with many instances.

### 12.5 Build commands (indicative)
```bash
# plugin repo
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target Nkido_VST3 NkidoFX_VST3 Nkido_CLAP NkidoFX_CLAP Nkido_Standalone
pluginval --strictness-level 8 build/.../Nkido.vst3 build/.../NkidoFX.vst3

# web (open repo)
cd web && bun run check && bunx playwright test
```

---

## 13. Impact Assessment

| Component | Status | Notes |
|-----------|--------|-------|
| `cedar/`, `akkado/` engine | **Stays** | Consumed natively via submodule; no engine changes required for v1 (variable-block is future). |
| `tools/nkido`, `tools/akkado` | **Stays** | Same native API the plugin uses; reused for render-equivalence tests. |
| `web/src/lib/stores/audio.svelte.ts` | **Modified** | Split into store shell + extracted backends (Phase 0). |
| `web/src/lib/audio/*` | **New files** | `audio-backend.ts`, `wasm-backend.ts`, `juce-backend.ts`. |
| `web/src/lib/plugin/*` | **New files** | Plugin-only UI panels (sample drag-drop, preset browser, host-transport indicators) gated by `IS_PLUGIN` build flag — see §5.6. |
| ~33 web UI consumer files | **Stays** | Still import `audioEngine`; no changes. |
| Web `compile.worker.ts`, worklet | **Stays** | Become web-only deps of `WasmAudioBackend`; the worklet's RPC surface is the contract the native bridge mirrors. |
| `web/` Playwright tests | **New** | Parity baseline. |
| `nkido-juce-plugin` repo | **New** | All JUCE wrapper, native bridge, build/packaging (closed). |
| Signing / installers / licensing | **Deferred** | Separate follow-up PRDs. |

---

## 14. References

- JUCE 8 WebView UIs (resource provider, relays): https://juce.com/blog/juce-8-feature-overview-webview-uis/
- `WebViewPluginDemo.h`: https://github.com/juce-framework/JUCE/blob/master/examples/Plugins/WebViewPluginDemo.h
- `WebBrowserComponent::Options`: https://docs.juce.com/master/classjuce_1_1WebBrowserComponent_1_1Options.html
- JUCE CMake API: https://github.com/juce-framework/JUCE/blob/master/docs/CMake%20API.md
- VST3 SDK now MIT (Oct 2025): https://www.kvraudio.com/news/steinberg-moves-vst-3-sdk-to-mit-open-source-license-asio-now-gplv3-65179
- `clap-juce-extensions`: https://github.com/free-audio/clap-juce-extensions
- Dynamic params in APVTS: https://forum.juce.com/t/dynamic-param-management-in-apvts/67460
- Atomic swap onto the audio thread (Timur Doumler): https://timur.audio/using-locks-in-real-time-audio-processing-safely
- Code signing audio plugins in 2025 (round-up): https://moonbase.sh/articles/code-signing-audio-plugins-in-2025-a-round-up/
- CHOC WebView (alternative): https://github.com/TheAudioProgrammer/webview_juce_plugin_choc
- Related internal PRDs: `prd-compile-off-audio-thread.md`, `prd-audio-input.md`, `prd-godot-extension.md`, `cross-platform-porting.md`, `prd-windows-port.md`
