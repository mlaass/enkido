> **Status: NOT STARTED — DRAFT** — Per-node latency declaration + report-only PDC for cedar; branch alignment sketched for a later revision (2026-07-04).

# PRD: Plugin & Node Latency (Report-Only PDC for Cedar)

**Date:** 2026-07-04

> **Cross-repo note:** `docs/research/studio-*.md` and
> `prd-studio-plugin-hosting.md` referenced below live in the **closed
> sibling repo `nkido-studio`** (`../nkido-studio/docs/…`), not in this open
> repo. This engine PRD stands on its own via the code evidence herein; the
> studio docs are motivating context only.

---

## 1. Executive Summary

Cedar has no concept of processing latency. Every node in the DAG is
implicitly zero-latency: an instruction reads its input buffers and writes its
output buffer within the same 128-sample block, and nothing in the
instruction format, the state pool, or the program metadata can say
otherwise. That assumption breaks the moment a node genuinely delays its
signal:

- **Hosted third-party plugins** (`prd-studio-plugin-hosting.md`, drafted in
  parallel; motivating consumer, fast-follow v1.x of nkido studio). A
  linear-phase EQ or lookahead limiter plugin reports hundreds to thousands
  of samples of latency via `getLatencySamples()` / CLAP `latency`. Mounted
  as a cedar DAG node, that delay is real but invisible to the engine.
- **Native opcodes, today and tomorrow.** `DYNAMICS_LIMITER` *already*
  delays its signal path when `lookahead > 0` — the output sample is read
  from the lookahead ring, i.e. the audio is late by `lookahead_samples`
  (`cedar/include/cedar/opcodes/dynamics.hpp:107,140-146,159-172`) — and
  reports it nowhere. Future FFT / linear-phase processing (an `IFFT`
  opcode slot is already reserved, `instruction.hpp:205`) will add more.
- **Host opcodes.** `prd-host-extension-api.md` lets embedders register
  arbitrary DSP behind a `HOST_OP` dispatch table — third-party native DSP
  enters cedar there, with the same unreported-latency problem.

Two consequences follow. First, **parallel branches mis-align**: a dry path
summed with a latent path comb-filters by the latency delta. Second,
**embedders cannot report PDC**: a JUCE wrapper needs a total-graph figure
for `setLatencySamples()` (`prd-juce-plugin.md` §6.4 already reports its
block-adapter ≤127 samples — graph latency must compose with it), and nkido
studio needs per-node figures for its mixer/stem UI.

This PRD adds, in Phase 1, a **report-only** latency system: per-node
declarations, a DAG aggregation pass (sum along chains, max across joins), a
total-graph query for hosts, and per-node surfacing next to the inspection
JSON. **Nothing is compensated in Phase 1** — per the research decision
(`docs/research/studio-plugin-hosting.md` §4.3), live coders mostly chain
serially, and a correct report is the prerequisite for everything else.
Phase 2 (automatic branch alignment, latency budgets, `pdc: off`) is sketched
only and deliberately [OPEN QUESTION]-heavy.

---

## 2. Current State (code evidence)

**No latency concept exists anywhere in the engine.** A case-insensitive
search for `latency` across `cedar/`, `akkado/`, `tools/`, and `web/wasm/`
returns zero hits in non-test code (verified 2026-07-04).

- **Instruction format has no room and no field.** `cedar::Instruction` is a
  fixed 20-byte record — `[opcode:8][rate:8][out:16][in0..4:16][flags:16]
  [state_id:32]`, `static_assert(sizeof == 20)`
  (`cedar/include/cedar/vm/instruction.hpp:373-380,407`) — and the 16-bit
  `flags` field is claimed up to bit 13 (`instruction.hpp:335-353`).
  Latency cannot live in the instruction.
