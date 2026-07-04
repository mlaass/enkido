> **Status: NOT STARTED — DRAFT** — Engine-side capture surface: a stable per-bus host tap API first, a live-codeable `record()` opcode later; capture UX belongs to prd-studio-daw-core (2026-07-04).

# PRD: Capture & Recording (Host Tap Surface + `record()` Opcode)

**Date:** 2026-07-04

> **Cross-repo note:** `studio-overview.md` / `studio-audio-midi-io.md` /
> `studio-architecture-strategy.md` (research) and `prd-studio-daw-core.md`
> referenced below live in the **closed sibling repo `nkido-studio`**
> (`../nkido-studio/docs/…`), not in this open repo. This engine PRD stands
> on its own via the code evidence herein; the studio docs are motivating
> context only.

## Executive Summary

The studio research sweep identified zero-config recording of jams as the
community's loudest unmet need, and the owner decided **"both, app-side
first"** ([studio-overview §Owner decisions](research/studio-overview.md)):
app-side retrospective capture + per-bus stem taps ship in studio v1, a
live-codeable `record()` engine opcode follows. Stems key on **buses**
(SHIPPED — [prd-bus-routing](prd-bus-routing.md)).

This PRD is the **engine side only**, serving any embedder:

- **Phase A — host tap surface.** After every `process_block` the engine
  already holds exactly the per-bus stems a DAW wants in its bus scratch
  buffers — but hosts cannot find them: no bus metadata is exported and
  buffer indices change on every recompile. Phase A exports a per-program
  **bus table**, publishes it with the program slot so reads always match
  the program that rendered the block, and documents the read contract
  (including crossfade semantics). Small, additive engine change.
- **Phase B — `record()` opcode (later).** A pass-through signal builtin
  (`in() |> record("take1")`) copying its input into a pre-allocated
  lock-free SPSC ring, drained by a host-provided disk thread. Zero
  allocations on the audio path (the zero-alloc trap stays green).
  Because `record` is a pass-through, not a sink, it composes into
  `mixer` closures — bus recording for free.

Appendix A gives the non-normative host-side JUCE `ThreadedWriter` drain
pattern (>4 GB / RF64 flagged for a spike). Out of scope: everything the
user touches — arming, retrospective buffer length, take/file management,
and the session log (eval/param capture) are
[prd-studio-daw-core](prd-studio-daw-core.md) (same 2026-07-04 sweep).
MIDI/event capture is **not** audio capture — the session log owns it.

---

## 1. Current State (code evidence)

### 1.1 Per-bus stems already exist inside the VM — invisibly

Bus routing shipped 2026-05-22 ([prd-bus-routing](prd-bus-routing.md)):
every `out()`/`bus(N,…)`/`<>` writer emits an `OUTPUT` instruction; with
`InstructionFlag::BUS_WRITE`
(`cedar/include/cedar/vm/instruction.hpp:338-342`) set, `op_output`
accumulates into an adjacent scratch pair
`(inst.out_buffer, inst.out_buffer + 1)` instead of the device sinks
(`cedar/include/cedar/opcodes/utility.hpp:48-82`). `emit_bus_epilogue`
(`akkado/src/codegen.cpp:3089`) allocates one adjacent stereo pair per
referenced bus index (`codegen.cpp:3164-3180`), inlines each bus's `mixer`
closure **in place** on its pair, sums non-zero buses into bus 0, runs the
master chain + forced ±1.0 clamp in place on bus 0's pair, then accumulates
bus 0 into `ctx.output_left/right` (`codegen.cpp:3222-3292`); a prologue
clears every bus pair each block (`codegen.cpp:3294-3310`).

So **after `process_block` returns, bus N's pair holds that bus's
post-`mixer` stem and bus 0's pair holds the post-master, post-safety
master** — precisely the stems the studio mixer wants. Three things make
them unreachable for a host today:

