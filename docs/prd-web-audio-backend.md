> **Status: NOT STARTED (drafted 2026-07-07).** Supersedes the `AudioBackend`
> design previously sketched in `prd-juce-plugin.md` §5.3–5.6 + Phase 0 (now
> pointers to this PRD, applied in commit 4abafff — see §11). Extracts a formal `AudioBackend`
> interface out of `web/src/lib/stores/audio.svelte.ts`, with a
> `WasmAudioBackend` (today's browser behavior) and a `JuceAudioBackend`
> (native, dormant in a plain browser), so the one shared SvelteKit UI drives
> either the WASM engine (site) or the native Cedar engine (studio/plugin, over
> the `nkido-studio` bridge protocol) behind one seam. Owner decisions
> 2026-07-07 recorded in §1.

# Web AudioBackend Abstraction PRD

**Date:** 2026-07-07

Consumer of this abstraction on the native side:
`nkido-studio/docs/bridge-protocol.md` (the transport the `JuceAudioBackend`
speaks) and `nkido-studio/docs/prd-studio-foundation.md` (the WebView shell that
hosts the UI).

---

## 1. Executive Summary

The SvelteKit web IDE talks to the audio engine through exactly **one** file —
`web/src/lib/stores/audio.svelte.ts` (2943 lines), the `audioEngine` singleton
imported by ~34 files across `components/`, `editor/`, `viz/` and `stores/`
(~14 of them `components/` proper). That file welds together two very different things:
the app's **UI state** (reactive `$state` for the editor, params, panels, loaded
assets) and the **audio transport** (an `AudioContext` + `AudioWorkletNode`, a
compile `Worker`, `fetch('/wasm/…')`, and ~30 `port.postMessage` call sites).

The `nkido-studio` plugin and studio render this *same* UI inside a JUCE WebView,
but there is no `AudioContext`/worklet/WASM there — the native Cedar engine owns
DSP and the host owns the clock. This PRD extracts an **`AudioBackend`
interface** so the shared UI is transport-agnostic:

- **`WasmAudioBackend`** — today's browser path (worklet + compile worker + WASM),
  moved behind the interface (behavior-preserving, with the messy corners
  cleaned up as found).
- **`JuceAudioBackend`** — a native adapter that satisfies the same interface by
  calling `window.__JUCE__` native functions / listening to bridge events
  (`nkido-studio` bridge protocol v1). Dormant in a plain browser (`window.__JUCE__`
  absent), so it never enters the site bundle's runtime path.

The store splits into a **backend-agnostic shell** (the `$state` + getters +
asset registries + param/viz bookkeeping the UI reads) and a **backend** (the
transport behind the interface).

### Why?

`prd-juce-plugin.md` §5.4 already established the core argument: plugin and site
edit essentially the same one file, so **extraction beats branching** — one
`AudioBackend` seam instead of `if (isPlugin)` scattered across ~30–40 methods of
a 2900-line file. What has changed since that PRD:

1. The **native host is real**: `nkido-studio` shipped a WebView shell + bridge
   protocol v1 (`prd-studio-foundation` Phase 5). The `JuceAudioBackend` now has
   a concrete transport to target, not a hypothetical one.
2. The extraction is worth **its own PRD** rather than a sub-phase of the plugin
   work, because it is shared by *both* native products (plugin **and** studio)
   and the web IDE, and because a **deeper split** (agnostic UI-state vs
   transport) than the plugin PRD's "thin, zero-behavior-change" Phase 0 is
   wanted.

### Key design decisions (owner, 2026-07-07)

- **Deeper split, not a thin seam.** Separate backend-agnostic UI state from a
  minimal transport interface; the store shrinks to a shell. Opportunistic
  cleanups of the messy WASM integration are **allowed**, each documented.
- **One codebase, build-flag gated.** No separate UI package. Host-only panels
  (studio device settings, bus mixer, plugin preset browser) live under
  `web/src/lib/native/` and are gated behind an `IS_NATIVE` build flag; the
  studio/plugin build their own bundle that tree-shakes site-only code and
  includes native panels. Resolves `prd-juce-plugin.md` **OQ11.6**.
