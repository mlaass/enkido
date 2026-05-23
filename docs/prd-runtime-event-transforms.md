> **Status: IN PROGRESS — Phase 1 + Phase 2a shipped.**
>
> - **Phase 1** (substrate: packed `EVENT_MAP` / `EVENT_FILTER` opcodes +
>   runtime `transpose` / `velocity`) — shipped 2026-05-22, commit `79b4b24`.
> - **Phase 2a** (closure-taking `event_map` / `event_filter` builtins +
>   Cedar opcode closure rework) — shipped 2026-05-23, commit `694eb84`.
> - **Phase 2b** (stdlib `akkado/stdlib/event_transforms.ak` modifier
>   migration + delete C++ handlers) — **deferred**: blocked on
>   [`prd-parameter-type-annotations.md`](prd-parameter-type-annotations.md).
>   The stdlib one-liners need `events: stream`-annotated `fn` params (user
>   `fn`s cannot carry a Pattern / EventSource param today).
> - **Phases 3 – 5** (rate scaling, structural transforms, quantize +
>   `TypedValue` cleanup) — not started.
>
> The one hard external dependency — runtime closure / first-class fn
> infrastructure (§0,
> [`prd-runtime-functions-control-flow.md`](prd-runtime-functions-control-flow.md))
> — shipped its L1→L3 phases plus §4.2 `BLOCK_BIND` (commits `41bb96c`,
> `9776ded`, `16b166c`, `ad6aed6`, `5b7746b`, `21f73ef`, `9690a6a`,
> `7a31d9d`). All §11 open questions are resolved; decisions are recorded
> inline in §11. `prd-cycle-length-cleanup.md` (§0.5) is **not** a hard
> prerequisite and may land in any order. `prd-pattern-event-arrays.md`
> (§0.6) was a soft dependency for chord-wide closures and is complete.

# PRD: Runtime Event-Stream Transforms (Pattern-Modifier Rework)

## Executive Summary

Today, every Akkado pattern modifier (`transpose`, `tune`, `fast`, `slow`, `early`, `late`, `rev`, `ply`, `swing`, `velocity`, `dur`, `bend`, `palindrome`, `zoom`, `segment`, `compress`, `iter`, `iterBack`, `linger`, `swingBy`, `bank`, `variant`, `aftertouch`, ~30 total) is a **compile-time AST transform**: the handler in `akkado/src/codegen_patterns.cpp` mutates a `std::vector<cedar::Event>` during codegen and emits `Opcode::NOP`. This forces every parameter to collapse to a constant, blocks parity with live MIDI streams, duplicates event-list-walk logic across 30+ handlers, and prevents any composability with user signals.

This PRD specifies a **rework into runtime event-stream transforms**: every modifier becomes a Cedar opcode that operates on `OutputEvents` — the existing runtime boundary already shared by `SequenceState` and `MidiQueueState`. The substrate is **six new Cedar opcodes** (`EVENT_MAP`, `EVENT_FILTER`, `EVENT_FANOUT`, `EVENT_REORDER`, `EVENT_RATE_SCALE`, `EVENT_QUANTIZE`). On top sits **one closure-taking builtin** (`event_map(events, (e) -> {...})`); most property modifiers (`transpose`, `velocity`, `dur`, `bend`, etc.) are rewritten as 1-line `akkado/stdlib/event_transforms.ak` definitions on top of `event_map`. Structural ops (`rev`, `ply`, `palindrome`, voicing) and the rate-scaling ops (`fast`, `slow`) lower directly to the appropriate primitive opcode.

**Key design decisions (locked — see §11 for resolved open questions):**

