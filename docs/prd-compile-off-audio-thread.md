> **Status: NOT STARTED** — design doc; ready for implementation.
> Discovered & root-caused 2026-05-28 via the new e2e test
> [`web/e2e/hot-swap-audio.spec.ts`](../web/e2e/hot-swap-audio.spec.ts).

# Compile Off the AudioWorklet Thread PRD

## 1. Executive Summary

`akkado_compile` currently runs inside the AudioWorklet
(`web/static/worklet/cedar-processor.js:765`). Because the worklet is
single-threaded and message handlers run to completion before
`process()` can fire again, every compile blocks the audio thread for
its full duration — measured at ~110 ms median and ~158 ms peak for the
user's unison-pad on this machine. Each audio block has 2.67 ms of
slack (128 samples / 48 kHz), so a 110 ms compile starves ~40
consecutive blocks → audible silence on every hot-swap. The bug is
trivially reproducible with the existing e2e test on the unison-pad and
manifests as a 240–280 ms gap right after the first recompile.

This PRD moves `akkado_compile` to a dedicated Web Worker that owns its
own WASM instance. The worker runs the compiler and all metadata
extraction (state inits, required samples, soundfonts, MIDI, viz, param
decls, diagnostics, disassembly). The main thread orchestrates the
existing sample/SF2/MIDI loading pipeline using the worker's result.
The AudioWorklet keeps its WASM instance for the VM but loses the
`'compile'` message handler entirely — it only handles `loadProgram`
(now: bytecode + state-init buffer) plus `process()` going forward.

### Why?

The AudioWorklet contract is that nothing on the worklet thread is
allowed to block `process()`. The current implementation violates it —
even though the wire protocol intends compile to be async (compile →
post `compiled` message → wait for `loadCompiledProgram`), the compile
itself runs in the worklet's single-threaded JS context, blocking
`process()` for as long as `akkado_compile` runs in WASM. This is an
architectural mistake, not a design choice.

### Key design decisions

- **Dedicated Web Worker** for compile (`compile.worker.ts`). Worker
  loads its own `nkido.wasm` instance, eagerly at page load alongside
  the worklet.
- **State inits travel as a packed WASM-heap buffer** from worker → main
  thread → worklet. The worklet unpacks via a single new WASM C API
  `cedar_apply_state_inits_from_buffer`. No re-hydration of
  `g_compile_result` in the worklet's WASM.
- **Wire format mirrors current compile-result shape** —
  `{success, bytecode, stateInits, requiredSamples, …}` — one big
  payload, minimum diff vs today's worklet-side compile handler.
- **Worker queue is `superseded-by-newest`**, not FIFO: an in-flight
  compile is discarded if a newer source arrives. Live coders always
  see the latest source applied.
- **The `loadCompiledProgram` SlotBusy retry loop is dropped.** With
  the worker enforcing one-pending-compile, the worklet can guarantee
  no overlapping load attempts; a single attempt suffices.
- **CLI tools are unaffected** — they already compile on the main
  thread, no AudioWorklet, no equivalent block.
- **Worklet contract** is documented in `CLAUDE.md` and at the top of
  `cedar-processor.js`: "the worklet thread may only run
  `process_block` + state-init application; no compile, no codegen, no
  CPU-heavy parsing".

---

## 2. Current State

### 2.1 Where the bug lives

```
main thread                         AudioWorklet thread
─────────────                        ─────────────────────
audioEngine.compile(src)
  port.postMessage({type:'compile',
                    source})  ───────►
                                     handleMessage({compile})
                                       ┌──────────────────────────┐
                                       │ _akkado_compile()        │
                                       │   (~110 ms WASM call,    │
                                       │    blocks worklet thread)│
                                       │ extractStateInits()      │
                                       │ extractDiagnostics()     │
                                       │ getRequired*()           │
                                       └──────────────────────────┘
                                     ◄───── post 'compiled'
  load samples (async, main thread)
  port.postMessage({type:
    'loadCompiledProgram'}) ─────────►
                                     handleMessage({loadCompiledProgram})
                                       _cedar_load_program()       (fast)
                                       _cedar_apply_state_inits()  (fast)
                                       reads g_compile_result
                                     ◄───── post 'programLoaded'

  // While compile was running above, process() did not fire:
  //   ~110 ms / 2.67 ms = ~40 starved blocks = 105 ms of silence.
```

