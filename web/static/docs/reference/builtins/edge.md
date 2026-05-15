---
title: Edge Primitives
category: builtins
order: 12
keywords: [gateup, gatedown, counter, edge, rising, falling, trigger, accumulator, sah, sample-and-hold]
group: tools
subgroup: state-io
icon: Triangle
tagline: Rising/falling edge detection and sample-and-hold.
---

# Edge Primitives

Edge primitives detect transitions in trigger / control signals and accumulate counts. All four variants share one backing opcode (`EDGE_OP`) for cache locality and consistent state semantics.

## sah

**Sample and hold.** Latches the current value of `in` whenever `trig` crosses zero from below. The held value is broadcast to every sample of the output until the next rising edge.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Signal to sample |
| trig  | signal | -       | Trigger signal (rising edge captures) |

```akk
// Sample noise once per beat
noise() |> sah(@, trigger(1)) |> out(@)
```

## gateup

**Rising-edge detector.** Outputs `1.0` on the single sample where `sig` transitions from `<= 0` to `> 0`, otherwise `0.0`.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| sig   | signal | -       | Signal to monitor |

```akk
// Trigger an envelope on every rising edge of an LFO
gate = gateup(lfo(2))
ar(gate, 0.01, 0.3) * osc("sin", 880) |> out(@)
```

## gatedown

**Falling-edge detector.** Outputs `1.0` on the single sample where `sig` transitions from `> 0` to `<= 0`, otherwise `0.0`. Useful for note-off events.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| sig   | signal | -       | Signal to monitor |

```akk
// Fire a percussive blip on note-off
release = gatedown(gate_signal)
ar(release, 0.001, 0.05) * noise() |> out(@)
```

## counter

**Accumulator.** Increments by `1.0` on every rising edge of `trig`. Optionally resets to `start` (default `0`) on every rising edge of `reset`. If `trig` and `reset` fire on the same sample, **reset wins** (the increment is skipped that sample).

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| trig  | signal | -       | Increment trigger (rising edge) |
| reset | signal | (none)  | Optional reset trigger (rising edge) |
| start | signal | 0       | Optional reset target value |

```akk
// Bare counter, never resets
n = counter(trigger(1))

// Reset every 4 beats
n = counter(trigger(4), beat(4))

// Reset to 7 instead of 0
n = counter(trigger(4), beat(4), 7)

// Index into an array of MIDI notes
notes = [60, 64, 67, 72]
notes[counter(trigger(4))] |> mtof(@) |> sine(@) |> out(@)
```

`counter` is the natural pair with [array indexing](../language/arrays.md), since `ARRAY_INDEX` wraps by default: an unbounded counter index always produces an in-range lookup.

Related: [state](state.md), [Method calls](../language/methods.md)
