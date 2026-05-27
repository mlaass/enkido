# Audio Input PRD — Live Signal Processing

> **Status: IMPLEMENTED** — All five phases shipped 2026-04-26. Cedar VM, Akkado builtin, CLI capture, WASM bridge, and web UI are wired end-to-end. Python experiment harness deferred (CedarTestHost would need an extension to set ctx.input_left/right; existing C++ unit tests in `cedar/tests/test_vm.cpp` under `[input]` cover the same behavior).

## Executive Summary

Cedar today is a closed system: all audio originates inside the graph (oscillators, samples, noise). This PRD adds a single new builtin, `in()`, that exposes external audio as a signal source usable anywhere in an Akkado program. On the web, the host routes a user-selected source — microphone, tab/system audio, or an uploaded file — into Cedar via WebAudio. In the native CLI, the host captures audio from a selected input device. Godot integration follows the same VM-level contract without requiring Godot-specific code in this PRD.

### Why?

Live-coding a synth is useful, but many musical workflows want to *process* an existing sound: filter a vocal, granularize a guitar, effect a loopback from another tab. There is currently no way to feed any external PCM into Cedar — the entire opcode library (filters, delays, reverbs, distortion, granular) is unreachable for that use case.

### Key design decisions

- **One new opcode, one new builtin.** `INPUT` opcode + `in` builtin. The language surface is minimal.
- **Silent fallback.** When input is unavailable (permission denied, device gone, file not loaded), `in()` returns zeros. No crash, no compile error.
- **Source selection has two layers.** A UI dropdown picks the default source; an optional string argument `in('mic' | 'tab' | 'file:name.wav')` overrides per-compile.
- **Stereo return via the universal stereo signal semantics PRD** ([`prd-stereo-support.md`](prd-stereo-support.md)). `in()` produces a `Stereo` `TypedValue` with adjacent L/R buffers. Auto-lift then handles the rest — `in() |> lp(@, 2000) |> out` is a stereo lowpass with independent per-channel filter state at no extra user effort.
- **Host populates input buffers before each block.** `ExecutionContext` gains `input_left` / `input_right` pointers symmetric to the existing `output_left` / `output_right`. Each host (WASM, CLI, Godot) is responsible for filling them.

---

## 1. Current State

### 1.1 Audio flow is one-way

| Stage | Today | With this PRD |
|---|---|---|
| Cedar graph input | None — all signal generated internally | External PCM via `in()` |
| `ExecutionContext` buffers | `output_left`, `output_right` | Adds `input_left`, `input_right` |
| WASM entry point | `cedar_process_block()` writes outputs | Worklet writes WASM-owned input buffers via `cedar_get_input_left/right()` pointers before each block |
| AudioWorklet (`cedar-processor.js`) | Reads WASM output, writes WebAudio destination | Also routes a `MediaStream` to WASM input |
| CLI (`AudioEngine`) | SDL2 output stream | Adds SDL2 input stream, configurable device |
| Godot | (covered by separate PRD) | Must populate `input_left/right` pre-block |

### 1.2 No "input" concept exists

No audio-input opcode exists in the `Opcode` enum in `cedar/include/cedar/vm/instruction.hpp` — the `STEREO_INPUT` identifier in that file is a dispatch flag bit introduced by [`prd-stereo-support.md`](prd-stereo-support.md) for dual-pass auto-lift, not an input source. Every signal source in the system today is either an oscillator, a noise generator, a pattern, a sample player, or a parameter lookup. `ExecutionContext` (`cedar/include/cedar/vm/context.hpp:17-88`) has only output pointers.

---

## 2. Goals and Non-Goals

### Goals

- `in()` Akkado builtin returns a stereo signal usable anywhere a signal is expected.
- `in('mic' | 'tab' | 'file:...')` optional string argument overrides the default source selected in the UI.
- Cedar-level contract: host code populates `input_left` / `input_right` on `ExecutionContext` before each `process_block()`.
- Web: dropdown in audio panel to pick source (microphone device, tab/system audio, uploaded file), with constraints (echo cancellation, noise suppression, auto-gain) exposed as user-facing toggles.
- CLI: `--input-device <name>` flag plus `--list-devices` to enumerate. Defaults to system default input.
- Silent fallback on unavailable input (permission denied, device missing, file not loaded, `in()` called before source granted).
- Sample-rate matching validated on the host side (WebAudio handles resampling; CLI asserts match).