- **All three artifacts in open `nkido/web`.** Interface + `WasmAudioBackend` +
  `JuceAudioBackend` (JS) are all open (MIT). The `JuceAudioBackend` runs only
  when `window.__JUCE__` exists, so it is inert in the site.
- **Transport = request + backend-pushed truth.** `play()`/`setBpm()` become
  *requests*; `isPlaying`/`bpm`/`currentBeat` are updated via backend callbacks.
  Web pushes engine-driven truth; a DAW plugin pushes host truth; the standalone
  studio pushes its own (it *is* the host). One uniform model (adopts
  `prd-juce-plugin.md` §5.5's inversion).
- **Viz stays pull/poll in v1.** The interface contract keeps the current
  on-demand model (`getProbeData`/`getFFTProbeData`/`getCurrentBeatPosition`);
  the `JuceAudioBackend` buffers its latest **pushed** bridge frame (meters /
  scope doorbell / playhead) and returns it when the UI polls. A **push
  interface may be added later** (hybrid) if desirable — not v1.
- **Web-Audio leak kept web-only-optional.** `getAnalyserNode`/`getAudioContext`
  stay on the interface returning `null` on native backends (only `test-hooks.ts`
  consumes them; production viz uses probe/FFT).
- **Single effort.** Interface + both backends + `IS_NATIVE` build land together.

---

## 2. Current State

### 2.1 One boundary, two concerns welded together

`audio.svelte.ts` is the **sole** audio boundary — a grep across `web/src`
finds **zero** `AudioWorkletNode` / `AudioContext` / `.port.postMessage` /
`WebAssembly` / `new Worker(` usage anywhere else. It is a factory
(`createAudioEngine()`) returning an object literal, exported as the singleton
`audioEngine`; internals (`workletNode`, `audioContext`, `compileWorker`,
resolver maps) are closure-private.

| Concern | Lives in `audio.svelte.ts` today | Belongs after this PRD |
|---|---|---|
| Reactive UI state (`isPlaying`, `bpm`, `params`, `vizDecls`, `loadedSamples`, `inputStatus`, …) | closure `$state` + getters | **shell** (backend-agnostic) |
| Asset registries (`loadedSamples/Soundfonts/MidiFiles`, IndexedDB restore, `midiBank`, sample-bank catalog) | closure state + helpers | **shell** (backend-agnostic) |
| Param / viz decl bookkeeping, pending-load promise maps | closure state | **shell** |
| `AudioContext` + `AudioWorkletNode` + Web Audio graph (gain→analyser→dest) | lines ~565–608 | **`WasmAudioBackend`** |
| `fetch('/wasm/nkido.js'|'.wasm')` + `init` byte injection to worklet | ~580–897 | **`WasmAudioBackend`** |
| compile `Worker` (`new Worker(compile.worker.ts)`) + packed-buffer handoff | ~935, ~995–1078 | **`WasmAudioBackend`** |
| ~30 `workletNode.port.postMessage(...)` sites | throughout | **`WasmAudioBackend`** |

### 2.2 The worklet RPC surface (the contract a native backend mirrors)

`static/worklet/cedar-processor.js` receives **26** message types
(`loadProgram, setBpm, setParam, reset, loadSample, loadSampleAudio,
clearSamples, loadSoundFont, loadMidiFile, loadWavetable, clearWavetables,
getBuiltins, getShapeIndex, getPatternInfo, queryPatternPreview,
getCurrentBeatPosition, getActiveSteps, inspectState, getPatternDebug,
getProbeData, getFFTProbeData, setInputSource, midi, setFileCcPlan,
setDefaultMidiDevice`, plus `init`) and sends **18**
(`requestInit, initialized, error, programLoaded, sampleLoaded, soundFontLoaded,
midiFileLoaded, wavetableLoaded, builtins, shapeIndex, patternInfo,
patternPreview, beatPosition, activeSteps, stateInspection, patternDebug,
probeData, fftProbeData`). Every worklet-sourced *read* is a one-shot
request→response promise pair (resolvers held in per-`stateId` maps). This is the
surface `bridge-protocol.md` must cover for the `JuceAudioBackend`; the plugin
PRD §6.8 called it "~24 in / ~18 out".

### 2.3 Compile handoff

`compile.worker.ts` runs `akkado::compile` off the audio thread and returns
`compileResult {gen, success, …}` with **transferable** packed buffers
(`bytecode, stateInitsBuf, midiSourcesBuf, blockTable`) + JS metadata
(`paramDecls, vizDecls, requiredSamples/Soundfonts/Wavetables/Uris/…,
disassembly`). The store forwards the packed buffers to the worklet as
`loadProgram`. `prd-compile-off-audio-thread.md` (IMPLEMENTED) owns this; it is
**unchanged** by this PRD — it becomes a web-only dependency of
`WasmAudioBackend`.

### 2.4 Visualization / data path

No `SharedArrayBuffer`/`Atomics` in app code. Two **pull/poll** paths, both
driven by per-frame `requestAnimationFrame` loops in the viz modules
(`visualizations/oscilloscope.ts`, `waveform.ts`, `spectrum.ts`, `waterfall.ts`;
`editor/visualization-widgets.ts`; `stores/pattern-highlight.svelte.ts`;
`Panel/StateInspector.svelte`):

1. **Global meters** — `getTimeDomainData()`/`getFrequencyData()` read the Web
   Audio `AnalyserNode` synchronously (web-only).
2. **Per-viz probe/FFT** — `getProbeData(stateId)`/`getFFTProbeData(stateId)`
   post to the worklet and await `probeData`/`fftProbeData` (plain `number[]`,
   copied into a fresh `Float32Array`). Beat cursor is **polled**
   (`getCurrentBeatPosition`), not pushed.

### 2.5 Build / serve

`@sveltejs/adapter-static` with `fallback: 'index.html'` → a **client-rendered
SPA** (no Node server at runtime), embeddable in a WebView. Main route
`p/[[slug]]`: `ssr = false`, `prerender = false`. Runtime deps the site fetches
from origin — `/wasm/nkido.js`, `/wasm/nkido.wasm`, `/worklet/cedar-processor.js`
— are exactly what `JuceAudioBackend` **replaces** (so the native bundle ships no
WASM/worklet, and the site's COOP/COEP pthread requirement is moot natively).

---

## 3. Goals and Non-Goals

### 3.1 Goals

- A formal **`AudioBackend` interface** (`web/src/lib/audio/audio-backend.ts`)
  that reproduces the UI-facing `audioEngine` surface (§4), transport modelled as
  request + backend-pushed truth.
- A **store shell** (`audio.svelte.ts` shrunk) holding only backend-agnostic UI
  state; it delegates transport to the active `AudioBackend` and receives state
  updates via backend callbacks. The ~33 consumer files keep importing
  `audioEngine` **unchanged**.
- **`WasmAudioBackend`** (`web/src/lib/audio/wasm-backend.ts`) — today's browser
  path moved verbatim behind the interface, messy corners cleaned up (documented).
  The site behaves identically; existing e2e/unit stay green.
- **`JuceAudioBackend`** (`web/src/lib/audio/juce-backend.ts`) — satisfies the
  interface over `window.__JUCE__` per `bridge-protocol.md`; buffers pushed
  bridge frames to answer the pull-model viz queries.
- **`IS_NATIVE` build flag** + `web/src/lib/native/` for host-only panels; a
  native bundle target for studio/plugin that tree-shakes site-only code.
- Backend selection at boot (`window.__JUCE__` present → Juce, else Wasm).
- A **backend-conformance checklist** both backends satisfy (§10).

### 3.2 Non-Goals

- **The native studio/plugin UI composition itself** (which panels, layout,
  device-settings UX) — owned by `nkido-studio` PRDs; this PRD only provides the
  `IS_NATIVE`/`lib/native/` seam and the backend.
- **Expanding the bridge protocol** beyond what `bridge-protocol.md` v1 + its
  documented growth already covers — new message types are added there as the
  `JuceAudioBackend` needs them; this PRD lists the required surface (§5.4) but
  the bridge doc owns it.
- **A push viz interface** — deferred (hybrid, later), see §1 / §12.
- **Engine/WASM changes** — none. `compile.worker.ts`, the worklet, and the WASM
  build are unchanged (they become `WasmAudioBackend` internals).
- **The compile-off-thread design** — owned by `prd-compile-off-audio-thread.md`,
  unchanged.

---

## 4. The `AudioBackend` Interface

The interface is the existing `audioEngine` method set, grouped. Reactive
`$state` + getters stay in the **shell**; backends push updates via callbacks.

| Group | Methods (representative) | Native (`JuceAudioBackend`) notes |
|---|---|---|
| Lifecycle | `initialize()`, `restart()`, `dispose()` | native: wait for bridge `ready`; no WASM fetch |
| Transport (request) | `play()`, `pause()`, `stop()`, `setBpm(n)`, `setVolume(v)` | requests; truth arrives via `onTransport` callback (host/engine-owned) |
| Compile/load | `compile(source) → {ok, diagnostics, decls}` | native: one `compile` native fn; result via `compileResult` event |
| Params | `setParam`, `pressButton`, `releaseButton`, `toggleParam`, `resetParam`, `clearParams` | native: `setParam` fn; **host automation writes back** via `onParam` callback |
| Assets | `loadSample/FromBytes/FromFile`, `loadSoundFont`, `loadWavetable`, `loadMidiFile`, `loadBank`, `loadAsset`, `clear*`, `forget*` | native: routed through the engine resolver over the bridge |
| Queries | `getBuiltins`, `getShapeIndex`, `getPatternInfo`, `queryPatternPreview`, `getCurrentBeatPosition`, `getActiveSteps`, `inspectState`, `getPatternDebug`, `getProbeData`, `getFFTProbeData` | native: native fns; hot ones (`getProbeData`/beat) served from the **last pushed bridge frame** |
| MIDI/input | `setInputSource`, `listInputDevices`, `register/unregisterInputFile`, `midi(...)`, `setDefaultMidiDevice` | native: DAW/host MIDI + input bus; Web MIDI/getUserMedia absent |
| Analyser (web-only) | `getAnalyserNode`, `getAudioContext`, `getTimeDomainData`, `getFrequencyData` | native: **return `null`** (only `test-hooks.ts` consumes; prod viz uses probe/FFT) |

**Backend → shell callbacks** (the push side of "backend-pushed truth"): the
shell registers these when it installs a backend; the backend invokes them to
update `$state`.

```ts
interface AudioBackendHost {
  onTransport(state: { isPlaying: boolean; bpm: number; currentBeat: number; currentBar: number }): void;
  onParam(id: string, value: number): void;   // host automation write-back
  onError(err: string): void;
  onAssetLoaded(kind, id): void;               // registry updates
  onCompileResult(r: { generation, ok, diagnostics, decls }): void;
}
```

### 4.1 Full method signatures

The interface below is the exact transport-facing surface, grounded in the
current `audioEngine` methods (referenced TS types — `CompileResult`,
`PatternInfo`, `PatternEvent`, `StateInspection`, `PatternDebugInfo`,
`FFTProbeData`, `ShapeIndexData`, `BuiltinsData`, `SoundFontInfo`,
`InputSourceConfig`, `InputConstraints` — are the existing ones from the store /
worklet-RPC modules). Signatures are copied verbatim from `audio.svelte.ts`;
only two names normalize (`restart` ← `restartAudio`, `dispose` ← the teardown /
`terminateCompileWorker` path). The **shell keeps the current public names**, so
the ~34 consumers are untouched; the shell maps them onto this interface.

```ts
interface AudioBackend {
  // Lifecycle
  initialize(): Promise<void>;
  restart(): Promise<void>;          // shell method: restartAudio()
  dispose(): void;                   // teardown incl. terminateCompileWorker()

  // Transport (request; truth arrives via AudioBackendHost.onTransport)
  play(): Promise<void>;
  pause(): Promise<void>;
  stop(): Promise<void>;
  setBpm(bpm: number): void;
  setVolume(volume: number): void;

  // Compile
  compile(source: string): Promise<CompileResult>;

  // Params (host automation writes back via AudioBackendHost.onParam)
  setParam(name: string, value: number, slewMs?: number): void;
  setParamValue(name: string, value: number, slewMs?: number): void;
  getParamValue(name: string): number;
  pressButton(name: string): void;
  releaseButton(name: string): void;
  toggleParam(name: string): void;
  resetParam(name: string): void;
  clearParams(): void;

  // Assets
  loadSample(name: string, audioData: Float32Array, channels: number, sampleRate: number): Promise<void>;
  loadSampleFromBytes(name: string, data: ArrayBuffer, origin?: 'builtin' | 'user'): Promise<boolean>;
  loadSampleFromFile(name: string, file: File | Blob, origin?: 'builtin' | 'user'): Promise<boolean>;
  loadSamplePack(samples: Array<{ name: string; url: string }>): Promise<number>;
  loadBank(url: string, name?: string): Promise<boolean>;
  loadSoundFont(name: string, data: ArrayBuffer, origin?: 'builtin' | 'user'): Promise<SoundFontInfo | null>;
  loadMidiFile(name: string, data: ArrayBuffer): Promise<boolean>;
  loadWavetable(name: string, data: ArrayBuffer): Promise<number>;
  loadAsset(uri: string, kind: 'sample' | 'soundfont' | 'wavetable' | 'sample_bank' | 'midi',
            name?: string, origin?: 'builtin' | 'user'): Promise<boolean | SoundFontInfo | null | number>;
  clearSamples(): void;
  clearWavetables(): void;
  forgetSample(name: string): Promise<void>;
  forgetSoundFont(sfId: number): Promise<void>;
  unregisterMidiFile(name: string): void;

  // Queries (native: hot ones — getProbeData/FFT/beat — served from last pushed frame)
  getBuiltins(): Promise<BuiltinsData | null>;
  getShapeIndex(source: string, cursorOffset: number): Promise<ShapeIndexData | null>;
  getPatternInfo(): Promise<PatternInfo[]>;
  queryPatternPreview(patternIndex: number, startBeat: number, endBeat: number): Promise<PatternEvent[]>;
  getCurrentBeatPosition(): Promise<number>;
  getActiveSteps(stateIds: number[]): Promise<Record<number, unknown>>;
  inspectState(stateId: number): Promise<StateInspection | null>;
  getPatternDebug(patternIndex: number): Promise<PatternDebugInfo | null>;
  getProbeData(stateId: number): Promise<Float32Array | null>;
  getFFTProbeData(stateId: number): Promise<FFTProbeData | null>;

  // MIDI / input (native: DAW/host MIDI + input bus; Web MIDI / getUserMedia absent → no-op)
  setInputSource(config: InputSourceConfig): Promise<void>;
  setInputConstraints(c: Partial<InputConstraints>): void;
  listInputDevices(): Promise<MediaDeviceInfo[]>;
  registerInputFile(name: string, data: ArrayBuffer): string;
  unregisterInputFile(name: string): void;
  getInputFileNames(): string[];
  setDefaultMidiDevice(name: string): void;
  ensureMidiAccess(): Promise<unknown>;

  // Analyser (web-only; native returns null / no-op — only test-hooks.ts consumes)
  getAnalyserNode(): AnalyserNode | null;
  getAudioContext(): AudioContext | null;
  getTimeDomainData(): Uint8Array;
  getFrequencyData(): Uint8Array;
}
```

The `midi(...)` message path (raw MIDI in) and the reactive `midiBank` / sample-bank
catalog helpers (`getBankNames`, `hasBank`, `getBankSampleNames`, …) stay in the
**shell** — they are registry/UI bookkeeping, not transport, so they are not on
the backend interface. The interface file (`audio-backend.ts`) is the source of
truth once written; the above is generated from today's surface and must match it.

---

## 5. Architecture

```
                          ┌──────────────────────────────────────────┐
   ~33 UI consumers ─────▶│ audioEngine (store SHELL)                 │
   (components/editor/viz)│  • reactive $state + getters (agnostic)   │
                          │  • asset registries, param/viz bookkeeping│
                          │  • delegates transport → AudioBackend     │
                          │  • AudioBackendHost callbacks ← backend   │
                          └───────────────────┬──────────────────────┘
                                              │ AudioBackend (interface)
                            ┌─────────────────┴──────────────────┐
                 ┌──────────▼───────────┐          ┌─────────────▼──────────────┐
                 │ WasmAudioBackend     │          │ JuceAudioBackend            │
                 │  AudioContext+worklet│          │  window.__JUCE__ native fns │
                 │  compile.worker      │          │  + bridge events (buffered  │
                 │  fetch('/wasm/…')    │          │  for pull-model viz)        │
                 │  Web MIDI / getUserM.│          │  dormant if no __JUCE__      │
                 └──────────────────────┘          └─────────────────────────────┘
                     site bundle                        native bundle (IS_NATIVE)
```

### 5.1 Store shell vs backend

The shell keeps the reactive `$state` and every getter the UI reads, the asset
registries (`loadedSamples/Soundfonts/MidiFiles`, IndexedDB restore, `midiBank`,
sample-bank catalog/registry), and param/viz decl bookkeeping — all
backend-agnostic. It owns an `AudioBackend` instance and forwards transport /
compile / asset / query calls to it, and it exposes an `AudioBackendHost`
callback object the backend uses to push state back into `$state`.

### 5.2 `WasmAudioBackend` (behavior-preserving + cleanups)

The ~350-line `compile()` orchestration (compile-worker → asset loaders →
`loadProgram`), the `AudioContext`/worklet setup, the ~30 `postMessage` sites,
`fetch('/wasm/…')`, Web MIDI, and `getUserMedia`/`getDisplayMedia` input move
here verbatim. Cleanups permitted where the current integration is messy (e.g.
consolidating the scattered resolver maps, the boot handshake) — each cleanup
noted in the PR so the "no behavior change" claim stays auditable against §10.

### 5.3 `JuceAudioBackend` (native, dormant in browser)

Implements the interface over `window.__JUCE__` per `bridge-protocol.md`:
`compile` → `compile` native fn (result via `compileResult` event); queries →
native fns; transport → observes host via a `transport`/`playhead` event feeding
`onTransport`; params bidirectional (`setParam` fn out, `onParam` in). Hot
pull-model queries are answered from the **last pushed bridge frame**: the
backend caches the newest `meters` batch, the newest scope frame (fetched on the
`scopeReady` doorbell), and the latest `playhead`, and returns slices of them
when the UI polls `getFFTProbeData`/`getProbeData`/`getCurrentBeatPosition`.
Constructed only when `window.__JUCE__` exists.

### 5.4 Host-capability reconciliation (from `prd-juce-plugin.md` §5.5)

| Browser provides | Native host provides instead |
|---|---|
| `AudioContext` + `AudioWorkletNode` (owns DSP+clock) | native Cedar engine owns DSP; **host owns clock/transport** |
| `fetch('/wasm/…')` + compile `Worker` | native `akkado_compile` on a JUCE background thread (CompileService) |
| Web MIDI (`requestMIDIAccess`) | DAW / device MIDI (studio `DeviceIO`, plugin `MidiBuffer`) |
| `getUserMedia`/`getDisplayMedia` input | native input bus |
| one-way UI→engine params | **bidirectional** — automation writes back |
| IndexedDB/Blob URLs/file pickers | IndexedDB/localStorage kept; asset resolution via native resolver; pickers via JUCE `FileChooser` bridged |

The **transport/clock-ownership inversion** is the one genuinely-new problem:
`$state` flips from *owning + pushing* to *observing* `onTransport`. Uniform
across backends — web pushes engine truth, plugin pushes DAW truth, studio pushes
its own.

### 5.5 `IS_NATIVE` build (resolves OQ11.6)

One Svelte codebase. `web/src/lib/native/` holds host-only panels (studio device
settings, bus mixer, plugin preset browser); imported only behind `IS_NATIVE`
(Vite `define`), tree-shaken from the site build. Two bundle targets: `web/`
(site) and a native bundle the `nkido-studio` ResourceProvider serves. Because
`JuceAudioBackend` only activates on `window.__JUCE__`, importing it never pulls
native code into the site runtime.

---

## 6. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| `web/src/lib/stores/audio.svelte.ts` | **Modified** | Split into shell (agnostic state) + delegation to `AudioBackend` |
| `web/src/lib/audio/audio-backend.ts` | **New** | The interface + `AudioBackendHost` callbacks |
| `web/src/lib/audio/wasm-backend.ts` | **New** | Moved web transport (worklet/worker/WASM/Web-MIDI/input) |
| `web/src/lib/audio/juce-backend.ts` | **New** | `window.__JUCE__` bridge adapter |
| `web/src/lib/native/**` | **New** | Host-only panels (studio/plugin), `IS_NATIVE`-gated |
| ~33 consumer files (components/stores/editor/viz) | **No change** | Still import `audioEngine` |
| `static/worklet/cedar-processor.js`, `src/lib/audio/compile.worker.ts` | **Stays** | Become web-only deps of `WasmAudioBackend`; RPC surface is the native contract |
| WASM build, engine (cedar/akkado) | **Stays — zero changes** | |
| `prd-juce-plugin.md` §5.3–5.6 + Phase 0 | **Modified (doc)** | Trimmed to pointers to this PRD (§11) |
| `prd-studio-foundation.md` §4.3/§5.7 + Phase-5 refs | **Modified (doc)** | Point to this PRD instead of "prd-juce-plugin Phase 0" |
| `vite.config.ts`, `svelte.config.js` | **Modified** | `IS_NATIVE` define + native bundle target |

---

## 7. File-Level Changes

| File | Change |
|---|---|
| `web/src/lib/audio/audio-backend.ts` | **New** — interface + `AudioBackendHost` |
| `web/src/lib/audio/wasm-backend.ts` | **New** — extracted web transport |
| `web/src/lib/audio/juce-backend.ts` | **New** — native adapter (bridge protocol) |
| `web/src/lib/audio/backend-select.ts` | **New** — pick backend by `window.__JUCE__` |
| `web/src/lib/stores/audio.svelte.ts` | **Modified** — shell only; delegate + callbacks |
| `web/src/lib/native/**` | **New** — host-only panels (studio/plugin) |
| `web/src/lib/test-hooks.ts` | **Modified** — tolerate `null` analyser on native |
| `web/vite.config.ts`, `web/svelte.config.js` | **Modified** — `IS_NATIVE` + native build target |
| `web/tests/**`, `web/e2e/**` | **Modified/New** — conformance checklist (§10) |

---

## 8. Implementation (single effort)

One landing, but four logical workstreams that reviewers can read independently:

1. **Interface + shell split** — write `audio-backend.ts`; carve `audio.svelte.ts`
   into shell (agnostic `$state` + registries) + delegation; define the
   `AudioBackendHost` callbacks. Consumers untouched.
2. **`WasmAudioBackend`** — move the transport code verbatim; wire the boot
   handshake, compile orchestration, `postMessage` sites, Web MIDI, input, and
   the analyser/probe reads behind the interface; push state via callbacks.
   Cleanups documented.
3. **`JuceAudioBackend`** — implement against `window.__JUCE__`; frame-buffer the
   pushed bridge streams to answer pull-model viz; observe transport.
4. **`IS_NATIVE` build + `lib/native/`** — the build flag, the native bundle
   target, and the empty host-panel seam (panels themselves are studio/plugin
   PRD work).

**Verification gate before merge:** the existing web e2e/unit suite stays green
(§10) and the conformance checklist passes for `WasmAudioBackend` (web) and
`JuceAudioBackend` (studio harness).

---

## 9. Edge Cases

| Situation | Expected behavior |
|---|---|
| `window.__JUCE__` absent | `WasmAudioBackend` selected; `juce-backend.ts` never constructed; no native code on the site runtime path |
| `JuceAudioBackend` before bridge `ready` | Interface calls queue until `ready`; no `emitEvent` into an uninitialised `__JUCE__` (bridge readiness gate) |
| UI polls `getProbeData` before any bridge frame arrived | Return an empty/zero frame (never throw); first real frame replaces it |
| Host automation writes a param the UI also edits | `onParam` updates `$state`; last write wins; no feedback loop (backend echoes host truth, not UI intent) |
| `getAnalyserNode()`/`getAudioContext()` on native | Return `null`; `test-hooks.ts` handles null; production viz unaffected (uses probe/FFT) |
| Transport truth conflicts with a just-issued `play()` request | The `onTransport` callback (host truth) wins; the request was advisory |
| Native bundle accidentally imports a site-only module | `IS_NATIVE` tree-shaking + a lint/build check; site-only code must not be in `lib/native` imports |
| WASM pthread COOP/COEP headers | Irrelevant natively — the native bundle ships no WASM/worklet; only the site needs them |
| Backend swapped at runtime (not a v1 need) | Out of scope — backend is chosen once at boot |

---

## 10. Testing / Verification

- **No-regression guard (web):** the existing suite stays green through the
  refactor — Playwright `web/e2e/hot-swap-audio.spec.ts`, the visualization e2e,
  and `web/tests/state-init-codec.test.ts`. This is the primary anchor that
  `WasmAudioBackend` preserved behavior.
- **`AudioBackend` conformance checklist:** a documented set of behaviors both
  backends must satisfy — compile→diagnostics, play/pause/bpm request→observed
  truth, `setParam` round-trip (incl. host write-back on native), each asset
  loader, each query returns well-formed data, viz frame available after a
  compile+play. `WasmAudioBackend` runs it under the web test env;
  `JuceAudioBackend` runs it studio-side via the existing bridge/Xvfb harness
  (`nkido-studio` — readiness handshake + page↔native RPC already proven).
- **Backend-selection test:** `window.__JUCE__` present → Juce, absent → Wasm;
  and that the site bundle contains no `lib/native` / juce-only code
  (build-artifact assertion).
- **Interface conformance (types):** `tsc` — both backends `implements AudioBackend`.

---

## 11. PRD landscape updates

This PRD **supersedes** the `AudioBackend` design that previously lived
elsewhere. The edits below make this PRD the single source of truth. **They have
already been applied** — the `prd-juce-plugin.md` edits landed in commit
4abafff (the same commit that added this PRD), and the `prd-studio-foundation.md`
edits are in place in the closed `nkido-studio` repo. Recorded here for
traceability:

- **`prd-juce-plugin.md`** — *applied (commit 4abafff)*
  - §5.2–5.6 (shared UI core, the `AudioBackend` interface table, extraction
    evidence, host-capability table, plugin-only panels) → replaced with a short
    pointer: "The `AudioBackend` abstraction + web store refactor + `IS_NATIVE`
    build are specified in `prd-web-audio-backend.md`. The plugin implements the
    `JuceAudioBackend` consumer + the C++ bridge side." Kept the plugin-specific
    bits (preset browser, drag-drop sample loader, license UI).
  - **Phase 0** ("Shared UI core extraction") → replaced with "Consume
    `prd-web-audio-backend.md` (interface + `WasmAudioBackend` + `JuceAudioBackend`
    + `IS_NATIVE` build)."
  - **OQ11.6** → marked **Resolved**: one codebase, `IS_NATIVE` build-flag gating,
    `lib/native/` panels (this PRD §5.5).
- **`prd-studio-foundation.md`** (closed repo) — *applied*
  - §4.3 open/closed table row + §5.7 "AudioBackend extraction" row + the Phase-5
    "UI side consumes … prd-juce-plugin Phase 0" wording + the top-status
    follow-up line → point to `prd-web-audio-backend.md`, and state the studio's
    UI = the shared web IDE built with `IS_NATIVE`, imported + layered with
    studio panels (no separate package). *(These are the "foundation PRD wording
    is wrong" fixes flagged 2026-07-07.)*

---

## 12. Open Questions

- **[OPEN QUESTION 1] Push viz interface (hybrid).** When/whether to add a push
  path (`onProbeFrame`/`onMeters`/`onBeat` subscriptions) so the native backend
  stops being polled and the web backend can emit too. Deferred; v1 is pull-only.
  Trigger: measured polling overhead, or a viz that needs frame-accurate push.
- **[OPEN QUESTION 2] Native bundle packaging.** Exactly how the `nkido-studio`
  ResourceProvider consumes the `IS_NATIVE` build output (checked-in built
  bundle vs a build step in the closed repo vs a published artifact). Decided
  with the studio UI PRD.
- **[OPEN QUESTION 3] `decls` typing.** Whether `compile()`'s returned
  `paramDecls`/`vizDecls` get a shared TS type both backends import (vs the
  current loosely-typed metadata arrays). Small; decide during the interface
  write.
- **[OPEN QUESTION 4] Studio transport source.** The standalone studio owns its
  own transport — where that truth originates (a native transport object feeding
  `playhead`) is a `nkido-studio` detail; this PRD only requires the
  `onTransport` callback exists.
