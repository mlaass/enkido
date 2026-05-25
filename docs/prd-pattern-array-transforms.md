# PRD: Pattern-Array Transforms — Whole-Pattern Operations as Stdlib Akkado

> **Status: NOT STARTED — re-engineering, not missing functionality.**
> Filed 2026-05-24 as the follow-up to
> [`prd-runtime-event-transforms.md`](prd-runtime-event-transforms.md)
> Phase 5. The 11 user-facing transforms (`rev`, `palindrome`, `ply`,
> `linger`, `segment`, `zoom`, `compress`, `iter`, `iterBack`, `fast`,
> `slow`) **ship today** via `EVENT_RATE_SCALE` / `EVENT_REORDER` /
> `EVENT_FANOUT` (parent Phase 3+4, opcodes 221–223) — kind-dispatched
> C++ opcodes that work correctly. This PRD lowers them onto stdlib
> akkado built on an `Array<Event>` substrate. It is a re-engineering
> for principle-alignment, not a delivery of missing features.
> `degrade` and `mask` are the only genuinely new user-facing
> additions. Two scoped items deferred from v1: top-level
> `event_random()` semantics (closure-only in v1) and explicit
> closure cycle_length metadata for clock+array composition (see
> §4 Non-Goals).

---

## Executive Summary

The 11 whole-pattern transforms above are user-visible and working
today through three kind-dispatched C++ opcodes. They violate the
parent PRD's foundational principle — *opcodes are for primitive
operations and DSP work, not for language constructs* — by hardcoding
named transforms into C++ switch statements. This PRD lowers them
onto **stdlib akkado functions** built on an **`Array<Event>`** value
type, and adds two genuinely new transforms (`degrade`, `mask`).
The three opcodes from parent Phase 3+4 (`EVENT_RATE_SCALE`,
`EVENT_REORDER`, `EVENT_FANOUT`) collapse into:

1. One **renamed, single-purpose clock primitive**: `EVENT_SPEED` (formerly
   `EVENT_RATE_SCALE`, no kind switch, no name dispatch).
2. **Three new closure-callable primitives**: `event_random()` (fresh
   per-event-onset scalar, deterministically seeded by
   `(pattern_id, cycle, event_idx)`), `pattern_active(p, t)` (boolean-ish
   active-at-time lookup returning a number), and `pattern_event(p, t)`
   (full event record at time `t`) — needed so `degrade` and `mask` are
   stdlib one-liners.
3. **Extending the shipped `DynArray` substrate** to record-typed elements
   so `Array<Event>` is just `DynArray<Event>`.
4. **Stream ↔ array bridges** in the FOREACH_EVENT runtime so closures can
   receive the upstream cycle's events as an array and return a rewritten
   array.

Key design decisions baked in from the question rounds:

- **Clock primitive named `event_speed`** (not `event_retime`) to match the
  user-level `fast`/`slow` it backs.
- **`fast`/`slow` accept any factor type**: signal, scalar, or
  pattern-of-numbers (e.g. `fast(p, v"<2 3 4>")`). Lowering through the
  closure-call substrate.
- **Live MIDI is explicitly out of scope** for whole-pattern transforms in
  v1. Array<Event> is built from the upstream's current-cycle known events;
  live MIDI sources are documented as unsupported.
- **Optimistic cycle_length propagation**: reuse today's
  `SequenceState.cycle_length` chaining as-is; escalate to explicit closure
  metadata only if specific compositions (e.g. `palindrome` + `event_speed`)
  fail in practice.
- **Per-cycle bridge cadence**: the stream→array bridge builds an
  `Array<Event>` once per upstream cycle boundary; the closure runs once
  per cycle; rewritten events stream out across the cycle's blocks. Whole-
  pattern transforms inherently want whole cycles, not partial blocks.
- **Hard cutover** on opcodes: `EVENT_REORDER` and `EVENT_FANOUT` are
  deleted once stdlib lowers ship and akkado tests pass; no deprecation
  window.
- **Three phases**: Substrate → Stdlib → Cleanup.
- **No compile-time folding** in this PRD: runtime closure dispatch only.
  Optional fold-when-constant is deferred to a future optimizer PRD.

---

## 1. Problem

The parent PRD's foundational principle is:

> **Opcodes are for primitive operations and DSP work, not for language constructs. Per-event-stream transforms expressible from generic closure primitives belong in stdlib akkado.**

Phases 1 + 2a + 2b honored this: per-event transforms (`transpose`,
`velocity`, `dur`, `bend`, `swing`, `early`, `late`, …) lower to stdlib
`event_map` / `event_filter` closure calls on top of two generic opcodes.

**Phases 3 and 4 drifted.** They shipped three kind-dispatched mega-opcodes:

| Opcode | Kinds | Used by |
|---|---|---|
| `EVENT_RATE_SCALE` (Phase 3, opcode 221) | none (takes a factor signal) | `fast`, `slow` |
| `EVENT_REORDER` (Phase 4, opcode 222) | REV / PALINDROME / ITER / ITER_BACK / ZOOM / COMPRESS | `rev`, `palindrome`, `iter`, `iterBack`, `zoom`, `compress` |
| `EVENT_FANOUT` (Phase 4, opcode 223) | PLY / LINGER / SEGMENT | `ply`, `linger`, `segment` |

Each opcode is a `switch` over hardcoded named transforms with per-kind
C++ bodies in `cedar/include/cedar/opcodes/event_transforms.hpp`. **Adding a
new transform name requires adding a new kind enum value and a new C++
function — exactly what stdlib lowering was meant to eliminate.** These
named transforms are language constructs, not DSP primitives.

The opcodes are functionally working and shipping audio; they are a
stopgap, not a regression in user-visible behavior — but they are
architectural debt this PRD pays down.

---

## 2. Why these transforms didn't fit Phase 2b's `event_map` mold

The Phase 2b stdlib lowering pattern is:

```akkado
fn transpose(events, n) = event_map(events, (e) -> {note: e.note + n})
```

A closure receives one event at a time, returns one event. The runtime
walks the upstream events and applies the closure per event.

**`rev`, `palindrome`, `ply`, `linger`, `segment`, `fast`, `slow`
fundamentally cannot work this way.** They need either:

- The **full array of events** for a cycle (`rev` reverses N events;
  `palindrome` concatenates forward+reverse; `ply` emits N copies per
  input).