### Non-Goals

- **Stereo signal representation** — defined by the companion [Universal Stereo Signal Semantics PRD](prd-stereo-support.md). `in()` declares `output_channels = Stereo` in that system; no new syntax is invented here.
- Feedback-loop detection or auto-attenuation (future work).
- More than two input channels (multi-channel ASIO, 5.1, etc.).
- Recording input to disk.
- Godot-specific wiring (lives in the Godot extension PRD; this PRD only defines the VM contract it must satisfy).
- Python (`cedar_core`) bindings for input — can be added later by exposing a numpy buffer setter.
- Latency compensation / input-to-output alignment measurement.

---

## 3. Target Syntax

### 3.1 Basic

```akkado
// Filter the microphone
in() |> lp(@, 2000, 0.7) |> out

// Delay and feed back
in() |> delay(@, 0.25, 0.5, 0.5, 0.5) |> out

// Mix with a synth
s = osc("saw", 220) * 0.3
in() + s |> out
```

### 3.2 Explicit source override

```akkado
in('mic')              // microphone (default)
in('tab')              // tab / system audio (via getDisplayMedia)
in('file:voice.wav')   // an uploaded file, looped

// Useful when the UI default is 'mic' but this patch wants a file:
in('file:drums.wav') |> freeverb(@) |> out
```

The argument is a **compile-time string literal**. The compiler does not interpret it semantically; it is forwarded to the host as metadata (e.g. via a new `cedar_set_input_source()` call triggered on compile).

### 3.3 Gain and routing

```akkado
// Gain trim
in() * 0.5 |> out

// Parallel dry/wet
dry = in()
wet = dry |> crush(@, 8)
dry * 0.5 + wet * 0.5 |> out
```

### 3.4 Multiple `in()` calls

All `in()` calls in a single program share the **same** input buffer. Cedar exposes one stereo input source per `ExecutionContext`; `in()` is a read from that buffer, not an allocation. If a future PRD adds multi-source routing (e.g. `in(0)`, `in(1)`), it extends this one.

---

## 4. Architecture

### 4.1 End-to-end flow

```
┌─────────────────────────────────────────────────┐
│ HOST                                            │
│ ┌──────────┐  ┌─────────┐  ┌──────────┐         │
│ │ Mic /    │  │ Tab     │  │ Uploaded │         │
│ │ Device   │  │ audio   │  │ file     │         │
│ └────┬─────┘  └────┬────┘  └────┬─────┘         │
│      └─────────────┴────────────┘               │
│                    │                            │
│            [selected source]                    │
│                    │                            │
│                    ▼                            │
│     Host fills input_left[128], input_right[128]│
│                    │                            │
└────────────────────┼────────────────────────────┘
                     ▼
        ExecutionContext { input_left, input_right,
                           output_left, output_right }
                     │
                     ▼
                ┌─────────┐
                │ INPUT   │  reads from input_left/right
                │ opcode  │  writes to a buffer register
                └────┬────┘
                     ▼
              [ Cedar DSP graph ]
                     ▼
                ┌─────────┐
                │ OUTPUT  │  writes output_left/right
                └────┬────┘
                     ▼
                Host plays it
```

### 4.2 `ExecutionContext` extension

`cedar/include/cedar/vm/context.hpp`:

```cpp
struct ExecutionContext {
    // ... existing fields ...
    float* output_left = nullptr;
    float* output_right = nullptr;

    // NEW: input buffers (populated by host before each block)
    float* input_left = nullptr;
    float* input_right = nullptr;
    // ...
};
```

If either pointer is `nullptr`, the INPUT opcode writes silence. This is the fallback for CLI builds without input support and the "permission not yet granted" web case.