1. **No bus enumeration.** `CompileResult`
   (`akkado/include/akkado/akkado.hpp:48-92`) exports state inits, required
   assets, param/viz decls, `required_buffers` — nothing about buses. The
   index→buffer `bus_left` map is a local inside `emit_bus_epilogue`
   (`codegen.cpp:3164`) and is discarded.
2. **Buffer indices are per-compile.** Bus pairs are allocated after the
   main DAG, so bus 1's slot moves whenever the program above it changes.
3. **No defined read contract.** `VM::buffers()` is documented "for
   testing/debugging" (`cedar/include/cedar/vm/vm.hpp:577-579`); nothing
   specifies when a read is coherent relative to swaps and crossfades.

Bus **identity**, in contrast, is already stable: the index is a
compile-time integer literal (`E260`, prd-bus-routing §2.2), so "bus 1" is
the same stem across arbitrary edits. `mixer` closures get a stable
semantic-ID path (`push_path("mixer#" + N)`, `codegen.cpp:2991`).

### 1.2 The master is already host-owned

`VM::process_block(float* output_left, float* output_right)`
(`vm.hpp:100`, `cedar/src/vm/vm.cpp:158`) writes into host pointers; the
offline render loop already captures them (`tools/nkido/main.cpp:429-470`).
The master tap needs **zero** engine change; only per-bus taps do.

### 1.3 Crossfade makes naive bus reads subtly wrong

During a hot-swap crossfade, `perform_crossfade` (`vm.cpp:282`) executes
**both** programs each block — state pool and audio arena snapshotted and
rolled back between runs, the **new program last**. The device output is
the equal-power mix, but the bus scratch buffers end the block holding only
the **new** program's un-faded values. A tap API must define this (§4.1.3,
OPEN QUESTION 2).

### 1.4 Capture-adjacent machinery (precedents, not solutions)

- **Probes** (`op_probe`, `utility.hpp:381-395`; `ProbeState` 1024-sample
  ring, `cedar/include/cedar/opcodes/dsp_state.hpp:1596-1622`): right
  shape, wrong guarantees — tiny, overwrite-on-full, polled lossily. Fine
  for scopes, unusable for gapless capture.
- **`MidiQueueState`** — the strongest Phase B precedent: arena-allocated
  SPSC ring created on the **host thread** at init, reused across
  hot-swaps (`vm.hpp:368-474`, ring alloc 427-443), release/acquire
  producer (`push_midi_event`, vm.hpp:537-561), drop-and-count overflow,
  and a drain API with a documented thread contract
  (`drain_pending_file_ccs`, vm.hpp:488-499). `record()` is this pattern
  with producer/consumer roles reversed.
- **Audio input** ([prd-audio-input](prd-audio-input.md), IMPLEMENTED):
  `in()` delivers live input via `ctx.input_left/right`
  (`cedar/include/cedar/vm/context.hpp:36-40`). Its non-goals say it
  plainly: "Recording input to disk" does not exist.
- **Offline render**: `nkido render` bounces the master to 16-bit PCM WAV
  (`write_wav_16`, `tools/nkido/main.cpp:326-361`; `handle_render_mode`,
  `main.cpp:364-505`). Master-only, no stems — a natural Phase A
  verification vehicle (`--stems`).

In one line: the master is host-owned, per-bus stems exist but are
unreachable (no enumeration, no index table, no read contract), and there
is no audio→host PCM ring anywhere — only the host→audio `MidiQueueState`
to model one on.

---

## 2. Goals and Non-Goals

### Goals

**Phase A — host tap surface**
- Compiler exports a **bus table** per program: every allocated bus index
  with its left buffer slot and provenance (writer count, has-mixer).
- The VM publishes the table **with the program slot**, so bus reads after
  `process_block` always match the program that rendered the block; a
  documented read API (`bus_count()` / per-bus L,R pointers) with defined
  crossfade semantics. Zero engine-side audio-path change.
- `nkido render --stems` as in-repo proof-of-consumer and test vehicle.