- **Clock control** over the upstream pattern's event generation rate
  (`fast(p, 2)` means "produce twice as many cycles worth of events per
  outer cycle" — clock manipulation, not stream rewriting).

A per-event-closure substrate cannot express these.

---

## 3. Proposed substrate

A pattern or MIDI seq is best conceptualized as an **array of event
records**, where each record has the defined event-field shape (`{note,
vel, dur, time, chance, notes, freqs, bend, at}`). Whole-pattern transforms
become array-rewriting closures:

```akkado
fn rev(events)        = events |> reverse
fn palindrome(events) = events ++ reverse(events)
fn ply(events, n)     = events |> flat_map((e) -> repeat({...e, dur: e.dur / n}, n))
fn linger(events, f)  = events |> filter((e) -> e.time < f) |> map((e) -> {...e, dur: e.dur / f})
fn zoom(events, start, end) =
    events |> filter((e) -> e.time + e.dur > start and e.time < end)
           |> map((e) -> {time: (max(e.time, start) - start) / (end - start), ...e})
fn compress(events, start, end) =
    events |> map((e) -> {time: start + e.time * (end - start), ...e})
fn iter(events, n)    =
    events |> map((e) -> {time: (e.time - (e.cycle mod n) / n + 1) mod 1, ...e})
```

`fast` / `slow` are different — they're clock-control, not array-rewriting.
They lower to one-line wrappers around `event_speed`:

```akkado
fn fast(events, factor) = event_speed(events, factor)
fn slow(events, factor) = event_speed(events, 1.0 / factor)
```

`event_speed` is a renamed `EVENT_RATE_SCALE` — same single-purpose factor
opcode, no kind switch, just a cleaner name that matches the user-facing
`fast`/`slow`.

### 3.1 Substrate primitives this PRD adds

| Primitive | Surface | Notes |
|---|---|---|
| `Array<Event>` value type | Akkado | Extends shipped `DynArray` (`prd-pattern-event-arrays.md`) to record-typed elements. `element_type = record` with the defined event-field shape. |
| Stream → array bridge (per cycle) | Cedar runtime | Fires once per upstream cycle boundary. Reads `SequenceState.output` into an `Array<Event>` DynArray containing that cycle's known events. **No live-MIDI buffering.** |
| Array → stream bridge (per cycle) | Cedar runtime | Writes the closure's returned `Array<Event>` into downstream `OutputEvents`. Events stream out across the cycle's blocks at their `e.time` offsets. Reuses chord-array DynArray write path. |
| Closure return-of-`Array<Event>` | Akkado closure body | Compiler accepts record-array literal / pipeline expressions as closure body return; codegen wires DynArray write through the bridge. |
| `event_speed(events, factor)` opcode | Cedar opcode (rename) | Renamed `EVENT_RATE_SCALE` → `EVENT_SPEED`. Single-purpose clock primitive, no kind switch. **Accepts signal, scalar, or pattern-of-numbers factor** (lowered via closure-call substrate). |
| `event_random()` | Cedar opcode + akkado builtin | Per-event-onset fresh scalar in `[0, 1)`. **Deterministically seeded** from `(pattern_id, cycle_index, event_index)` for reproducibility. Distinct from existing `random(n)` (which returns an `Array<Number>` per `prd-pattern-event-arrays.md`). Closure-body callable from inside `event_map` / `event_filter`. |
| `pattern_active(p, t)` | Cedar opcode + akkado builtin | Active-at-time lookup. Returns a number (`0` = inactive, `>0` = active intensity / gate value). Cheapest variant — what `mask` uses. Closure-body callable. |
| `pattern_event(p, t)` | Cedar opcode + akkado builtin | Full event record at time `t` (or an empty record if no event is active). For stdlib fns that need fields beyond gate (e.g. quantize-by-template, swap-based-on-source-velocity). Closure-body callable. |

Standard array primitives (`reverse`, `map`, `filter`, `flat_map`, `concat`,
`repeat`) become polymorphic over `Array<Event>` — most already work for
`Array<Number>` via Pattern Event Arrays.

### 3.2 `Array<Event>` memory: extend the DynArray pool

`Array<Event>` is a `DynArray` with `element_type = record`. Storage reuses
the shipped DynArray buffer pool (`prd-pattern-event-arrays.md` §3). The
shipped capacity `MAX_VALUES_PER_EVENT = 8` is **insufficient** for
whole-cycle event arrays (`ply(p, 16)` over a 16-step pattern produces 256
events). This PRD raises the capacity for record-typed DynArrays and
documents the limit:

| Constraint | Value | Rationale |
|---|---|---|
| `MAX_EVENTS_PER_CYCLE` (record-typed DynArray capacity) | **256** | Covers `ply(<16 step>, 16) = 256` and typical live-coding densities. Beyond this, emit warning `W…` and truncate (per coerce-don't-fail principle). |
| Allocation site | DynArray buffer pool, pre-allocated at swap time | No audio-path allocations. |
| Element shape | `OutputEvent` record (existing shape) | No new record type. |

Bump only the record-typed array capacity; numeric DynArrays stay at their
existing per-event slot limit.

### 3.3 `fast`/`slow` accepting patterns of factors

`event_speed` accepts:

- A **scalar** constant: `fast(p, 2)`.
- A **signal** buffer (audio-rate or control-rate): `fast(p, env)`.
- A **pattern of numbers** via `v"…"` inline: `fast(p, v"<2 3 4>")`. The
  value-pattern string lowers to a per-cycle signal stream that
  `event_speed` reads at cycle boundaries.

The factor is read once per upstream cycle (clock-control semantics; not
per sample). Signal inputs are sampled at cycle start.

### 3.4 `degrade` and `mask` stdlib

With `event_random()` and `pattern_active(p, t)` reachable inside closure
bodies:

```akkado
fn degrade(events, p) = event_filter(events, (e) -> event_random() > p)
fn mask(events, p)    = event_filter(events, (e) -> pattern_active(p, e.time) > 0)
```

`event_random()` is fresh per event onset (deterministically seeded from
`(pattern_id, cycle_index, event_index)`; not free-running). Same source
pattern + same cycle ⇒ same degradation, which is testable and matches
Strudel's `seed`-aware semantics.

`pattern_active(p, t)` evaluates the pattern `p` at cycle-relative time
`t ∈ [0, 1)` and returns the gate value (or value strength) at that moment.
For closure bodies that need richer information about the source event at
time `t` (e.g. velocity-aware masking), use the heavier `pattern_event(p, t)`
which returns the full event record.

---

## 4. Goals and Non-Goals

### Goals

- Replace `EVENT_REORDER` (kinds REV/PALINDROME/ITER/ITER_BACK/ZOOM/COMPRESS)
  and `EVENT_FANOUT` (kinds PLY/LINGER/SEGMENT) with stdlib akkado on top of
  an `Array<Event>` substrate.
- Rename `EVENT_RATE_SCALE` → `EVENT_SPEED`; expose builtin `event_speed`;
  make `fast`/`slow` 1-line stdlib wrappers.
- Add `event_random()` and `pattern_query(p, t)` primitives; ship `degrade`
  and `mask` as stdlib one-liners.
- Pay down the principle violation: zero kind-dispatched mega-opcodes for
  language-level transforms after this PRD ships.
- Unblock sibling `prd-scale-quantize.md` Phase 5 (patternable
  `scale("<c:major a:minor>")`) — its blocker was the same array substrate.

### Non-Goals

- **Live-MIDI whole-pattern transforms.** Out of scope; documented as a
  caveat. Live MIDI sources cannot be reversed/repeated whole-cycle in v1.
- **Compile-time folding of stdlib transforms.** Deferred to a future
  optimizer PRD. Runtime closure dispatch only.
- **Closure-as-factor for `fast`/`slow` accepting arbitrary user lambdas.**
  Accepts signal/scalar/pattern-of-numbers; arbitrary closures returning
  per-cycle floats are deferred.
- **Explicit cycle_length metadata on closure return.** Optimistic
  cycle_length runtime chaining is reused as-is in v1. Compositions
  that mix clock-control (`fast`/`slow`/`event_speed`) with array-
  rewriting (`palindrome`, `ply`, etc.) may produce mistimed events
  at the seam. This is a **documented v1 limitation**, not a
  blocking bug — a follow-up PRD will add explicit closure
  cycle_length metadata once a concrete failing composition needs
  fixing. v1 ships even if `palindrome |> fast(2)` drifts.
- **`event_random()` at top level (outside closures).** v1 supports
  `event_random()` only inside closure bodies (`event_map`,
  `event_filter`, or array `map`/`filter` over `Array<Event>`). Top-
  level use raises a compile-time error. The earlier-drafted "fresh
  per audio block, seeded from `(0, cycle_index, block_index)`"
  semantics had no real call site and are dropped.
- **`event_collect` whole-cycle buffering primitive.** Mentioned in original
  Q1; resolved by ruling out live-MIDI support. Not added.

---

## 5. Target syntax

End-state user experience (after Phase 2 ships):

```akkado
// All identical to today's surface — implementation changes underneath
"c4 e4 g4" |> rev() |> out
"<c4 e4 g4 b4>" |> palindrome() |> out
"c4 e4" |> ply(4) |> out                  // 8 events, each 1/4 of original dur
"c4 e4 g4 b4" |> linger(0.5) |> out      // first half, played twice as slow
pat("c e g") |> zoom(0.25, 0.75) |> out  // middle half
pat("c e g") |> compress(0.0, 0.5) |> out // first half of cycle
pat("c d e f") |> iter(4) |> out         // rotating offset per cycle
pat("c d e f") |> fast(2) |> out         // double speed
pat("c d e f") |> fast(v"<2 3 4>") |> out // patterned speed (new in this PRD)
pat("c d e f") |> degrade(0.5) |> out    // drop 50% of events (new in this PRD)
pat("c d e f") |> mask("1 0 1 0") |> out // gated by mask pattern (new in this PRD)
```

Composition (the chain works because every transform produces an `Array<Event>`):

```akkado
"c e g b" |> ply(2) |> rev() |> fast(2) |> out
```

Userspace `fn` definitions become possible (a new capability):

```akkado
fn shuffle(events) = events |> sort_by((a, b) -> event_random() - 0.5)
fn double(events)  = events ++ events |> map((e) -> {...e, vel: e.vel * 0.5})

"c e g" |> shuffle() |> double() |> out
```

---

## 6. Phasing

Three phases. Each phase is independently testable and ships behind the
previous phase's tests passing.

### Phase 1 — Substrate (foundation)

**Goal**: Land the `Array<Event>` value type, the stream↔array bridges,
and the renamed `EVENT_SPEED` opcode + `event_speed` builtin. No user-facing
behavior change.

| Step | Files |
|---|---|
| 1.1 | Extend `DynArray` to support `element_type = record`. `cedar/include/cedar/vm/dynarray.hpp` (or current location). |
| 1.2 | Bump record-typed DynArray capacity to `MAX_EVENTS_PER_CYCLE = 256`. |
| 1.3 | Implement stream→array bridge (read upstream `SequenceState.output` into Array<Event>). `cedar/include/cedar/opcodes/event_transforms.hpp`. |
| 1.4 | Implement array→stream bridge (write closure-returned Array<Event> to downstream events). Same file. |
| 1.5 | Rename `EVENT_RATE_SCALE` → `EVENT_SPEED` (opcode 221 stays). `cedar/include/cedar/vm/instruction.hpp`, `cedar/include/cedar/opcodes/event_transforms.hpp`. |
| 1.6 | Expose `event_speed(events, factor)` as akkado builtin. `akkado/include/akkado/builtins.hpp`. |
| 1.7 | Extend `event_speed` to accept pattern-of-numbers factor (lower `v"…"` to per-cycle signal). |

**Phase 1 verification**: parent-PRD tests for `EVENT_RATE_SCALE` still pass
under the new name; new cedar test exercises the stream→array→stream
round-trip closure body identity (`(events) -> events`); user-facing
`fast`/`slow` syntax continues to work (still routed via Phase 3 handler).

### Phase 2 — Stdlib lowering (user-facing migration)

**Goal**: Replace handler-based codegen for the 11 transforms with stdlib
akkado lowering. Add `event_random` + `pattern_query` primitives. Ship
`degrade` and `mask`.

| Step | Files |
|---|---|
| 2.1 | Add `event_random()` opcode + builtin. Per-event-onset fresh scalar in `[0, 1)`, deterministically seeded from `(pattern_id, cycle_index, event_index)`. |
| 2.2 | Add `pattern_active(p, t)` and `pattern_event(p, t)` opcodes + builtins. Active-at-time lookup (returns number) and full-event lookup (returns record). |
| 2.3 | Add stdlib akkado: `rev`, `palindrome`, `ply`, `linger`, `segment`, `zoom`, `compress`, `iter`, `iterBack`, `fast`, `slow`, `degrade`, `mask`. `akkado/stdlib/event_transforms.ak` (extend existing file). |
| 2.4 | Remove the 11 entries from `compile_pattern_for_transform`'s `is_transform` whitelist. `akkado/src/codegen_patterns.cpp:~2240–2262`. |
| 2.5 | Add akkado-level integration tests covering each migrated transform end-to-end. `akkado/tests/` (one file per family or per transform). |

**Phase 2 verification**: For each transform name, parity test compares
output events between (a) stdlib akkado lowering and (b) the legacy
handler-based codegen (still present in Phase 2). After parity passes for
all 11 transforms, Phase 3 begins.

### Phase 3 — Cleanup (delete the stopgap)

**Goal**: Hard cutover. Delete `EVENT_REORDER`, `EVENT_FANOUT`, all
kind-dispatched C++ handlers, kind enums, and tied cedar tests.

| Step | Files |
|---|---|
| 3.1 | Delete `EVENT_REORDER` (opcode 222) and `EVENT_FANOUT` (opcode 223) from `cedar/include/cedar/vm/instruction.hpp`. Renumber later opcodes if needed. |
| 3.2 | Delete `op_event_reorder`, `op_event_fanout`, kind enums, and helper functions from `cedar/include/cedar/opcodes/event_transforms.hpp`. |
| 3.3 | Delete kind constants in `cedar/include/cedar/opcodes/event_transform_encoding.hpp`. |
| 3.4 | Delete handler methods (`handle_rev_call`, `handle_palindrome_call`, `handle_ply_call`, `handle_linger_call`, `handle_zoom_call`, `handle_segment_call`, `handle_compress_call`, `handle_iter_call`, `handle_iter_back_call`, `handle_fast_call`, `handle_slow_call`) from `akkado/src/codegen_patterns.cpp`. |
| 3.5 | Delete cedar-level tests: `cedar/tests/test_event_reorder.cpp`, `cedar/tests/test_event_fanout.cpp`. |
| 3.6 | Audit `akkado/tests/test_reorder.cpp` (32 cases against old chain semantics): retain cases that still exercise the user-facing transform behavior; delete cases that exercise the old opcode internals directly. |
| 3.7 | Regenerate opcode metadata: `cd web && bun run build:opcodes`. |

**Phase 3 verification**: full test suite green; web build + WASM build
pass; manual smoke test of every migrated transform in the web IDE.

---

## 7. File-Level Changes

### Phase 1

| File | Change |
|---|---|
| `cedar/include/cedar/vm/instruction.hpp` | Rename `EVENT_RATE_SCALE` → `EVENT_SPEED` (opcode 221 retained). |
| `cedar/include/cedar/opcodes/event_transforms.hpp` | Rename `op_event_rate_scale` → `op_event_speed`. Add stream↔array bridge helpers. |
| `cedar/include/cedar/vm/dynarray.hpp` (or current location) | Extend element-type to support records. Bump record-typed capacity to `MAX_EVENTS_PER_CYCLE = 256`. |
| `akkado/include/akkado/builtins.hpp` | Add `event_speed` builtin. Extend factor type to accept pattern-of-numbers via `v"…"`. |
| `cedar/tests/test_event_speed.cpp` | **New.** Rename + cover `EVENT_SPEED` (replaces old `test_event_rate_scale.cpp` if it exists). |
| `cedar/tests/test_event_array_bridge.cpp` | **New.** Round-trip closure body identity, capacity tests, record-typed DynArray. |

### Phase 2

| File | Change |
|---|---|
| `akkado/stdlib/event_transforms.ak` | Add `fn rev`, `fn palindrome`, `fn ply`, `fn linger`, `fn segment`, `fn zoom`, `fn compress`, `fn iter`, `fn iterBack`, `fn fast`, `fn slow`, `fn degrade`, `fn mask`. |
| `akkado/src/codegen_patterns.cpp:~2240–2262` | Remove the 11 transform names from `is_transform` whitelist so they no longer compile-time-fold via handlers. |
| `cedar/include/cedar/vm/instruction.hpp` | Add `EVENT_RANDOM`, `PATTERN_ACTIVE`, `PATTERN_EVENT` opcodes. |
| `cedar/include/cedar/opcodes/event_transforms.hpp` | Add `op_event_random`, `op_pattern_active`, `op_pattern_event`. |
| `akkado/include/akkado/builtins.hpp` | Register `event_random`, `pattern_active`, `pattern_event`. |
| `akkado/tests/test_rev.cpp` (and per-transform siblings) | **New.** End-to-end integration tests for each migrated transform via stdlib akkado lowering. |
| `akkado/tests/test_degrade_mask.cpp` | **New.** `degrade` + `mask` parity / coverage. |
| `web/static/docs/concepts/array-of-events.md` | **New.** User-facing doc for the Array<Event> mental model + how userspace `fn` definitions work. |

### Phase 3

| File | Change |
|---|---|
| `cedar/include/cedar/vm/instruction.hpp` | Delete `EVENT_REORDER` (222) and `EVENT_FANOUT` (223). |
| `cedar/include/cedar/opcodes/event_transforms.hpp` | Delete `op_event_reorder`, `op_event_fanout`, kind enums, helper bodies. |
| `cedar/include/cedar/opcodes/event_transform_encoding.hpp` | Delete kind constants. |
| `akkado/src/codegen_patterns.cpp` | Delete the 11 `handle_*_call` handler methods. |
| `cedar/tests/test_event_reorder.cpp` | **Delete.** |
| `cedar/tests/test_event_fanout.cpp` | **Delete.** |
| `akkado/tests/test_reorder.cpp` | Audit + delete cases tied to old opcode internals; retain user-facing surface tests (if any remain unique). |
| `cedar/include/cedar/generated/opcode_metadata.hpp` | Regenerate via `cd web && bun run build:opcodes`. |

### Files that explicitly **stay unchanged**

| File | Why |
|---|---|
| `EVENT_MAP` / `EVENT_FILTER` opcodes | Substrate from parent PRD §3.1. Stays. |
| `BLOCK_CALL` / `FOREACH_EVENT` / subprogram table | Shipped closure infrastructure. Stays. |
| `akkado/stdlib/scale_quantize.ak` | Sibling PRD's stdlib. Unaffected by this work (its Phase 5 unblocks but isn't done here). |
| Per-event stdlib (`transpose`, `velocity`, `dur`, `bend`, `swing`, `early`, `late`, `voice`, `invert`) | Already lower via `event_map`/`event_filter`. Stays. |

---

## 8. Edge Cases

### 8.1 Capacity overflow

`ply(p, n)` where `n × upstream_events > MAX_EVENTS_PER_CYCLE` (256):

- **Behavior**: truncate at 256 events, emit warning `W… "event-array
  capacity exceeded; truncated to 256 events"`.
- **Rationale**: live-coding principle (coerce, don't fail). User can still
  hear something; the warning surfaces in the panel.

### 8.2 Empty event arrays

`filter` removes all events; downstream consumer reads an empty
`Array<Event>`:

- **Behavior**: zero events for the cycle. No error. `out` is silent for
  that cycle.

### 8.3 `palindrome` + `event_speed` composition (Q2 from draft)

`p |> palindrome |> fast(2)` doubles cycle_length, then halves it:

- **Behavior**: rely on today's `SequenceState.cycle_length` runtime
  chaining. Compositions that mix clock-control with array-rewriting
  may produce mistimed events at the seam.
- **v1 commitment**: ship as-is. The composition does not crash; the
  timing may drift. Per Non-Goals, explicit closure cycle_length
  metadata is deferred to a follow-up PRD. No verification test
  asserts exact timing for clock+array compositions in v1.
- **Documented limitation**: the user-facing concept doc lists
  compositions known to drift so live coders can work around them.

### 8.4 Live MIDI sources

User pipes a live MIDI input through `rev`:

- **Behavior**: compile-time warning `W… "whole-pattern transforms do not
  support live MIDI sources; transform applied to current cycle's known
  events only"`. No runtime error; transform applies to whatever events
  exist in the current cycle, which may be empty for live MIDI.

### 8.5 User-defined `fn rev` shadowing stdlib

User writes their own `fn rev(events) = …`:

- **Behavior**: standard akkado name resolution. User definition wins in
  its scope. No special diagnostic.
- **Rationale**: same as any other stdlib fn override; stdlib has no
  reserved status.

### 8.6 `event_random()` outside a closure body

User calls `event_random()` at top-level:

- **Behavior**: compile-time error `E… "event_random() requires an
  event context; call from inside event_map / event_filter / array
  closure"`. Per Non-Goals.
- **Why error, not coerce**: there is no defensible "what would this
  even mean" semantics at top-level — the function's whole identity
  is per-event-onset determinism, and the fallback considered in
  earlier drafts (per-audio-block PRNG seeded from
  `(0, cycle_index, block_index)`) has no real call site to justify
  it. Coerce-don't-fail applies when the user's intent is clear;
  here it is not.

### 8.7 `pattern_active(p, t)` / `pattern_event(p, t)` with `t ∉ [0, 1)`

User calls `pattern_active(p, 1.5)`:

- **Behavior**: wrap to `[0, 1)` (modulo 1.0). Same as pattern cycle
  semantics elsewhere. Same wrap applies to `pattern_event`.

### 8.8 Nested closures over `Array<Event>`

`events |> map((e) -> e) |> filter((e) -> event_random() > 0.5)`:

- **Behavior**: each stage builds + writes its own Array<Event> through the
  bridges. No special pooling for intermediates beyond DynArray buffer pool
  capacity.
- **Constraint**: ≤ 256 events per intermediate array.

---

## 9. Testing / Verification Strategy

### Phase 1 tests

- `cedar/tests/test_event_speed.cpp`: parity with old
  `test_event_rate_scale.cpp` (if exists), plus pattern-of-numbers factor
  via `v"…"` inline.
- `cedar/tests/test_event_array_bridge.cpp`:
  - Identity round-trip: `(events) -> events` produces byte-identical
    downstream events.
  - Empty array round-trip.
  - 256-event capacity exact-fit; 257-event overflow with truncation +
    warning.
  - Record-typed DynArray load/store of all OutputEvent fields.

### Phase 2 parity tests (per transform)

For each of `rev`, `palindrome`, `ply`, `linger`, `segment`, `zoom`,
`compress`, `iter`, `iterBack`, `fast`, `slow`:

- Compile same source via (a) handler path (Phase 3 still present), and
  (b) stdlib akkado lowering (Phase 2's new path). Compare downstream
  event streams over a long cycle window (≥ 300 simulated seconds per the
  experiment methodology). They MUST match.

For `degrade`:

- Statistical: over 1000 cycles, `degrade(p, 0.5)` drops 50% ± 5% of
  events.
- Seed determinism: same upstream events + same `event_random()` seed
  produce identical output.

For `mask`:

- `mask("1 0 1 0")` against a 4-step source keeps events at slots 0 and 2,
  drops slots 1 and 3.
- `mask(p)` where `p` is a complex pattern: events kept iff
  `pattern_active(p, e.time) > 0`.
- `pattern_event(p, t)` returns the expected record fields (note, vel,
  dur) for at least one composed example; empty record when no event
  active at `t`.

### Phase 3 cleanup tests

- Build green; all existing tests pass; web build + WASM build pass.
- Manual smoke test: each migrated transform tried in the web IDE; audio
  output matches expectation by ear.

### Long-run integration (per CLAUDE.md experiment methodology)

Each transform integration test that drives a sequence over time runs ≥
300 simulated seconds. Trace-only checks may run the long path; render a
shorter WAV separately for human listening.

---

## 10. Why deferred from parent PRD Phase 5

This PRD's substrate — record-typed `Array<Event>` DynArrays, closure
return-of-array, stream/array bridges, deterministic per-onset PRNG,
active-at-time / event-at-time pattern query primitives — is a substantial
design that warrants its own PRD with its own scoping, sequencing, and
open-questions resolution. Parent PRD Phase 5 is strictly per-event work
(chord-array support, `scale`/`key`/`voice`/`invert` on top of `event_map`,
`TypedValue` cleanup) and ships independently.

Once parent Phase 5 lands, this follow-up is the next major workstream and
retires the Phase 3+4 stopgap opcodes.

---

## 11. Open Questions

All original Q1–Q6 resolved in question rounds. Remaining minor:

- **OQ1.** Should Phase 3 also delete the now-unused
  `event_transform_encoding.hpp` kind constants file entirely? Likely yes
  if `EVENT_SPEED` doesn't use it. Confirm during Phase 3.
- **OQ2.** Exact `MAX_EVENTS_PER_CYCLE` value: 256 is the proposed default
  based on `ply(<16>, 16) = 256`. Revisit if a real-world live-coding session
  hits the cap.
- **OQ3.** `pattern_event(p, t)` when `p` produces overlapping events at
  `t`: return the first, the last, or merge? Default proposed: first
  active at `t` by `e.time`; finalize during Phase 2 implementation.

---

## 12. Related PRDs

- [`prd-runtime-event-transforms.md`](prd-runtime-event-transforms.md) —
  parent. §3.1 substrate stays at `EVENT_MAP` + `EVENT_FILTER`. Phase 3+4
  opcodes flagged stopgap; this PRD retires them.
- [`prd-scale-quantize.md`](prd-scale-quantize.md) — sibling. Its Phase 5
  (patternable `scale("<c:major a:minor>")`) is unblocked by this PRD's
  array substrate.
- [`prd-pattern-event-arrays.md`](prd-pattern-event-arrays.md) — SHIPPED
  2026-05-21. Provides the `DynArray` value type this PRD extends to
  record-typed elements.
- [`prd-runtime-functions-control-flow.md`](prd-runtime-functions-control-flow.md)
  — SHIPPED. Provides closure infrastructure (`BLOCK_CALL`,
  `FOREACH_EVENT`, subprogram table) that closures over `Array<Event>`
  build on.
