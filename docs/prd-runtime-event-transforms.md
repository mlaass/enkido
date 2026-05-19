> **Status: FIRST DRAFT — NOT READY FOR IMPLEMENTATION.** This PRD is a captured first-draft design from a brainstorming session. It has **two** hard external dependencies and several open questions that must be resolved before the design is locked. See §§0–0.5 (Dependencies) and §11 (Open Questions) before treating any decision as final. Do not begin implementation until both dependency PRDs land AND the open questions are resolved.

# PRD: Runtime Event-Stream Transforms (Pattern-Modifier Rework)

## Executive Summary

Today, every Akkado pattern modifier (`transpose`, `tune`, `fast`, `slow`, `early`, `late`, `rev`, `ply`, `swing`, `velocity`, `dur`, `bend`, `palindrome`, `zoom`, `segment`, `compress`, `iter`, `iterBack`, `linger`, `swingBy`, `bank`, `variant`, `aftertouch`, ~30 total) is a **compile-time AST transform**: the handler in `akkado/src/codegen_patterns.cpp` mutates a `std::vector<cedar::Event>` during codegen and emits `Opcode::NOP`. This forces every parameter to collapse to a constant, blocks parity with live MIDI streams, duplicates event-list-walk logic across 30+ handlers, and prevents any composability with user signals.

This PRD specifies a **rework into runtime event-stream transforms**: every modifier becomes a Cedar opcode that operates on `OutputEvents` — the existing runtime boundary already shared by `SequenceState` and `MidiQueueState`. The substrate is **six new Cedar opcodes** (`EVENT_MAP`, `EVENT_FILTER`, `EVENT_FANOUT`, `EVENT_REORDER`, `EVENT_RATE_SCALE`, `EVENT_QUANTIZE`). On top sits **one closure-taking builtin** (`event_map(events, (e) -> {...})`); most property modifiers (`transpose`, `velocity`, `dur`, `bend`, etc.) are rewritten as 1-line `akkado/stdlib/event_transforms.ak` definitions on top of `event_map`. Structural ops (`rev`, `ply`, `palindrome`, voicing) and the rate-scaling ops (`fast`, `slow`) lower directly to the appropriate primitive opcode.

**Key design decisions (first draft — subject to PRD review):**

- **Replace, don't coexist.** Compile-time modifier handlers are deleted, not deprecated. Constants and signals lower to the same runtime path. (Migration story per §8.)
- **Six-opcode substrate** split by *event-stream shape* (per-event rewrite / predicate filter / fanout / reorder / rate-scale / quantize) — not one mega-opcode and not one per modifier.
- **Closure-taking `event_map`** as the high-level surface; existing closures-as-compile-time-AST are insufficient and the design depends on the runtime closure PRD (§0).
- **Signal sampling at event onset.** When a closure body references an external buffer (`e.note + lfo`), the buffer is indexed at the event's emission sample, not block-start and not re-sampled mid-event.
- **`fast`/`slow` become continuous rate scalers.** Signal-rate multiplier on the phase feeding upstream `SEQPAT_QUERY`, sample-accurate.
- **Scope includes** core modifiers + scale quantize (snap to scale/key) + voicing/chord expansion/inversion + filter/predicate ops (`degrade`, `mask`).
- **Phased delivery in 5 phases** (see §9), each independently testable.
- **Assumes cycles-pure clock model has landed.** Hard prereq per §0.5 — every `e.time` reference, the `fast`/`slow` phase rescaler, and the `EventStreamPayload` cleanup are written in cycles-pure terms.

---

## 0. Hard Dependency: Runtime Closure Infrastructure (separate PRD)

Cedar today has **no runtime function dispatch** — no CALL/RET opcodes, no first-class function values, no closure objects in the state pool. Akkado closures (`(e) -> ...`) only exist as compile-time AST nodes; they get inlined at every call site via `handle_user_function_call` (`akkado/src/codegen_functions.cpp:69-300`).

The clean `event_map(events, (e) -> ...)` design depends on **first-class runtime closures** existing in Cedar. **A separate PRD owns this work** and must land first. That PRD needs to deliver, at minimum:

- A `Closure` runtime value (captures + bytecode pointer, stored in `DSPState` or a new closure pool).
- An opcode (provisional name `INVOKE_CLOSURE`) the VM dispatches per call.
- Akkado codegen support: closure values flow through `TypedValue` as a new variant, callable by builtins.
- Decisions on: control flow inside the VM (currently absent), capture lifetime, allocation strategy.