### 2.2 Why moving compile elsewhere requires a WASM API change

Today, state-init application reads from
`g_compile_result.state_inits` inside the WASM the worklet owns —
because compile populated `g_compile_result` in that same WASM. If
compile moves to a separate worker (separate WASM instance), the
worklet's `g_compile_result` is empty. State-init data has to be
shipped across the worker → main → worklet boundary, then re-applied
inside the worklet's WASM from external data, not from
`g_compile_result`.

### 2.3 Measured timings on the user's unison-pad

Captured via the diagnostic test
(`web/e2e/hot-swap-audio.spec.ts → DIAGNOSTIC: load same bytecode`):

| Step | Time |
|------|------|
| `editor.evaluate()` end-to-end, median | 112 ms |
| `editor.evaluate()` peak | 158 ms |
| Audio gap after first recompile | 240–280 ms |
| Silent block count above worklet's silence threshold | reliably reached |

Simple `osc("sin", 440) |> out(@)` produces a 560-byte bytecode and is
fast enough that no gap reproduces — the symptom is patch-complexity
dependent today, but the architecture is broken regardless.

### 2.4 What's known clean — already verified

- The **cedar runtime is bit-perfect across identical-source hot-swap**.
  Proved by `akkado/tests/test_hot_swap_event_transforms.cpp` — six
  test cases including the user's full pad (poly 64, unison 7) render
  swap-laden audio rmse=0 vs continuous baseline. The runtime swap
  mechanism and every state-init function are audio-transparent.
- The **e2e test in `web/e2e/hot-swap-audio.spec.ts` reliably catches
  the bug** on the unison-pad (20 rapid recompiles, 250 ms apart, post-
  warmup RMS drops below 30% threshold for ≥ 12 samples). It will go
  green when this PRD is implemented.

---

## 3. Goals and Non-Goals

### Goals

- `akkado_compile` and every `extract*` / `getRequired*` accessor run
  **outside the AudioWorklet thread**.
- The AudioWorklet thread does only:
  1. `_cedar_process_block` (audio rendering, every 2.67 ms),
  2. `_cedar_apply_state_inits_from_buffer` (called once per swap, must
     stay under 2 ms),
  3. `_cedar_load_program` (already fast).
- E2E test `web/e2e/hot-swap-audio.spec.ts` goes green: no audible gap
  during rapid recompiles of the unison-pad, however long compile takes.
- Compile latency on the audio thread is **architecturally bounded** —
  not measured-and-hoped-for. Regardless of patch size or future
  compiler features, the audio thread cannot starve from compile work.
- Worklet contract is documented + linked from `CLAUDE.md` so the next
  person editing the worklet cannot reintroduce the bug.
- SlotBusy retry path removed. New flow has no concurrent load attempts
  by construction.

### Non-Goals

- **Speeding up `akkado_compile` itself.** Compiler perf is its own
  topic; this PRD only relocates the work.
- **Moving sample / SF2 / MIDI file LOADING** out of the main thread.
  Those are already async I/O on the main thread and don't block audio.
  Only the *metadata enumeration* (`getRequired*` accessors) follows
  the compile move.
- **CLI tools.** `tools/nkido` compiles in the main thread, no
  AudioWorklet exists, no equivalent contract to enforce.
- **Backwards compatibility with the old worklet-compile message
  protocol.** This is a single-landing replacement; the obsolete
  `'compile'` handler is deleted, not deprecated.
- **Eliminating WASM duplication.** Two WASM instances (worker +
  worklet) is intentional. SharedArrayBuffer / COOP-COEP / COI is out
  of scope.
- **`SharedArrayBuffer` for state-init transfer.** Plain
  `postMessage` is fast enough for the ~few-KB state-init buffer
  produced per compile.

---

## 4. Architecture

### 4.1 New thread layout