- **The "DAG" is implicit; there is no explicit topo-sort pass.** Codegen is
  a single depth-first AST visit (`visit(ast.root)`,
  `akkado/src/codegen.cpp:173`; dispatch at `:257`) emitting a linear
  instruction stream. Emission order *is* execution order and is
  topological by construction: input buffers are always produced by earlier
  instructions, and edges exist only as buffer indices (producer's
  `out_buffer` = consumer's `inputs[k]`). An aggregation pass can therefore
  be one linear sweep over the bytecode.
- **Program metadata carries no per-node facts beyond state IDs.**
  `ProgramSlot` holds instructions, a subprogram `BlockEntry` table, and the
  `state_ids` set; `ProgramSignature` hashes semantic IDs and raw
  instruction bytes
  (`cedar/include/cedar/vm/program_slot.hpp:13-17,54-88,148-204`). No side
  table exists for an embedder to query per node.
- **A host can learn nothing per-node except DSP state.** The only per-node
  introspection surface is `StatePool::inspect_state_json(state_id)`
  (`cedar/include/cedar/vm/state_pool.hpp:410`; exposed via
  `web/wasm/nkido_wasm.cpp:1643`) — oscillator phase, filter memory, no
  timing metadata.
- **Builtin metadata has no latency field.** `BuiltinInfo` declares opcode,
  arity, defaults, channel shape, stereo-nativeness
  (`akkado/include/akkado/builtins.hpp:124-165`) — nothing temporal.
- **Precedents this design reuses:** compiler → VM side-band metadata via
  the `StateInitData` list (`akkado/include/akkado/codegen.hpp:140-185`,
  wire-packed in `state_init_buffer.hpp:143`, applied off the audio thread);
  staged auxiliary tables at load (`VM::set_block_table`,
  `cedar/include/cedar/vm/vm.hpp:77`); arena-allocated delay memory
  (`DelayState::ensure_buffer`,
  `cedar/include/cedar/opcodes/dsp_state.hpp:152-197` — the pattern any
  Phase-2 compensation delay must follow); hot-swap crossfade on structural
  change, skipped for byte-identical programs (`cedar/src/vm/vm.cpp:351-363`,
  `crossfade_state.hpp:11-21`); bus joins — every program ends in a bus
  epilogue summing non-zero buses into bus 0 (`akkado/src/codegen.cpp:181`),
  and `OUTPUT` accumulates, optionally into a bus scratch pair via
  `BUS_WRITE` (`instruction.hpp:339-342`).

---

## 3. Goals / Non-Goals

### 3.1 Goals (Phase 1)

- **G1 — Declaration.** Any node can declare integer latency in samples:
  native opcodes statically (compiler-known), hosted plugins and host
  opcodes dynamically (embedder-reported per semantic ID, off audio thread).
- **G2 — Aggregation.** Per program, cedar derives per-node cumulative
  latency, per-join skew, per-bus and total-graph latency — sum along
  chains, max across joins.
- **G3 — Host query.** One call gives an embedder the total in samples
  (→ `setLatencySamples()`, composed with the block adapter's ≤127,
  `prd-juce-plugin.md` §6.4) plus a JSON report for UI.
- **G4 — Inspection surfacing.** Per-node latency visible in the debug UI,
  keyed by `state_id` to join the existing `inspect_state_json` surface.
- **G5 — Hot-swap correctness.** Latency changes on recompile (or a plugin's
  dynamic change) republish the report with zero audio-thread work.
- **G6 — Fix the existing gap.** `limiter(..., lookahead: N)` with a
  constant lookahead declares its latency.

### 3.2 Non-Goals (Phase 1)

- **No compensation.** Branch skew is reported, not fixed (Phase 2).
- **No fractional latency.** Integer samples only.
- **No per-voice latency.** Subprogram bodies (poly/foreach/BLOCK_CALL) get
  a conservative policy, not per-body aggregation (§8.4).
- **No UI work beyond the JSON surface** (studio/web UI PRDs consume it).
- **No tail-length concept.** Tails (`getTailLengthSeconds()`) are a hosting
  concern (`prd-studio-plugin-hosting.md`), orthogonal to latency.

---

## 4. Design — Phase 1 (report-only)

### 4.1 Data model: three declaration sources, one table

A node's latency is keyed by its **`state_id`** (the semantic hash every
stateful instruction carries, `instruction.hpp:379`). Every latency-capable
node is stateful (delay memory is state), and `state_id` gives
hot-swap-stable identity for free.