### 4.3 `INPUT` opcode

New opcode in `cedar/include/cedar/vm/instruction.hpp` (placement TBD in the enum; next available value in the utility range).

Behavior: copies `input_left` / `input_right` into a pair of output buffers. Stateless. The codegen allocates an **adjacent buffer pair** (right = left + 1) per the invariant already enforced by `BufferAllocator` and relied upon by [`prd-stereo-support.md`](prd-stereo-support.md) §5.1 / §5.4. The resulting `TypedValue` has `channels = Stereo`, so downstream mono DSP is auto-lifted transparently.

Pseudocode:

```cpp
// In cedar/include/cedar/opcodes/utility.hpp (stateless; no dsp_state)
void op_input(ExecutionContext& ctx, const Instruction& inst) {
    // out_buffer is the left slot; right is guaranteed adjacent (out_buffer + 1)
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(inst.out_buffer + 1);
    if (ctx.input_left && ctx.input_right) {
        std::memcpy(out_l, ctx.input_left,  BLOCK_SIZE * sizeof(float));
        std::memcpy(out_r, ctx.input_right, BLOCK_SIZE * sizeof(float));
    } else {
        std::memset(out_l, 0, BLOCK_SIZE * sizeof(float));
        std::memset(out_r, 0, BLOCK_SIZE * sizeof(float));
    }
}
```

The `TypedValue` returned by codegen uses the `TypedValue::stereo_signal(left, right)` factory (from [`prd-stereo-support.md`](prd-stereo-support.md) §5.1), so `channels = Stereo` and `right_buffer = left + 1` are set consistently with the rest of the stereo type system.

### 4.4 Akkado builtin

`akkado/include/akkado/builtins.hpp`:

```cpp
{"in", {cedar::Opcode::INPUT, 0, 1, false,  // 0 required + 1 optional
        {"source", "", "", "", "", ""},
        {NAN, NAN, NAN, NAN, NAN},           // defaults array only encodes numeric
                                             // defaults; the string `source` has
                                             // no numeric default
        "Live audio input. Optional source: 'mic' (default), 'tab', 'file:NAME'.",
        /*extended_param_count=*/0,
        /*param_types=*/{akkado::ParamValueType::String, {}, {}, {}, {}, {}}}}
```

Per the type-system extension in [`prd-stereo-support.md`](prd-stereo-support.md) §5.2, `in()` has the channel-type signature:

```cpp
{ input_channels: {},        // no signal inputs
  output_channels: Stereo,   // produces stereo
  auto_lift:       false }   // fixed output type
```

The `source` argument is a string. Strings are not buffer inputs — the codegen intercepts them (via `ParamValueType::String`) and attaches them as metadata on the emitted instruction. The existing pattern to mirror is `soundfont(pattern, "file.sf2", preset)`, implemented in `CodeGenerator::handle_soundfont_call` (`akkado/src/codegen_patterns.cpp:2766`), which takes a string-literal filename and stores it in a compile-side required-resource table forwarded to the host.

### 4.5 Host integration — Web

**WASM** (`web/wasm/nkido_wasm.cpp`):

```cpp
static float g_input_left [BLOCK_SIZE] = {0};
static float g_input_right[BLOCK_SIZE] = {0};

// Mirror the output path: expose WASM-owned buffer pointers so the worklet
// writes directly into the heap, avoiding an extra copy per block.
EMSCRIPTEN_KEEPALIVE float* cedar_get_input_left()  { return g_input_left;  }
EMSCRIPTEN_KEEPALIVE float* cedar_get_input_right() { return g_input_right; }

// In cedar_process_block, before running VM:
ctx.input_left  = g_input_left;
ctx.input_right = g_input_right;
```

Also a string-setter for the source override:

```cpp
EMSCRIPTEN_KEEPALIVE
void cedar_set_input_source(const char* source);
```

Compiler emits a call alongside bytecode if it sees a string literal in `in('...')`.

**AudioWorklet** (`web/static/worklet/cedar-processor.js`):

