> **Status: IMPLEMENTED 2026-05-29 (v0.4.3).** Compile runs in
> `web/src/lib/audio/compile.worker.ts`; the worklet consumes pre-packed
> buffers via `loadProgram` and the four `*_from_buffer` WASM exports.
> Discovered & root-caused 2026-05-28 via the e2e test
> [`web/e2e/hot-swap-audio.spec.ts`](../web/e2e/hot-swap-audio.spec.ts).
>
> **Deviation from the original design (§5.4):** the wire-format codec is
> *not* an auto-generated TS packer. Pack **and** unpack live co-located in
> C++ at `akkado/include/akkado/state_init_buffer.hpp`; the worker packs by
> calling the WASM `_akkado_pack_*_buffer` exports and shipping the raw
> bytes. Because both WASM builds memcpy the same structs, wire-format drift
> is structurally impossible (the `WIRE_VERSION` header guards the
> stale-browser-cache case). The TS side is a generated **validator** —
> `web/src/lib/audio/state-init-codec.ts` (a decoder + size constants,
> emitted by `web/scripts/build-state-init-codec.ts`) — cross-checked
> against real C++-packed bytes by `web/tests/state-init-codec.test.ts`.
> The C++ round-trip test `akkado/tests/test_state_init_buffer_codec.cpp`
> is the primary byte-level guard. See §5.4 / §10.2.
>
> Reviewed 2026-05-28: wire format expanded to cover block_table,
> MIDI sources, and sample mappings; SequenceProgram events memcpy
> the raw `cedar::Event` struct; SlotBusy retry is kept but simplified;
> the <5 ms target reframed as the worklet load-step duration, not
> `evaluate()` E2E.

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
  the worklet. Compile requests issued before the worker WASM finishes
  loading are queued and flushed on `ready` — and the queue itself is
  supersede-by-newest, so only the latest pre-ready compile lands.
- **Three packed WASM-heap buffers** travel worker → main → worklet:
  (a) `bytecode`, (b) `stateInitsBuf` (records keyed by
  `StateInitData::Type`, plus inline sample-mapping records resolved
  at the worklet's sample bank), (c) `midiSourcesBuf` (one record per
  `RequiredMidiSource`). The L3 `BlockEntry[]` table travels as a
  fourth typed-array field on the `loadProgram` message. The worklet
  exposes three new WASM C APIs that consume them in place — no
  re-hydration of `g_compile_result` in the worklet's WASM.
- **Sequence `Event` payloads are memcpy'd raw** from the C++ struct.
  Worker and worklet share a build, so layouts match by construction;
  the `version` field in the buffer header guards mismatch. The
  packer/unpacker for every record type is auto-generated from the
  C++ headers at build time (see §5.4), so layout drift surfaces at
  build, not at runtime.
- **Worker queue is `superseded-by-newest`**, not FIFO: an in-flight
  compile is discarded if a newer source arrives. Live coders always
  see the latest source applied.
- **The worklet's `loadProgram` keeps a (simplified) SlotBusy retry.**
  Supersede-by-newest serializes compiles, but two compiles finishing
  close together can still post two `loadProgram` messages to the
  worklet before the audio thread fires `process_block` and frees a
  swap slot. SlotBusy is a runtime-thread invariant on `SwapController`,
  unrelated to upstream serialization — the retry stays, simplified to a
  single per-block retry instead of today's multi-attempt state machine.
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

Today, the worklet's WASM populates `g_compile_result` via
`_akkado_compile` and then *every* post-compile step reads from it:

| Today's post-compile call | Reads from `g_compile_result` |
|---------------------------|-------------------------------|
| `cedar_load_program(bytecode, len)` (wasm) | `required_buffers` (advisory; redundant with `max_buffer_index`), `block_table` + `main_instruction_count` (required for FOREACH_EVENT / `iter()` / poly L3) |
| `akkado_patch_sample_ids_in_bytecode` (wasm) | `scalar_sample_mappings` (for direct `sample("name", …)` calls) |
| `akkado_resolve_sample_ids` (wasm) | `state_inits[].sequence_sample_mappings` (patches sample IDs into pattern events) |
| `cedar_apply_state_inits` (wasm) | `state_inits[]` (all 8 `StateInitData::Type`s) |
| `cedar_apply_midi_sources` (wasm) | `required_midi_sources` (one `init_midi_queue_state` per `midi(...)` call) |