**Source A — static, compiler-known.** `BuiltinInfo` gains
`latency_samples : std::uint32_t = 0` (for parameter-dependent cases like
the limiter, codegen computes the value from a constant-folded argument —
`lookahead_ms * sr / 1000`). Codegen collects non-zero entries into a new
`CodeGenResult::latency_decls` vector of `LatencyDecl{state_id, samples}` —
same shape and lifecycle as `state_inits`
(`akkado/include/akkado/akkado.hpp:60`), wire-packed for the web worker path
(`state_init_buffer.hpp` precedent).

**Source B — host opcodes.** The `HostOpRegistry` of
`prd-host-extension-api.md` gains an optional per-index
`latency_samples` (static) declared at `register_host_opcode(...)` time.
The loader folds these into the same table when it encounters `HOST_OP`
instructions. (Dynamic per-instance host-op latency goes through Source C.)

**Source C — runtime, embedder-reported.** New VM call, any non-audio
thread:

```
VM::set_node_latency(std::uint32_t state_id, std::uint32_t samples)
```

Used by nkido studio's `HostedPluginNode` after
`prepareToPlay(48000, 128)` → `getLatencySamples()`, and again whenever the
plugin signals `ChangeDetails::latencyChanged` (see §5.1). Source C
overrides A/B for the same `state_id`.

The merged result is a fixed-capacity **`LatencyTable`**
(`std::array<LatencyEntry{state_id, samples}, MAX_STATES>` — `MAX_STATES` =
512, `cedar/include/cedar/dsp/constants.hpp:50-52`) living next to the
`ProgramSlot`, not inside it: declarations survive across swaps keyed by
semantic ID, exactly like DSP state in the `StatePool`.

### 4.2 Aggregation algorithm

Run **off the audio thread**, at program load and after every
`set_node_latency`. One linear sweep over the main instruction stream
(`[0, main_count)`, `program_slot.hpp:67-70`), which is already in
topological order (§2):

```
L[b] : latency of the value currently in buffer index b   (init: all 0)
total_bus[k], total : 0

for each instruction inst in emission order:
    own  = table.lookup(inst.state_id)          // 0 if undeclared
    in_max = max over wired inputs k of L[inst.inputs[k]]
             (0xFFFF slots skipped; STEREO_INPUT also reads inputs[0]+1)
    skew  = in_max - min over wired inputs      // >0 ⇒ join skew, record it
    L[inst.out_buffer] = in_max + own
    if STEREO_OUTPUT: L[inst.out_buffer + 1] = same
    special cases:
      DELAY_TAP:  L[out] = 0        // musical delay, not latency (§8.1)
      OUTPUT:     bus/device sink joins: total_bus[k] = max(total_bus[k], L[in0])
per-node report entry: {state_id, own, cumulative = L[out]}
total = max over buses (bus epilogue sums buses into bus 0 — a join)
```

Buffer indices are reused across a program (`required_buffers` is a peak
count, `akkado/include/akkado/codegen.hpp:369`); because the sweep runs in
execution order, `L[b]` always describes the value currently in `b`, so
reuse is handled with no extra bookkeeping.

All wired inputs are treated uniformly — a latent *control* input (e.g. an
envelope follower driving a cutoff) conservatively propagates its latency to
the audio path. Over-reporting is the safe direction; refinement is
deferred (§10 Q7).

### 4.3 Worked example: parallel branches

```akkado
saw(110) as src
src |> lp(@, 800)              as dry_arm    // 0 samples
src |> plugin("LinPhaseEQ")    as wet_arm    // 512 samples (hosted, Source C)
dry_arm + wet_arm |> limiter(@, -1, 100, lookahead: 5) |> out(@)
                                             // 5 ms @ 48k = 240 samples (Source A)
```