- Receive a `MediaStream` from main thread via `MessagePort`.
- On init, cache `inputLeftPtr = module._cedar_get_input_left()` and `inputRightPtr = module._cedar_get_input_right()` (symmetric to the existing `outputLeftPtr` / `outputRightPtr`).
- AudioWorklet `process(inputs, outputs)` already receives `inputs[0]` — before calling `cedar_process_block`, copy `inputs[0][0]` / `inputs[0][1]` (or duplicate `inputs[0][0]` to both for mono sources) directly into the WASM heap at `inputLeftPtr` / `inputRightPtr` using a fresh `Float32Array(module.wasmMemory.buffer)` view.

**Main thread** (`web/src/lib/stores/audio.svelte.ts` + new `web/src/lib/audio/input-source.ts`):

- Source selection state: `'mic' | 'tab' | { file: string }`.
- `'mic'`: `navigator.mediaDevices.getUserMedia({ audio: { echoCancellation, noiseSuppression, autoGainControl } })` with toggles from settings.
- `'tab'`: `navigator.mediaDevices.getDisplayMedia({ audio: true, video: false })`.
- `'file:NAME'`: resolve file from the existing `akkado::SampleRegistry` / web upload flow (the same registry that feeds `cedar_load_sample` / `cedar_load_audio_data` — see `web/src/lib/audio/bank-registry.ts` and `CedarProcessor.loadSampleAudio` in `web/static/worklet/cedar-processor.js`), decode, and loop through an `AudioBufferSourceNode` with `loop = true`. WebAudio handles resampling from the file's native rate to the `AudioContext` rate automatically. No crossfade at the loop boundary in MVP (sample-accurate wrap; clicks are acceptable as long as the file is well-formed). If the file isn't decoded yet, `in()` returns silence until decode completes — no blocking.
- Connect chosen source → `MediaStreamAudioSourceNode` (or file equivalent) → AudioWorklet input port.

**UI** (`web/src/lib/components/Panel/AudioInputPanel.svelte`, new):

- Dropdown: Mic / Tab audio / File / None
- Device picker sub-dropdown when Mic is selected (populated via `enumerateDevices()`)
- File upload zone when File is selected (reuses sample-loading infrastructure)
- Toggles for echo cancellation / noise suppression / auto-gain
- Status: Granted / Denied / Unavailable / Active
- "Enable input" button when permission not yet granted

**Mono → stereo.** When the selected source is mono (common for mics), the host duplicates the single channel into both `input_left` and `input_right` before calling `cedar_set_input_buffer`.

### 4.6 Host integration — Native CLI

**`tools/nkido-cli/audio_engine.hpp` / `.cpp`**:

- Open a second SDL2 audio device with `SDL_OpenAudioDevice(..., is_capture=1, ...)`.
- Capture callback fills an internal ring buffer; playback callback pulls 128 samples and sets `ctx.input_left` / `input_right` before `vm().process_block()`.
- `--list-devices`: prints `SDL_GetNumAudioDevices(1)` + `SDL_GetAudioDeviceName(i, 1)`.
- `--input-device <name>`: resolves to device index; falls back to default if not found (with warning).
- No input device → `input_left/right = nullptr`, `in()` returns silence.

### 4.7 Host integration — Godot (contract only)

The Godot extension must, before each `process_block()` call, fill `ctx.input_left` / `ctx.input_right` with the current stereo input frame from whatever Godot source the extension PRD chooses (`AudioStreamMicrophone`, bus tap, etc.). If no source is connected, leave the pointers as `nullptr`. **No Godot code lives in this PRD.**

---

