> **Status: NOT STARTED — DRAFT** — Engine-side architecture for hosting N concurrent akkado programs (one VM each) on a shared clock/transport with host-mixed outputs; v1 ships single-program with every interface N-ready (2026-07-04).

# PRD: Multi-Program Host (N VMs, Shared Clock, Per-Program Hot-Swap)

**Date:** 2026-07-04

> **Cross-repo note:** `docs/research/studio-*.md` and `prd-studio-*.md`
> referenced below live in the **closed sibling repo `nkido-studio`**
> (`../nkido-studio/docs/…`), not in this open repo. This engine PRD stands
> on its own via the code evidence herein; the studio docs are motivating
> context only.

A pure **engine** PRD (cedar/akkado), serving any embedder — nkido studio,
the JUCE plugin (`prd-juce-plugin.md`), the Godot addon (shipped external),
an ESP32 port — with nkido studio as the motivating consumer. It contains
**no clip model, no GUI timeline concepts, no session grid** (owner
decision: "the code is the DAW", `docs/research/studio-overview.md:78-83`).

Binding owner decisions implemented here
(`docs/research/studio-overview.md:84,95-97`): **"one program is one track
or session"** — a session running multiple track-programs needs the engine
to host N programs — and **"multi-program sessions: start single, design
for N"** — v1 runs ONE program per session; this PRD defines the N-program
architecture and, as the actual v1 deliverable, the list of interfaces that
must be N-ready from day one so single-program v1 doesn't corner us (§6).

Siblings drafted in parallel: `prd-studio-foundation.md`,
`prd-studio-daw-core.md`, `prd-capture-recording.md`. Related engine PRDs:
`prd-bus-routing.md` (SHIPPED), `prd-crossfade-audio-fixes.md` (DONE,
Phases 2-3 deferred), `prd-pattern-transport.md` (SHIPPED),
`prd-host-extension-api.md` (NOT STARTED).

---

## 1. Executive Summary

Cedar today runs **one program per VM**, and every embedder instantiates
**exactly one VM**. Hot-swapping that program is glitch-free, but
all-or-nothing: recompiling any part of the code swaps and crossfades the
*whole* program. A session hosting N track-programs needs:

- **N independent VMs** — each with its own program, hot-swap
  slots/crossfade, StatePool, and arena — so recompiling track 3 never
  touches track 1's audio. Per-program hot-swap isolation is the entire
  payoff.
- **A shared clock/transport** — one bpm, sample position, and play state
  published to every VM at block boundaries so all patterns share one grid.
- **Host-mixed outputs** — each VM renders its own stereo block; the host
  sums them (summing location is an open question, §7.1).

The proposal is a thin engine layer, working name **`cedar::ProgramHost`**,
owning N `cedar::VM` instances plus the shared transport. The bare
`cedar::VM` API stays untouched — every existing single-program embedder
keeps working unchanged; `ProgramHost` is additive. v1 ships `ProgramHost`
at N=1 with all addressing, wire formats, and namespaces designed for N.

---

## 2. Current State (code evidence)

### 2.1 One VM = one program; nothing static blocks N instances

`cedar::VM` is a plain instantiable class, non-copyable but movable
(`cedar/include/cedar/vm/vm.hpp:51-55`); no global/static engine state
prevents multiple instances. But **all tooling assumes one**: the WASM host
holds a singleton (`static std::unique_ptr<cedar::VM> g_vm;`,
`web/wasm/nkido_wasm.cpp:38`, created at `:81`); the CLI audio engine
embeds one (`cedar::VM vm_;`, `tools/nkido/audio_engine.hpp:114`); the
render path creates one (`tools/nkido/main.cpp:375`). Every WASM export and
every `tools/nkido` helper (`tools/nkido/program_loader.hpp:18-56`) takes
the one VM with no program scope.

### 2.2 Hot-swap slots are per-VM (the "A/B" crossfade)