If compile moves to a separate worker (separate WASM instance), the
worklet's `g_compile_result` is empty for every one of the rows above.
Every datum the apply-side reads has to be shipped across the worker →
main → worklet boundary, then re-applied from external data — not from
`g_compile_result`. This PRD adds three packed buffers
(`stateInitsBuf`, `midiSourcesBuf`, `blockTable`) and three new WASM
exports (`cedar_apply_state_inits_from_buffer`,
`cedar_apply_midi_sources_from_buffer`, `cedar_set_block_table`) to
replace the in-WASM `g_compile_result` indirection.

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
  2. `_cedar_set_block_table` (per swap, fast),
  3. `_cedar_apply_state_inits_from_buffer` (per swap, target <2 ms),
  4. `_cedar_apply_midi_sources_from_buffer` (per swap, fast),
  5. `_akkado_patch_sample_ids_in_bytecode` + `_akkado_resolve_sample_ids_from_buffer` (per swap, fast — operate on caller-provided buffers, not `g_compile_result`),
  6. `_cedar_load_program` (already fast).
- E2E test `web/e2e/hot-swap-audio.spec.ts` goes green: no audible gap
  during rapid recompiles of the unison-pad, however long compile takes.
- Compile latency on the audio thread is **architecturally bounded** —
  not measured-and-hoped-for. Regardless of patch size or future
  compiler features, the audio thread cannot starve from compile work.
- Worklet contract is documented + linked from `CLAUDE.md` so the next
  person editing the worklet cannot reintroduce the bug.
- SlotBusy retry stays in the worklet's `loadProgram` handler,
  simplified to a single per-`process_block` retry. `pendingProgram`
  + `pendingLoadRetry` state shrinks, but the retry idea is kept
  because supersede-by-newest serializes compiles, not load messages.

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
       ┌───────────────────────────┐    ┌──────────  stateInitsBuf,──┐
       │  Compile Worker           │    │  midiSourcesBuf,           │
       │  (compile.worker.ts)      │    │  blockTable}                │
       │                           │    │                              │
       │  WASM instance #1         │    │  AudioWorkletProcessor       │
       │    _akkado_compile        │    │  (cedar-processor.js)        │
       │    _akkado_get_*          │    │                              │
       │    _akkado_get_diag_*     │    │  WASM instance #2            │
       │                           │    │    _cedar_set_block_table   │
       │                           │    │      (NEW)                   │
       │                           │    │    _cedar_load_program       │
       │                           │    │    _akkado_patch_sample_ids_ │
       │                           │    │      in_bytecode             │
       │                           │    │    _akkado_resolve_sample_   │
       │                           │    │      ids_from_buffer (NEW)   │
       │                           │    │    _cedar_apply_state_inits_ │
       │                           │    │      from_buffer (NEW)       │
       │                           │    │    _cedar_apply_midi_sources_│
       │                           │    │      from_buffer (NEW)       │
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
  pack stateInits into u8 buffer     ◄── §5.1–5.3 (incl. sample-mapping records)
  pack midi sources into u8 buffer   ◄── §5.6
  serialize BlockEntry[] + main_inst ◄── §5.7 (Int32Array)
  post back {success, bytecode,
             stateInitsBuf,
             midiSourcesBuf,
             blockTable, mainInstCount,
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
          stateInitsBuf,
          midiSourcesBuf,
          blockTable,
          mainInstCount} to worklet
  │
  ▼
[Worklet] handleMessage({loadProgram})
  _cedar_set_block_table(ptr, count, mainInstCount)
                                     ◄── stages L3 subprogram table
  _nkido_malloc + memcpy bytecode    ◄── ~tens of µs
  _akkado_patch_sample_ids_in_bytecode(buf, len)
                                     ◄── now reads from stateInitsBuf, NOT g_compile_result
  _cedar_load_program(ptr, len)      ◄── fast: ensure_capacity + swap-arm.
                                     ◄── On SlotBusy: hold buf, retry from next process_block.
  _nkido_malloc + memcpy stateInits
  _akkado_resolve_sample_ids_from_buffer(ptr, len)
                                     ◄── patches sample IDs into events in-place
  _cedar_apply_state_inits_from_buffer(ptr, len)
                                     ◄── unpack + per-init init_* calls
  _nkido_malloc + memcpy midiSources
  _cedar_apply_midi_sources_from_buffer(ptr, len)
                                     ◄── one init_midi_queue_state per record
  free all pointers
  post 'programLoaded' back
  │
  ▼