## 5. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| Cedar VM core | **Modified** | `ExecutionContext` gains two pointers; one new opcode |
| Cedar opcode library | **New** | `op_input` in `utility.hpp` |
| Akkado compiler | **Modified** | New builtin; string-literal metadata path reused from `osc()` |
| Akkado analyzer | **Modified** | Validates `in(source?)` string argument |
| WASM bridge | **Modified** | Two new exported functions |
| AudioWorklet processor | **Modified** | Routes `inputs[0]` to WASM |
| Web UI | **New** | Audio-input panel, source selector, permission flow |
| Web audio store | **Modified** | Track selected source, handle permission |
| CLI `AudioEngine` | **Modified** | Capture device, `--list-devices`, `--input-device` |
| Generated opcode metadata | **Modified** | Rebuild via `bun run build:opcodes` |
| Documentation | **New** | Entry in `web/static/docs/` for `in()` |
| Sample-loading subsystem | **Reused** | `file:NAME` source uses `akkado::SampleRegistry` and the existing web upload flow |
| Godot extension | **Unaffected by this PRD** | Must eventually implement contract |
| Python `cedar_core` bindings | **Unaffected** | Future extension |

---

## 6. File-Level Changes

### Modify

| File | Change |
|---|---|
| `cedar/include/cedar/vm/context.hpp` | Add `input_left`, `input_right` pointers |
| `cedar/include/cedar/vm/instruction.hpp` | Add `INPUT` opcode enum value |
| `cedar/include/cedar/opcodes/utility.hpp` | Add `op_input` implementation |
| `cedar/src/vm/vm.cpp` | Dispatch case for `INPUT` |
| `akkado/include/akkado/builtins.hpp` | Register `in` builtin |
| `akkado/src/analyzer.cpp` | Accept optional string argument; validate format |
| `akkado/src/codegen.cpp` | Emit host-side source-set call for `in('...')` (if non-default) |
| `web/wasm/nkido_wasm.cpp` | Export `cedar_get_input_left`, `cedar_get_input_right`, `cedar_set_input_source`; wire `ctx.input_left/right` into `cedar_process_block` |
| `web/static/worklet/cedar-processor.js` | Route `inputs[0]` into WASM each block |
| `web/src/lib/stores/audio.svelte.ts` | Source-selection state, permission handling, `MediaStream` routing |
| `web/src/lib/components/Panel/*` | Add audio-input panel to sidebar tabs |
| `web/src/lib/settings.svelte.ts` | Persist input-constraint toggles and default source |
| `tools/nkido-cli/audio_engine.hpp/.cpp` | SDL2 capture device + device selection |
| `tools/nkido-cli/main.cpp` | `--list-devices` and `--input-device` flags |

### Create

| File | Purpose |
|---|---|
| `web/src/lib/components/Panel/AudioInputPanel.svelte` | UI for source selection and constraints |
| `web/src/lib/audio/input-source.ts` | MediaStream acquisition & switching |
| `web/static/docs/in.md` | Docs page for the `in()` builtin |
| `experiments/test_op_input.py` | Python experiment: verify INPUT copies `input_left/right` to output register and writes silence when pointers are null |

### Modify (tests)

| File | Change |
|---|---|
| `cedar/tests/test_vm.cpp` | Add `[input]` test cases for INPUT opcode (memcpy path, silent-fallback path, statelessness) |
| `akkado/tests/test_codegen.cpp` | Add compile tests for `in()`, `in('mic')`, `in('tab')`, `in('file:NAME')`, and `in('garbage')` error case |

### Explicitly NOT changed

- `cedar/include/cedar/opcodes/dsp_state.hpp` — INPUT is stateless, no state type needed
- Buffer pool / state pool — no new pool slot types
- Pattern / sequencer code — `in()` is a signal source, not a pattern
- Existing output path — completely untouched

### Post-change commands

- `cd web && bun run build:opcodes` — regenerates `opcode_metadata.hpp` with the new INPUT entry
- `cd web && bun run build:docs` — regenerates the docs lookup index if `in.md` is indexed

---

## 7. Edge Cases