- **Replace, don't coexist.** Compile-time modifier handlers are deleted, not deprecated. Constants and signals lower to the same runtime path. (Migration story per §8.)
- **Six-opcode substrate** split by *event-stream shape* (per-event rewrite / predicate filter / fanout / reorder / rate-scale / quantize) — not one mega-opcode and not one per modifier.
- **Closure-taking `event_map`** as the high-level surface; existing closures-as-compile-time-AST are insufficient and the design depends on the runtime closure PRD (§0).
- **Signal sampling at event onset.** When a closure body references an external buffer (`e.note + lfo`), the buffer is indexed at the event's emission sample, not block-start and not re-sampled mid-event.
- **`fast`/`slow` become continuous rate scalers.** Signal-rate multiplier on the phase feeding upstream `SEQPAT_QUERY`, sample-accurate.
- **Scope includes** core modifiers + scale quantize (snap to scale/key) + voicing/chord expansion/inversion + filter/predicate ops (`degrade`, `mask`).
- **Phased delivery in 5 phases** (see §9), each independently testable.
- **Assumes the cycles-pure clock model has landed** (it has — the "1 cycle = 1 beat" headline shipped with the 2026-05-19 parser revert, commit `9d99490`). Every `e.time` reference and the `fast`/`slow` phase rescaler are written in cycles-pure terms. This is **not** a hard dependency on `prd-cycle-length-cleanup.md`; per §0.5 the `cycle_length` field is live and stays.

---

## 0. Hard Dependency: Runtime Closure Infrastructure — SHIPPED

When this PRD was drafted, Cedar had **no runtime function dispatch** — no CALL/RET opcodes, no first-class function values, no closure objects in the state pool. Akkado closures (`(e) -> ...`) existed only as compile-time AST nodes, inlined at every call site via `handle_user_function_call` (`akkado/src/codegen_functions.cpp`).

**That gap is now closed.** [`prd-runtime-functions-control-flow.md`](prd-runtime-functions-control-flow.md) shipped its L1→L3 phases plus §4.2 `BLOCK_BIND`. The runtime surface the `event_map(events, (e) -> ...)` design depends on now exists:

- **`FOREACH_EVENT`** — the per-event dispatch opcode. Iterates an upstream event stream and invokes a subprogram block per event, with the lambda parameter delivered as a **full event record** (`n.freq` / `n.vel` / `n.dur` / `n.note` / `n.chance` / `n.time` map to convention slots; `n.gate` / `n.trig` are synthesized per iteration; `E408` for fields the event model cannot supply). This is the dispatch primitive the `EVENT_*` opcodes in §3.1 build on.
- **`BLOCK_CALL` / `BLOCK_BIND`** — runtime subprogram dispatch: a user `fn` body compiles once into `ProgramSlot::blocks[]` and dispatches at runtime. `BLOCK_CALL` carries args in `inputs[0..4]`; `BLOCK_BIND` extends the convention to 6..32 params. This is what lets `event_map(events, (e) -> ...)` lower to a real runtime closure instead of per-site inlining.
- **Subprogram table** — `ProgramSlot::blocks[]` + the block-aware load handshake + `set_state_id_xor` per-call-site isolation. Closure bodies are runtime-resident and hot-swap-stable via a `block:<fn>@callsite_<N>` path hash.
- **VM allocators** VOICE_POOL / PER_ITERATION / SHARED, and the higher-order DSL surface (`each()`, `each_voice()`, `reduce()`).

**Mapping to this PRD's design.** Where earlier drafts said "Closure" and "INVOKE_CLOSURE", the shipped names are `BlockRef` and `FOREACH_EVENT` / `BLOCK_CALL`. §3.1's "closure handle stored in `StateInitData`" is a `block_id` into the subprogram table. The escape-hatch codegen-inlining fallback (specialised `TransformKind` enum + per-event mini-VM) described in earlier drafts is **no longer needed and is dropped**. See §11 OQ-8 for the full coupling-surface list.

---

## 0.5. Related Cleanup: Clock-Phase & MIDI-Streaming (separate PRD)