**This PRD assumes that infrastructure exists.** Without it, we would fall back to a codegen-inlining approach (specialised `TransformKind` enum + per-RHS-pattern matching + a small per-event mini-VM bytecode for arbitrary expressions). That fallback is workable but is documented here only as an escape hatch — not the recommended design.

> **PRD reviewers**: §0 is load-bearing. If the closure PRD is descoped or significantly changes shape, every section below (especially §3, §4, §5, §9) needs revisiting.

---

## 0.5. Hard Dependency: Cycles-Pure Clock Model (separate PRD)

A second PRD — **"Cycles-pure clock model — drop beats from Cedar"** — must land **before this PRD's Phase 1**. The cycles-pure refactor removes `cycle_length` from `SequenceState` / `PatternPayload`, drops the "1 cycle = 4 beats" convention, makes the cycle the only musician-facing time unit (BPM → CPS), and normalises every event to cycle phase `[0, 1)`. The captured design lives in the project-memory entry `project_strudel_cycles_pure_model_followup.md`.

**Why it's a hard prereq (not a follow-up)**:

- §3.1's signal-sampling formula in the original draft referenced `spb` (samples per beat). Post-refactor that's `spc` (samples per cycle), and the event-onset sample is derived directly from the scheduler — no `cycle_length` arithmetic.
- §3.3's stdlib `early` / `late` are already cycle-phase by design (`mod 1.0`) — they are *forward-compatible* with cycles-pure but would need a "this is cycles, not beats" disclaimer if shipped before the refactor.
- §3.3's `swing` / `swingBy` definitionally assumed a beat-grid; cycles-pure forces an explicit grid-subdivision argument (see §11 OQ-10).
- §3.4's Phase B `EventStreamPayload` cleanup must drop `cycle_length`, not carry the dead field forward.
- §11 OQ-7's interaction between `fast`/`slow` and `cycle_length` largely *evaporates* post-refactor — there is no `cycle_length` to mutate, only the cycle-phase rate feeding `SEQPAT_QUERY`.

**Touch-points in this PRD that depend on cycles-pure semantics**:

- §3.1 — event-onset sampling derived from scheduler in sample units.
- §3.3 — `e.time` is cycle phase; `swing` defers to OQ-10.
- §3.4 — Phase B drops `cycle_length`.
- §7 — file list overlaps with the cycles-pure refactor's scope.
- §11 OQ-7 — rewritten in cycles-pure terms.
- §11 OQ-9, OQ-10 — new questions specific to cycle-pure event semantics.

> **PRD reviewers**: §0.5 is load-bearing in the same way §0 is. If the cycles-pure PRD is descoped or significantly changes shape, every time-related reference below needs revisiting.

---

## 1. Current State

### 1.1 What exists today

**~30 pattern modifiers, all compile-time.** Handlers live in `akkado/src/codegen_patterns.cpp`. Each one walks a `std::vector<cedar::Event>` and mutates fields, then re-serialises into a `cedar::Sequence` via `SequenceCompiler`. The opcode emitted is `Opcode::NOP`.

Examples (file:line refs in `akkado/src/codegen_patterns.cpp`):

| Modifier      | Handler                       | Line  |
|---------------|-------------------------------|-------|
| `transpose`   | `handle_transpose_call`       | 3238  |
| `tune`        | `handle_tune_call`            | 4297  |
| `fast`        | `handle_fast_call`            | 3117  |
| `slow`        | `handle_slow_call`            | 3056  |
| `velocity`    | `handle_velocity_call`        | 3311  |
| `dur`         | `handle_dur_call`             | 3637  |
| `bend`        | `handle_bend_call`            | 3629  |
| `early`       | `handle_early_call`           | 4367  |
| `late`        | `handle_late_call`            | 4430  |
| `rev`         | `handle_rev_call`             | 3178  |
| `palindrome`  | `handle_palindrome_call`      | 4493  |
| `ply`         | `handle_ply_call`             | 4560  |
| `linger`      | `handle_linger_call`          | 4624  |
| `zoom`        | `handle_zoom_call`            | 4693  |
| `segment`     | `handle_segment_call`         | 4764  |
| `compress`    | `handle_compress_call`        | 5487  |
| `swing`       | `handle_swing_call`           | 5380  |
| `swingBy`     | `handle_swing_by_call`        | 5431  |
| `iter`        | `handle_iter_call`            | 4844  |
| `iterBack`    | `handle_iter_back_call`       | 4905  |
| `bank`        | `handle_bank_call`            | 3738  |
| `variant`     | `handle_variant_call`         | 3899  |