next process() block executes the swap (handle_swap inside VM)
audio continues uninterrupted because the worklet thread was never
blocked for more than the load-step duration (target <5 ms).
```

### 4.3 Worker lifetime

- Spawned in `audioEngine.initialize()` (same place as the worklet),
  same `nkido.wasm` URL. Worker and worklet share the same WASM
  build — the worker exercises the `akkado_*` + `_akkado_get_*`
  surface; the worklet exercises `_cedar_*` + the new
  `*_from_buffer` / `_cedar_set_block_table` surface.
- Worker is held in the audio engine module as a singleton reference.
- **Boot-time queueing.** The worker emits `{type:'ready'}` after its
  WASM is initialized. `audioEngine.compile(source)` resolves against
  a `workerReady` promise; calls issued before ready are queued, and
  the queue itself is supersede-by-newest (only the latest pending
  source is dispatched once ready). This avoids dropping the user's
  first Ctrl+Enter if it lands during WASM init.
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

Supersede-by-newest only serializes **compiles**, not the
`loadProgram` post to the worklet. If two non-superseded compiles
finish close together, both may post `loadProgram` before the audio
thread fires `process_block` — the second call would hit
`LoadResult::SlotBusy` on the worklet's `SwapController`. The worklet
keeps a single-shot retry on SlotBusy (drive from the next
`process_block`) so the second message doesn't get dropped silently.
See §9.8.

---

## 5. Wire Formats

Three packed `u8` buffers + one `Int32Array` cross the worker → main →
worklet boundary. All buffers are little-endian. Layouts that mirror
C++ structs are produced by `memcpy` from worker WASM and consumed by
`memcpy` in worklet WASM — worker and worklet are the same build, so
struct layouts match by construction; the `version` field in each
buffer header guards mismatch at the boundary (e.g. stale browser
cache during deploys).

The buffers:

| Field on `loadProgram` message | Format | Consumer in worklet |
|--------------------------------|--------|---------------------|
| `bytecode` | `Uint8Array` of `cedar::Instruction[]` | `_cedar_load_program` (after `_akkado_patch_sample_ids_in_bytecode`) |
| `stateInitsBuf` | §5.1–5.3 | `_akkado_resolve_sample_ids_from_buffer` then `_cedar_apply_state_inits_from_buffer` |
| `midiSourcesBuf` | §5.6 | `_cedar_apply_midi_sources_from_buffer` |
| `blockTable` + `mainInstCount` | `Int32Array` of `BlockEntry` packed + `u32` | `_cedar_set_block_table` (called *before* `_cedar_load_program` so the table is staged for the load) |

`stateInitsBuf` carries both `StateInitData` records **and** sample-
mapping records (`SequenceSampleMapping[]` per sequence record;
`ScalarSampleMapping[]` for direct `sample("name", …)` calls). The
worklet's sample bank is queried at apply time — the worker doesn't
need a sample-bank snapshot.

### 5.1 stateInitsBuf — top-level layout

```
[ magic: u32 = 0x494E4954 ("INIT") ]
[ version: u16 = 1 ]
[ record_count: u16 ]
[ record_0 ]
[ record_1 ]
...
[ record_{N-1} ]
```

Records are either `StateInit` records (one per
`g_compile_result.state_inits[]` entry — §5.3) or `SampleMapping`
records (§5.3 sub-section, batched per scope). Forward-compatible:
unknown record types log and skip.

### 5.2 Record header (every record)

```
[ kind: u8 ]          ; 0 = StateInit (payload's first byte is StateInitData::Type)
                      ; 1 = SequenceSampleMapping batch (binds events in a SequenceProgram)
                      ; 2 = ScalarSampleMapping batch (patches PUSH_CONSTs)
[ pad: u8[3] ]
[ state_id: u32 ]     ; for kind=0 the owning state; for kind=1 the parent SequenceProgram state; for kind=2 unused (zero)
[ payload_size: u32 ] ; bytes of type-specific payload following this header
[ payload bytes ... ]
```

The worklet's unpacker walks records using the header size; unknown
kinds log and skip. Forward-compatible.

### 5.3 Per-type payloads (kind = 0, StateInit)

The first byte of each StateInit payload is the `StateInitData::Type`
discriminant (see `akkado/include/akkado/codegen.hpp:140` for the
authoritative enum); remaining bytes map 1:1 to an existing `init_*`
function in the VM. Field order mirrors the function signature.

> **Layouts that contain `cedar::Event`, `cedar::Sequence`,
> `cedar::TimelineState::Breakpoint`, `BlockEntry`, etc. are emitted
> by `memcpy` of the C++ struct** (with explicit struct-padding
> preserved). Worker and worklet share the build, so layouts match by
> construction. The `version` field gates the build-mismatch case.
> The pack/unpack code on both sides is **auto-generated** from the
> C++ headers (see §5.4) so layout drift fails at build, not runtime.

#### `Timeline` (type = 1)

```
[ type: u8 = 1, pad: u8[3] ]
[ loop: u8, pad: u8[3] ]
[ loop_length: f32 ]
[ num_points: u32 ]
[ points: memcpy of cedar::TimelineState::Breakpoint[num_points] ]
```

#### `SequenceProgram` (type = 2)

```
[ type: u8 = 2, pad: u8[3] ]
[ cycle_length: f32 ]
[ is_sample_pattern: u8, pad: u8[3] ]
[ total_events: u32 ]
[ num_sequences: u32 ]
[ sequences:
  { duration: f32,
    mode: u8, pad: u8[3],
    num_events: u32,
    events: memcpy of cedar::Event[num_events]
            (full struct: time, duration, chance, velocity,
             midi_note, type, num_values, type_id, source_offset,
             source_length, values[16], velocities[16], notes[16],
             prop_vals[4], prop_set_mask, etc. — see
             cedar/include/cedar/opcodes/sequence.hpp:44 for the
             authoritative layout)
  }[num_sequences]
]
```

`SequenceSampleMapping` records for this sequence travel as
**separate `kind = 1` records** in the same buffer, carrying
`state_id` of the owning sequence in the record header (see §5.3.x).

#### `PolyAlloc` (type = 3)

```
[ type: u8 = 3, pad: u8[3] ]
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
[ type: u8 = 4, pad: u8[3] ]
[ count: u8, pad: u8[3] ]
[ constants: f32[MAX_EXTENDED_PARAMS]    ; 8 floats fixed ]
[ buffer_indices: u16[MAX_EXTENDED_PARAMS] ; 8 u16s fixed ]
```

#### `SoundfontEvents` (type = 5)

```
[ type: u8 = 5, pad: u8[3] ]
[ sf_seq_state_id: u32 ]
[ sf_preset_idx: i32 ]
```

#### `ForeachAlloc` (type = 6)

```
[ type: u8 = 6, pad: u8[3] ]
[ allocator_kind: u8, pad: u8[3] ]
[ block_id: u32 ]
[ event_src_state_id: u32 ]
[ max_iterations: u16, pad: u8[2] ]
[ max_voices: u8 ]    ; VOICE_POOL only (kind 0) — zero for others
[ mode: u8 ]          ; VOICE_POOL only
[ steal_strategy: u8 ]; VOICE_POOL only
[ prop_count: u8 ]    ; VOICE_POOL only
[ release_seconds: f32 ]                          ; VOICE_POOL only
[ prop_defaults: f32[MAX_PROPS_PER_EVENT] ]       ; VOICE_POOL only
```

#### `EventTransform` (type = 7) / `RateScale` (type = 8) / `Reorder` (type = 9) / `Fanout` (type = 10)

Type discriminants match `akkado::StateInitData::Type` exactly (see
`akkado/include/akkado/codegen.hpp:142–171`).

`EventTransform`, `Reorder`, `Fanout` share a payload:

```
[ type: u8 = {7|9|10}, pad: u8[3] ]
[ cycle_length: f32 ]
[ is_sample_pattern: u8, pad: u8[3] ]
[ total_events: u32 ]   ; output buffer capacity
```

`RateScale`:

```
[ type: u8 = 8, pad: u8[3] ]
; no further payload
```

#### `SequenceSampleMapping` batch (kind = 1)

One record per parent `SequenceProgram`. `state_id` in the record
header is the owning sequence's state_id.

```
[ count: u32 ]
[ mappings:
  { seq_idx: u16, event_idx: u16, value_slot: u8, variant: u8, pad: u8[2],
    bank_len: u16, name_len: u16,
    bank_chars: u8[bank_len], name_chars: u8[name_len]
  }[count]
]
```

Worklet calls `_akkado_resolve_sample_ids_from_buffer(buf, len)` which
walks all `kind = 1` records, looks each name up in the worklet's
sample bank, and writes the resolved ID into the corresponding
`Event::values[value_slot]` already laid down by the matching
SequenceProgram record (which was applied earlier in the buffer
walk).

#### `ScalarSampleMapping` batch (kind = 2)

```
[ count: u32 ]
[ mappings:
  { instruction_index: u32, variant: u8, pad: u8[3],
    bank_len: u16, name_len: u16,
    bank_chars: u8[bank_len], name_chars: u8[name_len]
  }[count]
]
```

Read by `_akkado_patch_sample_ids_in_bytecode(bytecode, len,
mappings_buf, mappings_len)` — same as today's function, but the
mappings source is the buffer parameter instead of `g_compile_result`.
Worklet calls this after `_nkido_malloc + memcpy(bytecode)` and before
`_cedar_load_program`, as today.

### 5.4 Pack/unpack symmetry — co-located in C++, validated from TS

> **As shipped, this differs from the original "generate a TS packer"
> plan.** Packing was kept in C++ instead, which removes the drift risk
> at its source; the generated TS artifact became a validator. Rationale
> and the as-built shape follow.

**Packer + unpacker live together in C++.** Both the `pack_*` and the
`apply_*` / `resolve_*` halves of the wire format are defined in one
header, `akkado/include/akkado/state_init_buffer.hpp`
(`detail::Writer` / `detail::Reader`, `pack_state_inits` /
`apply_state_inits`, `pack_midi_sources` / `apply_midi_sources`,
`pack_block_table`). The worker WASM and the worklet WASM compile the
**same** header from the **same** build, so every record's byte layout
matches by construction — the worker's `packStateInits()` etc.
(`compile.worker.ts`) just call the `_akkado_pack_*_buffer` exports and
ship the raw bytes; no TS packing happens on the production path.

Because there is no second (TS) implementation of the layout, the
"keep the TS packer aligned with C++" problem the original §5.4
solved **does not exist**. The `WIRE_VERSION` field in each buffer
header still guards the one residual case — a stale browser cache
serving an old worklet against a new worker after a deploy.

**Drift guards (two layers):**

1. **C++ round-trip test** — `akkado/tests/test_state_init_buffer_codec.cpp`
   (`[state_init_codec]`) packs a real `CompileResult` and applies it to
   a fresh VM, comparing against the legacy per-`init_*` path, plus a
   bit-perfect hot-swap render of the user pad shape. This is the
   primary byte-level guard. Struct sizes the wire format memcpy's are
   pinned with `static_assert` (`sizeof(cedar::Event)`,
   `TimelineState::Breakpoint`, and `BlockEntry` at its own definition).

2. **Generated TS validator** — `web/scripts/build-state-init-codec.ts`
   (a generator analogous to `build:opcodes` / `build:docs`, run via
   `bun run build:state-init-codec`) parses the authoritative facts out
   of the C++ headers — `MAGIC_*`, `WIRE_VERSION`, `RecordKind`
   (`state_init_buffer.hpp`), `StateInitData::Type` (`codegen.hpp`),
   and the `static_assert(sizeof(...))` pins for `Event` / `Breakpoint`
   / `BlockEntry` — and emits `web/src/lib/audio/state-init-codec.ts`: a
   **decoder** plus size constants (no packer). `web/tests/state-init-codec.test.ts`
   loads the real `nkido.wasm`, compiles representative patches, asks the
   C++ packer for the actual `stateInitsBuf` bytes, and walks them with
   the generated decoder — stepping over `cedar::Event[]` via the
   generated `EVENT_SIZE` and asserting the walk lands exactly on each
   record's `payload_size` and on `buf.byteLength`. A wrong struct size
   or field order fails this test. Regenerate after any wire-format
   change; the regenerated file must be byte-identical (CI drift check).

The worklet only consumes via the C-side WASM exports below — it
never sees the JS objects, so no codec on the worklet side.

### 5.5 New WASM exports

Add to `web/wasm/CMakeLists.txt` `NKIDO_EXPORTED_FUNCTIONS`:

```
_cedar_set_block_table
_cedar_apply_state_inits_from_buffer
_cedar_apply_midi_sources_from_buffer
_akkado_resolve_sample_ids_from_buffer
```

Update the existing `_akkado_patch_sample_ids_in_bytecode` signature
to take a mappings buffer parameter (was: reads
`g_compile_result.scalar_sample_mappings`; now: reads caller-provided
buffer).

Signatures in `web/wasm/nkido_wasm.cpp`:

```cpp
// Stage the FOREACH_EVENT subprogram table for the next
// cedar_load_program call. Mirrors today's load_program path that
// reads g_compile_result.block_table.
WASM_EXPORT int32_t cedar_set_block_table(
    const uint8_t* entries_buf, uint32_t entry_count,
    uint32_t main_instruction_count);

// Parse stateInitsBuf (format §5.1–5.3) and route each StateInit
// record to the matching VM::init_*_state call. Sample-mapping
// records (kind=1,2) are validated but not applied here — see
// _akkado_resolve_sample_ids_from_buffer.
// Returns # of state-init records applied, or -1 on malformed buffer.
WASM_EXPORT int32_t cedar_apply_state_inits_from_buffer(
    const uint8_t* buf, uint32_t byte_count);

// Walk SequenceSampleMapping records (kind=1) in stateInitsBuf and
// patch Event::values[slot] = bank.get_sample_id(name) for each.
// Must run AFTER cedar_apply_state_inits_from_buffer (so events
// exist) and AFTER sample bank is populated.
WASM_EXPORT int32_t akkado_resolve_sample_ids_from_buffer(
    const uint8_t* buf, uint32_t byte_count);

// Parse midiSourcesBuf (format §5.6) and call VM::init_midi_queue_state
// for each record. Must run AFTER cedar_load_program (new program's
// state pool active).
WASM_EXPORT int32_t cedar_apply_midi_sources_from_buffer(
    const uint8_t* buf, uint32_t byte_count);
```

Delete `cedar_apply_state_inits`, `cedar_apply_midi_sources`,
`akkado_resolve_sample_ids` (no callers after this PRD).

### 5.6 midiSourcesBuf

One record per `g_compile_result.required_midi_sources[]` entry.
Mirrors `cedar::MidiSourceKind` and `MidiQueueState::TempoMode`.

```
[ magic: u32 = 0x4D494449 ("MIDI") ]
[ version: u16 = 1 ]
[ record_count: u16 ]
[ records:
  { state_id: u32,
    kind: u8, channel_filter: u8, loop: u8, tempo_mode: u8,
    name_len: u16, pad: u8[2],
    name_chars: u8[name_len], pad to 4-byte boundary
  }[record_count]
]
```

### 5.7 blockTable

Posted as a `Uint8Array` (memcpy of `BlockEntry[]`) + a `u32`
`mainInstCount`. The worklet calls
`_cedar_set_block_table(entries, count, mainInstCount)` **before**
`_cedar_load_program` so the table is staged for that load (matches
today's `g_compile_result.block_table` flow). Empty table is fine —
patches without `foreach()`/`iter()` have zero entries.

---

## 6. Impact Assessment

| Component | Status | Notes |
|-----------|--------|-------|
| Cedar VM (`cedar/`) | **Unchanged** | Hot-swap mechanism already bit-perfect (proven). |
| Akkado compiler (`akkado/`) | **Unchanged** | Still produces `CompileResult` with state_inits; just runs in a different WASM instance. |
| `tools/nkido`, `tools/akkado` | **Unchanged** | No worklet, no contract violation. |
| `web/wasm/nkido_wasm.cpp` | **Modified** | Add `cedar_set_block_table`, `cedar_apply_state_inits_from_buffer`, `akkado_resolve_sample_ids_from_buffer`, `cedar_apply_midi_sources_from_buffer`. Change `akkado_patch_sample_ids_in_bytecode` to take a mappings-buffer argument. Delete `cedar_apply_state_inits`, `cedar_apply_midi_sources`, `akkado_resolve_sample_ids` (no callers after this PRD). |
| `web/wasm/CMakeLists.txt` | **Modified** | Update `NKIDO_EXPORTED_FUNCTIONS` list (add 4 new, remove 3 old). |
| `web/static/worklet/cedar-processor.js` | **Modified** | Delete entire `'compile'` message handler + `extract*` helpers. Replace `'loadCompiledProgram'` with `'loadProgram'` taking pre-packed `{bytecode, stateInitsBuf, midiSourcesBuf, blockTable, mainInstCount}`. Keep a single per-`process_block` SlotBusy retry; drop today's multi-attempt `pendingLoadRetry` state machine. Add a top-of-file comment block stating the worklet-thread contract. |
| `web/src/lib/audio/compile.worker.ts` | **New** | Owns compile WASM. Mirrors today's worklet `compile` handler, plus pack steps for stateInitsBuf, midiSourcesBuf, blockTable. Returns one big payload. Emits `{type:'ready'}` after WASM init. |
| `akkado/include/akkado/state_init_buffer.hpp` | **New** | C++ wire-format codec (pack + unpack co-located), compiled into both WASM builds. Replaced the planned TS packer — see §5.4. |
| `akkado/include/akkado/state_init_buffer.hpp` | **New** | The wire-format codec — pack **and** unpack co-located in C++ (`Writer`/`Reader`, `pack_*` / `apply_*` / `resolve_*`). Compiled into both the worker and worklet WASM. `static_assert` size pins for the memcpy'd structs. This replaced the planned TS packer (see §5.4). |
| `web/src/lib/audio/state-init-codec.ts` | **New, auto-generated** | Wire-format **validator** (decoder + size constants), NOT a packer. Generated by `web/scripts/build-state-init-codec.ts` from the C++ headers (see §5.4). Don't hand-edit. |
| `web/scripts/build-state-init-codec.ts` | **New** | Validator-codec generator. Run via `bun run build:state-init-codec` (standalone, like `build:opcodes`). |
| `web/tests/state-init-codec.test.ts` | **New** | Cross-check: loads real `nkido.wasm`, packs via C++, decodes with the generated TS validator, asserts exact-byte walk. |
| `web/src/lib/stores/audio.svelte.ts` | **Modified** | Spawn worker in `initialize()` with boot-time queueing (§4.3). Replace `compile()` body: post `compile` to worker (with generation tag), await `compileResult`, drop if superseded, then run existing sample-load pipeline, then post `loadProgram` with all four buffers. Surface worker error/death as a compile diagnostic; respawn lazily. |
| `web/e2e/hot-swap-audio.spec.ts` | **Unchanged** | Same test, will go green. |
| `akkado/tests/test_hot_swap_event_transforms.cpp` | **Unchanged** | CLI runtime test unaffected. |
| `CLAUDE.md` | **Modified** | Add a "Worklet thread contract" section. |

---

## 7. File-Level Changes

### 7.1 Files to create

| File | Purpose |
|------|---------|
| `web/src/lib/audio/compile.worker.ts` | Web Worker entry point. Loads its own WASM instance. On WASM init, posts `{type:'ready'}`. Listens for `{type:'compile', gen, source}`. Calls `_akkado_compile`, extracts diagnostics + bytecode + state-init records + sample mappings + MIDI sources + block table, packs them via the §5 codecs, and posts back `{type:'compileResult', gen, success, ...payload}`. |
| `akkado/include/akkado/state_init_buffer.hpp` | The C++ wire-format codec: `pack_*` (worker side) + `apply_*` / `resolve_*` (worklet side) in one header, shared by both WASM builds. Replaced the planned TS packer (§5.4). |
| `web/src/lib/audio/state-init-codec.ts` | **Auto-generated.** Wire-format **validator** — a decoder + size constants (no packer). Generated by `build-state-init-codec.ts` from C++ headers (§5.4). Don't hand-edit. |
| `web/scripts/build-state-init-codec.ts` | Validator-codec generator. Parses `akkado/include/akkado/state_init_buffer.hpp` (magics, `WIRE_VERSION`, `RecordKind`, `sizeof` pins) and `akkado/include/akkado/codegen.hpp` (`StateInitData::Type`); emits the decoder + size constants into `state-init-codec.ts`. |
| `web/tests/state-init-codec.test.ts` | Vitest cross-check of the generated decoder against real C++-packed bytes from `nkido.wasm`. |

### 7.2 Files to modify

| File | Change |
|------|--------|
| `web/wasm/nkido_wasm.cpp` | Add `cedar_set_block_table`, `cedar_apply_state_inits_from_buffer`, `akkado_resolve_sample_ids_from_buffer`, `cedar_apply_midi_sources_from_buffer`. Change `akkado_patch_sample_ids_in_bytecode` to take a mappings buffer arg. Delete `cedar_apply_state_inits`, `cedar_apply_midi_sources`, `akkado_resolve_sample_ids` (no callers). |
| `web/wasm/CMakeLists.txt` | Update `NKIDO_EXPORTED_FUNCTIONS`: add the four new `*_from_buffer` / `_cedar_set_block_table` exports; remove the three deleted exports. |
| `web/package.json` | Add `build:state-init-codec` script (wraps `build-state-init-codec.ts`). Standalone, run on demand like the other `build:*` codegen scripts. |
| `web/static/worklet/cedar-processor.js` | Remove `case 'compile':`, the entire compile + extract helper stack (`extractStateInits`, `extractDiagnostics`, `extractParamDecls`, `extractVizDecls`, `extractBuiltinVarOverrides`, `getRequired*` helpers). Rename `loadCompiledProgram` → `loadProgram` and accept `{bytecode, stateInitsBuf, midiSourcesBuf, blockTable, mainInstCount}`. Keep a single per-`process_block` SlotBusy retry; drop today's multi-attempt `pendingLoadRetry` state machine. Add a top-of-file comment block stating the worklet-thread contract. |
| `web/src/lib/stores/audio.svelte.ts` | In `initialize()`: spawn the compile worker, load its WASM, await `ready`. Replace `compile()` body: queue-then-send to worker (with generation tag), await `compileResult`, drop if superseded, then run existing sample-load pipeline, then post `loadProgram` with all four buffers. Surface worker error/death as a compile diagnostic; respawn lazily. |
| `CLAUDE.md` (nkido project) | Add a "Web architecture: worklet thread contract" section: only `process_block` + `_cedar_set_block_table` + `_cedar_apply_*_from_buffer` + `_akkado_*_from_buffer` may run inside the worklet. Compile, parsing, codegen, fetch, decode — all forbidden on the worklet thread. Pointer to this PRD. |

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
3. **State-init codec** — *(as shipped)* implement pack + unpack together
   in C++ at `akkado/include/akkado/state_init_buffer.hpp` and expose the
   `_akkado_pack_*_buffer` + `*_from_buffer` exports. Move every
   `extract*` helper from the worklet into the worker. The generated TS
   `state-init-codec.ts` is a decoder-only validator, not the worker
   packer. **Verify**: `akkado/tests/test_state_init_buffer_codec.cpp`
   round-trips real compiles through pack→apply; `web/tests/state-init-codec.test.ts`
   decodes real C++-packed bytes with the generated TS validator.
4. **Worklet rewire** — delete the compile handler + extract helpers.
   Replace `loadCompiledProgram` with
   `loadProgram({bytecode, stateInitsBuf, midiSourcesBuf, blockTable, mainInstCount})`.
   Wire `_cedar_set_block_table` → `_akkado_patch_sample_ids_in_bytecode`
   (with mappings buffer) → `_cedar_load_program` →
   `_akkado_resolve_sample_ids_from_buffer` →
   `_cedar_apply_state_inits_from_buffer` →
   `_cedar_apply_midi_sources_from_buffer`. Keep a single per-block
   SlotBusy retry. **Verify**: app boots, simple patch plays; a patch
   using `foreach()` / `iter()` and one using `midi(...)` both play.
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

### 9.6.b Worker not yet ready, user hits Ctrl+Enter

`audioEngine.compile()` awaits a `workerReady` promise (§4.3). Pre-
ready compiles are queued in a single-slot supersede-by-newest queue:
only the latest pre-ready compile is dispatched once `ready` fires.
The user never loses the most recent source they typed even if they
press Ctrl+Enter mid-WASM-init.

### 9.7 Initial program load (first compile, no prior program)

Same path as hot-swap: worker compiles, main loads samples, worklet
calls `_cedar_load_program` (which sees no previous program, no
crossfade, direct install). State-init buffer is applied. No special
case.

### 9.8 Two `loadProgram` messages arrive between `process_block` ticks

Both compiles passed the supersede-by-newest filter (e.g. quick
edit, quick edit again). Main thread posts `loadProgram` A then B.
Worklet handles A → `_cedar_load_program` arms a swap. Worklet then
handles B → `_cedar_load_program` returns `SlotBusy` (both A/B slots
occupied because audio thread hasn't yet performed A's swap). Worklet
parks B's bytecode + buffers in `pendingProgram`; the next
`process_block` finishes A's swap and immediately calls the parked
load. Single-shot retry, no multi-attempt state machine.

### 9.9 State-init buffer is malformed (bug in packer)

`cedar_apply_state_inits_from_buffer` returns -1 and logs
`[CEDAR BUG]` to console. The new program loads with no state inits,
which is a known broken state (patterns won't play). User sees broken
audio. Better to surface loudly than silently swallow. Same handling
for `cedar_apply_midi_sources_from_buffer` and
`akkado_resolve_sample_ids_from_buffer` (each returns -1 on bad
magic / truncated buffer).

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

### 10.2 Wire-format round-trip tests *(as shipped)*

**C++ (primary guard):** `akkado/tests/test_state_init_buffer_codec.cpp`
(tag `[state_init_codec]`):
1. `akkado::compile()` a real patch → `CompileResult`.
2. `pack_state_inits()` into the §5 wire format.
3. `apply_state_inits()` it onto a fresh VM.
4. Asserts each state matches a control VM driven by the legacy
   per-`init_*` path; plus `pack_midi_sources` / `pack_block_table`
   round-trips, bad-magic rejection, and a bit-perfect hot-swap render
   of the user pad shape (`ratio < 1e-3`).

```
cmake --build build --target akkado_tests
./build/akkado/tests/akkado_tests "[state_init_codec]"
```

**TS (validator cross-check):** `web/tests/state-init-codec.test.ts`
loads the real `nkido.wasm`, packs via the C++ exports, and walks the
bytes with the generated decoder (`state-init-codec.ts`), asserting the
walk consumes the buffer exactly — catches any drift between the
generated TS layout and the C++ wire format.

```
cd web && bun run build:state-init-codec   # regen (must be a clean diff)
cd web && bun run test -- state-init-codec
```

### 10.3 E2E test (must go green)

```
cd web && bun run test:e2e -- --grep "unison-pad"
```

The acceptance criteria are **audio-gap-only** — `editor.evaluate()`
end-to-end latency is *not* a goal because the worker still spends
~110 ms in `_akkado_compile` (non-goal §3: not speeding up the
compiler). The gap goes away because compile no longer runs on the
worklet thread, not because compile got faster.

Expected after this PRD:
- `worstRun` of low-RMS samples **<2** (currently 12–14, ~280 ms).
- No `Output silent for 100 blocks` warning in console output.
- Test passes 5 of 5 consecutive runs (currently fails 3 of 3 with
  aggressive timing).
- Worklet `loadProgram` handler duration **<5 ms** median (measured
  via a worklet-side `console.timeEnd('loadProgram')` log captured
  in the test). This is the *true* metric the PRD bounds — what the
  worklet thread does per swap.

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