Each VM owns one `SwapController` (`cedar/include/cedar/vm/vm.hpp:714`)
holding a fixed `std::array<ProgramSlot, 3>`
(`cedar/include/cedar/vm/swap_controller.hpp:231`) — one Active, at most
one Fading (crossfade source), the rest Empty/Loading/Ready. Crossfade
dual-execution snapshots the StatePool and audio arena into per-VM shadows
(`shadow_state_pool_`, `shadow_audio_arena_`, `vm.hpp:739,762`). These
lifecycles are inherently per-program: a swap in one program must never
snapshot another's state.

### 2.3 Clock and transport are per-VM fields, not a shared service

Timing lives inside each VM's `ExecutionContext`: `sample_rate`, `bpm`,
`global_sample_counter`, `block_counter`, `beat_phase`
(`cedar/include/cedar/vm/context.hpp:51-63`), derived per block by
`update_timing()` (`context.hpp:65-70`). The host pokes it per VM:
`VM::set_sample_rate` / `VM::set_bpm` (`vm.hpp:142-143`); seek is per-VM
(`vm.hpp:129-136`). There is **no clock object** that could be shared —
two VMs today would free-run on private counters and skew the moment their
`set_bpm` calls landed on different block boundaries.

### 2.4 Buses are per-program (verified)

The shipped bus system (`docs/prd-bus-routing.md`, SHIPPED 2026-05-22) is a
**compiler rewrite inside one program**: `bus(N,…)` writes to per-bus
scratch buffers in the program's own buffer space, and a per-program
epilogue sums buses into bus 0, runs `mixer`/`master` closures, applies the
forced safety stage, then stores to the device sinks (prd-bus-routing §5.1,
§5.3). The VM/context was explicitly unchanged — "`output_left/right`
remain the device sinks; buses use ordinary arena scratch buffers"
(prd-bus-routing §10). **Bus indices are a program-local namespace** —
bus 3 in track A and bus 3 in track B are unrelated buffers.

### 2.5 Per-VM resource ownership (what N instances multiply)

| Resource | Owner | Evidence | N-scaling note |
|---|---|---|---|
| StatePool + shadow | per VM | `vm.hpp:733,739` | fixed table, `MAX_STATES = 512` (`cedar/include/cedar/dsp/constants.hpp:52`) |
| AudioArena + shadow | per VM | `vm.hpp:753,762` | 32 MB each by default (`cedar/include/cedar/vm/audio_arena.hpp:35-40`), ~64 MB/VM; overridable via `CEDAR_ARENA_SIZE` |
| BufferPool (slabs, lazy) | per VM | `vm.hpp:732`, `constants.hpp:34-47` | grows on demand up to `MAX_BUFFERS = 16384` |
| EnvMap (params) | per VM | `vm.hpp:740` | name-hash keyed, `MAX_ENV_PARAMS = 256` (`constants.hpp:70`) |
| SampleBank | per VM | `vm.hpp:741`; heap maps (`cedar/include/cedar/vm/sample_bank.hpp:192-193`) | naive N banks duplicate identical sample data N times |
| SoundFont / wavetable registries | per VM | `vm.hpp:743,746` | same duplication concern |
| MIDI queues | per state, per VM | `push_midi_event(state_id,…)`, `vm.hpp:537-561` | addressing has no program dimension |

### 2.6 Semantic-ID namespace is program-local by accident

State IDs are FNV-1a hashes of semantic paths
(`cedar/include/cedar/vm/state_pool.hpp:32-39`), e.g. `main/track1/osc`.
Two *different* programs will routinely produce **identical hashes** (both
have `main/osc`) — harmless today only because each VM has a private
StatePool. A merged pool would need per-program salting (the mechanism
exists: `StatePool::set_state_id_xor`, `state_pool.hpp:88`), but GC
touched-flags, fading pools, and crossfade shadows are per-program
lifecycles, which argues for keeping pools per-program (§5.3).