> **Revised 2026-05-20 — this section was rewritten after a false premise was
> found.** It previously named [`prd-cycle-length-cleanup.md`](prd-cycle-length-cleanup.md)
> a **hard prerequisite** on the assumption that it removes the "now-dead"
> `cycle_length` field, leaving `EventStreamPayload` free of it. That premise
> is wrong: `cycle_length` is **live** — the `slow`/`fast`/`palindrome`/`linger`
> pattern transforms compute it (`akkado/src/codegen_patterns.cpp`), tests
> verify the computed values, and the runtime scales event times by it. The
> field **stays**. `prd-cycle-length-cleanup.md` has been rescoped to the
> genuinely-dead plumbing only (MIDI `1e6f` sentinel → `streaming: bool`;
> `beat_phase`/`bar_phase` → one `cycle_offset`; a comment/terminology pass).

[`prd-cycle-length-cleanup.md`](prd-cycle-length-cleanup.md) is **not a hard
prerequisite** of this PRD. Its rescoped work — the MIDI `streaming` flag and
the clock-phase collapse — is largely orthogonal to runtime event transforms.
The two PRDs can land in either order; the only soft coupling is the shared
`EventStreamPayload`/`EventSourcePayload` surface, which **carries
`cycle_length`** in both designs.

> **Note (2026-05-20):** the original dependency named here was `prd-cycles-pure-clock-model.md`. That PRD has been split. Its headline — "1 cycle = 1 beat", `samples_per_cycle()` dropping `* 4` — already shipped with the 2026-05-19 parser revert (commit `9d99490`). The remaining work was split into (1) `prd-cycle-length-cleanup.md`, the clock-phase / MIDI-streaming cleanup, and (2) `prd-beats-per-cycle.md`, an optional user-facing time-signature knob. **This PRD does NOT depend on `prd-beats-per-cycle.md`.** Every `e.time` reference below is already cycle phase `[0, 1)` post-revert.

**Time-semantics points still valid post-revert** (independent of the cleanup PRD):

- §3.1's signal-sampling formula in the original draft referenced `spb` (samples per beat). Post-revert that's `spc` (samples per cycle), and the event-onset sample is derived directly from the scheduler.
- §3.3's stdlib `early` / `late` are already cycle-phase by design (`mod 1.0`).
- §3.3's `swing` / `swingBy` definitionally assumed a beat-grid; under cycle = beat they need an explicit grid-subdivision argument (see §11 OQ-10).

**Corrections forced by the false-premise discovery — sections needing rework**:

- §3.4's Phase B `EventStreamPayload` cleanup must **keep** `cycle_length`, not drop it. The field is live; the unified payload carries it.
- §11 OQ-7's interaction between `fast`/`slow` and `cycle_length` does **not** evaporate — `fast`/`slow` *are* the `cycle_length` mutation. OQ-7 remains a live design question and must be answered, not deferred.
- Any other section that assumed `e.time` arithmetic could ignore `cycle_length` must be re-checked: `slow`-wrapped patterns produce `cycle_length != 1.0`.

> **Corrected 2026-05-21.** §3.1, §3.4, §7 and §11 OQ-7 below have been
> reworked to keep `cycle_length` live. No pre-correction wording remains.

---

## 0.6. Soft Dependency: Pattern Event Arrays (separate PRD)

A third PRD — **`prd-pattern-event-arrays.md`** ("Pattern Event Arrays — Userspace Access to Chord & Polyphonic Data") — is a **soft** dependency, gating **§11 OQ-1 only**. It surfaces `OutputEvent.values[]` as a first-class dynamic-array value type (`DynArray`), adds `notes(e)` / `freqs(e)` accessors (with `e.notes` / `e.freqs` UFCS sugar), makes `len()` polymorphic, and — per its own Q3 — defines `map()` over runtime-varying-length arrays.

**Why soft, not hard**: property modifiers that rewrite only the *primary* voice (`transpose`, `velocity`, `dur`, `bend` acting on `e.note`) need nothing from it. Only **chord-wide** transforms — rewriting every note in `values[]` — need the dynamic-array model. Per §11 OQ-1, the chord-wide closure form `(e) -> {notes: map(e.notes, (v) -> v + 7)}` is only well-defined once Pattern Event Arrays lands: that PRD owns both the `DynArray` type and `map`-over-`DynArray`.