**Phase B — `record()` opcode**
- `record(signal, "name")` pass-through builtin: audible chain unchanged,
  input copied into a pre-allocated SPSC ring; host drain API on a
  non-audio disk thread; drop-and-count on overflow; never blocks or
  allocates on the audio path.
- Recording identity = semantic-ID `state_id` + name; survives hot-swap by
  standard state rebind (continuation policy: OPEN QUESTION 1). Start/stop
  edges (call site appears/disappears, gate) surfaced for file rotation.

### Non-Goals

- **Capture UX** — arming, retrospective buffer length, take naming, punch,
  file management: [prd-studio-daw-core](prd-studio-daw-core.md).
- **File I/O inside the engine.** Cedar never opens files; hosts own
  encoders, formats, paths. Engine hands out float32 PCM at VM rate.
- **MIDI/event/eval capture.** The daw-core session log owns it. PCM only.
- **Cross-VM stems.** Per-program buses only; multi-VM summing/capture is
  [prd-multi-program-host](prd-multi-program-host.md) (same sweep).
- **Retrospective buffering in-engine.** The "last N minutes" ring is
  app-side, fed by Phase A taps. Likewise recording raw device input
  channels is host-side ([studio-audio-midi-io §7](research/studio-audio-midi-io.md)).

---

## 3. Target Syntax (Phase B)

`record` is a **pass-through signal builtin** — it returns its input
unchanged, so it drops into any chain without changing the sound:

```akkado
// Record the live input while monitoring it through FX
in() |> record("take1") |> lp(@, 2000) |> out(@)

// Record a synth line dry, listen wet
n"c4 e4 g4" as e |> saw(e.freq) |> record("synth_dry") |> reverb(@) <>

// Gate-controlled: start/stop from a UI toggle (or any signal edge)
rec = toggle("rec", false)
in() |> record("loop_a", gate: rec) |> out(@)
```

Not being a sink, `record` is legal inside `mixer`/`master` closures
(`E261` rejects only `out`/`bus`/`mixer`/`master`), so bus and master
recording need no new routing surface:

```akkado
kick  <>(1)
snare <>(1)
mixer(1, (s) -> s |> comp(@, -8, 6) |> record("drum_bus"))  // post-FX stem

master((s) -> s |> softclip(@, 0.9) |> record("master"))    // pre-safety master
```