---

## 3. Problem

1. **Whole-program swap is the wrong granularity for a session.** With N
   tracks in one program (which the shipped bus system supports), editing
   one track recompiles and crossfades everything, and a compile error in
   track 3's code holds *all* tracks hostage on the last good program.
2. **No shared clock exists.** N VMs cannot currently be kept phase-locked;
   each free-runs its own `global_sample_counter` (§2.3).
3. **Host APIs have no program dimension.** Params, MIDI, sample-id
   resolution, state inspection, probes — every host-facing surface
   addresses "the VM". If v1 embedders ship on program-scopeless APIs,
   migrating later breaks session files, wire formats, and automation
   bindings — exactly the cornering the owner decision instructs us to
   avoid.

---

## 4. Goals / Non-Goals

### 4.1 Goals

- G1. Define `cedar::ProgramHost`: N program instances (each a `cedar::VM`),
  a shared transport clock, and a per-block render loop producing N stereo
  outputs for the host to mix.
- G2. **Per-program hot-swap isolation**: `load_program` on program *k*
  swaps/crossfades only program *k*; all other programs render
  bit-identical audio through the swap (tested, §12.3).
- G3. **Shared clock/transport**: one `{bpm, sample position, play state}`
  published to all programs at block boundaries; `seek` fans out; programs
  added mid-session join at the current position.
- G4. **The N-ready checklist (§6) lands in v1** even though v1 runs N=1.
- G5. Bare `cedar::VM` remains a supported, unchanged public surface for
  single-program embedders (ESP32, Godot addon, existing WASM/CLI paths).
- G6. Zero allocations in the audio path, per program and in the host
  render loop, preserved under N.

### 4.2 Non-Goals

- **No clip model, no GUI timeline, no session grid** (owner decision;
  app-layer concerns live in `prd-studio-daw-core.md`).
- **No cross-program signal routing in v1-of-N** — sends are Q3 (§7.2).
- **No plugin hosting, no PDC** — `prd-host-extension-api.md` and the
  planned plugin-node-latency PRD.
- **No capture/recording taps** — `prd-capture-recording.md`.
- **No per-program tempo (polytempo)** — one bpm; a program wanting a
  different feel uses `.slow()`/`.fast()` in code.
- **No akkado language changes; no in-program bus-system changes.**
- **No multi-program-aware compiler.** Each program compiles independently.

---

## 5. Proposed Architecture (for N)

```
                 HOST (embedder: studio / JUCE plugin / godot / CLI / ESP32)
   compile threads (programs compile + load independently)
        │ load_program(k, bytecode…)
   ┌────▼──────────────────────────────────────────────────────────────────┐
   │ cedar::ProgramHost                                                    │
   │                                                                       │
   │   ┌───────────────────────────────┐    shared assets (proposal §5.4)  │
   │   │ TransportState (host-owned)   │    ┌────────────────────────────┐ │
   │   │  bpm · sample_pos · playing   │    │ shared sample / SF2 / WT   │ │
   │   │  seek() · set_bpm() · play()  │    │ store  [OPEN QUESTION Q4]  │ │
   │   └──────────────┬────────────────┘    └────────────────────────────┘ │
   │                  │ published to every program at block boundary       │
   │   ┌──────────────▼┐  ┌───────────────┐       ┌───────────────┐        │
   │   │ Program 0     │  │ Program 1     │  ...  │ Program N-1   │        │
   │   │ (cedar::VM)   │  │ (cedar::VM)   │       │ (cedar::VM)   │        │
   │   │  swap slots + │  │               │       │               │        │
   │   │  crossfade    │  │  each: own    │       │               │        │
   │   │  StatePool    │  │  program,     │       │               │        │
   │   │  AudioArena   │  │  own hot-swap,│       │               │        │
   │   │  EnvMap       │  │  own state    │       │               │        │
   │   │  buses+safety │  │               │       │               │        │
   │   └────L ─ R──────┘  └────L ─ R──────┘       └────L ─ R──────┘        │
   │         └────────────────┴───────────┬───────────────┘                │
   │                              host mixer: Σ (out_k × gain_k)           │
   │                              [summing location OPEN QUESTION Q1]      │
   └──────────────────────────────────────┼───────────────────────────────┘
                                          ▼
                              device / plugin output bus
```

