---
title: Higher-Order DSL — each_voice, each, reduce
category: concepts
order: 9
keywords: [each_voice, each, reduce, higher-order, foreach, poly, event, lambda, per-event, instrument, FOREACH_EVENT, accumulator]
---

# Higher-Order DSL — `each_voice`, `each`, `reduce`

Three operators run a lambda **once per pattern event**. They are the
higher-order generalization of `poly()`: where `poly()` takes a fixed
3-argument instrument function, these take an inline lambda and apply it across
a dynamic event stream. All three compile to the `FOREACH_EVENT` opcode backed
by a subprogram block table.

| operator | what the lambda does | result |
|---|---|---|
| `each_voice(input, lambda)` | produces a signal per event | mixed stereo signal |
| `each(input, lambda)` | calls `out()` itself per event | nothing (a sink) |
| `reduce(input, fn, init)` | threads an accumulator | the accumulator signal |

The operand must be an event stream — a mini-notation pattern (`n"…"`, `s"…"`,
…), `seq(...)`, or a MIDI input source. Passing a plain signal is a compile
error (`E242`).

## `each_voice` — mix a signal per event

```akkado
n"c4 e4 g4" |> each_voice(@, (n) -> sine(n.freq) * ar(n.trig, 0.01, 0.3))
            |> out(@)
```

`each_voice` sums the bodies of all events active in the block and returns a
stereo signal.

## `each` — side-effecting per-event sink

```akkado
n"c4 e4 g4" |> each(@, (n) -> saw(n.freq) * 0.25 |> out(@))
```

`each` does not aggregate a result — the lambda body calls `out()` itself, and
every event's output accumulates onto the global bus. `each(...)` has no value;
it cannot be piped further.

## `reduce` — accumulator over the event stream

```akkado
n"c2 c3 c4" |> reduce(@, (acc, n) -> acc + n.freq, 0.0) as total |> ...
```

`reduce(collection, fn, init)` threads an accumulator through every event. The
lambda takes two parameters — the running accumulator and the per-event record
— and returns the next accumulator. `reduce` returns the accumulator as a
mono signal.

`reduce` is **polymorphic**: over a compile-time array it unrolls at compile
time (`reduce([1, 2, 3], (a, b) -> a + b, 0)`); over a pattern it runs at audio
rate via `FOREACH_EVENT`. The accumulator is re-seeded from `init` every block,
so `reduce` over a pattern is a per-block reduction starting from `init` (which
may itself be a runtime signal).

## Event-record lambda parameter

The per-event lambda parameter is an **event record**. Bare use (`osc("sin",
n)`) reads the event frequency; field access reads the named field:

| field (with aliases) | meaning |
|---|---|
| `n.freq` (`frequency`, `pitch`, `f`) | event frequency in Hz |
| `n.vel` (`velocity`, `v`) | velocity, 0–1 |
| `n.dur` (`duration`) | event duration in beats |
| `n.note` (`midi`) | MIDI note number |
| `n.chance` | per-event probability field |
| `n.time` (`t0`, `start`) | event onset position in the cycle |
| `n.gate` (`g`) | gate signal — high for the event's window |
| `n.trig` (`trigger`) | 1-sample pulse at the event onset |

`gate` and `trig` are synthesized per iteration from the event's timing, so a
per-event body can host gated/triggered envelopes (`adsr(n.gate, …)`,
`ar(n.trig, …)`). Fields the event model does not carry (`type`, `sample_id`,
`phase`) are a compile error (`E408`).

## How it relates to `poly()`

`poly()` and `each_voice`/`each`/`reduce` compile to the same `FOREACH_EVENT`
opcode; the difference is the *allocator*:

- `poly()` uses the **VOICE_POOL** allocator — gate-on allocates a voice,
  gate-off releases it, and overlapping notes each get their own voice with a
  release tail. Use it for held/sustained polyphonic instruments.
- `each_voice` / `each` use the **PER_ITERATION** allocator — every event in
  the block fires its body once. There is no voice-stealing or release-tail
  bookkeeping; `n.gate`/`n.trig` are synthesized from event timing.
- `reduce` uses the **SHARED** allocator — one accumulator threaded through
  every event.

All three isolate per-iteration DSP state (oscillator phase, filter memory) so
iterations don't bleed into each other.