```
                      ┌───────────────────────────────────┐
                      │  Main thread (audio.svelte.ts)    │
                      │  - orchestrator                   │
                      │  - sample/SF2/MIDI loading        │
                      │  - editor UI                      │
                      └──┬─────────────────────────┬──────┘
                         │                         │
       postMessage       │                         │  port.postMessage
       {compile, source} │                         │  {loadProgram,
                         ▼                         ▼   bytecode,
       ┌───────────────────────────┐    ┌──────────  stateInits}──────┐
       │  Compile Worker           │    │  AudioWorkletProcessor      │
       │  (compile.worker.ts)      │    │  (cedar-processor.js)        │
       │                           │    │                              │
       │  WASM instance #1         │    │  WASM instance #2            │
       │    _akkado_compile        │    │    _cedar_load_program       │
       │    _akkado_get_*          │    │    _cedar_apply_state_inits_ │
       │    _akkado_get_diag_*     │    │      from_buffer  (NEW)      │
       │                           │    │    _cedar_process_block      │
       └───────────────────────────┘    └──────────────────────────────┘
```

The worker and worklet never communicate directly. The main thread is
the single integrator.

### 4.2 End-to-end flow on user "compile"

```
user hits Ctrl+Enter
  │
  ▼
editorStore.evaluate()
  │
  ▼
audioEngine.compile(source)
  │  post {type:'compile', source} to worker
  ▼
[Compile Worker]
  _akkado_compile(source)            ◄── slow but on worker thread
  extractStateInits()                ◄── reads worker WASM heap
  extractRequiredSamples() etc.
  pack stateInits into a u8 buffer   ◄── §5
  post back {success, bytecode,
             stateInitsBuf,
             requiredSamples, ...,
             diagnostics?}
  │
  ▼
[Main thread] receives compile result
  if !success: surface diagnostics, stop.
  else:
    await sample/SF2/MIDI loads     ◄── existing code path, unchanged
    post {type:'loadProgram',
          bytecode,
          stateInitsBuf} to worklet
  │
  ▼
[Worklet] handleMessage({loadProgram})
  _nkido_malloc + memcpy bytecode    ◄── ~tens of µs
  _cedar_load_program(ptr, len)      ◄── fast: ensure_capacity + swap-arm
  _nkido_malloc + memcpy stateInits
  _cedar_apply_state_inits_from_buffer(ptr, len)
                                     ◄── unpack + per-init init_* calls
  free both pointers
  post 'programLoaded' back
  │
  ▼
next process() block executes the swap (handle_swap inside VM)
audio continues uninterrupted because the worklet thread was never
blocked for more than the load-step duration (target <5 ms).
```

### 4.3 Worker lifetime

- Spawned in `audioEngine.initialize()` (same place as the worklet),
  same `nkido.wasm` URL.
- Worker is held in the audio engine module as a singleton reference.
- On worker error or unexpected termination: surface a synthetic
  compile diagnostic (`'Compile worker unavailable, restarting'`),
  respawn lazily on next compile.
- Worker is reused for every compile in the page session — no per-
  compile spawn cost.

### 4.4 Concurrent compiles — supersede-by-newest

The worker holds at most one in-flight compile. Main thread tracks the
"latest source requested" generation:

```ts
let nextGen = 0;
let latestRequested = 0;

async function compile(source: string): Promise<CompileResult> {
  const gen = ++nextGen;
  latestRequested = gen;
  worker.postMessage({type: 'compile', gen, source});
  const result = await awaitResultFor(gen);   // resolves on matching gen
  if (gen !== latestRequested) {
    // a newer compile already started; this result is stale.
    return {success: false, superseded: true};
  }
  return result;
}
```

The worker doesn't need to know about supersession — every compile
runs to completion. The main thread just drops stale results. Cheap and
simple; avoids the complexity of compile interruption.

---

## 5. State-Init Buffer Wire Format

A single contiguous `u8` buffer the worker produces and the worklet
consumes via `cedar_apply_state_inits_from_buffer`. Little-endian.

### 5.1 Top-level layout