| Case | Expected behavior |
|---|---|
| Mic permission denied | `in()` returns silence; UI shows "Denied" with a retry button; no error thrown |
| Permission not yet granted when program compiles | `in()` returns silence until granted; signal starts flowing live, no recompile needed |
| Device disconnected mid-session | Silence until reconnect; UI shows "Unavailable" |
| Sample rate mismatch (web) | WebAudio resamples automatically into the AudioContext rate; no action needed |
| Sample rate mismatch (CLI) | Capture device opens at VM sample rate; SDL2 resamples if needed, else error at startup |
| `in('file:foo.wav')` with no such file uploaded | Silence; UI shows warning under the panel |
| `in('file:foo.wav')` before file decoded | Silence until decode completes |
| Multiple `in()` calls | All read same input buffer (single source per context) |
| Source switched while playing | Glitch-free if both sources can be connected simultaneously; brief crossfade or fallback to silence otherwise |
| Feedback loop (mic in, speakers out, no headphones) | **Not mitigated in MVP.** Warn in docs. Future PRD may add detection |
| Echo cancellation toggled mid-session | Triggers `getUserMedia` re-negotiation with new constraints; brief silence acceptable |
| `in()` in code but UI source is "None" | Silence |
| Hot-swap removes `in()` | Input buffer still populated but unread; host may keep capture active or pause — capture pauses if no `in()` in current bytecode (optimization, not correctness) |
| Program has `in()` but host doesn't support input (old CLI build) | `ctx.input_left == nullptr` → silence |
| `in('invalid_source_string')` | Compile-time error with suggestions |
| Mono mic into stereo `in()` | Host duplicates mono channel to both L and R |

---

## 8. Testing / Verification

### 8.1 Cedar unit tests (added to `cedar/tests/test_vm.cpp` under `[input]` tag)

- INPUT opcode with non-null `input_left/right` copies buffer contents to register.
- INPUT opcode with null pointers writes zeros.
- INPUT opcode is stateless (running it twice with same inputs produces identical output).
- Confirm the adjacent-buffer invariant: `out_buffer + 1` is the right-channel slot, matching the convention used by `PAN`, `WIDTH`, etc. in `cedar/include/cedar/opcodes/stereo.hpp`.

### 8.2 Akkado tests (added to `akkado/tests/test_codegen.cpp` under `[input]` tag)

- `in()` compiles and emits an INPUT instruction with a `TypedValue` built via `TypedValue::stereo_signal(left, right)` (i.e. `channels == ChannelCount::Stereo`, `right_buffer == left + 1`).
- Feeding `in()` into a mono DSP builtin (e.g. `in() |> lp(@, 2000)`) emits one instruction with the `InstructionFlag::STEREO_INPUT` bit set and allocates an adjacent L/R output pair (depends on stereo-PRD Phase 4 auto-lift codegen landing first).
- `in('mic')`, `in('tab')`, `in('file:sample.wav')` all compile.
- `in('garbage_value')` is a compile-time error.
- `in() |> out` end-to-end compiles and produces runnable bytecode.

### 8.2a Python experiment (`experiments/test_op_input.py`)

- Build instruction sequence: `INPUT` → `OUTPUT` (stereo).
- Feed known L/R buffers through `cedar_core` harness (requires minor harness extension to set `ctx.input_left/right` — document this in the experiment).
- Verify output equals input bit-for-bit when pointers are set; verify silence when pointers are null.
- Save WAV for human evaluation as per `docs/dsp-experiment-methodology.md`.

### 8.3 Web manual tests

- Open audio panel → dropdown shows: Microphone / Tab audio / File / None.
- Select Microphone → browser prompt appears → grant → status turns green.
- Compile `in() |> out` with headphones → user hears own voice.
- Deny permission → no error, silence, UI shows Denied.
- Select Tab audio → display-media prompt appears → select tab with music → music flows through `in()`.
- Upload a WAV via file zone → `in('file:NAME.wav')` loops the file.
- Toggle echo cancellation / noise suppression / auto-gain → hear difference immediately.
- Switch source while playing → no crash, brief silence acceptable.

### 8.4 CLI manual tests

- `nkido --list-devices` prints numbered list of capture devices.
- `nkido --input-device "USB Audio"` selects named device.
- `nkido --input-device "nonexistent"` warns and falls back to default.
- Piping input through `in() |> lp(@, 800) |> out` in a `.cedar` file works audibly.

