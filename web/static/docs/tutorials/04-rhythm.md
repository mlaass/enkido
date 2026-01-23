---
title: Rhythm & Patterns
order: 4
category: tutorials
keywords: [rhythm, pattern, beat, trigger, euclidean, sequencer, clock, tutorial]
---

# Rhythm & Patterns

Music is about time. In this tutorial, you'll learn to create rhythmic patterns, from basic beats to complex polyrhythms.

## The Clock

Everything in Akkado syncs to a global clock. The `trigger` function creates pulses:

```akk
// Trigger 4 times per beat (16th notes)
trigger(4)
```

Use triggers to control envelopes:

```akk
// Kick on quarter notes
osc("sin", 55) * ar(trigger(1), 0.01, 0.2) |> out(%, %)
```

## Building a Basic Beat

Combine different trigger rates for drum patterns:

```akk
// Simple kick and hi-hat
kick = osc("sin", 55) * ar(trigger(1), 0.005, 0.15)
hat = osc("noise") |> hp(%, 8000) * ar(trigger(4), 0.001, 0.03) * 0.3

kick + hat |> out(%, %)
```

## Euclidean Rhythms

The `euclid` function creates mathematically interesting patterns by distributing hits evenly:

```akk
// Tresillo (3 hits over 8 steps)
osc("sin", 55) * ar(euclid(3, 8), 0.01, 0.15) |> out(%, %)
```

Classic Euclidean patterns:
- `euclid(3, 8)` - Cuban tresillo
- `euclid(5, 8)` - West African bell pattern
- `euclid(7, 16)` - Brazilian samba

```akk
// Layered Euclidean rhythms
kick = osc("sin", 55) * ar(euclid(4, 16), 0.005, 0.15)
perc = osc("noise") |> bp(%, 2000, 4) * ar(euclid(5, 16), 0.001, 0.05) * 0.4

kick + perc |> out(%, %)
```

## Rotation

Rotate patterns to shift the accent:

```akk
// Rotated pattern - different feel, same hits
osc("sin", 55) * ar(euclid(3, 8, 1), 0.01, 0.15) |> out(%, %)
```

## Step Sequencing

Use `seq_step` to create melodic patterns:

```akk
// 4-note pattern
freq = mtof(48 + seq_step(2) * 5)
osc("saw", freq) * ar(trigger(2)) |> lp(%, 800) |> out(%, %)
```

## Combining Rhythm and Melody

Build a complete sequence:

```akk
// Bass line with rhythm
notes = 36 + seq_step(2) * 7  // MIDI notes
freq = mtof(notes)

osc("saw", freq)
    |> moog(%, 400 + ar(trigger(2)) * 800, 2)
    * ar(trigger(2), 0.01, 0.2)
    |> out(%, %)
```

## LFO for Movement

Add subtle motion with LFOs:

```akk
// Rhythmic with filter movement
osc("saw", 110)
    |> lp(%, 500 + lfo(0.25) * 1000)
    * ar(trigger(4))
    |> out(%, %)
```

## A Complete Drum Pattern

```akk
// Kick drum
kick = osc("sin", 55 * (1 + ar(trigger(1), 0.001, 0.02) * 2))
    * ar(trigger(1), 0.005, 0.2)

// Snare
snare = osc("noise") |> bp(%, 1000, 2)
    * ar(euclid(2, 8, 4), 0.001, 0.1) * 0.5

// Hi-hat
hat = osc("noise") |> hp(%, 8000)
    * ar(trigger(4), 0.001, 0.03) * 0.2

// Ride
ride = osc("noise") |> bp(%, 6000, 8)
    * ar(euclid(3, 8), 0.001, 0.1) * 0.15

kick + snare + hat + ride |> out(%, %)
```

## Polyrhythms

Create tension with conflicting rhythms:

```akk
// 3 against 4
bass = osc("saw", 55) * ar(euclid(3, 12), 0.01, 0.15) |> lp(%, 400)
perc = osc("noise") |> hp(%, 4000) * ar(euclid(4, 12), 0.001, 0.05) * 0.3

bass + perc |> out(%, %)
```

## Mini-Notation Patterns

For melodic sequences, use mini-notation:

```akk
// Melodic pattern
pat("c3 e3 g3 c4") |> ((f) ->
    osc("saw", f) |> lp(%, 800) * ar(trigger(4))
) |> out(%, %)
```

## Next Steps

You now have the tools to create complex rhythmic music! Explore further:
- [Mini-Notation](../mini-notation/basics.md) for pattern syntax
- [Effects](../builtins/reverbs.md) for space and depth
- [Dynamics](../builtins/dynamics.md) for polish

Congratulations on completing the tutorial series! You're ready to make music with Akkado.