Instruction-level sweep (buffer indices illustrative):

```
idx  opcode           out  in        own   L[out]   note
 0   OSC_SAW          b2   b0(freq)    0      0
 1   FILTER_SVF_LP    b3   b2,b1       0      0     dry arm
 2   HOST_OP(eq)      b4   b2        512    512     wet arm
 3   ADD              b5   b3,b4       0    512     JOIN: skew = 512-0 = 512
 4   DYNAMICS_LIMITER b6   b5,...    240    752     chain: sum
 5   OUTPUT           —    b6          0      —     total = max(total, 752)
```

Phase-1 report: per-node `{lp:0, eq:512, limiter:240}`, one join with
512-sample skew at the `ADD` (this is the comb filter the user hears), bus 0
total **752 samples (15.7 ms)**. A JUCE embedder reports
`setLatencySamples(752 + adapter_latency)`. Phase 2 would insert a
512-sample compensation delay on the `dry_arm` edge into the `ADD`.

### 4.4 Query surface

- `VM::total_latency_samples() → uint32_t` — lock-free read of a
  double-buffered report published by the aggregation pass.
- `VM::latency_report_json() → std::string` — off-audio-thread; JSON:
  `{ total, buses: [{bus, samples}], nodes: [{state_id, own, cumulative}],
  joins: [{instruction_index, skew}] }`.