```
[ magic: u32 = 0x494E4954 ("INIT") ]
[ version: u16 = 1 ]
[ record_count: u16 ]
[ record_0 ]
[ record_1 ]
...
[ record_{N-1} ]
```

### 5.2 Record header (every record)

```
[ type: u8 ]          ; matches akkado::StateInitData::Type
[ pad: u8[3] ]
[ state_id: u32 ]
[ payload_size: u32 ] ; bytes of type-specific payload following this header
[ payload bytes ... ]
```

The worklet's unpacker walks records using the header size; unknown
types log and skip. Forward-compatible.

### 5.3 Per-type payloads

Each maps 1:1 to an existing `init_*` function in the VM. Field order
mirrors the function signature.

#### `Timeline` (type = 1)

```
[ loop: u8 ]
[ pad: u8[3] ]
[ loop_length: f32 ]
[ num_points: u32 ]
[ points: { time: f32, value: f32, curve: u8, pad: u8[3] }[num_points] ]
```

#### `SequenceProgram` (type = 2)

```
[ cycle_length: f32 ]
[ is_sample_pattern: u8 ]
[ pad: u8[3] ]
[ total_events: u32 ]
[ num_sequences: u32 ]
[ sequences:
  { duration: f32,
    mode: u8, pad: u8[3],
    num_events: u32,
    events: { time: f32,
              duration: f32,
              midi_note: f32,
              velocity: f32,
              num_values: u8, pad: u8[3],
              values: f32[num_values]   ; variable length
            }[num_events]
  }[num_sequences]
]
```

#### `PolyAlloc` (type = 3)

```
[ seq_state_id: u32 ]
[ max_voices: u8 ]
[ mode: u8 ]
[ steal_strategy: u8 ]
[ prop_count: u8 ]
[ release_seconds: f32 ]
[ prop_defaults: f32[MAX_PROPS_PER_EVENT]  ; 4 floats fixed ]
```

#### `ExtendedParams` (type = 4)

```
[ count: u8 ]
[ pad: u8[3] ]
[ constants: f32[count] ]
[ buffer_indices: u16[count] ]
[ pad to 4-byte boundary ]
```

#### `SoundfontEvents` (type = 5)

```
[ sf_seq_state_id: u32 ]
[ sf_preset_idx: i32 ]
```

#### `ForeachAlloc` (type = 6)

```
[ allocator_kind: u8 ]
[ pad: u8[3] ]
[ block_id: u32 ]
[ event_src_state_id: u32 ]
[ max_iterations: u16 ]
[ pad: u8[2] ]
[ max_voices: u8 ]    ; only for VOICE_POOL (kind 0)
[ mode: u8 ]          ; only for VOICE_POOL
[ steal_strategy: u8 ]; only for VOICE_POOL
[ prop_count: u8 ]    ; only for VOICE_POOL
[ release_seconds: f32 ]                          ; only for VOICE_POOL
[ prop_defaults: f32[MAX_PROPS_PER_EVENT] ]       ; only for VOICE_POOL
```

#### `EventTransform` (type = 7), `RateScale` (type = 8) — wait

The numeric IDs above are placeholders. **The final mapping must
match `akkado::StateInitData::Type` exactly** (see
`akkado/include/akkado/codegen.hpp:140`). Implementation must read the
enum, not invent values.

#### `EventTransform` / `Reorder` / `Fanout`

```
[ cycle_length: f32 ]
[ is_sample_pattern: u8 ]
[ pad: u8[3] ]
[ total_events: u32 ]   ; output buffer capacity hint
```

#### `RateScale`

```
; no payload
```

### 5.4 Pack/unpack symmetry

The worker's packer and the worklet's unpacker share the layout above.
A small shared TS module (`web/src/lib/audio/state-init-codec.ts`)
exports both `packStateInits(jsObjects): Uint8Array` and a TS
description of the format for sanity checks. The worklet only needs the
C side (`cedar_apply_state_inits_from_buffer`) — it never sees the JS
objects.

### 5.5 New WASM exports

Add to `web/wasm/CMakeLists.txt` `NKIDO_EXPORTED_FUNCTIONS`:

```
_cedar_apply_state_inits_from_buffer
```