### 8.5 Cedar → CLI → Web integration sanity

Same Akkado program compiled and run on both platforms with the same input (a pre-recorded file playing into system audio) produces the same filtered output (within float tolerance / AGC differences).

---

## 9. Implementation Phases

**Phase 1 — Cedar VM & Akkado builtin (1 day)**

- Add `input_left/right` to `ExecutionContext`.
- Add `INPUT` opcode + implementation + unit tests.
- Register `in` builtin; compile `in()` emits INPUT.
- Compiler accepts optional string literal; passed as metadata only (no VM-level effect yet).
- Verify: `./build/cedar/tests/cedar_tests "[input]"` passes.

**Phase 2 — CLI input (0.5 day)**

- Add SDL2 capture device to `AudioEngine`.
- `--list-devices`, `--input-device`.
- Populate `ctx.input_left/right` before `process_block()`.
- Verify: mic input → filter → speakers works on Linux + macOS.

**Phase 3 — WASM bridge (0.5 day)**

- Export `cedar_set_input_buffer` and `cedar_set_input_source`.
- Wire into `cedar_process_block`.
- Route `inputs[0]` in AudioWorklet.
- Verify: calling `cedar_set_input_buffer` with a sine buffer in a browser console passes through.

**Phase 4 — Web UI + source selection (1–1.5 days)**

- `input-source.ts`: mic / tab / file acquisition.
- `AudioInputPanel.svelte`: dropdown, status, constraints toggles.
- Permission flow, error surfaces, auto-request on first `in()` compile.
- File source reuses the uploaded-file registry.
- Verify: all Web manual tests in §8.3 pass.

**Phase 5 — Docs & polish (0.5 day)**

- `web/static/docs/in.md`
- Run `bun run build:opcodes` and `bun run build:docs`.
- Update CLAUDE.md section on audio I/O if applicable.

**Future (separate PRDs)**

- Feedback-loop detection / auto-attenuation
- Multi-source routing (`in(0)`, `in(1)`)
- Python bindings for offline input
- Recording / export
- Godot extension wiring

---

## 10. Open Questions

- **Dependency ordering on stereo PRD.** This PRD's `in()` type signature assumes `ChannelCount` and adjacent-buffer output pairs from [`prd-stereo-support.md`](prd-stereo-support.md). The Target Syntax in §3 — most notably `in() |> lp(@, 2000) |> out` — relies on the stereo PRD's **Phase 1** (type system), **Phase 3** (`STEREO_INPUT` VM dispatch), and **Phase 4** (auto-lift codegen) all being in place. If `in()` ships before Phases 3–4 land, the INPUT opcode still works (host fills two buffers, opcode copies them), but downstream mono DSP silently drops the right channel — users would need to pipe through existing stereo opcodes (`width`, `pingpong`) or explicit `pan(left(in()), right(in()))` routing. Recommendation: land the stereo PRD's Phases 1–4 before wiring `in()` into auto-lift, so the UX promised in §3 is real on day one.
- **Source string grammar** — the PRD uses `'mic'`, `'tab'`, `'file:NAME'`. Should `file:` support relative paths, IDs, or both? Recommend: whatever the existing sample-loading uses.
- **Does hot-swap pause capture?** Optimization, not correctness. Deferred to implementation.
- **Latency measurement / alignment** — out of scope; users who need tight input-output alignment can add a short delay manually.

---

## 11. Related Work

- [`prd-stereo-support.md`](prd-stereo-support.md) — Universal Stereo Signal Semantics. Defines the `ChannelCount` type, auto-lift, `stereo()` / `mono()` conversions, and adjacent-buffer-pair invariant that this PRD's `INPUT` opcode relies on.
- [`prd-sample-loading-before-playback.md`](prd-sample-loading-before-playback.md) and [`web_sample_loading.md`](web_sample_loading.md) — Uploaded-file registry reused by the `in('file:NAME')` source.
- [`prd-godot-extension.md`](prd-godot-extension.md) — Godot integration where the input buffer contract defined here must be satisfied.