**Shared boundary**: both PRDs operate on the same `OutputEvents` wire format. Pattern Event Arrays' `notes(e)` accessor reads `OutputEvent.values[]`; every `EVENT_*` opcode in this PRD reads and writes `OutputEvents`. Consequently `notes(e)` composes downstream of a transform chain for free — `n"[c4,e4,g4]".transpose(7) … e.notes` works with no extra wiring, because PatternPayload still points at the final transform's `state_id`.

**Sequencing**: Pattern Event Arrays is unblocked today (its own prerequisite, `prd-userspace-state-and-edge-primitives.md`, shipped 2026-04-26) and should land before this PRD's **Phase 2** (`event_map` closures). **Phase 1** (constant-only `EVENT_MAP` / `EVENT_FILTER`, primary-voice) does not depend on it.

> **PRD reviewers**: unlike §0 / §0.5, descoping this dependency does not block the PRD — it only forces OQ-1 to collapse to "primary-voice-only, no chord-wide closure form."

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
- **Signal sampling at event onset**: when a closure body reads an external buffer (`e.note + lfo`), the opcode indexes the buffer at the event's scheduled onset sample within the current block. The scheduler provides that per-event sample offset directly — no `samples-per-beat` arithmetic (the cycles-pure headline shipped 2026-05-19). `cycle_length` remains live but is not consulted on this path. Latched per event; no mid-event re-sampling.
- **Downstream**: PatternPayload/EventStream pointers are rewired to the final transform's state_id; `as e |> osc(@, e.freq)` keeps working because SEQPAT_STEP still reads OutputEvents and refills per-field buffers.
- **Per-block memory budget**: each opcode lazily sizes its `OutputEvents` buffer to `upstream_cap × fanout_factor`. No hard chain-length cap (§11 OQ-4).

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