**Runtime event boundary already exists.** Both `SequenceState` (`cedar/include/cedar/opcodes/sequence.hpp:190` ff) and `MidiQueueState` (`cedar/include/cedar/opcodes/midi.hpp:46`) publish an `OutputEvents` array (`sequence.hpp:122-184`) per block. `SEQPAT_QUERY` fills it; `SEQPAT_STEP` / `SEQPAT_GATE` / `SEQPAT_PROP` read from it and destructure into per-field signal buffers (FREQ, VEL, TRIG, GATE, NOTE, DUR, CHANCE, TIME, PHASE, SAMPLE_ID — see `akkado/include/akkado/typed_value.hpp:108-119`). `poly()` and `soundfont` read OutputEvents directly via `StatePool::resolve_output_events`.

**TypedValue payloads.** `PatternPayload` carries per-field buffer indices, `voice_freqs[]`, `custom_fields`, plus a `state_id` pointing to the SequenceState. `EventSourcePayload` (`typed_value.hpp:144`) wraps MIDI streams — just a state_id + cycle_length, no buffers. MIDI events get baked to mono buffers for `as e` field access via the `is_runtime_event_source` flag.

### 1.2 The gap

1. **No signals as parameters.** `transpose(p, lfo)` is impossible.
2. **No MIDI parity.** Modifiers only work on baked patterns; live MIDI has no equivalent path.
3. **Implementation sprawl.** 30+ near-identical handlers; bug fixes need fanning out.
4. **No user composability.** You can't write a new modifier in akkado userland that operates per-event.
5. **No first-class chord/voicing/scale ops.** Things like "snap to minor scale" or "expand to chord" must be retrofitted; today they would each need their own compile-time handler.

---

## 2. Goals and Non-Goals

### 2.1 Goals

1. Introduce **6 new Cedar opcodes** (`EVENT_MAP`, `EVENT_FILTER`, `EVENT_FANOUT`, `EVENT_REORDER`, `EVENT_RATE_SCALE`, `EVENT_QUANTIZE`) operating on `OutputEvents`.
2. Introduce **`event_map` and `event_filter`** closure-taking Akkado builtins.
3. **Rewrite ~all property modifiers as 1-line stdlib akkado** (`akkado/stdlib/event_transforms.ak`) on top of `event_map`. Delete the corresponding C++ handlers.
4. **Migrate structural modifiers** (`rev`, `ply`, `palindrome`, `linger`, `zoom`, `segment`, `compress`, `iter`, `iterBack`) to `EVENT_FANOUT` / `EVENT_REORDER`.
5. **Migrate `fast`/`slow`** to `EVENT_RATE_SCALE` with continuous-signal support.
6. **Achieve uniform pattern/MIDI parity.** Every transform works identically on patterns, MIDI streams, and any future event source.
7. **Add new capabilities**: `scale`/`key` (pitch quantize), chord expansion / voicing / inversion via `EVENT_FANOUT`, `degrade`/`mask`/`filter_events` via `EVENT_FILTER`.
8. **Ship docs and examples** covering the new model end-to-end.

### 2.2 Non-Goals (deferred)

- **Runtime closure infrastructure itself.** Owned by §0's separate PRD.
- **Removing pattern compile-time baking entirely.** `n"c4 e4 g4"` still bakes a `cedar::Sequence` at compile time — only the *modifier* layer becomes runtime.
- **Replacing `bank` / `variant` sample resolution with runtime asset loading.** Sample assets must still resolve at compile time so `RequiredSamples` propagation works. (See §11 OQ-3 for nuance.)
- **A `with`-record-literal sugar syntax.** Explicitly rejected by the user in favour of closure-only ergonomics.
- **Per-sample mid-event signal re-sampling for closure parameters.** Event-onset latching only (§3.1).

---

## 3. Design

### 3.1 Runtime substrate — six new Cedar opcodes

Split by *event-stream shape*, not by modifier name. Each opcode reads upstream `OutputEvents` via `StatePool::resolve_output_events` (which already handles `SequenceState` + `MidiQueueState` uniformly — `cedar/include/cedar/opcodes/state_pool.hpp:128`) and publishes its own `OutputEvents`:

| Opcode             | Shape                                                  | Used by                                                                       |
|--------------------|--------------------------------------------------------|-------------------------------------------------------------------------------|
| `EVENT_MAP`        | per-event field rewrite via closure                    | `event_map`, `transpose`, `velocity`, `dur`, `bend`, `tune`, `aftertouch`, `early`, `late`, `swing`, `swingBy` |
| `EVENT_FILTER`     | predicate drop via closure                             | `degrade`, `mask`, `filter_events`                                            |
| `EVENT_FANOUT`     | output count > input count                             | `ply`, `linger`, `segment`, `voice` (chord expansion)                         |
| `EVENT_REORDER`    | permute / re-time                                      | `rev`, `palindrome`, `iter`, `iterBack`, `zoom`, `compress`                   |
| `EVENT_RATE_SCALE` | rewires phase feeding upstream `SEQPAT_QUERY`          | `fast`, `slow` (continuous-signal variant)                                    |
| `EVENT_QUANTIZE`   | snap pitch to scale/key                                | `scale`, `key`                                                                |

Common conventions:

- **State**: each instance owns a `SequenceState` (reuses the existing struct — no new `DSPState` variant). `OutputEvents` is the wire format.
- **Upstream `state_id`**: packed across `inputs[2]+inputs[3]` (32 bits in two 16-bit slots). Leaves `inputs[0..1]` for the two most-common signal parameters; 3+-param transforms use `ExtendedParams<N>` per `docs/extended-params-mechanism.md`.
- **Closure handle**: stored in `StateInitData` as a runtime closure reference (delivered by the closure PRD per §0).
- **Signal sampling at event onset**: when a closure body reads an external buffer (`e.note + lfo`), the opcode indexes the buffer at the event's scheduled onset sample within the current block (provided directly by the runtime scheduler — no `samples-per-beat` arithmetic; per §0.5 cycles-pure, `cycle_length` is gone). Latched per event; no mid-event re-sampling.
- **Downstream**: PatternPayload/EventStream pointers are rewired to the final transform's state_id; `as e |> osc(@, e.freq)` keeps working because SEQPAT_STEP still reads OutputEvents and refills per-field buffers.
- **Per-block memory budget**: each opcode allocates an `OutputEvents` buffer sized to `upstream_cap × fanout_factor`. See §11 OQ-4 for chain-length caps.

### 3.2 Closure-taking builtin

```akkado
event_map(events, (e) -> {note: e.note + 7, vel: e.vel * 0.8})
```

- **Signature**: `(EventStream, Closure((Event) -> Record)) -> EventStream`.
- **Return contract**: the returned record's fields override matching event fields; unspecified fields pass through unchanged.
- **Allowed inside body**: field reads on `e`, arithmetic + math builtins, external buffer references (sampled at event onset), calls to whitelisted pure helpers (`scale_to`, `quantize`, `mod`, `if`).
- **Forbidden inside body** (compile-time errors):
  - Stateful opcodes (proposed `E170`).
  - State cells / `state(init)` / `get(s)` / `set(s, v)` (proposed `E171`).
  - `out()` calls (proposed `E173`).
  - Nested `event_map` is **allowed** — it just produces a transformed stream the outer can consume.
- **Lowering**: codegen emits a single `EVENT_MAP` instruction with the closure reference in `StateInitData`. The runtime opcode body invokes the closure for each upstream event, copies/merges field outputs, writes the new event to its `OutputEvents`.

`event_filter` mirrors `event_map`:

```akkado
event_filter(events, (e) -> e.vel > 0.5)
```

- **Signature**: `(EventStream, Closure((Event) -> Bool)) -> EventStream`.
- **Lowering**: `EVENT_FILTER` opcode invoking the predicate per event; events are copied through only if it returns truthy.

### 3.3 Stdlib redefinitions

Move ~all property modifiers out of C++ codegen handlers and into `akkado/stdlib/event_transforms.ak`:

```akkado
fn transpose(events, n)   = event_map(events, (e) -> {note: e.note + n})
fn velocity(events, v)    = event_map(events, (e) -> {vel:  e.vel * v})
fn dur(events, d)         = event_map(events, (e) -> {dur:  e.dur * d})
fn bend(events, b)        = event_map(events, (e) -> {bend: b})
fn aftertouch(events, a)  = event_map(events, (e) -> {at:   a})
fn early(events, t)       = event_map(events, (e) -> {time: (e.time - t) mod 1.0})
fn late(events, t)        = event_map(events, (e) -> {time: (e.time + t) mod 1.0})
fn tune(events, cents)    = event_map(events, (e) -> {micro: e.micro + cents})
```

