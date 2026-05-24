# PRD: Pattern-Array Transforms — Whole-Pattern Operations as Stdlib Akkado

> **Status: NOT STARTED — design draft.** Filed 2026-05-24 as the follow-up
> to [`prd-runtime-event-transforms.md`](prd-runtime-event-transforms.md)
> Phase 5. Captures the architectural debt from Phases 3 and 4 of the parent
> PRD and proposes the correct substrate for whole-pattern transforms.

## 1. Problem

The parent PRD's foundational principle is:

> **Opcodes are for primitive operations and DSP work, not for language constructs. Per-event-stream transforms expressible from generic closure primitives belong in stdlib akkado.**

Phases 1 + 2a + 2b honored this principle: per-event transforms (`transpose`, `velocity`, `dur`, `bend`, `swing`, `early`, `late`, etc.) lower to stdlib `event_map` / `event_filter` closure calls on top of two generic opcodes (`EVENT_MAP`, `EVENT_FILTER`).

**Phases 3 and 4 drifted.** They shipped three kind-dispatched mega-opcodes:

| Opcode | Kinds | Used by |
|---|---|---|
| `EVENT_RATE_SCALE` (Phase 3, opcode 221) | none (takes a factor signal) | `fast`, `slow` |
| `EVENT_REORDER` (Phase 4, opcode 222) | REV / PALINDROME / ITER / ITER_BACK / ZOOM / COMPRESS | `rev`, `palindrome`, `iter`, `iterBack`, `zoom`, `compress` |
| `EVENT_FANOUT` (Phase 4, opcode 223) | PLY / LINGER / SEGMENT | `ply`, `linger`, `segment` |

Each opcode is a `switch` over hardcoded named transforms with per-kind C++ bodies in `cedar/include/cedar/opcodes/event_transforms.hpp`. **Adding a new transform name requires adding a new kind enum value and a new C++ function — exactly what stdlib lowering was meant to eliminate.** This violates the principle: these named transforms are language constructs, not DSP primitives.

The opcodes are functionally working and shipping audio. They're a stopgap, not a regression in user-visible behavior — but they're architectural debt that this PRD captures and proposes a fix for.

## 2. Why these transforms didn't fit Phase 2b's `event_map` mold

The Phase 2b stdlib lowering pattern is:

```akkado
fn transpose(events, n) = event_map(events, (e) -> {note: e.note + n})
```

A closure receives one event at a time, returns one event. The runtime walks the upstream events and applies the closure per-event.

**`rev`, `palindrome`, `ply`, `linger`, `segment`, `fast`, `slow` fundamentally cannot work this way.** They need either:

- The **full array of events** for a cycle (rev reverses N events; palindrome concatenates forward+reverse; ply emits N copies per input).
- **Clock control** over the upstream pattern's event generation rate (`fast(p, 2)` means "produce twice as many cycles worth of events per outer cycle" — that's clock manipulation, not stream rewriting).

A per-event-closure substrate cannot express these.

## 3. Proposed substrate

A pattern or MIDI seq is best conceptualized as an **array of event records**, where each record has a defined event-field shape (`{note, vel, dur, time, chance, notes, freqs, bend, at}`). Whole-pattern transforms then become array-rewriting closures:

```akkado
fn rev(events)        = events |> reverse
fn palindrome(events) = events ++ reverse(events)
fn ply(events, n)     = events |> flat_map((e) -> repeat({...e, dur: e.dur / n}, n))
fn linger(events, f)  = events |> filter((e) -> e.time < f) |> map((e) -> {...e, dur: e.dur / f})
fn zoom(events, start, end) = events |> filter((e) -> e.time + e.dur > start and e.time < end)
                                    |> map((e) -> {time: (max(e.time, start) - start) / (end - start), ...})
fn compress(events, start, end) = events |> map((e) -> {time: start + e.time * (end - start), ...})
fn iter(events, n)    = events |> map((e) -> {time: (e.time - (e.cycle mod n) / n + 1) mod 1, ...})
```

`fast` / `slow` are different — they're clock-control, not array-rewriting. Two viable approaches:

- **A.** Keep a single-purpose `event_retime(events, factor)` primitive (renamed from today's `EVENT_RATE_SCALE`, no kind switch — already clean as a single-purpose factor opcode). Stdlib `fast` and `slow` are 1-line wrappers. This honors the principle: `event_retime` is a clock primitive, not a language construct.
- **B.** Express `fast`/`slow` purely via array operations (compress events into a sub-cycle and let the upstream re-fire). Significant semantic change: `fast(p, 2)` would compress 1 cycle's events into half a cycle and not repeat — diverges from Strudel/Tidal.

(A) is recommended.

### 3.1 Substrate primitives this PRD needs to add

| Primitive | Surface | Notes |
|---|---|---|
| `Array<Event>` value type | Akkado | Extends Pattern Event Arrays' `DynArray` to record-typed elements |
| Closure return-of-array-of-records | Akkado closure body + Cedar runtime | Builds on Phase 5's chord-array DynArray write path (parent PRD §3.3) |
| Stream → array bridge (per cycle) | Cedar runtime | Same-block reads upstream `SequenceState.output` into `Array<Event>` |
| Array → stream bridge (per cycle) | Cedar runtime | Writes the closure's returned `Array<Event>` into downstream `OutputEvents` |
| `event_retime(events, factor)` (renamed `EVENT_RATE_SCALE`) | Cedar opcode | Clock-control primitive for `fast`/`slow`. Single-purpose, no kind switch. |

Standard array primitives (`reverse`, `map`, `filter`, `flat_map`, `concat`, `repeat`) become polymorphic over `Array<Event>` — most already work for `Array<Number>` via Pattern Event Arrays.

## 4. Migration plan

Once the substrate above lands:

1. Add stdlib `fn rev`, `fn palindrome`, `fn ply`, `fn linger`, `fn segment`, `fn zoom`, `fn compress`, `fn iter`, `fn iterBack`, `fn fast`, `fn slow` to `akkado/stdlib/event_transforms.ak`.
2. Remove the entries from `compile_pattern_for_transform`'s `is_transform` whitelist (`akkado/src/codegen_patterns.cpp:~2240`) so they no longer compile-time-fold.
3. Delete the corresponding handlers in `codegen_patterns.cpp` (`handle_rev_call`, `handle_palindrome_call`, `handle_ply_call`, `handle_linger_call`, `handle_zoom_call`, `handle_segment_call`, `handle_compress_call`, `handle_iter_call`, `handle_iter_back_call`, `handle_fast_call`, `handle_slow_call`).
4. Delete `EVENT_REORDER` and `EVENT_FANOUT` opcodes from `cedar/include/cedar/vm/instruction.hpp`, their bodies and kind enums from `cedar/include/cedar/opcodes/event_transforms.hpp`, and the kind constants in `event_transform_encoding.hpp`.
5. Rename `EVENT_RATE_SCALE` → `EVENT_RETIME` (single-purpose, no kind switch). Add `fn event_retime` builtin.
6. Delete Phase 3+4 tests tied to the kind-dispatched opcodes (`cedar/tests/test_event_reorder.cpp`, `cedar/tests/test_event_fanout.cpp`, `akkado/tests/test_reorder.cpp`); rewrite coverage against the stdlib akkado forms.

## 5. Open questions

- **Q1.** Does `event_collect` (whole-cycle dispatch) buffer cross-cycle for live MIDI sources, or only operate within the current block's known events?
- **Q2.** How does `palindrome(p)` (which doubles cycle_length) interact with a downstream `event_retime`? Does the cycle_length propagation still work via runtime chaining, or do we need explicit cycle-length-aware closure metadata?
- **Q3.** Should `fast`/`slow` accept a closure as the factor (e.g. per-cycle pattern of factors), or only a signal/scalar? Today's `EVENT_RATE_SCALE` accepts a signal.
- **Q4.** Patternable `scale("<c:major a:minor>")` — sibling PRD `prd-scale-quantize.md` Phase 5 (deferred there too) — sits on this PRD's array substrate. Confirm.
- **Q5.** Naming: `event_retime` vs `event_rate` vs `event_clock_scale`?
- **Q6.** `degrade` + `mask` stdlib fns (parent PRD Phase 5 Commit H, deferred 2026-05-24). Both need primitives that aren't currently reachable from an `event_map` / `event_filter` closure body: a per-event-onset scalar `random()` and a `pattern_active_at_time(p, t)` lookup. Today's akkado `random(n)` returns an array (PRD prd-pattern-event-arrays) — not a fresh scalar per event onset; and there's no `pattern_active_at_time` primitive at all. Land these alongside the array substrate so `degrade(events, p) = event_filter(events, (e) -> random() > p)` and `mask(events, p) = event_filter(events, (e) -> pattern_active_at_time(p, e.time) > 0)` become one-liners.

## 6. Why deferred from parent PRD Phase 5

This PRD's substrate (array-of-records value type, closure return-of-array, stream/array bridges, clock primitive) is a substantial design that warrants its own PRD with its own scoping, sequencing, and open-questions resolution. Phase 5 of the parent PRD is **strictly per-event** work (chord-array support, `scale`/`key`/`voice`/`invert` on top of `event_map`, `TypedValue` cleanup) and ships independently.

Once Phase 5 lands, this follow-up is the next major workstream and retires the Phase 3+4 stopgap opcodes.

## 7. Related PRDs

- [`prd-runtime-event-transforms.md`](prd-runtime-event-transforms.md) — parent. §3.1 substrate stays at `EVENT_MAP` + `EVENT_FILTER`. Phase 3+4 opcodes are flagged stopgap pending this follow-up.
- [`prd-scale-quantize.md`](prd-scale-quantize.md) — sibling. Patternable scale/key argument (its Phase 5) is gated on this PRD's array substrate.
- [`prd-pattern-event-arrays.md`](prd-pattern-event-arrays.md) — SHIPPED 2026-05-21. Provides the `DynArray` value type this PRD extends to record-typed elements.
- [`prd-runtime-functions-control-flow.md`](prd-runtime-functions-control-flow.md) — SHIPPED. Provides the runtime closure infrastructure (`BLOCK_CALL`, `FOREACH_EVENT`, subprogram table) that closures over `Array<Event>` build on.
