---
title: Microtonal Notation
category: mini-notation
order: 2
keywords: [microtonal, tuning, edo, quarter tone, 17edo, 24edo, 31edo, tune,
           xenharmonic, cents, micro-step, step up, step down, caret]
group: sequencing
subgroup: patterns
icon: KeyRound
tagline: EDO tunings and per-note micro-step adjustments.
---

# Microtonal Notation

Akkado supports microtonal pitch notation through micro-step operators (`^`, `v`, `+`) and the `tune()` function, which selects how those operators map to frequency.

## Micro-step operators

Micro-step operators go between the note name/accidentals and the octave number:

| Operator | Meaning | Example |
|----------|---------|---------|
| `^` | Step up | `c^4`: C4 raised by one EDO step |
| `v` | Step down | `cv4`: C4 lowered by one EDO step |
| `+` | Step up (alias for `^`) | `c+4`: same as `c^4` |

Operators stack: `c^^4` raises by two steps, `cvvv4` lowers by three. Mixed operators cancel: `c^v4` = `c4`.

Standard accidentals (`#`, `b`, `x`) remain unchanged and always shift by semitones. They can combine with micro-steps:

```akk
// Sharp + micro step up
pat("c#^4")   // C#4 + one EDO step

// Flat + micro step down
pat("Bbv4")   // Bb4 - one EDO step
```

## The tune() function

`tune()` wraps a pattern and sets the EDO tuning for micro-step resolution:

```akk
// 31-EDO: each ^ step = 38.7 cents
tune("31edo", pat("c4 c^4 c^^4")) |> ((f) -> osc("sin", f) * ar(trigger(3))) |> out(@)

// 24-EDO: each ^ step = 50 cents (quarter tone)
tune("24edo", pat("a4 a^4 a^^4")) |> ((f) -> osc("sin", f) * ar(trigger(3))) |> out(@)
```

Without `tune()`, the default is 12-EDO where each `^` step equals one semitone (100 cents), so `c^4` is the same as `c#4`.

Accepted formats: `"31edo"`, `"31-edo"`, `"31-EDO"`.

## How EDO steps work

An EDO system divides the octave (1200 cents) into N equal steps. The `^`/`v` operators move by one step in the active tuning:

| EDO | Step size | `^` = | Notes |
|-----|-----------|-------|-------|
| 12 | 100.0 cents | semitone | Standard Western tuning |
| 17 | 70.6 cents | ~3/4 semitone | Narrower intervals, wider major thirds |
| 19 | 63.2 cents | ~2/3 semitone | Near-just minor thirds |
| 24 | 50.0 cents | quarter tone | Arabic/Turkish maqam music |
| 31 | 38.7 cents | ~1/3 semitone | Near-just thirds and sevenths |
| 53 | 22.6 cents | Holdrian comma | Near-just for all intervals |

**Important**: Notes without micro-steps sound identical in all tunings. `pat("c4 e4 g4")` produces the same frequencies whether wrapped in `tune("31edo", ...)` or not. `tune()` only changes how `^` and `v` are resolved.

## Micro-step arithmetic

Sharps/flats and micro-steps are independent axes:

```akk
// # shifts the MIDI note by a semitone (always 100 cents)
// ^ shifts by one EDO step (depends on tuning)
tune("31edo", pat("c4 c#4 c^4 c#^4"))
// c4   = 261.6 Hz (Middle C)
// c#4  = 277.2 Hz (+ 100 cents, semitone)
// c^4  = 267.5 Hz (+ 38.7 cents, one 31-EDO step)
// c#^4 = 283.3 Hz (+ 100 + 38.7 = 138.7 cents)
```

## Example: Fur Elise retuned

The opening motif of Beethoven's Fur Elise, the E/D# trill, is a useful testbed for microtonal retuning because the character of a trill depends on interval size.

### Standard (12-EDO)

The E-D# trill spans exactly 100 cents:

```akk
pat("e5 d#5 e5 d#5 e5 b4 d5 c5 a4@2 ~ c4 e4 a4 b4@2 ~ e4 g#4 b4 c5@2")
    |> ((f) -> osc("tri", f) * ar(trigger(20), 0.005, 0.3))
    |> out(@)
```

### 17-EDO: the narrow trill

In 17-EDO the semitone shrinks to 70.6 cents. D#-to-E becomes tighter and more restless. The trill wants to resolve but can't quite settle:

```akk
tune("17edo", pat("e5 d#5 e5 d#5 e5 b4 d5 c5 a4@2 ~ c4 e4 a4 b4@2 ~ e4 g#4 b4 c5@2"))
    |> ((f) -> osc("tri", f) * ar(trigger(20), 0.005, 0.3))
    |> out(@)
```

Add micro-step ornaments for a xenharmonic flavor. `e^5` and `ev5` nudge notes by 70.6 cents:

```akk
tune("17edo", pat("e5 d#5 e^5 d#5 ev5 d#5 e5 b4 d5 c5 a4@2"))
    |> ((f) -> osc("tri", f) * ar(trigger(12), 0.005, 0.3))
    |> out(@)
```

### 31-EDO: microtonal cascade

In 31-EDO each step is 38.7 cents. Instead of a binary E/D# trill, replace it with a micro-step descent. Each `v` lowers by a third of a semitone, creating a portamento-like effect:

```akk
tune("31edo", pat("e5 ev5 evv5 ev5 e5 b4 d5 c5 a4@2 ~ c4 e4 a4 b4@2"))
    |> ((f) -> osc("tri", f) * ar(trigger(14), 0.005, 0.3))
    |> out(@)
```

Deeper cascade with three micro-steps, a glide spanning ~116 cents:

```akk
tune("31edo", pat("e5 ev5 evv5 evvv5 evv5 ev5 e5 b4 d5 c5 a4@2"))
    |> ((f) -> osc("tri", f) * ar(trigger(11), 0.005, 0.3))
    |> out(@)
```

### Side-by-side: the trill

Minimal examples to hear the difference in trill character:

```akk
// 12-EDO: 100-cent trill (standard semitone)
pat("e5 d#5 e5 d#5 e5 d#5 e5 d#5")
    |> ((f) -> osc("sin", f) * ar(trigger(8), 0.005, 0.1))
    |> out(@)
```

```akk
// 17-EDO: 70.6-cent trill (narrow, restless)
tune("17edo", pat("e5 d#5 e5 d#5 e5 d#5 e5 d#5"))
    |> ((f) -> osc("sin", f) * ar(trigger(8), 0.005, 0.1))
    |> out(@)
```

```akk
// 31-EDO: 38.7-cent micro-step shimmer
tune("31edo", pat("e5 ev5 e5 ev5 e5 ev5 e5 ev5"))
    |> ((f) -> osc("sin", f) * ar(trigger(8), 0.005, 0.1))
    |> out(@)
```

## Tips

- **Start with 24-EDO**: quarter tones are the most intuitive entry point to microtonal music.
- **`^^` in 31-EDO** (77.4 cents) is close to a quarter tone, useful as a mental anchor.
- **`tune()` only affects `^`/`v`**: standard notes, sharps, and flats are unchanged across all tunings.
- **Samples are unaffected**: `bd`, `sn`, and other sample tokens ignore micro-step operators.
- **Velocity still works**: `c^4:0.5` is a micro-stepped note at half velocity.

Related: [Mini-Notation Basics](basics), [Oscillators](../builtins/oscillators)