### 5.1 Per-program: everything that already exists, times N

Each program instance is a full `cedar::VM`: its own triple-buffer
`SwapController` (each program keeps its own "A/B" crossfade slots exactly
as today), StatePool + shadow, AudioArena + shadow, BufferPool, EnvMap,
MIDI queues. Recompiling program *k* exercises only program *k*'s swap
machinery; the other N-1 programs' pools and arenas are never read,
snapshotted, or GC'd by it. **Per-track hot-swap isolation comes for free**
— it already exists at the VM boundary; the work is hosting N of them.

### 5.2 Shared clock/transport

A host-owned `TransportState { double bpm; std::uint64_t sample_pos; bool
playing; float sample_rate; }` is the single source of truth. At the top of
each host block, `ProgramHost` publishes it to every VM before calling
`process_block` — the same values a lock-stepped VM would compute itself,
so N=1 behavior is bit-identical to a bare VM (verified, §12.2).
bpm/sample-rate changes apply to all programs at the same block boundary
(no cross-program skew); `seek(beat)` fans out to `VM::seek` (`vm.hpp:129`)
with one shared `SeekConfig`; a program added mid-session is loaded, then
seeked to the current transport position before its first audible block, so
its patterns land on the session grid (reusing `seek`'s deterministic-state
reconstruction). The bare VM keeps owning its clock when used standalone;
`ProgramHost` is the writer of record when present. No `ExecutionContext`
restructuring is required for v1 (§9 Phase 1 makes the seam explicit).

### 5.3 StatePool: per-program (decided)

Shared-pool designs were rejected: semantic-ID hashes collide across
programs by construction (§2.6); GC and crossfade shadow snapshots are
per-program lifecycles, and sharing would make program *k*'s crossfade
snapshot cost proportional to *all* programs' state; `MAX_STATES = 512` is
a per-program budget and a shared pool would let one program's state
pressure evict another's. Per-program pools keep isolation structural
instead of policed. No host API may assume a state ID is unique beyond
`(program_id, state_id)`.

### 5.4 Sample/asset banks: shared store proposed [OPEN QUESTION Q4]

N private `SampleBank`s duplicate identical sample data per program — 8
tracks using the same drum kit would hold 8 copies. Proposal: a host-owned,
read-only shared sample store (refcounted immutable buffers) with
per-program name→id resolution views, so each program's compiled sample IDs
stay program-local. SoundFont/wavetable registries follow the same pattern
(the wavetable registry's per-block `shared_ptr` pinning, `vm.hpp:746-752`,
is already the right shape). Whether v1-of-N ships the shared store or
accepts N private banks first is Q4 — but the sample-id resolution **wire
format carries program scope either way** (§6 row 6).

### 5.5 Rendering: serial loop first, parallel later [OPEN QUESTION Q2]

v1-of-N renders serially: one audio thread iterates programs and mixes.
Sessions are "one program per track", so N is small (order 8-32), against a
2.67 ms budget per 128-sample block (`constants.hpp:17`). Parallel
per-program rendering (VMs are share-nothing, so programs are
embarrassingly parallel; CLAUDE.md's perf notes already mention
cpp-taskflow) is deferred until profiling shows the serial loop failing a
realistic session — worker-wakeup latency inside 2.67 ms is not obviously a
win.

### 5.6 Summing and the master stage [OPEN QUESTIONS Q1, Q5]

