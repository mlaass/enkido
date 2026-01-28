---
title: Mini-Notation Basics
category: mini-notation
order: 1
keywords: [mini-notation, pattern, sequence, rhythm, pitch, chord, rest, tidal, strudel, modifier, speed, slow, repeat, chance, alternation]
---

# Mini-Notation

Mini-notation is a compact syntax for describing musical patterns, inspired by TidalCycles and Strudel.

## Pitch Tokens

Specify notes with letter name and octave:

```akk
// Single notes
pat("c4")           // Middle C
pat("f#3")          // F sharp, octave 3
pat("Bb5")          // B flat, octave 5
```

## Sequences

Space-separated notes play in sequence over one cycle:

```akk
// Four notes per cycle
pat("c4 e4 g4 c5") |> ((f) -> osc("sin", f) * ar(trigger(4))) |> out(%, %)
```

## Rests

Use `~` or `_` for silence:

```akk
// Rest on beat 3
pat("c4 e4 ~ g4")
```

## Chords

Use the `chord()` function with standard chord symbols:

```akk
// Major chord
chord("C")      // C major triad

// Minor seventh
chord("Am7")    // A minor seventh

// Chord progression
chord("Am C Dm G")  // One chord per beat

// Available chord qualities:
// m (minor), maj, 7, maj7, m7, dim, aug, sus2, sus4
```

## Inline Chords

Play multiple notes simultaneously with square brackets:

```akk
// C major as inline chord
pat("[c4 e4 g4]")
```

## Grouping

Use square brackets to subdivide time:

```akk
// Second beat subdivided
pat("c4 [e4 f4] g4 c5")
```

## Polyrhythms

Comma separates parallel patterns:

```akk
// 3 against 4
pat("c4 e4 g4, c3 g3 c3 g3")
```

## Modifiers

Modifiers change how pattern elements are played. **Important**: Modifiers must be inside the pattern string.

### Speed (`*n`)

Repeat an element n times in its time slot:

```akk
// Repeat c4 four times (fits 4 notes in 1 slot)
pat("c4*4 e4")

// Speed up a group
pat("[c4 e4]*2 g4")  // [c4 e4 c4 e4] in first half
```

### Slow (`/n`)

Stretch an element to span n times its normal duration:

```akk
// Stretch pattern to 2 cycles
pat("[c4 e4 g4 b4]/2")  // Events at beats 0, 2, 4, 6

// Slow down individual note
pat("c4/2 e4 g4")
```

### Repeat (`!n`)

Replicate an element n times (extends the sequence):

```akk
// a!2 b = a a b (3 elements, each takes 1/3 of time)
pat("c4!2 e4")  // c4 appears twice, then e4

// Compare with speed (*n) which compresses:
// a*2 b = [a a] b (2 elements, first has 2 notes in half the time)
pat("c4*2 e4")  // c4 plays twice quickly, then e4
```

### Chance (`?n`)

Set probability (0-1) of the element playing:

```akk
// 50% chance each note plays
pat("c4?0.5 e4?0.5 g4?0.5")
```

### Weight/Elongation (`@n`)

Adjust temporal weight (how much time an element takes relative to siblings):

```akk
// First note takes twice as much time
pat("c4@2 e4 g4")  // c4: 50%, e4: 25%, g4: 25%

// Create uneven rhythms
pat("bd@3 sn")    // bd: 75%, sn: 25%
```

## Alternation

Use angle brackets for elements that alternate each cycle:

```akk
// Plays c4 on cycle 1, e4 on cycle 2, g4 on cycle 3, then repeats
pat("<c4 e4 g4>")

// Alternate between groups
pat("<[c4 e4] [g4 b4]>")
```

## Modifier Placement

**Critical**: Pattern modifiers must be inside the pattern string:

```akk
// CORRECT: modifier inside string
pat("[bd sn]/2")   // Pattern plays over 2 cycles

// WRONG: modifier outside string
pat("bd sn")/2     // This divides the SIGNAL by 2, not the pattern!
```

## Pattern Functions

### pat()

Basic pattern playback:

```akk
pat("c4 e4 g4") |> ((f) -> osc("sin", f)) |> out(%, %)
```

### seq()

Sequence with explicit timing:

```akk
seq("c4 e4 g4 c5")
```

### note()

Single note with full control:

```akk
note("c4")
```

## Practical Examples

```akk
// Simple melody
pat("c4 e4 g4 e4") |> ((f) ->
    osc("saw", f) |> lp(%, 1500) * ar(trigger(4))
) |> out(%, %)
```

```akk
// Chord progression
chord("C Em Am G") |> ((f) ->
    osc("saw", f) |> lp(%, 800) * ar(trigger(1), 0.1, 0.5)
) |> out(%, %)
```

```akk
// Rhythmic pattern with rests
pat("c4 ~ e4 ~ g4 ~ e4 ~") |> ((f) ->
    osc("tri", f) * ar(trigger(8), 0.01, 0.1)
) |> out(%, %)
```

Related: [Sequencing](../builtins/sequencing), [Closures](../language/closures)
