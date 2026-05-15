---
title: Rhythm & Patterns
order: 4
category: tutorials
keywords: [rhythm, pattern, beat, trigger, euclidean, sequencer, clock, tutorial, select, conditional, gate, threshold, and, or, not]
---

# Rhythm & Patterns

This tutorial covers rhythmic patterns, from basic beats to polyrhythms.

## The clock

Everything in Akkado syncs to a global clock. The `trigger` function creates pulses:

```akk
// Trigger 4 times per beat (16th notes)
trigger(4)
```

Use triggers to control envelopes:

```akk
// Kick on quarter notes
osc("sin", 55) * ar(trigger(1), 0.01, 0.2) |> out(@)
```

## Building a basic beat

Combine different trigger rates for drum patterns:

```akk
// Simple kick and hi-hat
kick = osc("sin", 55) * ar(trigger(1), 0.005, 0.15)
hat = osc("noise") |> hp(@, 8000) * ar(trigger(4), 0.001, 0.03) * 0.3

kick + hat |> out(@)
```

## Euclidean rhythms

The `euclid` function distributes hits evenly across a number of steps:

```akk
// Tresillo (3 hits over 8 steps)
osc("sin", 55) * ar(euclid(3, 8), 0.01, 0.15) |> out(@)
```

Classic Euclidean patterns:
- `euclid(3, 8)` - Cuban tresillo
- `euclid(5, 8)` - West African bell pattern
- `euclid(7, 16)` - Brazilian samba

```akk
// Layered Euclidean rhythms
kick = osc("sin", 55) * ar(euclid(4, 16), 0.005, 0.15)
perc = osc("noise") |> bp(@, 2000, 4) * ar(euclid(5, 16), 0.001, 0.05) * 0.4

kick + perc |> out(@)
```

## Rotation

Rotate patterns to shift the accent:

```akk
// Rotated pattern - different feel, same hits
osc("sin", 55) * ar(euclid(3, 8, 1), 0.01, 0.15) |> out(@)
```

## Step sequencing

Use `pat()` mini-notation for melodic patterns:

```akk
// 4-note melodic pattern
pat("c3 e3 g3 c4") |> ((f) ->
    osc("saw", f) * ar(trigger(4)) |> lp(@, 800)
) |> out(@)
```

## Combining rhythm and melody

A complete sequence:

```akk
// Bass line with rhythm
pat("c2 g2 d#2 a#1") |> ((f) ->
    osc("saw", f)
        |> moog(@, 400 + ar(trigger(2)) * 800, 2)
        * ar(trigger(2), 0.01, 0.2)
) |> out(@)
```

## LFO for movement

Add motion with LFOs:

```akk
// Rhythmic with filter movement
osc("saw", 110)
    |> lp(@, 500 + lfo(0.25) * 1000)
    * ar(trigger(4))
    |> out(@)
```

## A complete drum pattern

```akk
// Kick drum
kick = osc("sin", 55 * (1 + ar(trigger(1), 0.001, 0.02) * 2))
    * ar(trigger(1), 0.005, 0.2)

// Snare
snare = osc("noise") |> bp(@, 1000, 2)
    * ar(euclid(2, 8, 4), 0.001, 0.1) * 0.5

// Hi-hat
hat = osc("noise") |> hp(@, 8000)
    * ar(trigger(4), 0.001, 0.03) * 0.2

// Ride
ride = osc("noise") |> bp(@, 6000, 8)
    * ar(euclid(3, 8), 0.001, 0.1) * 0.15

kick + snare + hat + ride |> out(@)
```

## Polyrhythms

Create tension with conflicting rhythms:

```akk
// 3 against 4
bass = osc("saw", 55) * ar(euclid(3, 12), 0.01, 0.15) |> lp(@, 400)
perc = osc("noise") |> hp(@, 4000) * ar(euclid(4, 12), 0.001, 0.05) * 0.3

bass + perc |> out(@)
```

## Mini-notation patterns

For melodic sequences, use mini-notation:

```akk
// Melodic pattern
pat("c3 e3 g3 c4") |> ((f) ->
    osc("saw", f) |> lp(@, 800) * ar(trigger(4))
) |> out(@)
```

## Conditional triggers

So far every gate has come from a pattern function (`trigger`, `euclid`, `pat`). You can also build gates from **conditionals**: comparisons and logic operators that work sample-by-sample.

### Threshold gates

Compare a continuous signal against a value to produce a `0.0` / `1.0` gate:

```akk
// Open the kick only on the louder half of an LFO
loud = lfo(0.5) > 0
osc("sin", 55) * ar(loud, 0.005, 0.15) |> out(@)
```

The `>` operator outputs `1.0` whenever its left-hand side is greater than its right-hand side, and `0.0` otherwise. Anything you can use as a trigger source you can build this way.

### Selecting between two voices

`select(cond, a, b)` is a sample-rate ternary: it outputs `a` whenever `cond > 0`, otherwise `b`:

```akk
// Alternate timbre on every other beat
flip = pat("1 0 1 0")
voice = select(flip, osc("saw", 110), osc("sqr", 110))
voice * ar(trigger(2), 0.005, 0.2) |> out(@)
```

There's no `?:` ternary in Akkado. `select` is the canonical form.

### Combining gates with `&&` and `||`

Logical AND (`&&`) fires only when both inputs are truthy. Logical OR (`||`) fires when at least one is truthy.

```akk
// OR: layer two patterns into a single gate
g1 = pat("1 0 0 0")
g2 = pat("0 0 1 0")
combined = g1 || g2  // "1 0 1 0"
osc("sin", 55) * ar(combined, 0.005, 0.15) |> out(@)
```

```akk
// AND: accent only when a beat lines up with a slow LFO peak
loud = lfo(0.5) > 0.5
hit  = trigger(4)
accent = loud && hit
osc("noise") * ar(accent, 0.001, 0.05) |> out(@)
```

The prefix `!` operator inverts a gate, useful for "play during the rests":

```akk
// A pad that fills the space between drum hits
gate = trigger(4)
sustain = !gate
osc("sin", 220) * sustain * 0.2 |> out(@)
```

For the full operator precedence table and the complete list of comparison/logic builtins, see [Operators](../reference/language/operators.md) and [Conditionals & Logic](../reference/language/conditionals.md).

## Next steps

- [Mini-Notation](../mini-notation/basics.md) for pattern syntax
- [Pattern Modulation](./06-pattern-modulation.md): patterns as values: `bend(notes, v"<0 0.5 -0.5>")`, `e.cutoff` custom properties, scalar arithmetic
- [Effects](../builtins/reverbs.md) for space and depth
- [Dynamics](../builtins/dynamics.md) for polish
