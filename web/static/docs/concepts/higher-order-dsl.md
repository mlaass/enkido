---
title: Higher-Order DSL — each_voice
category: concepts
order: 9
keywords: [each_voice, higher-order, foreach, poly, event, lambda, per-event, instrument, FOREACH_EVENT]
---

# Higher-Order DSL — `each_voice`

`each_voice` runs an instrument lambda **once per pattern event** and mixes
every iteration's output. It is the higher-order generalization of `poly()`:
where `poly()` takes a fixed 3-argument instrument function, `each_voice` takes
an inline lambda and applies it across a dynamic event stream.

## Usage

```akkado
n"c4 e4 g4" |> each_voice(@, (v) -> osc("sin", v) * 0.3) |> out(@)
```

The lambda's single parameter (`v` above) is the **per-event frequency** — the
pitch of the event currently being iterated. The lambda body is any
signal-producing expression; `each_voice` sums the bodies of all events active
in the block and returns a stereo signal.

The operand must be an event stream — a mini-notation pattern (`n"…"`, `s"…"`,
…), `seq(...)`, or a MIDI input source. Passing a plain signal is a compile
error (`E242`).

## How it relates to `poly()`

Both compile to the same runtime mechanism — the `FOREACH_EVENT` opcode backed
by a subprogram block table (see
[Cedar Runtime Functions PRD](https://github.com/) §4.3). The difference is the
*allocator*:

- `poly()` uses the **VOICE_POOL** allocator — gate-on allocates a voice,
  gate-off releases it, and overlapping notes each get their own voice with a
  release tail. Use it for held/sustained instruments.
- `each_voice` uses the **PER_ITERATION** allocator — every event in the block
  fires its body exactly once, with no gate lifecycle. Use it for one-shot
  per-event sounds.

Both isolate per-iteration DSP state (oscillator phase, filter memory) so
voices/iterations don't bleed into each other.

## v1 limitations

- The lambda parameter is the per-event **frequency signal**. Full event-record
  access (`v.vel`, `v.dur`, `v.gate`) is a planned follow-up; for now reach for
  `poly()` when you need the gate and velocity signals.
- `each()` (side-effecting iteration) and `fold()` (shared accumulator) are not
  yet exposed — the underlying allocators exist in the engine but have no
  surface syntax yet.