Signature in `web/wasm/nkido_wasm.cpp`:

```cpp
// Parse a packed buffer (format §5.1–5.3) and route each record to the
// matching VM::init_*_state call. Returns the number of records applied,
// or -1 on a malformed buffer (bad magic, truncated, unknown type).
WASM_EXPORT int32_t cedar_apply_state_inits_from_buffer(
    const uint8_t* buf, uint32_t byte_count);
```

The implementation lives next to the existing `cedar_apply_state_inits`
(deleted in this PRD — see §7).

---

## 6. Impact Assessment

| Component | Status | Notes |
|-----------|--------|-------|
| Cedar VM (`cedar/`) | **Unchanged** | Hot-swap mechanism already bit-perfect (proven). |
| Akkado compiler (`akkado/`) | **Unchanged** | Still produces `CompileResult` with state_inits; just runs in a different WASM instance. |
| `tools/nkido`, `tools/akkado` | **Unchanged** | No worklet, no contract violation. |
| `web/wasm/nkido_wasm.cpp` | **Modified** | New `cedar_apply_state_inits_from_buffer`. Delete old `cedar_apply_state_inits` (no caller after this PRD). |
| `web/wasm/CMakeLists.txt` | **Modified** | Update `NKIDO_EXPORTED_FUNCTIONS` list. |
| `web/static/worklet/cedar-processor.js` | **Modified** | Delete entire `'compile'` message handler + `extract*` helpers. Replace `'loadCompiledProgram'` with `'loadProgram'` taking pre-packed `stateInitsBuf`. Drop SlotBusy retry loop. |
| `web/src/lib/audio/compile.worker.ts` | **New** | Owns compile WASM. Mirrors today's worklet `compile` handler, returns one big payload. |
| `web/src/lib/audio/state-init-codec.ts` | **New** | TS pack function + format docs. Used in worker. |
| `web/src/lib/stores/audio.svelte.ts` | **Modified** | Spawn worker in `initialize()`. Route `compile()` to worker, sample-load on main, post `loadProgram` to worklet. Generation/supersede logic. Surface worker-down diagnostics. |
| `web/e2e/hot-swap-audio.spec.ts` | **Unchanged** | Same test, will go green. |
| `akkado/tests/test_hot_swap_event_transforms.cpp` | **Unchanged** | CLI runtime test unaffected. |
| `CLAUDE.md` | **Modified** | Add a "Worklet thread contract" section. |

---

## 7. File-Level Changes

### 7.1 Files to create

| File | Purpose |
|------|---------|
| `web/src/lib/audio/compile.worker.ts` | Web Worker entry point. Loads its own WASM instance. Listens for `{type:'compile', gen, source}`. Calls `_akkado_compile`, extracts diagnostics + bytecode + state_inits (packed) + required-sample/SF2/MIDI/viz/param/disassembly metadata. Posts back `{type:'compileResult', gen, success, ...payload}`. |
| `web/src/lib/audio/state-init-codec.ts` | Pack JS-side state-init records into the wire format from §5. Format documented inline. Used only by the worker. |

### 7.2 Files to modify

| File | Change |
|------|--------|
| `web/wasm/nkido_wasm.cpp` | Add `cedar_apply_state_inits_from_buffer`. Delete `cedar_apply_state_inits` (no callers). |
| `web/wasm/CMakeLists.txt` | Replace `_cedar_apply_state_inits` with `_cedar_apply_state_inits_from_buffer` in exports list. |
| `web/static/worklet/cedar-processor.js` | Remove `case 'compile':`, the entire compile + extract helper stack (`extractStateInits`, `extractDiagnostics`, `extractParamDecls`, `extractVizDecls`, `extractBuiltinVarOverrides`, `getRequired*` helpers). Rename `loadCompiledProgram` → `loadProgram` and accept `{bytecode, stateInitsBuf}` directly (no `pendingProgram` indirection). Drop the SlotBusy retry loop — the new orchestrator guarantees no overlap. Add a top-of-file comment block stating the worklet-thread contract. |
| `web/src/lib/stores/audio.svelte.ts` | In `initialize()`: spawn the compile worker, load its WASM. Replace `compile()` body: post `compile` to worker (with generation tag), await `compileResult`, drop if superseded, then run existing sample-load pipeline, then post `loadProgram` to worklet. Surface worker error/death as a compile diagnostic; respawn lazily. |
| `CLAUDE.md` (nkido project) | Add a "Web architecture: worklet thread contract" section: only `process_block` + `cedar_apply_state_inits_from_buffer` may run inside the worklet. Compile, parsing, codegen, fetch, decode — all forbidden on the worklet thread. Pointer to this PRD. |