Each program's output is already post-safety (per-program bus-0 hard rail
±1.0, prd-bus-routing §4.3), but the *sum* of N safe programs is not safe.
Where the session master lives is Q1 (§7.1); regardless of the outcome, the
host mix point needs at least a final NaN-guard + rail (Q5).

---

## 6. The N-Ready Checklist (what single-program v1 must ship)

Interfaces/formats that must be N-ready from day one. Each row is cheap now
and a breaking migration later. "v1" = the single-program `ProgramHost`
plus every *new* host-facing surface added by the studio/plugin work.

| # | Surface | N-ready requirement for v1 |
|---|---|---|
| 1 | **Host API shape** | Program-scoped from day one: `ProgramHost` hands out a program handle; `load_program`, `set_param`, `push_midi_event`, asset loading, state inspection, and probe queries take it. v1 has exactly one handle (id 0). New WASM/bridge RPC entry points carry a `program` field (existing scopeless exports stay as single-program compat, §8.3). |
| 2 | **Clock ownership** | Transport writes (`set_bpm`, `set_sample_rate`, play/stop, seek) go through the host object, never per-program calls, in every new embedder. Bare-VM setters remain for standalone use, documented as single-program mode. |
| 3 | **Semantic-ID namespaces** | All host-side keying of engine state (param rebind maps, automation bindings, session recall, probe subscriptions) uses `(program_id, state_id)` / `(program_id, semantic_path)`, never bare hashes. The JUCE plugin's 64-macro-slot rebind map (prd-juce-plugin §6.6) reserves the program field in its serialized form. |
| 4 | **Param namespaces** | `param("cutoff")` in two programs are two distinct params (per-program EnvMap). Host-facing addressing = `(program_id, name)`; session/preset formats key param values per program. No global-uniqueness assumption on names. |
| 5 | **Bus namespaces** | Buses stay **per-program** (§2.4); indices are program-local, never merged. Anything host-side referencing a bus (stem taps in `prd-capture-recording.md`, mixer strips in `prd-studio-daw-core.md`) addresses `(program_id, bus_index)`. A session "master" is a host concept, not bus 0 of some program (pending Q1). |
| 6 | **Sample-bank sharing** | Sample IDs are program-local. The sample-id resolution wire format (`akkado_resolve_sample_ids_from_buffer` and the studio's native equivalent) carries program scope so a shared store (§5.4) slots in without format changes. Asset-loading APIs take the program handle. |
| 7 | **MIDI routing** | MIDI event addressing = `(program_id, state_id)`. The host owns a device→program routing table; the engine never assumes "all MIDI goes to the VM". The midi-sources wire format reserves the program field. |
| 8 | **Render/offline paths** | `nkido render` and any offline-bounce API are structured as "render a ProgramHost", not "render a VM", so N-program bounce and per-program stems stay API-compatible. |

Anti-checklist (deliberately **not** in v1): no N>1 execution, no shared
sample store implementation (format only), no parallel rendering, no
cross-program sends, no host mixer semantics.

---

## 7. Open Architecture Questions (presented, not decided)

### 7.1 Q1 — Where does summing live?

From `docs/research/studio-architecture-strategy.md` (open questions):

**(a) Host-side float mixing.** `ProgramHost` (or the embedder) sums N
stereo outputs with per-program gain plus a final NaN-guard/rail; master FX
are the host's problem. *Pros:* trivial, allocation-free, keeps the
engine's embeddable-synth identity (~N×256 adds per block — noise).
*Cons:* a live-codeable session-wide master chain needs a second mechanism;
per-program gains live outside the language.

**(b) Engine-level "VM group" with a master program.** The mix of programs
0..N-2 feeds a designated master program (via the shipped
`set_input_buffers` path, `vm.hpp:106`, or a dedicated mix-in) whose akkado
code hosts sends/master FX; its bus-0 safety stage becomes the session
guard. *Pros:* the master chain is live-codeable akkado — maximally
on-brand; one safety story. *Cons:* a privileged program breaks N-symmetry;
master-program recompile touches all audio (mitigated by its own
crossfade); ordering questions once cross-program sends exist.

No decision. Note (a) does not preclude (b): (b) is expressible *on top of*
(a) by wiring the mix into one more program's input — a reason to ship (a)
first and let the studio prototype (b) without engine changes.

### 7.2 Q3 — Cross-program routing

Are cross-program sends needed, or is per-program stereo out + host mixing
enough for v1-of-N? In-track sends already work (shipped buses); the first
real casualty is a cross-track sidechain ("duck the pads from the kick
track"). When it bites: host-side routing of one program's output into
another's `set_input_buffers` (one-block delay, no engine change) vs
engine-level cross-program buses (real design work: ordering, cycles,
hot-swap). The N-ready checklist does not depend on the answer.

### 7.3 Remaining open questions (introduced inline)

- **Q2 — serial vs parallel rendering**: §5.5. Serial first; parallel
  (cpp-taskflow or a fork-join pool) only after profiling a realistic
  session shows the serial loop missing the 2.67 ms budget.
- **Q4 — shared vs per-program sample banks**: §5.4. Recommendation: shared
  immutable store with per-program views; open whether it ships with
  v1-of-N or later (8 tracks × one 200 MB kit is the forcing function).
- **Q5 — host-level safety stage**: per-program bus-0 rails don't bound the
  sum (§5.6). Mandatory engine rail at the mix point (symmetric with
  prd-bus-routing §4.3) vs embedder responsibility; interacts with Q1(b),
  where the master program's own safety stage would make it redundant.
- **Q6 — program add/remove audibility**: hard cut (host ramps gain) vs
  engine-provided fade-in/out reusing the crossfade machinery. Cheap either
  way; decides where the "no pops" guarantee lives.

---

## 8. Relation to Existing Systems

### 8.1 A/B crossfade slots

Unchanged and multiplied: each program keeps its own `SwapController`
triple-buffer and crossfade state (§2.2, §5.1); the fixes and deferred
phases of `prd-crossfade-audio-fixes.md` apply per program. Nothing about
crossfading spans programs — a swap in one program is inaudible in others
by construction (and by test, §12.3).

### 8.2 Bus system

Per-program today, stays per-program (§2.4). The bus epilogue — default
softclip, `mixer`/`master` closures, forced safety stage — runs inside each
program; `ProgramHost` consumes each program's post-bus-0 stereo output.
The bus system is *why* per-program stereo out is a plausible v1-of-N
answer to Q3.

### 8.3 Existing single-program embedder APIs (compat/migration)

**`cedar::VM` public API: no changes.** WASM exports, `tools/nkido`, the
Godot addon, and the JUCE plugin plan all keep working against the bare VM;
`ProgramHost` is additive. Migration for an embedder wanting N: replace its
one `cedar::VM` with a `ProgramHost`, route transport writes through it,
add the program handle to its own RPC/wire surfaces (day-one for the
studio; optional/deferred for the plugin). The four existing worklet
apply-path buffers stay as-is for the single-program web IDE; new consumers
(studio native bridge) use the program-scoped variants from the start
(§6 rows 1, 6, 7).

### 8.4 Memory-integrity harness

The four-legged harness (`docs/prd-memory-integrity-tests.md`,
`scripts/memory/run_all.sh`) is the verification backbone for every phase:
explosion guard, sanitizer suite, zero-alloc trap, drift fuzz. The drift
fuzz gains an N-program variant in Phase 2 (§12.4). Budgets live in
`scripts/memory/budgets.sh`; N programs legitimately multiply the RSS
envelope (~64 MB of arenas per VM by default, §2.5) — any budget raise
ships with the phase that causes it, with rationale, never silently.

---

## 9. Implementation Phases

Each phase ends green on: both unit suites, `./scripts/memory/run_all.sh`,
and the phase-specific verification. Sequence-driven audio tests follow the
≥300 s rule (§12.1).

### Phase 1 — Interface refactor, zero behavior change (v1 ships here)

- Introduce `cedar::ProgramHost` (new `cedar/include/cedar/host/`) wrapping
  exactly one VM: program handle type, `load_program(handle, …)`,
  `set_param(handle, …)`, `push_midi_event(handle, …)`,
  asset-load-with-handle, and the `TransportState` publish seam (§5.2).
  With one program, `ProgramHost::process_block` delegates to the VM
  bit-exactly. Document bare-VM transport setters as single-program mode.
- **Verification:** offline renders of the full fixture corpus through
  `ProgramHost(N=1)` byte-identical to bare-VM renders (including across
  hot-swaps, seeks, bpm changes); all existing tests green untouched;
  memory harness green with unchanged budgets.

### Phase 2 — N programs, shared clock, serial render, host mix

- `ProgramHost` holds N VMs (init-time capacity, no audio-path allocation);
  serial per-block loop; per-program gain; host mix with a final
  NaN-guard/rail (interim answer to Q5). Shared transport publish; fan-out
  seek; join-at-position for programs added mid-session. Program-scoped
  addressing live end-to-end for params/MIDI/assets.
- **Verification:** clock lock-step test (§12.6); hot-swap isolation test
  (§12.3); zero-alloc trap armed around the whole host loop (§12.5);
  N-program drift fuzz (§12.4) with an explicitly justified budget update.

### Phase 3 — Program lifecycle + shared-asset format

- Add/remove programs at runtime with the Q6-decided audibility policy;
  teardown of a removed program off the audio thread. Program-scoped
  sample-id resolution wire format implemented (shared store itself still
  optional per Q4).
- **Verification:** add/remove soak ≥300 s with continuous audio, asserting
  no discontinuities (windowed RMS check) on surviving programs; sanitizer
  suite over the lifecycle tests; memory harness.

### Phase 4 — Performance + deferred decisions

- Profile realistic sessions (N ∈ {4, 8, 16, 32}); decide Q2 on data;
  prototype Q1(b) as host-side wiring (§7.1) with the studio; revisit
  Q3/Q4 under real session pressure.
- **Verification:** per-block render-time histograms under the 2.67 ms
  budget at target N on reference hardware; regression gate on the serial
  path; memory legs at deep iteration counts (`scripts/check-release.sh`
  tier).

---

## 10. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| `cedar::VM` and `cedar/include/cedar/vm/*` | **Stays** | No public API change; ProgramHost composes VMs. |
| `cedar/include/cedar/host/` (`ProgramHost`, `TransportState`) | **New** | Phase 1. |
| Bus system / akkado compiler | **Stays** | Program-local, untouched (§8.2). |
| `tools/nkido` | **Modified (Phases 1-2)** | Render/serve paths restructured over ProgramHost (§6 row 8); behavior identical at N=1. |
| `web/wasm/nkido_wasm.cpp` + worklet | **Stays (v1)** | Single-program compat surface; new exports carry program scope. |
| Godot addon, ESP32-class embedders | **Stays** | Bare-VM path untouched; `CEDAR_ARENA_SIZE` trims footprint. |
| JUCE plugin (prd-juce-plugin) | **Constraint only** | Serialized rebind map reserves program field (§6 row 3). |
| Studio PRDs (foundation / daw-core / capture) | **Consumers** | Address everything by `(program_id, …)` per §6. |
| `scripts/memory/budgets.sh` | **Modified (Phase 2)** | N-program envelope, justified in-commit (§8.4). |
| Session/project formats (studio-side) | **Constraint only** | Per-program keying from v1 (§6 rows 3-4). |

---

## 11. Edge Cases

| Situation | Expected behavior |
|-----------|-------------------|
| Compile error in program *k* | Program *k* keeps its last good program playing (existing per-VM behavior); other programs entirely unaffected. |
| Rapid recompiles of one program | Existing supersede-Ready-slot path (`swap_controller.hpp:46-57`) per program; other programs unaffected. |
| Two programs' semantic paths hash-collide | By design harmless: per-program StatePools (§5.3); collision only matters if a future shared pool skips salting. |
| bpm / sample-rate change mid-session | Applied to all programs at the same block boundary via TransportState publish; no cross-program skew. |
| Seek while N programs play | Fan-out `VM::seek` with one `SeekConfig`; pre-roll (if configured) runs per program before audible output resumes. |
| Program added mid-session | Load + seek-to-transport-position before first audible block; patterns land on the session grid. |
| Program removed while sounding | Q6 policy (engine fade vs host ramp); teardown never on the audio thread. |
| All programs silent / none loaded | Host mix outputs silence; per-VM early-exit when no program loaded is unchanged. |
| Sum of N safe programs exceeds ±1.0 | Host mix point rail (interim Q5 answer) bounds the device signal; per-program rails still bound each program. |
| One program exhausts its arena | Per-program failure: that program's `arena_exhaustion_count` rises (`vm.hpp:587`); other programs' arenas are independent. |
| MIDI device event with no routing entry | Dropped at the host routing table with a host-visible counter; never broadcast to all programs implicitly. |
| Embedder on bare VM (no ProgramHost) | Fully supported forever; single-program mode documented (§6 row 2). |
| N × default arenas exceed small-target RAM | Embedder builds set `CEDAR_ARENA_SIZE` / caps N; budgets doc the default envelope (§8.4). |

---

## 12. Testing / Verification

1. **≥300 s rule**: every test driving sequences/patterns/poly across
   programs simulates at least 300 seconds; failures report the failing
   block/time and are never fixed by shortening the run.
2. **N=1 equivalence (Phase 1 gate):** fixture corpus rendered via
   `ProgramHost(N=1)` vs bare VM must be byte-identical, including across
   hot-swaps, seeks, and bpm changes.
3. **Hot-swap isolation (the headline test):** run programs A and B ≥300 s;
   recompile/hot-swap B on a schedule (including structural changes and
   compile errors); assert A's captured output is byte-identical to a
   control run where B never swaps.
4. **N-program drift fuzz:** extend `akkado_tests "[drift_fuzz]"` to mutate
   and re-swap one randomly chosen program of N per iteration in a live
   ProgramHost; bounded peak RSS + linear-slope RSS per the harness
   contract.
5. **Zero-alloc trap:** `[zero_alloc]`-style trap armed around
   `ProgramHost::process_block`, covering the publish/mix loop and all N
   VMs.
6. **Clock lock-step:** identical programs on N handles stay sample-aligned
   over ≥300 s through bpm changes, seeks, and stop/start (cross-correlate
   outputs; any nonzero lag fails).
7. **Sanitizer leg:** `cmake --preset sanitize` + ctest over all new host
   tests, especially Phase 3 lifecycle paths.
8. **Safety at the sum:** adversarial programs (each railing ±1.0) through
   the host mix must never deliver |sample| > 1.0 or non-finite values to
   the device.

---

## 13. Open Questions

| # | Question | Where discussed |
|---|---|---|
| Q1 | Summing location: host-side float mixing vs engine-level "VM group" with a master program hosting sends/master FX | §7.1 |
| Q2 | One audio thread iterating programs vs parallel per-program rendering (cpp-taskflow) | §5.5, §7.3 |
| Q3 | Cross-program sends, or per-program stereo out + host mixing suffices for v1-of-N | §7.2 |
| Q4 | Sample-bank sharing: shared immutable store with per-program views vs N private banks (and when) | §5.4, §7.3 |
| Q5 | Host-level safety stage: mandatory engine rail at the mix point vs embedder responsibility | §5.6, §7.3 |
| Q6 | Program add/remove audibility: engine-provided fades vs host gain ramps | §7.3 |