`e.time` is **cycle phase** `[0, 1)` within the current cycle (matching Strudel's `co`), so `early(p, 0.25)` shifts by a quarter-cycle. For closures that need cross-cycle reasoning, `e.cycle` exposes the absolute cycle count as a float (e.g., `3.25` = quarter-way through cycle 3). Field naming is locked per §11 OQ-9: `e.time` = phase, `e.cycle` = absolute count. `swing` / `swingBy` take an explicit grid-subdivision argument (`grid:`, default `8`) under cycles-pure — the old implicit "8th-note grid" assumed 4 beats per cycle and no longer holds; per §11 OQ-10:

```akkado
fn swing(events, amount, grid = 8) =
  event_map(events, (e) -> {time: e.time + swing_offset(e.time * grid, amount)})
```

Structural transforms (`rev`, `ply`, `palindrome`, etc.) stay builtins because they don't fit the per-event-record-rewrite shape — they lower directly to `EVENT_FANOUT` / `EVENT_REORDER`. `fast`/`slow` stay builtins for the same reason — they lower to `EVENT_RATE_SCALE`. `bank`/`variant` keep their compile-time sample-resolution path (sample-ref propagation is unchanged) but stamp `type_id` at runtime through `EVENT_MAP` with a small `BANK_SET` helper. See §11 OQ-3.

### 3.4 TypedValue evolution (two phases)

- **Phase A (compatible)**: add `upstream_state_id` and `transform_chain` fields to `PatternPayload` (`akkado/include/akkado/typed_value.hpp:50-120`). Existing `as e |> osc(@, e.freq)` keeps working — `SEQPAT_FIELD` just points at the final transform's `state_id`.
- **Phase B (cleanup)**: collapse `EventSourcePayload` into a unified `EventStreamPayload`, deprecate `ValueType::EventSource`. Pattern and MIDI become the same type. Per §0.5, the unified payload **carries `cycle_length`** — the field is live (the `slow`/`fast`/`palindrome`/`linger` transforms compute it and the runtime scales event times by it), so both `PatternPayload` and the future `EventStreamPayload` keep it.

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
| `E180` | event-transform chain                           | **Reserved, not emitted.** §11 OQ-4 resolved to no hard chain-length cap; code held for a future budget guard. |
| `E181` | `EVENT_FANOUT`                                  | Output event count exceeds allocated capacity                                          |
| `E182` | `EVENT_QUANTIZE`                                | Unknown scale or key name                                                              |

---

## 7. Critical Files

> The cycles-pure headline (`spb`/`* 4` math) already shipped with the 2026-05-19 revert, so the files below are written in cycles-pure terms. `cycle_length` plumbing is **live and stays** (per §0.5) — none of the work below removes it. The remaining `prd-cycle-length-cleanup.md` work (MIDI `streaming` flag, clock-phase collapse) is orthogonal and may land in either order.

**Cedar (engine):**
- `cedar/include/cedar/vm/instruction.hpp` — new `Opcode` enum entries.
- `cedar/include/cedar/opcodes/sequence.hpp` — `OutputEvents` struct (existing); add `op_event_map`, `op_event_filter`, `op_event_fanout`, `op_event_reorder`, `op_event_rate_scale`, `op_event_quantize`.
- `cedar/include/cedar/opcodes/state_pool.hpp:128` — `resolve_output_events` (existing — verify it covers transform-owned `SequenceState`s).
- `cedar/include/cedar/dsp_state.hpp:1405` — `DSPState` variant (reuse existing `SequenceState`; no new variant).

**Akkado (compiler):**
- `akkado/include/akkado/builtins.hpp:1291-1383` — replace handler bindings for all listed modifiers.
- `akkado/src/codegen_patterns.cpp` — delete handlers for the modifiers in §5; some become small shims emitting `EVENT_*` opcodes; most disappear in favour of stdlib akkado.
- `akkado/include/akkado/typed_value.hpp:50-120` — `PatternPayload` extensions (Phase A); unify to `EventStreamPayload` (Phase B).
- `akkado/src/codegen_functions.cpp:69-733` — interplay with closure handling (depends on §0 work).
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

**Migration approach** — **hard cutover** (§11 OQ-5, resolved). One PR per phase; old compile-time modifier handlers are deleted, not deprecated. No `--legacy-modifiers` flag. Documentation tells users that any behavioral regression from the old compile-time path is a bug to file.

---

## 9. Phasing

| Phase | Deliverable                                                                                                                                                                                | Tests                                                                                       |
|-------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------|
| **0a** | ~~External PRD: `cycle_length` plumbing cleanup~~ — **not a hard prerequisite** (§0.5); may land in any order, no longer gates Phase 1                                                     | Owned by `prd-cycle-length-cleanup.md`                                                      |
| **0b** | **External PRD: runtime closure / first-class fn infrastructure — ✅ SHIPPED** (L1→L3 + §4.2 `BLOCK_BIND`)                                                                                  | Shipped by `prd-runtime-functions-control-flow.md`                                          |
| **1** | Substrate: `EVENT_MAP` + `EVENT_FILTER` opcodes; manually-wired `transpose` and `velocity` in C++ codegen (no closures yet, constants only)                                                | `cedar/tests/test_event_map.cpp`, `experiments/test_op_event_map.py` (≥300 s)               |
| **2a** | `event_map` / `event_filter` builtins taking closures; Cedar `EVENT_MAP`/`EVENT_FILTER` opcode closure rework. **Unblocked — in progress.**                                                  | `cedar/tests/test_event_map.cpp`, `akkado/tests/test_event_map.cpp` for closure plumbing     |
| **2b** | Migrate property modifiers to `akkado/stdlib/event_transforms.ak`; delete corresponding C++ handlers. **DEFERRED — blocked on [`prd-parameter-type-annotations.md`](prd-parameter-type-annotations.md)**: the stdlib one-liners need `events: stream`-annotated fn params (user fns cannot carry a Pattern/EventSource param today). | `akkado/tests/test_event_map.cpp` |
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

## 11. Resolved Design Decisions

All open questions were resolved 2026-05-21. The decisions below are final and locked; implementation follows them directly.

**OQ-1. Chord events vs. field arithmetic. → RESOLVED: primary-voice-only in Phase 1–2; chord-wide form kept specced, gated on Pattern Event Arrays before Phase 2.**
`(e) -> {note: e.note + 7}` transposes only the **primary voice**. Chord-wide transforms use the dynamic-array form `(e) -> {notes: map(e.notes, (v) -> v + 7)}`, where `e.notes` is the `notes(e)` accessor (UFCS sugar) from `prd-pattern-event-arrays.md` returning a `DynArray`. **Use `e.notes` consistently — no separate `e.values` spelling.** Pattern Event Arrays is NOT STARTED today; it is a soft dependency (§0.6) that must land before this PRD's **Phase 2**. Phase 1 (constant-only, primary-voice) does not need it. If Pattern Event Arrays slips past Phase 2, the chord-wide form ships in a follow-up — it does not block the rest of this PRD.

**OQ-2. Closure return-value passthrough merge. → RESOLVED: shallow overlay; unspecified fields pass through unchanged.**
The closure's returned record is a shallow overlay: each field present in the returned record overrides the matching event field; every field absent passes through from the upstream event unchanged. No deep merge, no field deletion. A closure returning `{}` is an identity transform.

**OQ-3. `bank` / `variant` runtime path. → RESOLVED: hybrid.**
`RequiredSamples` propagation stays compile-time (sample assets must load before audio starts). The event's `type_id` is **runtime-stamped** via `EVENT_MAP` plus a small `BANK_SET` helper, enabling `events.bank(other_bank_signal)`. The compile-time sample-ref propagation path is unchanged; only the per-event `type_id` field becomes runtime-writable. The §5 migration-table row for `bank`/`variant` already reflects this hybrid.

**OQ-4. Per-block OutputEvents memory budget. → RESOLVED: lazy sizing, no hard cap.**
Each `EVENT_*` opcode lazily sizes its `OutputEvents` buffer to `upstream_cap × fanout_factor`. There is **no hard chain-length limit**. Worst-case 30-chain × 4096-event ≈ 24 MB fits the 128 MB arena comfortably. `E181` (`EVENT_FANOUT` per-op capacity overflow) still guards the per-opcode bound; `E180` is reserved-but-unused (see §6) in case a global budget guard is wanted later.

**OQ-5. Migration period. → RESOLVED: hard cutover.**
Option A. One PR per phase; the old compile-time modifier handlers are **deleted**, not deprecated. No `--legacy-modifiers` flag. Documentation tells users that any behavioral regression from the old compile-time path is a bug to file. §8's Option B is dropped.

**OQ-6. Specialised codegen lowering for closed-form RHS. → RESOLVED: uniform dispatch only.**
Every `event_map` closure lowers to the same `EVENT_MAP` + `FOREACH_EVENT` dispatch path, including closed-form bodies like `e.X + const`. No specialised `TransformKind` fast path. A per-event fast path that skips the dispatcher is filed as a **future, non-blocking optimization PRD** — it is not in scope here and must not gate any phase.

**OQ-7. `fast` / `slow` and downstream `co`. → RESOLVED: Strudel semantics, locked.**
`EVENT_RATE_SCALE` rescales the cycle-phase signal feeding the upstream `SEQPAT_QUERY`. `cycle_length` is **live** — `fast`/`slow` are precisely the transforms that compute it (per §0.5), and `EVENT_RATE_SCALE` is its runtime expression. Downstream `co` (cycle offset) reports the **consumer's** cycle phase, unchanged by `fast(p, 2)`; the inner pattern simply produces 2× as many cycles' worth of events per outer cycle. This matches Strudel and is locked.

**OQ-8. Closure-PRD coupling surface. → RESOLVED: enumerated below (dependency shipped).**
This PRD consumes the following surfaces from the now-shipped `prd-runtime-functions-control-flow.md`:
- `FOREACH_EVENT` opcode — per-event subprogram dispatch; the dispatch primitive under every `EVENT_*` opcode.
- `BLOCK_CALL` / `BLOCK_BIND` opcodes + the `ProgramSlot::blocks[]` subprogram table — closure bodies compiled once, dispatched at runtime; `block_id` is the "closure handle" in §3.1's `StateInitData`.
- The block-aware program-load handshake + `set_state_id_xor` per-call-site state isolation + `block:<fn>@callsite_<N>` hot-swap path hashing.
- The event-record lambda parameter convention (`n.freq`/`n.vel`/`n.dur`/`n.note`/`n.chance`/`n.time` → convention slots; `n.gate`/`n.trig` synthesized; `E408` for unsupplyable fields).
- VM allocators VOICE_POOL / PER_ITERATION / SHARED and the `each()` / `reduce()` higher-order surface (used by chord/voicing fanout in Phase 5).
All are SHIPPED. `prd-runtime-functions-control-flow.md` §13 already cross-references this PRD as its consumer.

**OQ-9. `e.time` vs `e.cycle` field naming. → RESOLVED: `e.time` = phase, `e.cycle` = absolute.**
`e.time` is cycle phase `[0, 1)` within the current cycle (matches Strudel's `co`, matches the existing `e.time` field meaning so no rename). `e.cycle` is the absolute cycle count as a float (e.g. `3.25`). The `e.phase`/`e.t` alternatives are rejected. Locked before stdlib ships.

**OQ-10. `swing` / `swingBy` grid spec. → RESOLVED: explicit `grid:` arg, default 8.**
`swing(p, amount, grid = 8)` / `swingBy(p, amount, grid = 8)` take an explicit subdivision count, defaulting to `8` (preserves observable behaviour for typical 8-step patterns). The implicit-from-pattern and `grid_fraction` forms are rejected — explicit and predictable beats magic. Stdlib definition in §3.3.

**OQ-11. Cycles-pure-PRD coupling surface. → RESOLVED: enumerated below.**
The cycles-pure headline ("1 cycle = 1 beat", `samples_per_cycle()` without `* 4`) already shipped with the 2026-05-19 revert (commit `9d99490`). This PRD assumes:
- `spc` (samples-per-cycle) accessor and the scheduler's per-event onset-sample timestamp — used by §3.1 signal sampling at event onset.
- `e.time` = cycle phase `[0, 1)`, `e.cycle` = absolute cycle count (§3.3, OQ-9).
- `EventStreamPayload` / `EventSourcePayload` **carry `cycle_length`** — the field is live (corrected per §0.5; the earlier "dropped" premise was false).
None of this depends on `prd-cycle-length-cleanup.md`; that PRD's remaining work (MIDI `streaming` flag, clock-phase collapse) is orthogonal.

---

## 12. Next Step

Both design-blocking prerequisites are cleared:

1. ✅ **`prd-runtime-functions-control-flow.md`** (§0) — L1→L3 + §4.2 `BLOCK_BIND` SHIPPED.
2. ✅ **§11 open questions** — all 11 resolved (2026-05-21); decisions locked inline in §11.
3. **`prd-pattern-event-arrays.md`** (§0.6) — NOT STARTED; soft dependency, must land before **Phase 2** (chord-wide closures). Phase 1 is unblocked without it.
4. **`prd-cycle-length-cleanup.md`** (§0.5) — not a hard prerequisite; may land in any order.

**Implementation may begin now at Phase 1** of §9 (`EVENT_MAP` + `EVENT_FILTER` opcodes, constant-arg `transpose` / `velocity`). Each subsequent phase is reviewed and merged independently.