Per §0.5, `e.time` is **cycle phase** `[0, 1)` within the current cycle (matching Strudel's `co`), so `early(p, 0.25)` shifts by a quarter-cycle. For closures that need cross-cycle reasoning, `e.cycle` exposes the absolute cycle count as a float (e.g., `3.25` = quarter-way through cycle 3) — see §11 OQ-9 for final field naming. `swing` / `swingBy` need an explicit grid-subdivision argument under cycles-pure (the old implicit "8th-note grid" assumed 4 beats per cycle) — see §11 OQ-10.

Structural transforms (`rev`, `ply`, `palindrome`, etc.) stay builtins because they don't fit the per-event-record-rewrite shape — they lower directly to `EVENT_FANOUT` / `EVENT_REORDER`. `fast`/`slow` stay builtins for the same reason — they lower to `EVENT_RATE_SCALE`. `bank`/`variant` keep their compile-time sample-resolution path (sample-ref propagation is unchanged) but stamp `type_id` at runtime through `EVENT_MAP` with a small `BANK_SET` helper. See §11 OQ-3.

### 3.4 TypedValue evolution (two phases)

- **Phase A (compatible)**: add `upstream_state_id` and `transform_chain` fields to `PatternPayload` (`akkado/include/akkado/typed_value.hpp:50-120`). Existing `as e |> osc(@, e.freq)` keeps working — `SEQPAT_FIELD` just points at the final transform's `state_id`.
- **Phase B (cleanup)**: collapse `EventSourcePayload` into a unified `EventStreamPayload`, deprecate `ValueType::EventSource`. Pattern and MIDI become the same type. Per §0.5, the unified payload does **not** carry `cycle_length` (removed by the cycles-pure refactor).

---

## 4. Worked Examples

### 4.1 Static transpose (constant)

```akkado
n"c4 e4 g4".transpose(7) |> osc("sin", @.freq) |> out(@)
```

Codegen:
- `n"c4 e4 g4"` → `SEQPAT_QUERY` populating `SequenceState A` per block.
- `transpose(p, 7)` lowers (via stdlib) to `event_map(p, (e) -> {note: e.note + 7})`.
- `EVENT_MAP` opcode reads `SequenceState A`'s `OutputEvents`, applies the closure (constant +7), writes to `SequenceState B`.
- Downstream `SEQPAT_STEP` reads `SequenceState B`, fills FREQ buffer with transposed frequencies.

### 4.2 Signal-driven transpose

```akkado
lfo = osc("sin", 0.2) * 6
n"c4 e4 g4".transpose(lfo) |> osc("sin", @.freq) |> out(@)
```

Closure body becomes `(e) -> {note: e.note + lfo[event_offset]}`. The runtime `EVENT_MAP` opcode, for each event firing at sample-offset `t`, reads `lfo[t]` and adds it to the event's note before writing.

### 4.3 MIDI parity

```akkado
midi("ctrl1").transpose(12).velocity(0.7) |> poly(@, instr, 8)
```

`midi(...)` produces a `MidiQueueState` publishing `OutputEvents`. The same `EVENT_MAP` opcodes chain on top. `poly` reads the final state's `OutputEvents` via `StatePool::resolve_output_events`. No special-casing for MIDI.

### 4.4 Continuous rate scaling

```akkado
n"c d e f g".fast(osc("sin", 0.1) * 1.5 + 2) |> ...
```

`fast(p, sig)` lowers to `EVENT_RATE_SCALE` which produces a modulated phase signal that replaces `SEQPAT_QUERY`'s clock input (uses the existing external-clock path at `sequencing.hpp:372-380`).

### 4.5 Composable user code

```akkado
fn arp_up(events, steps) =
  event_map(events, (e) -> {note: e.note + (cycle_count() mod steps) * 12})

n"c4 g4".arp_up(3) |> osc("saw", @.freq) |> out(@)
```

Userland-defined modifier in 1 line, working on patterns or MIDI.

---

## 5. Migration Table

| Modifier               | Old handler (codegen_patterns.cpp:line) | New form                                                                                          |
|------------------------|------------------------------------------|---------------------------------------------------------------------------------------------------|
| `transpose`            | `handle_transpose_call:3238`             | stdlib `event_map`                                                                                |
| `tune`                 | `handle_tune_call:4297`                  | stdlib `event_map`                                                                                |
| `velocity`             | `handle_velocity_call:3311`              | stdlib `event_map`                                                                                |
| `dur`                  | `handle_dur_call:3637`                   | stdlib `event_map`                                                                                |
| `bend`                 | `handle_bend_call:3629`                  | stdlib `event_map`                                                                                |
| `aftertouch`           | (delegates to property_transform)        | stdlib `event_map`                                                                                |
| `early` / `late`       | `handle_early_call:4367` / `:4430`       | stdlib `event_map` on `time` field                                                                |
| `swing` / `swingBy`    | `handle_swing_call:5380` / `:5431`       | stdlib `event_map` (helper: `swing_offset(grid_pos, amount)`)                                     |
| `fast` / `slow`        | `handle_fast_call:3117` / `:3056`        | builtin → `EVENT_RATE_SCALE`                                                                      |
| `rev`                  | `handle_rev_call:3178`                   | builtin → `EVENT_REORDER`                                                                         |
| `palindrome`           | `handle_palindrome_call:4493`            | builtin → `EVENT_REORDER`                                                                         |
| `ply`                  | `handle_ply_call:4560`                   | builtin → `EVENT_FANOUT`                                                                          |
| `linger`               | `handle_linger_call:4624`                | builtin → `EVENT_FANOUT`                                                                          |
| `zoom`                 | `handle_zoom_call:4693`                  | builtin → `EVENT_REORDER`                                                                         |
| `segment`              | `handle_segment_call:4764`               | builtin → `EVENT_FANOUT`                                                                          |
| `compress`             | `handle_compress_call:5487`              | builtin → `EVENT_REORDER`                                                                         |
| `iter` / `iterBack`    | `handle_iter_call:4844` / `:4905`        | builtin → `EVENT_REORDER` (rotation by cycle index)                                               |
| `bank` / `variant`     | `handle_bank_call:3738` / `:3899`        | hybrid: keep compile-time sample-ref propagation; `type_id` stamped at runtime via `EVENT_MAP`    |
| `degrade` / `mask`     | (new)                                    | builtin → `EVENT_FILTER`                                                                          |
| `scale` / `key`        | (new)                                    | builtin → `EVENT_QUANTIZE`                                                                        |
| `voice` / `invert`     | (new)                                    | builtin → `EVENT_FANOUT` (chord/voicing expansion)                                                |

---

## 6. Error Codes (proposed)

| Code | Site                                             | Meaning                                                                                |
|------|--------------------------------------------------|----------------------------------------------------------------------------------------|
| `E170` | `event_map` / `event_filter` closure validation | Closure body invokes a stateful opcode                                                 |
| `E171` | …                                              | Closure body touches a state cell (`state` / `get` / `set`)                            |
| `E172` | …                                              | Reserved (was: nested `event_map`); nested is allowed, code reserved for future        |
| `E173` | …                                              | Closure body calls `out()` or any sink                                                 |
| `E180` | event-transform chain                           | Chain length exceeds the configured cap (see §11 OQ-4)                                 |
| `E181` | `EVENT_FANOUT`                                  | Output event count exceeds allocated capacity                                          |
| `E182` | `EVENT_QUANTIZE`                                | Unknown scale or key name                                                              |

---

## 7. Critical Files

> Several files below overlap with the §0.5 cycles-pure refactor's scope (`sequence.hpp`, `dsp_state.hpp`, `state_pool.hpp`, `SEQPAT_TRANSPORT`, `typed_value.hpp`). Per §0.5, cycles-pure lands first; this PRD assumes `cycle_length` plumbing and `spb` math are already gone.

**Cedar (engine):**
- `cedar/include/cedar/vm/instruction.hpp` — new `Opcode` enum entries.
- `cedar/include/cedar/opcodes/sequence.hpp` — `OutputEvents` struct (existing); add `op_event_map`, `op_event_filter`, `op_event_fanout`, `op_event_reorder`, `op_event_rate_scale`, `op_event_quantize`.
- `cedar/include/cedar/opcodes/state_pool.hpp:128` — `resolve_output_events` (existing — verify it covers transform-owned `SequenceState`s).
- `cedar/include/cedar/dsp_state.hpp:1405` — `DSPState` variant (reuse existing `SequenceState`; no new variant).

**Akkado (compiler):**
- `akkado/include/akkado/builtins.hpp:1291-1383` — replace handler bindings for all listed modifiers.
- `akkado/src/codegen_patterns.cpp` — delete handlers for the modifiers in §5; some become small shims emitting `EVENT_*` opcodes; most disappear in favour of stdlib akkado.
- `akkado/include/akkado/typed_value.hpp:50-120` — `PatternPayload` extensions (Phase A); unify to `EventStreamPayload` (Phase B).
- `akkado/src/codegen_functions.cpp:69-300` — interplay with closure handling (depends on §0 work).
- `akkado/stdlib/event_transforms.ak` — **NEW** file: stdlib redefinitions of property modifiers.

**Web / docs:**
- `web/static/docs/concepts/` — new "event streams" concept doc.
- `web/static/docs/builtins/` — update every modifier doc page; add `event_map`, `event_filter`.
- `web/src/lib/docs/` — rebuild `lookup-index.ts` via `bun run build:docs`.
- `web/wasm/` — `bun run build:opcodes` after new opcodes land.

**Tests:**
- `cedar/tests/test_event_*.cpp` — per-opcode unit tests.
- `akkado/tests/test_event_map.cpp` — closure-lowering correctness.
- `experiments/test_op_event_*.py` — long-form WAV renders (≥300 s simulated per CLAUDE.md:338).

---

## 8. Migration Story for User Code

Every existing example, demo patch, test, and doc using a modifier in §5 will route through the new path. Because constants and signals lower through the *same* `EVENT_MAP`, **constant-arg behaviour is preserved**; only internal mechanics change.

**Compatibility risks:**
- Timing of event mutations: old compile-time path applied modifications before `cedar::Sequence` was baked; new runtime path applies them after each block's `SEQPAT_QUERY`. For pure field rewrites (transpose, velocity, dur, bend) the observable behaviour is identical. For modifiers that interacted with `cycle_length` (`fast`, `slow`, `palindrome`), behaviour must be verified case-by-case — see §11 OQ-5.
- `iter` / `iterBack` already used runtime state for per-cycle rotation; behaviour should be preserved exactly.
- `bank` / `variant`: sample-ref propagation is preserved (still compile-time); runtime `type_id` stamping must produce identical sample routing.

**Migration approach** (see §11 OQ-5 for final decision):
- **Option A**: hard cutover, one PR per phase. Documentation tells users any behavioral regressions are bugs to file.
- **Option B**: ship a `--legacy-modifiers` flag (default off) for one release that re-enables old compile-time paths in parallel, then remove in the following release.

---

## 9. Phasing

| Phase | Deliverable                                                                                                                                                                                | Tests                                                                                       |
|-------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------|
| **0a** | **External PRD**: cycles-pure clock model lands (§0.5)                                                                                                                                    | Owned by the cycles-pure PRD                                                                |
| **0b** | **External PRD**: runtime closure / first-class fn infrastructure in Cedar lands (§0)                                                                                                     | Owned by the closure PRD                                                                    |
| **1** | Substrate: `EVENT_MAP` + `EVENT_FILTER` opcodes; manually-wired `transpose` and `velocity` in C++ codegen (no closures yet, constants only)                                                | `cedar/tests/test_event_map.cpp`, `experiments/test_op_event_map.py` (≥300 s)               |
| **2** | `event_map` / `event_filter` builtins taking closures; migrate property modifiers to `akkado/stdlib/event_transforms.ak`; delete corresponding C++ handlers                                  | `akkado/tests/test_event_map.cpp` for closure plumbing                                      |
| **3** | `EVENT_RATE_SCALE` opcode for `fast`/`slow` with signal input; `early`/`late`/`swing` via `event_map`                                                                                       | Rate-scaled WAV experiments                                                                 |
| **4** | `EVENT_FANOUT` + `EVENT_REORDER`: migrate `rev`, `palindrome`, `ply`, `linger`, `iter`, `iterBack`, `zoom`, `segment`, `compress`                                                            | Structural-transform experiments                                                            |
| **5** | `EVENT_QUANTIZE` (scale/key snap) + chord expansion / voicing / inversion via `EVENT_FANOUT`; `TypedValue` cleanup (Phase B)                                                                | Scale-quantize WAV; chord-expansion polyphony tests                                         |

Each phase ships Catch2 tests + ≥300 s rendered-WAV experiments per CLAUDE.md.

---

## 10. Verification

End-to-end checks once Phase 5 lands:

- `cmake --build build && ./build/cedar/tests/cedar_tests "[event-transform]" && ./build/akkado/tests/akkado_tests "[event-map]"` — all unit tests pass.
- `cd experiments && ./run_all.sh` — every `test_op_event_*.py` produces a clean WAV and a green pass.
- **Manual** (web dev server): paste `n"c4 e4 g4".transpose(7).velocity(0.8) |> osc("sin", @.freq) * @.vel |> out(@)`, verify transposed playback.
- **Manual MIDI parity**: `midi("ctrl1").transpose(12) |> poly(@, instr, 8)` — confirms MIDI parity.
- **Manual signal scaling**: `n"c d e".fast(lfo("sin", 0.2) * 1.5 + 2) |> ...` — confirms continuous rate-scaling works.
- **Manual composability**: a user-defined akkado modifier (`fn arp_up(events, steps) = ...`) compiles, runs on patterns AND MIDI.

---

## 11. Open Questions

Resolution required before locking the PRD.

**OQ-1. Chord events vs. field arithmetic.**
Should `(e) -> {note: e.note + 7}` transpose only the primary voice or every voice in `values[]`?
Proposed default: primary voice only; explicit `e.values` access for chord-wide transforms (`(e) -> {values: map(e.values, (v) -> v + 7)}`).

**OQ-2. Closure return-value passthrough merge spec.**
If the closure returns a record missing some fields, do unspecified fields pass through unchanged?
Proposed: yes. Spec the exact merge (shallow overlay).

**OQ-3. `bank` / `variant` runtime path.**
Sample assets must be loaded at compile time, so `RequiredSamples` propagation must remain compile-time. But the event's `type_id` could be runtime-stamped (enabling `events.bank(other_bank_signal)`).
Worth the complexity, or keep these compile-time-only and call it out?

**OQ-4. Per-block OutputEvents memory budget.**
Worst-case 30-chain × 4096-event cap ≈ 24 MB; fits 128 MB arena.
Cap chain length at 16 (`E180`)? Allow override via a project setting? Always lazily size to upstream-cap × fanout-factor?

**OQ-5. Migration period.**
Hard cutover (Option A in §8), or `--legacy-modifiers` flag for one release (Option B)?

**OQ-6. `event_map` codegen lowering when a field's RHS is a closed-form known to fit a specialised TransformKind.**
Even with runtime closures available, certain RHS patterns (`e.X + const`, `e.X * buf`) could lower to a specialised faster path that doesn't invoke the closure dispatcher per event. Worth pursuing as an optimisation? Or keep dispatch uniform for simplicity?

**OQ-7. `fast` / `slow` and downstream `co`.**
Per §0.5, `cycle_length` is removed by the cycles-pure refactor — `EVENT_RATE_SCALE` simply rescales the cycle-phase signal feeding upstream `SEQPAT_QUERY`. The remaining question: what should `co` (cycle offset) report downstream of `fast(p, 2)` — the *consumer's* cycle phase (unchanged) or the *inner pattern's* accelerated phase? Strudel's semantics: outer `co` is unchanged; the inner pattern produces 2× as many cycles' worth of events per outer cycle. Confirm and lock.

**OQ-8. Closure-PRD coupling points.**
List every API surface this PRD assumes the closure PRD will provide. Cross-reference both PRDs explicitly in their respective "Dependencies" sections.

**OQ-9. `e.time` vs `e.cycle` field naming.**
Per §0.5 cycles-pure, event records expose two time-like fields: phase within the current cycle (`[0, 1)`, used by `early` / `late` / `swing` stdlib) and the absolute cycle count (float, e.g. `3.25`, useful for cross-cycle reasoning inside closures). Proposed naming: `e.time` = phase, `e.cycle` = absolute count. Alternatives: `e.phase` + `e.time`, or `e.t` + `e.cycle`. Lock the names before stdlib ships.

**OQ-10. `swing` / `swingBy` grid spec under cycles-pure.**
The old `swing` assumed an 8th-note grid (4 beats / cycle → 8 grid positions). Under cycles-pure there is no "beat" — `swing` must take an explicit subdivision count or grid fraction.
Options:
- `swing(p, amount)` with implicit grid derived from the upstream pattern's natural top-level step count.
- `swing(p, amount, grid: 8)` explicit subdivision argument with default 8 (preserves observable behaviour for typical 8-step patterns).
- `swing(p, amount, grid_fraction: 1/8)` cycle-fraction form.

**OQ-11. Cycles-pure-PRD coupling points.**
List every API surface this PRD assumes the cycles-pure PRD will provide (`spc` accessor, scheduler's per-event sample timestamp, `EventStreamPayload` without `cycle_length`, `e.time` = phase, `e.cycle` = absolute count). Cross-reference both PRDs explicitly in their respective "Dependencies" sections.

---

## 12. Next Step

1. **Land the cycles-pure PRD** (§0.5) — design, review, implement.
2. **Land the runtime-closure PRD** (§0) — design, review, implement. Can proceed in parallel with cycles-pure; both must merge before Phase 1.
3. **Resolve §11 open questions** in this PRD draft. Promote to "READY FOR IMPLEMENTATION" status only after every OQ has a concrete decision.
4. **Implement Phase 1** of §9. Each subsequent phase is reviewed and merged independently.

**Do not begin implementation while this PRD is in `FIRST DRAFT` status.**
