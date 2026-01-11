---
title: Mini-Notation Basics
category: mini-notation
keywords: [mini-notation, pattern, sequence, rhythm, pitch, chord, rest, tidal, strudel]
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
pat("c4 e4 g4 c5") |> ((f) -> sin(f) * ar(trigger(4))) |> out(%, %)
```

## Rests

Use `~` or `_` for silence:

```akk
// Rest on beat 3
pat("c4 e4 ~ g4")
```

## Chords

Specify chords with `:` notation:

```akk
// Major chord
pat("c4:maj")   // C, E, G

// Minor seventh
pat("a3:min7")  // A, C, E, G

// Available chord types:
// maj, min, dom7, maj7, min7, dim, aug, sus2, sus4
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

## Repetition

Use `*` to repeat elements:

```akk
// Repeat c4 four times
pat("c4*4 e4")
```

## Pattern Functions

### pat()

Basic pattern playback:

```akk
pat("c4 e4 g4") |> ((f) -> sin(f)) |> out(%, %)
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
    saw(f) |> lp(%, 1500) * ar(trigger(4))
) |> out(%, %)
```

```akk
// Chord progression
pat("c3:maj e3:min a3:min g3:maj") |> ((f) ->
    saw(f) |> lp(%, 800) * ar(trigger(1), 0.1, 0.5)
) |> out(%, %)
```

```akk
// Rhythmic pattern with rests
pat("c4 ~ e4 ~ g4 ~ e4 ~") |> ((f) ->
    tri(f) * ar(trigger(8), 0.01, 0.1)
) |> out(%, %)
```

Related: [Sequencing](../builtins/sequencing), [Closures](../language/closures)