- WASM export `cedar_get_latency_report()` following the
  `inspect_state_json` precedent (`web/wasm/nkido_wasm.cpp:1643`); runs in
  the compile worker / main thread, **never** the worklet (not on the
  worklet's allowed-call list).
- Inspection: the debug UI joins `nodes[]` to its per-state view by
  `state_id`. `inspect_state_json` itself is unchanged — latency is program
  metadata, not DSP state, so it ships as a sibling report rather than
  being spliced into `StatePool`'s serializer.

---

## 5. Interactions

### 5.1 Hot-swap

- **Recompile changes latency** (code edit adds/removes a latent node): the
  new program's aggregation runs at load; the report swaps with the program.
  Nothing new — `content_hash` differs, normal structural swap + crossfade
  (`vm.cpp:351-363`).
- **Dynamic latency change, identical bytecode** (hosted plugin fires
  `latencyChanged` while code is untouched): per the research decision,
  handled as a hot-swap *administratively* — `set_node_latency()` re-runs
  aggregation and republishes the report — but with **no crossfade in
  Phase 1**, because no audio structure changed (report-only). The
  byte-identical-skip logic (`vm.cpp:358-363`) is untouched. In Phase 2 a
  latency change alters compensation delays → different instructions → the
  existing structural-swap machinery handles it with no special casing —
  the payoff of compiling compensation into bytecode rather than patching
  the VM.
- `ProgramSignature` does **not** fold in the latency table (it hashes
  instruction bytes only, `program_slot.hpp:148-204`); the report carries
  its own generation counter so UIs can detect staleness.

### 5.2 Zero-alloc audio path

Phase 1 adds zero audio-thread work: declarations, aggregation, and JSON run
off-thread; the audio thread at most reads the double-buffered total. Phase
2's compensation delays must allocate ring buffers from the `AudioArena` at
load/first-touch exactly like `DelayState::ensure_buffer`
(`dsp_state.hpp:174-197`) and stay inside the `[zero_alloc]` harness leg's
rules. (Hosted-plugin `processBlock` allocation exemptions belong to
`prd-studio-plugin-hosting.md`.)

### 5.3 Fixed 128-sample blocks

Latency is declared and reported in **samples**, never rounded to block
multiples — a 240-sample lookahead reports 240. Only the embedder-side
block adapter adds its own ≤127-sample figure on top (`prd-juce-plugin.md`
§6.4). Phase-2 compensation likewise operates at sample granularity via
delay lines; nothing about `BLOCK_SIZE = 128` (`dsp/constants.hpp:14-17`)
quantizes it.

### 5.4 Buses

`OUTPUT` accumulates into device sinks or numbered buses (`BUS_WRITE`,
`instruction.hpp:339-342`), and the bus epilogue sums buses into bus 0
(`codegen.cpp:181`). Both are joins: per-bus totals are the max over
contributors; the graph total is the max over buses. Per-bus figures are
reported individually because nkido studio's stem recording/mixer keys on
buses (`docs/research/studio-overview.md`) and needs them to align exported
stems.

---

## 6. Design — Phase 2 (sketched only)

Automatic branch alignment: at each join with non-zero skew, codegen inserts
a compensation delay of `(max sibling latency − own branch latency)` samples
on each shorter input edge, materialized as ordinary delay instructions with
arena-backed state so hot-swap/crossfade/zero-alloc all work unchanged.

- **[OPEN QUESTION]** Insertion site: akkado codegen emitting real
  instructions (favored — signature/crossfade fall out for free, and the
  instructions are inspectable) vs. a VM-side implicit delay on join reads
  (no bytecode growth, but invents a second, invisible execution mechanism).
- **[OPEN QUESTION]** Latency budget: default threshold (samples or ms?)
  above which a W-class warning fires ("this plugin adds 4096 samples to the
  master path") — consistent with coerce-don't-fail; proposed default in the
  10–20 ms range, undecided.
- **[OPEN QUESTION]** Escape hatch syntax: per-node `plugin("X", pdc: off)`
  kwarg vs. a program-level directive; interaction with the option-record
  convention (`OptionSchema`).
- **[OPEN QUESTION]** Compensation memory ceiling: worst case is
  (join count × max skew) samples of arena; does it need its own budget in
  `scripts/memory/budgets.sh`?
- **[OPEN QUESTION]** UI surfacing: where do per-node/per-join figures
  appear (editor gutter? mixer strip? pattern debug panel?) — owned by the
  studio / web UI PRDs, but the JSON contract may need extending.

---

## 7. Impact Assessment

| Area | Change | Risk |
|---|---|---|
| `akkado/include/akkado/builtins.hpp` | `BuiltinInfo::latency_samples` field (default 0) | Low — additive; ~zero entries initially besides limiter |
| `akkado/src/codegen*.cpp` | Collect `LatencyDecl`s; constant-fold limiter lookahead | Low |
| `akkado/include/akkado/akkado.hpp` | `CodeGenResult::latency_decls` + wire packing | Low — mirrors `state_inits` |
| `cedar/include/cedar/vm/vm.hpp` / `vm.cpp` | `LatencyTable`, `set_node_latency`, aggregation pass, queries | Medium — new subsystem, but fully off-audio-thread |
| `web/wasm/nkido_wasm.cpp` | `cedar_get_latency_report()` export | Low |
| `prd-host-extension-api.md` (when implemented) | optional `latency_samples` at host-op registration | Low — amendment noted there |
| Audio thread | none in Phase 1 | — |
| Bytecode format / `Instruction` | **unchanged** (20 bytes stands) | — |

Backward compatibility: programs with no declarations aggregate to 0
everywhere; embedders that never call the queries see no behavior change.

---

## 8. Edge Cases

### 8.1 Latency in feedback loops

Cedar bytecode is forward-only (control flow never rewinds,
`instruction.hpp:213-216`); audio feedback exists solely through
`DELAY_TAP` / `DELAY_WRITE` pairs coordinated via state, with no buffer edge
from write back to tap (`instruction.hpp:89-90`, `dsp_state.hpp:165-170`).
Two rules:

- A delay line is **musical time, not latency**: `DELAY_TAP` output latency
  is defined as 0 and propagation stops there.
- A latent node **inside** a tap/write feedback body accrues its latency on
  every loop pass — mathematically uncompensatable. Phase 1: report it (the
  write-side cumulative figure makes it visible) and W-warn when codegen
  sees a declared-latency builtin inside a `tap_delay` closure body. Phase 2
  must **never** insert compensation inside a feedback body; proposed
  policy is warn + clamp to 0 across the loop boundary. Final policy:
  §10 Q5.

### 8.2 Latency changing at hot-swap

Covered in §5.1. One addition: a hosted plugin node's semantic ID survives
the swap (instance pooling, `prd-studio-plugin-hosting.md`), so its
Source-C declaration survives too — moving the node in code keeps its
latency without re-reporting. A recompile that *removes* the node leaves a
stale `LatencyTable` entry; entries are GC'd alongside `StatePool` slots
when the program no longer references the `state_id`.

### 8.3 Latency exceeding block-size multiples (or sane bounds)

Nothing special at non-multiples of 128 (§5.3). Upper bound: declarations
clamp to `MAX_NODE_LATENCY` (proposed 2^20 samples ≈ 21.8 s @ 48 kHz —
above any real plugin, near the arena's compensation ability, cf.
`DelayState::MAX_DELAY_SAMPLES = 960000`, `dsp_state.hpp:154`) with a
W-warning on clamp.

### 8.4 Subprogram bodies (poly / foreach / shared fns)

Bodies live after the main stream and are dispatched per-voice/per-call
(`program_slot.hpp:65-74`). All voices of one `poly()` share one body, so
inter-voice skew cannot arise; the only question is the body's own chain
latency surfacing at the call site. Phase 1 baseline: latency-declaring
nodes inside bodies contribute 0 at the call site + W-warning; per-body
aggregation (sweep the body once, add at `FOREACH_EVENT` / `BLOCK_CALL`
site) is a stretch goal. §10 Q4.

### 8.5 Signal-rate latency parameters

`limiter(..., lookahead: sine(0.1) * 5)` has genuinely time-varying latency.
Declaration is only made when the argument constant-folds; otherwise declare
0 and W-warn ("lookahead driven by a signal — latency unreported"). Same
policy for any future parameter-dependent-latency builtin.

### 8.6 Multiple `out()` / silent programs

Multiple `OUTPUT`s are joins (max). A program with no `out()` still gets the
bus epilogue (`codegen.cpp:181`) and reports total 0.

---

## 9. Implementation Phases & Verification

**Phase 1a — declaration plumbing.** `BuiltinInfo::latency_samples`;
limiter constant-fold; `LatencyDecl` in `CodeGenResult` + wire packing;
`VM::set_node_latency`; `LatencyTable`.
*Verify:* akkado unit tests — constant lookahead produces the expected
decl; signal-rate lookahead produces none + W-warning; wire round-trip.

**Phase 1b — aggregation + queries.** Linear sweep, join-skew recording,
per-bus totals, `total_latency_samples()`, `latency_report_json()`.
*Verify:* cedar unit tests over hand-assembled instruction streams: chain
sums; join maxes and records skew; buffer reuse (two successive writers to
one index) tracks per-value latency; stereo pair propagation; `DELAY_TAP`
stops propagation; bus joins; the §4.3 example reproduces
`{512 skew, 752 total}` exactly.

**Phase 1c — surfacing + integration.** WASM export; debug-UI join by
`state_id`; docs (`web/static/docs/` + `bun run build:docs`); note in
`prd-juce-plugin.md` that reported latency = graph total + adapter ≤127.
*Verify:* end-to-end — compile a latent patch in the web app, read the
report from the compile worker; `[zero_alloc]` leg stays green; drift-fuzz
leg shows no growth from report regeneration across recompiles.

**Phase 2 — branch alignment** (separate revision of this PRD before any
implementation): compensation insertion, budget warning, `pdc: off`, UI.
*Verification bar set now:* the §9.1 impulse-alignment test must pass with
0-sample error.

### 9.1 Testing / Verification (concrete)

- **Impulse-alignment ground truth.** Add a test-only known-latency node (a
  dummy host op or test opcode: pure N-sample delay declaring latency N).
  Drive an impulse through `[passthru, dummy(N)] |> mix`; cross-correlate
  the two arms at the join. Phase 1 assertion: measured arm offset == the
  reported join skew, and reported total == N, for N ∈ {1, 127, 128, 129,
  512, 4800}. Phase 2 assertion: after compensation, arms align within **0
  samples** and total is unchanged.
- **Limiter regression.** Impulse through `limiter(lookahead: 5)`; measured
  output delay (argmax of impulse response) must equal the declared latency
  ±1 sample. This pins G6 to measured behavior, not just metadata.
- **Hot-swap latency churn (≥300 s rule).** A sequence-driven patch
  (`n"c3 e3 g3 …"` through a latent node) recompiled repeatedly while
  `set_node_latency` flips values, simulating ≥300 seconds of audio:
  report stays consistent with the last declaration, no audio-thread
  allocation (`[zero_alloc]`), bounded RSS (drift-fuzz leg). Report the
  failure block/time if it fails; do not shorten the run.
- **Aggregation unit matrix** (Phase 1b list above) in
  `akkado/tests` / `cedar/tests`, tagged `[latency]`.
- **Report determinism.** Same program + same declarations twice →
  byte-identical JSON (guards against unordered-container leakage into the
  report).

---

## 10. Open Questions

1. **Source-C seam.** Is `VM::set_node_latency(state_id, samples)` the
   right single entry point for both hosted plugins and dynamic host ops, or
   should `HostOpRegistry` own a per-instance latency callback that the
   loader polls?
2. **Report staleness contract.** Generation counter on the report (proposed)
   vs. folding the `LatencyTable` into `ProgramSignature` — does any
   consumer need latency-change to *look* structural in Phase 1?
3. **Latency in `inspect_state_json` proper.** Keep the sibling-report
   design, or additionally echo `own_latency` inside each state's JSON for
   one-stop debugging?
4. **Subprogram policy.** Is warn+zero (§8.4) acceptable for v1, or does
   `poly()` through a latent instrument body need real aggregation from day
   one?
5. **Feedback-loop policy.** Warn + clamp-to-zero across the loop boundary
   (proposed) vs. hard E-class error on declared latency inside a feedback
   body — does coerce-don't-fail extend this far?
6. **Phase-2 insertion site** — codegen-emitted delay instructions vs.
   VM-implicit join delays (§6).
7. **Control-input propagation.** Phase 1 propagates latency through *all*
   wired inputs (conservative). Should parameter-class inputs (cutoff, rate)
   be exempted, and if so, is `BuiltinInfo::param_types` authority enough to
   classify them?
8. **Budget threshold + syntax** for Phase 2 (`pdc: off`, W-number
   allocation, default ms figure).
9. **Fractional latency.** VST3/CLAP report integers, but resampler-style
   nodes have fractional group delay; is integer-only a permanent contract
   or a v1 simplification?
10. **Flap damping.** A plugin that toggles latency rapidly (e.g. oversampling
    mode flips) re-triggers aggregation each time; is debouncing the
    embedder's job (studio) or does `set_node_latency` need built-in
    coalescing?

---

## 11. References

- `docs/research/studio-overview.md` — binding decision record; this PRD is
  item 8 of the engine-side breakdown.
- `docs/research/studio-plugin-hosting.md` §4.2-4.3 — phase-1 report-only
  PDC decision; dynamic latency as hot-swap; budget/`pdc: off` sketch.
- `docs/prd-studio-plugin-hosting.md` — motivating consumer (drafted in
  parallel; closed-repo PRD).
- `docs/prd-juce-plugin.md` §6.4 — block-adapter `setLatencySamples()`
  prior art; graph latency composes with it.
- `docs/prd-host-extension-api.md` — `HOST_OP` table where third-party
  native DSP enters; gains an optional latency declaration.
- Current-state evidence: file:line citations throughout §2.