### 7.3 Files that explicitly require **no changes**

| File | Reason |
|------|--------|
| `cedar/` (entire) | Runtime is already correct (proven by `test_hot_swap_event_transforms.cpp`). |
| `akkado/` (entire) | Compiler is unchanged; runs in worker WASM instead of worklet WASM. |
| `tools/` | CLI tools don't use the worklet. |
| `web/e2e/hot-swap-audio.spec.ts` | Same test, just expected to go green. |

---

## 8. Implementation Tasks (single landing)

Order matters but everything ships as one PR. Each step has a clear
verification step.

1. **WASM API** — implement `cedar_apply_state_inits_from_buffer` in
   `nkido_wasm.cpp` and add to exports. **Verify**: write a small C++
   round-trip test that packs a state-init buffer (using a TS-mirror
   pack written by hand for the test) and confirms `apply_*` calls
   produce the same VM state as the existing per-type `init_*` calls.
   Or: temporarily expose a JS pack helper and use the existing e2e
   test as the integration check after step 4.
2. **Worker scaffold** — create `compile.worker.ts`. Make it load WASM,
   call `_akkado_compile` on a dummy source, post the bytecode back.
   **Verify**: a tiny console-only test in `audio.svelte.ts` boot path,
   removed before merge.
3. **State-init codec** — create `state-init-codec.ts` with the pack
   function. Move every `extract*` helper from the worklet into the
   worker (or into a shared helper module the worker imports).
   **Verify**: snapshot the JS output of the OLD worklet `extractStateInits`
   for a few patches, then check the NEW worker produces identical JS,
   and that the packed buffer round-trips through the WASM unpacker.
4. **Worklet rewire** — delete the compile handler + extract helpers.
   Replace `loadCompiledProgram` with `loadProgram(bytecode, stateInitsBuf)`.
   Drop SlotBusy retry. **Verify**: app boots, simple patch plays.
5. **Main-thread orchestrator** — `audio.svelte.ts` compile() routes
   to worker, runs sample loads, posts `loadProgram` to worklet.
   Implement supersede-by-generation. **Verify**: e2e test
   `hot-swap-audio.spec.ts` goes green on `unison-pad` (target: no
   silence run > 2 samples = 40 ms in the 7 s capture window).
6. **Worker error handling** — surface worker death as a compile
   diagnostic; respawn on next compile. **Verify**: a unit/integration
   test that calls `worker.terminate()` mid-flight, confirms the next
   compile rebuilds the worker and succeeds.
7. **Contract documentation** — add the worklet-thread contract to
   `CLAUDE.md`. Add the top-of-file comment block to
   `cedar-processor.js`. Link to this PRD.

---

## 9. Edge Cases

### 9.1 Compile fails (syntax error, type error)

Worker sends `{success: false, diagnostics: [...]}`. Main thread
surfaces diagnostics via the existing `setDiagnostics`/`setCompileError`
path. Worklet is not contacted. Last valid program keeps playing.

### 9.2 Compile succeeds but a required sample fails to load

Existing main-thread sample-loading code path already handles this —
returns a compile-style diagnostic, `loadProgram` is never posted to
worklet. Unchanged.

### 9.3 User hits compile while one is in flight

Newer compile gets a higher `gen`. Worker is single-threaded so the
new compile starts after the previous one finishes. Main thread sees
both responses but drops the older one (its `gen !== latestRequested`).
The newer source wins. No race.

### 9.4 User hits compile *many* times rapidly (e.g. holds Ctrl+Enter)

