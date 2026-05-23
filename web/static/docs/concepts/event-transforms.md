---
title: Event Transforms
category: concepts
order: 12
keywords: [event-transforms, event_map, event_filter, transpose, velocity, dur, bend, aftertouch, early, late, swing, swingBy, stdlib, stream, modifier, pattern-transform]
---

# Event Transforms

A pattern modifier in Akkado is a function that takes an **event stream** — a `Pattern` or a MIDI `EventSource` — and returns a new one with per-event fields rewritten. They compose with the pipe operator and chain freely.

```akkado
n"c4 e4 g4".transpose(7).velocity(0.4) |> osc("sin", @.freq) * @.vel |> out(@)
```

Under the hood every per-field modifier is a **one-line stdlib `fn`** sitting on top of two primitive builtins: `event_map` and `event_filter`. The mapping is direct — `transpose(p, n)` *is* `event_map(p, (e) -> {note: e.note + n})`. You can read the actual definitions in [`akkado/stdlib/event_transforms.ak`](https://github.com/) (one file, ~15 lines).

This means you can write your own modifier and have it work everywhere a builtin does — patterns, chord patterns, MIDI streams, chained transforms — with no compiler change.

## The two primitives

### `event_map(events, closure)`

Rewrite every event's fields with a closure.

```akkado
n"c4 e4 g4".event_map((e) -> {note: e.note + 12, vel: e.vel * 0.5})
```

The closure receives an **event record** with these readable fields: `freq`, `vel`, `dur`, `note`, `chance`, `time`, `gate`, `trig`. It returns a record literal naming the fields to **overlay** — unspecified fields pass through unchanged.

The writable fields are `note`, `vel`, `dur`, `time`, `chance`, `bend`, `at`. Writing `note` rewrites the primary `midi_note` *and* every chord voice's `notes[]` and `values[]` — `transpose(7)` shifts an entire chord stack, not just the root.

Closure bodies are pure: stateful opcodes (oscillators, filters, state cells, `out()`) are not allowed inside. Arithmetic, math builtins, and references to outer signals (the LFO buffer is sampled at each event's onset) are fine.

### `event_filter(events, predicate)`

Drop events whose closure returns `≤ 0`.

```akkado
n"c4 d4 e4 f4 g4".event_filter((e) -> e.vel - 0.3)  // keep only events with vel > 0.3
```

## Writing your own modifier

Any `fn` annotated with `events: stream` accepts a Pattern or EventSource and can plug into a pipe chain via UFCS.

```akkado
fn arp_up(events: stream, steps) -> {
    event_map(events, (e) -> {note: e.note + (cycle_count() mod steps) * 12})
}

n"c4 g4".arp_up(3) |> osc("saw", @.freq) |> out(@)
```

The `events: stream` annotation is part of [parameter type annotations](parameter-type-annotations). Without it, a user `fn` parameter that receives a polyphonic Pattern would error with `E160`; the annotation tells the analyzer this fn is an event-stream consumer.

## What ships in stdlib

| Modifier | Field written | Math |
|---|---|---|
| `transpose(events, n)` | `note` (coupled across chord voices) | `+ n` |
| `velocity(events, v)` | `vel` | `* v` |
| `dur(events, d)` | `dur` | `* d` |
| `bend(events, b)` | `bend` | `= b` |
| `aftertouch(events, a)` | `at` | `= a` |
| `early(events, t)` | `time` | `(time − t) mod 1.0` |
| `late(events, t)` | `time` | `(time + t) mod 1.0` |
| `swing(events, grid=8)` | `time` | 1/3-amount swing on `grid`-slice grid |
| `swingBy(events, amount, grid=8)` | `time` | `amount` swing on `grid`-slice grid |

Structural transforms (`rev`, `palindrome`, `ply`, `linger`, `zoom`, `segment`, `compress`, `iter`, `iterBack`) and rate scalers (`fast`, `slow`) remain C++ builtins — they don't fit the per-event-record-rewrite shape. The PRD `prd-runtime-event-transforms.md` Phases 3–5 migrate them too.

## Shadowing built-in modifiers

User `fn`s shadow stdlib ones. To redefine `transpose` with a different convention (say, scale degrees instead of semitones), just write it:

```akkado
scale = [0, 2, 4, 5, 7, 9, 11]  // major scale steps

fn transpose(events: stream, deg) -> {
    event_map(events, (e) -> {note: e.note + scale[deg]})
}

n"c4 e4 g4".transpose(2) |> osc("sin", @.freq) |> out(@)
```

This overrides the stdlib definition for the rest of the file.

## Time-shift wraparound

`early` and `late` operate on cycle phase `[0, 1)`. The event-time field wraps via `fmod` and re-sorts at the runtime opcode boundary, so events that move past the cycle edge land in the right block position automatically:

```akkado
n"c4 e4 g4 b4".late(0.25)  // shifts every event +0.25 cycles; the b4 wraps to time 0
```

For cross-cycle reasoning, `e.cycle` exposes the absolute cycle count.

## Composing with MIDI

Because both Patterns and MIDI EventSources share the `OutputEvents` wire format, the same transforms apply uniformly:

```akkado
midi("ctrl1").transpose(12).velocity(0.7) |> poly(@, instr, 8)
```

## See also

- [Parameter Type Annotations](parameter-type-annotations) — the `events: stream` annotation that makes user-fn modifiers possible
- [Higher-Order DSL](higher-order-dsl) — `each_voice`, `each`, `reduce` for per-event instrument and accumulation patterns
- [PRD prd-runtime-event-transforms](https://github.com/) — the full design including the deferred rate-scaling and structural-transform migrations