Semantics: `name` is a compile-time string literal keying the recording
(part of the call's semantic identity). Optional `gate:` (default 1 =
recording while the call exists): rising edge = start, falling = stop,
edges reported through the drain stream so the host can rotate files. Mono
in records mono, stereo records interleaved stereo — channel count fixed at
compile time and reported in metadata. Duplicate names: W-warning
(coerce-don't-fail), second call gets a de-duplicated identity.

---

## 4. Architecture

### 4.1 Phase A — host tap surface

#### 4.1.1 Compiler: bus table export

`emit_bus_epilogue` already computes everything needed; Phase A stops
discarding it. `CompileResult` gains:

```cpp
struct BusDecl {
    std::uint16_t index;         // bus number (0 = master), stable identity
    std::uint16_t left_buffer;   // BufferPool slot of L; R is left_buffer + 1
    std::uint16_t writer_count;  // # of out()/bus()/<> writers targeting it
    bool          has_mixer;     // mixer(N,…)/master(…) closure attached
};
std::vector<BusDecl> bus_decls;  // ascending by index; empty if no sinks
```

Note the existing skip: a program with no sinks emits no epilogue and no
bus buffers (`codegen.cpp:3126-3133`) — `bus_decls` is empty and hosts must
treat "no buses" as valid.

#### 4.1.2 VM: bus table published with the program slot

Mirroring the staged block-table pattern (`set_block_table`,
`vm.hpp:77-78`): the host stages the table before `load_program*`, the load
copies it into the `ProgramSlot`, queries read the **current** slot's
table:

```cpp
void set_bus_table(std::span<const BusEntry> buses);   // staged, host thread
std::uint32_t bus_count() const;                        // current slot's table
// Audio thread, valid from process_block() return until the next
// process_block()/reset(). False if out of range / no program.
bool bus_output(std::uint32_t i, const float** L, const float** R,
                std::uint16_t* bus_index) const;
```

Because the table lives in the slot, a block rendered by the old program
reads the old table and the first new-program block reads the new one — no
host-side swap bookkeeping, no torn reads.

#### 4.1.3 Read contract (normative)

- **Thread**: audio thread only, between `process_block` return and the
  next audio-path call — the same contract as `drain_pending_file_ccs`
  (`vm.hpp:482-486`). Host cost: one memcpy per bus (8 buses = 8 KB per
  2.67 ms block, negligible).
- **What you get**: bus N (N > 0) = post-`mixer` stem; bus 0 = post-master,
  post-safety signal — byte-identical to that block's contribution to the
  device output **when no crossfade is active**. During a crossfade
  (3 blocks default) the device output is the equal-power mix while
  `bus_output` reflects the **new program only**, unfaded (§1.3).
  Normative guidance: hosts capture the master-as-heard from their own
  `process_block` out-pointers (always correct) and use `bus_output` for
  stems, accepting a ≤ 8 ms hard cut at swap boundaries. Crossfade-aware
  stems: OPEN QUESTION 2.

Host callback shape: fill `input_left/right` → `vm.process_block(L, R)` →
memcpy `L,R` into the master FIFO → for each `i < bus_count()`,
`bus_output(i, …)` and memcpy into stem FIFO `i` — the disk/retrospective
threads drain those FIFOs off the audio thread (Appendix A).

#### 4.1.4 Stem identity across edits

The bus **index** is the identity: a compile-time literal, unaffected by
unrelated edits; the studio keys stems and mixer strips on it.
`BusDecl.left_buffer` is per-compile plumbing, never identity. Bus display
names have no engine surface today — OPEN QUESTION 3.

### 4.2 Phase B — `record()` ring + host drain

```text
 audio thread                         │            host
──────────────────────────────────────┼─────────────────────────────────────
 op_record (pass-through):            │  disk thread (host-owned):
   out[i] = in[i]                     │    loop:
   ring write (SPSC producer,         │      n = vm.drain_recording(id, cb)
     release write_pos; on full:      │      cb(chunk*, frames, flags)
     ++overflow_count, drop)          │        → ThreadedWriter / retro ring
                                      │      sleep / condition wait
 RecordState (per call site):         │
   float* ring        (audio arena,   │  init (compile thread, pre-publish):
     allocated at init — host thread) │    for r in required_recordings:
   atomic write_pos / read_pos        │      vm.init_record_state(r.state_id,
   channels, name_hash, gate edges    │        r.channels, ring_frames)
```

Design points, each anchored to an existing precedent:

- **State + ring pre-allocated off the audio thread.** `CompileResult`
  gains `required_recordings` (state_id, name, channels, gate?) analogous
  to `required_midi_sources`; the host calls `init_record_state(...)`
  before publishing bytecode, exactly as `init_midi_queue_state` allocates
  its ring from the audio arena on the host thread (`vm.hpp:427-443`). The
  opcode body performs no allocation — the zero-alloc trap
  (`cedar/tests/zero_alloc_hooks.cpp`, `[zero_alloc]`) must stay green
  with `record()` in the program.
- **Strict SPSC.** Producer = `op_record` on the audio thread
  (release-store `write_pos`); consumer = one host drain thread
  (acquire-load, advances `read_pos`). Same memory-order discipline as
  `push_midi_event` (`vm.hpp:544-560`), roles reversed. Overflow =
  drop-newest + counter, never block; the host polls fill level and
  surfaces "disk can't keep up".
- **Hot-swap.** The semantic-ID path gives a stable `state_id`; on
  recompile `init_record_state` finds existing arena-backed storage and
  reuses the ring (`MidiQueueState` reuse pattern, `vm.hpp:426-427`), so
  undrained audio is never lost. Continue-into-same-file vs rotate is host
  policy on an engine-reported swap edge — OPEN QUESTION 1. (Unlike
  `MidiQueueState`, `RecordState` holds no `std::string` — only a name
  hash — so state-pool copies during crossfade stay allocation-free.)
- **Crossfade correctness for free.** The audio arena is snapshotted and
  rolled back between the old and new program runs (`vm.cpp:282-345`), so
  the ring receives exactly one program's writes per block (the new one).
  Caveat: during the ≤ 3 crossfade blocks `record()` captures the new
  program's un-faded signal, not the mix the listener hears (§4.1.3).

#### 4.2.1 File naming / rotation semantics

The engine defines identity and edges; the host maps them to files.
Identity: `(state_id, name)`; drained chunks carry start/stop/overflow
flags and the start's `global_sample_counter`, so takes are
sample-accurately placeable. Start edge: call site first appears or `gate:`
rises. Stop edge: gate falls, call site disappears on recompile (state GC'd
after the standard fade window), or `reset()`. Recommended host policy
(non-normative): `<name>.wav`, rotating `<name>-002.wav` … per stop/start
pair, timestamp suffix on collision. The studio's actual policy lives in
prd-studio-daw-core.

---

## 5. Impact Assessment

| Area | Phase A | Phase B |
|---|---|---|
| `akkado/src/codegen.cpp` | Export `bus_decls` from `emit_bus_epilogue` (data already computed) | `record` handler; `required_recordings`; dup-name warning |
| `akkado.hpp` / `builtins.hpp` | `BusDecl` in `CompileResult` | `record` BuiltinInfo (pass-through, `requires_state`) |
| `cedar` VM (`vm.hpp`/`vm.cpp`) | `set_bus_table` / `bus_count` / `bus_output`; `BusEntry` in `ProgramSlot` | `init_record_state`, `drain_recording`, fill/overflow queries |
| `cedar/include/cedar/opcodes/` | none | `RECORD` opcode + `RecordState` (arena ring) |
| `tools/nkido` | `render --stems`; serve-mode bus listing | drain thread writing float32 WAV in play/render |
| `web/` | none (worklet contract untouched) | drain path design — OPEN QUESTION 7 |
| Memory | none (buffers already allocated) | ring per call site from the 128 MB arena; `scripts/memory/budgets.sh` entry — OPEN QUESTION 6 |
| Audio-path cost | zero engine-side; host memcpys | one block memcpy per `record()` call |
| Risk | low — additive metadata + read-only accessors | medium — new SPSC ring, hot-swap edges, budget discipline |

Hot-swap, seek, and crossfade shadow-state machinery are untouched by
Phase A; Phase B rides the existing arena-snapshot semantics (§4.2).

---

## 6. Implementation Phases

### Phase A — host tap surface

1. **A1 — compiler export.** `BusDecl` vector populated in
   `emit_bus_epilogue`. Tests: single-bus, multi-bus, mixer-only bus
   (writer_count 0), sink-less program (empty), `bypass_master` (empty).
2. **A2 — VM bus table.** `set_bus_table` staged like `set_block_table`;
   table stored per `ProgramSlot`; `bus_count`/`bus_output` accessors.
   Tests: values match `out()` accumulation byte-exactly when idle; table
   flips atomically with the swap; crossfade blocks return new-program
   buffers.
3. **A3 — CLI consumer + docs.** `nkido render --stems` writes
   `out.bus0.wav`, `out.bus1.wav`, … alongside the master; read contract
   documented in `docs/cedar-architecture.md`.

**Verification:** all existing tests green; ≥ 300 s simulated audio through
`render --stems` on a multi-bus pattern program, plus a hot-swap loop test
(recompile every few seconds for the full window) asserting bus reads never
fault and stems stay finite; `[zero_alloc]` leg and
`./scripts/memory/run_all.sh` green.

### Phase B — `record()` opcode

1. **B1 — opcode + state.** `RECORD` opcode, `RecordState` (arena ring,
   SPSC atomics, gate edges, overflow counter), `record` builtin,
   `required_recordings`, `init_record_state`.
2. **B2 — drain + CLI proof.** `drain_recording(state_id, cb)` +
   fill/overflow queries; `nkido play`/`render` grow a background drain
   thread writing float32 WAV; gate + hot-swap edge reporting.
3. **B3 — hardening.** `budgets.sh` entry; drift-fuzz seeds including
   `record()`; builtin reference docs.

**Verification:** ≥ 300 s simulated audio recording a pattern-driven
program, asserting (a) drained PCM equals the pass-through output
sample-for-sample outside crossfade windows, (b) zero overflow at a
realistic drain rate, (c) a deliberate slow-drain run shows drop-and-count,
never a stall. `[zero_alloc]` armed across blocks containing `record()`;
hot-swap loop with a live recording — ring reused, no leaks (sanitizer
leg), bounded RSS (drift leg).

---

## 7. Edge Cases

| Case | Behavior |
|---|---|
| **Disk full / slow disk backpressure** | Host concern: writer fails or lags, host stops/slows draining; the seconds-sized ring absorbs bursts, sustained deficit → drop-newest + `overflow_count`, host surfaces "disk can't keep up". Engine never blocks or errors on the audio path. |
| **Hot-swap mid-recording** | Same call site: state rebinds by semantic ID, ring reused, undrained audio preserved; swap edge reported (OPEN QUESTION 1). Call removed: stop edge; host finalizes after draining. |
| **Bus disappears on recompile** | New program's bus table lacks the index; host sees it missing at the swap block and finalizes that stem file. Reappearing later = same identity (index). |
| **Crossfade window** | Master-as-heard: capture from `process_block` out-pointers. Bus stems + `record()`: new program only, un-faded, ≤ 3 blocks (§4.1.3, §4.2). |
| **Program with no sinks** | No epilogue, no bus buffers (`codegen.cpp:3126-3133`); `bus_count() == 0`; hosts must not assume bus 0 exists. |
| **Seek during recording** (`vm.seek`) | PCM keeps flowing; chunks carry sample positions, so the host sees the discontinuity and can split the take. |
| **Sample-rate change** | Host re-init anyway; recordings stop (stop edge), files finalized at the old rate. |
| **Duplicate `record` names** | W-warning; identities de-duplicated (§3). |
| **`record()` inside `poly`/`each`** | v1: reject with a clear diagnostic (per-voice rings under XOR isolation are real work) — revisit on demand. |

---

## 8. Testing / Verification

- **Unit (akkado)**: `bus_decls` matrix (§6 A1); `record` arg validation;
  duplicate-name warning; record-in-poly rejection.
- **Unit (cedar)**: slot-table atomicity across swap; `RecordState` SPSC
  stress test; overflow counting; gate edges.
- **Null test**: with no crossfade active, bus 0's pair must be
  byte-identical to the block's contribution to `ctx.output_left/right` —
  a strict invariant tying the tap to what shipped to the device.
- **Long-run rule**: every sequencing/recording test simulates ≥ 300 s
  (CLAUDE.md experiments rule); failures report block/time; durations are
  never shortened to pass.
- **Memory legs**: `[zero_alloc]` armed over blocks with buses +
  `record()`; sanitize preset over the new state/ring code; drift-fuzz
  seeds extended with `record()`+bus programs; explosion-guard corpus
  gains a many-bus fixture.
- **End-to-end**: `render --stems` golden files; CLI record-to-WAV
  compared sample-exact against a same-seed `render` of the tapped point.

---

## 9. Open Questions

1. **[OPEN QUESTION] Hot-swap continuation policy for `record()`.** A
   surviving call site keeps its ring — should the default host policy
   write on into the same file (a live set is one performance) or rotate a
   take per swap? Engine reports the edge either way. Owner call.
2. **[OPEN QUESTION] Crossfade-aware stems.** Are hard-cut stems during
   the ≤ 3-block crossfade acceptable (master stays correct via
   out-pointers), or should the engine dual-render and crossfade bus pairs
   for taps? Measure audibility before adding machinery.
3. **[OPEN QUESTION] Bus display names.** Stems key on indices; do we want
   `mixer(1, closure, {name: "drums"})` via record-as-options, a
   standalone declaration, or host-side labels only? Language-surface
   change — needs its own design pass with the owner.
4. **[OPEN QUESTION] >4 GB files (host-side).** Verify JUCE
   `WavAudioFormat` behavior past 4 GB in a spike (RF64? silent
   corruption?) and pick RF64/W64/auto-split — carried from
   [studio-audio-midi-io Q8](research/studio-audio-midi-io.md); at
   8ch × float32 × 48 kHz ≈ 5.5 GB/hour, not hypothetical.
5. **[OPEN QUESTION] Xrun telemetry correlation.** Should the engine
   expose a sample-stamped swap/crossfade event log so hosts can correlate
   device xruns (`getXRunCount()`) with hot-swap events
   ([studio-audio-midi-io Q10](research/studio-audio-midi-io.md))? Cheap
   and diagnostic; rides Phase A or waits for daw-core telemetry.
6. **[OPEN QUESTION] Ring sizing & budget.** Default seconds-per-ring and
   max concurrent recordings against the 128 MB arena — needs a
   `budgets.sh` entry with rationale. Strawman: 4 s/ring (≈ 1.5 MB
   stereo), 16 recordings ≈ 24 MB worst case.
7. **[OPEN QUESTION] Web host drain path.** The worklet may not do disk or
   heavy work (CLAUDE.md worklet contract); draining a WASM-side ring
   likely means SharedArrayBuffer reads from the main thread or a worker +
   OPFS. Design when the web host wants `record()`; not a native blocker.
8. **[OPEN QUESTION] Pre-master master tap.** Bus 0's pair is post-master
   post-clamp; mix-later workflows may want the pre-chain sum so re-summed
   stems reconstruct it. Extra tap, or is `record()` placement inside
   `master(...)` the answer?
9. **[OPEN QUESTION] Drain framing.** Fixed block-multiple chunks vs
   arbitrary reads; whether stop edges flush partial blocks with explicit
   frame counts. Decide at B1 design review — drain ABI only.

---

## Appendix A — Host-side writer reference pattern (non-normative)

The studio (and any JUCE host) drains Phase A taps and Phase B rings with
the canonical JUCE pattern
([studio-audio-midi-io §7](research/studio-audio-midi-io.md)):
**`AudioFormatWriter::ThreadedWriter`** wraps a format writer with a
background `TimeSliceThread` + FIFO — the audio callback only memcpys.
Create/destroy the writer **off** the audio thread; publish via atomic
pointer; tear down as null-the-atomic → flush → delete. FIFO sized ≥
several seconds at channel-count × rate; overflow is drop — monitor fill
level and warn instead of silently losing audio. Format: 32-bit-float WAV
(long sets exceed 4 GB → RF64/W64 or auto-split pending the OPEN QUESTION 4
spike); FLAC for archival. Retrospective capture is a host-side circular
buffer fed from the same per-bus memcpys — "keep that" freezes the ring and
replays it through a ThreadedWriter; the engine surface it needs is exactly
Phase A. Overdub placement compensation:
`getInputLatencyInSamples() + getOutputLatencyInSamples()`.

## References

[studio-overview](research/studio-overview.md) (binding owner decisions) ·
[studio-audio-midi-io §7](research/studio-audio-midi-io.md) ·
[studio-architecture-strategy](research/studio-architecture-strategy.md)
(recording-tap discussion) · [prd-bus-routing](prd-bus-routing.md) (SHIPPED)
· [prd-audio-input](prd-audio-input.md) (IMPLEMENTED; capture-to-disk out
of scope there) · [prd-studio-daw-core](prd-studio-daw-core.md) (capture
UX + session log, same sweep) ·
[prd-multi-program-host](prd-multi-program-host.md) (cross-VM buses, same
sweep) · [prd-juce-plugin](prd-juce-plugin.md) (host-core context).