Every call gets a new `gen`. Only the latest matters. Worker churns
through compiles but only the most recent result lands in the worklet.
In the worst case the worklet does N `loadProgram` calls if every
intermediate gen was the latest at completion time — fine, each load is
fast.

### 9.5 Worker dies (WASM trap, OOM, browser kill)

Main thread's worker `.onerror` / message-port-closed fires. Mark
worker dead, surface a synthetic compile diagnostic
`'Compile worker crashed — restarting'`. Next `compile()` call
re-spawns the worker before sending the message.

### 9.6 Worker WASM fails to load (network error, etc.)

Initial `audioEngine.initialize()` await catches it. Surface as a UI
error similar to today's worklet-WASM-failed path. Audio engine sits in
a degraded state — runtime still works for the currently loaded
program (if any), but compile is unavailable until reload.

### 9.7 Initial program load (first compile, no prior program)

Same path as hot-swap: worker compiles, main loads samples, worklet
calls `_cedar_load_program` (which sees no previous program, no
crossfade, direct install). State-init buffer is applied. No special
case.

### 9.8 Hot-swap during compile result delivery

Worklet's `process()` keeps running throughout. When `loadProgram`
arrives, the next `process_block` will see `swap_controller_`'s pending
flag and execute the handover. There is no window where audio stops
because nothing on the worklet thread blocks `process()` for more than
the load step (target <5 ms).

### 9.9 State-init buffer is malformed (bug in packer)

`cedar_apply_state_inits_from_buffer` returns -1 and logs
`[CEDAR BUG]` to console. The new program loads with no state inits,
which is a known broken state (patterns won't play). User sees broken
audio. Better to surface loudly than silently swallow.

### 9.10 Worker takes 5+ seconds to compile (huge patch, slow compiler)

Audio continues uninterrupted because it's on the worklet thread.
UI shows "compiling…" indicator (existing `state.isEvaluating` flag).
User can edit further; the new edit supersedes the in-flight compile
(§9.3).

---

## 10. Testing / Verification Strategy

### 10.1 Existing CLI tests (must stay green)

```
cmake --build build --target akkado_tests
./build/akkado/tests/akkado_tests "[hot_swap],[fuzz][recompile],[determinism]"
```

The C++ runtime is unchanged; these are the safety net that catches any
accidental regression in `cedar`/`akkado`.

### 10.2 New WASM-API round-trip test (cedar level)

A C++ test in `cedar/tests/` or `akkado/tests/` that:
1. Builds a state-init JS-equivalent object in memory.
2. Hand-packs it into the §5 wire format.
3. Calls `cedar_apply_state_inits_from_buffer` on a fresh VM.
4. Asserts each state in `vm.states()` matches what
   `apply_seq_state_inits` produces for the same input.

Locks down the wire format independently of the worker.

### 10.3 E2E test (must go green)

```
cd web && bun run test:e2e -- --grep "unison-pad"
```

Expected after this PRD:
- `editor.evaluate()` median **<5 ms** (currently 112 ms).
- `worstRun` of low-RMS samples **<2** (currently 12–14, ~280 ms).
- No `Output silent for 100 blocks` warning in console output.
- Test passes 5 of 5 consecutive runs (currently fails 3 of 3 with
  aggressive timing).

### 10.4 Worker error recovery test (new)

A Playwright test that:
1. Boots the app.
2. Calls `window.__nkidoTest.audioEngine.compile(SOURCE)` — success.
3. Terminates the worker via `window.__nkidoTest` (new accessor).
4. Calls compile again — surfaces a diagnostic.
5. Calls compile a third time — succeeds with a respawned worker.

### 10.5 Manual smoke before merge

- Load the welcome patches, recompile each 20× rapidly — no audible
  gap on any.
- Open the user's `web/static/patches/unison-pad.akk` — recompile
  with no source changes — no gap.
- Open a parser-error patch — see the diagnostic, no audio interruption.
- Refresh the page mid-compile — no zombie worker, no audio glitch.

---

## 11. Open Questions

*(none — all design decisions resolved during PRD authoring on
2026-05-28. Implementation may surface details that need follow-up; add
to this section as they appear.)*
