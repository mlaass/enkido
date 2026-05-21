---
title: Chords
category: mini-notation
order: 3
keywords: [chord, chords, notes, freqs, dynamic array, arpeggiator, voicing, voicings, anchor, mode, addVoicings, drop2, drop3, close, open, inversion, triad, seventh, sixth, ninth, eleventh, thirteenth, extended, polyphony, poly, soundfont, E410, E181, M, maj, m, min, dim, aug, sus, sus2, sus4, "5", power, "6", m6, min6, "7", dom7, M7, maj7, "^", "^7", m7, min7, "-7", dim7, o7, m7b5, "0", aug7, "+7", mM7, "m^7", minmaj7, "9", M9, maj9, m9, min9, add9, add2, "11", m11, "13", "-", "+", o]
group: sequencing
subgroup: patterns
icon: Music2
tagline: Triads, sevenths, sixths, extended chords, and voicings.
subfeatures:
  - name: Triads
    anchor: triad
    tagline: Major, minor, dim, aug, sus.
    snippet: 'c"C Am F G"'
  - name: Sevenths
    anchor: seventh
    tagline: Maj7, m7, dom7, dim7, mM7, m7b5.
    snippet: 'c"Cmaj7 Am7 Dm7 G7"'
  - name: Extended
    anchor: extended
    tagline: 9ths, 11ths, 13ths, adds.
    snippet: 'c"Cmaj9 Am11 D13"'
  - name: Voicings
    anchor: voicing
    tagline: Inversions and spread voicings.
    snippet: '.voicing("open")'
  - name: Notes
    anchor: notes
    tagline: Chord notes as a dynamic array.
    snippet: 'e.notes.step(trigger(8))'
---

# Chords

Two paths to chordal patterns: the `chord()` function with chord-symbol literals (`Am7`, `Cmaj7`), and inline chord brackets in `n"…"` strings (`[c4 e4 g4]`). For voice leading, the `anchor`, `mode`, `voicing`, and `addVoicings` transforms reshape chord events into musical voicings.

## chord

**Chord pattern** - Parse a string of chord symbols into a polyphonic event stream.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| str   | string | -       | Chord symbols separated by spaces |

```akk
// Major chord
chord("C")          // C major triad

// Minor seventh
chord("Am7")        // A minor seventh

// Extended chord
chord("Cmaj9")      // C major 9th — 5 voices

// Chord progression
chord("Am C Dm G")  // One chord per beat
```

Both syntaxes — `chord("C^7")` and `c"C^7"` mini-notation — share one
canonical quality table (`akkado::CHORD_INTERVALS`). Whatever works in one
works in the other.

## triad

A **triad** is a 3-note chord: root, third, fifth. Bare letter names (`C`, `Am`, `F`) parse as major triads. All quality aliases:

| Suffix              | Intervals     | Meaning |
|---------------------|---------------|---------|
| (none), `M`, `maj`  | `0 4 7`       | Major triad |
| `m`, `min`, `-`     | `0 3 7`       | Minor triad |
| `dim`, `o`          | `0 3 6`       | Diminished |
| `aug`, `+`          | `0 4 8`       | Augmented |
| `sus2`              | `0 2 7`       | Suspended 2nd |
| `sus4`, `sus`       | `0 5 7`       | Suspended 4th (bare `sus` = sus4) |
| `5`                 | `0 7`         | Power chord (no third) |

```akk
// Lots of triad flavors
c"C Dm Eo Faug Gsus4 A- B+ E5" |> soundfont(@, "gm", 0) |> out(@)
```

## seventh

A **seventh** chord adds a 7th degree:

| Suffix                   | Intervals       | Meaning |
|--------------------------|-----------------|---------|
| `7`, `dom7`              | `0 4 7 10`      | Dominant 7 |
| `M7`, `maj7`, `^`, `^7`  | `0 4 7 11`      | Major 7 |
| `m7`, `min7`, `-7`       | `0 3 7 10`      | Minor 7 |
| `dim7`, `o7`             | `0 3 6 9`       | Diminished 7 (fully dim) |
| `m7b5`, `0`              | `0 3 6 10`      | Half-diminished 7 |
| `aug7`, `+7`             | `0 4 8 10`      | Augmented 7 |
| `mM7`, `m^7`, `minmaj7`  | `0 3 7 11`      | Minor-major 7 |

```akk
// Jazz progression — same chord, three ways
chord("Cmaj7 Am7 Dm7 G7")
chord("CM7 A-7 D-7 G7")        // alt-symbol style
chord("C^7 Am7 Dm7 G7")        // Strudel-style ^
```

## sixth

A **sixth** chord stacks a 6th instead of a 7th — open, lush, bossa-nova flavor:

| Suffix         | Intervals     | Meaning |
|----------------|---------------|---------|
| `6`            | `0 4 7 9`     | Major 6 |
| `m6`, `min6`   | `0 3 7 9`     | Minor 6 |

```akk
chord("C6 Am6 Dm6 G6") |> soundfont(@, "gm", 0) |> out(@)
```

## extended

**Extended** chords stack 9ths, 11ths, or 13ths above a 7th. They produce 5–6 voices per step.

| Suffix             | Intervals             | Meaning |
|--------------------|-----------------------|---------|
| `9`                | `0 4 7 10 14`         | Dominant 9 |
| `M9`, `maj9`       | `0 4 7 11 14`         | Major 9 |
| `m9`, `min9`       | `0 3 7 10 14`         | Minor 9 |
| `add9`             | `0 4 7 14`            | Major triad + 9 (no 7th) |
| `add2`             | `0 2 4 7`             | Major triad + 2 (close-voiced) |
| `11`               | `0 4 7 10 14 17`      | Dominant 11 |
| `m11`              | `0 3 7 10 14 17`      | Minor 11 |
| `13`               | `0 4 7 10 14 21`      | Dominant 13 |

```akk
// Modal jazz vamp — full extensions
c"Cmaj9 Am11 D13 G13" |> soundfont(@, "gm", 0) |> out(@)
```

## inline

**Inline chords** in pattern strings use square brackets: every note plays simultaneously rather than in sequence.

```akk
// C major as inline chord
n"[c4 e4 g4]"
```

```akk
// Mixed sequence and chord
n"c4 [c4 e4 g4] e4 g4"
```

## polyphony

Chord patterns produce events with multiple voices per step. How those voices reach the audio output depends on what consumes the pattern:

- **Internally polyphonic instruments** (`soundfont`) accept chord patterns directly. Every chord voice is dispatched to a separate voice slot inside the instrument, and the per-voice outputs are summed automatically.

  ```akk
  c"CM Am Dm G" |> soundfont(@, "gm", 0) |> out(@)
  ```

- **Mono synths** (oscillators, filters, single-voice DSP) require an explicit `poly(N, instrument_fn)` wrapper to allocate voices. A chord patched directly into a mono synth raises **E410** because there is no implicit voice allocation:

  ```akk
  // ✗ E410 — chord into a mono synth has no voice allocator
  c"CM Am Dm G" |> osc("saw", @freq) |> out(@)

  // ✓ poly() wraps the synth in N parallel voices
  fn lead({freq, gate, vel}) ->
    osc("saw", freq) |> lp(@, 2000 * adsr(gate)) |> @ * vel
  c"CM Am Dm G" |> poly(@, lead, 8) |> out(@)
  ```

> **Voice limit**: chord events carry up to **16 voices** per step (`MAX_VALUES_PER_EVENT`). The largest built-in quality is `13` at 6 voices, so every chord in the standard table fits comfortably. Custom voicings registered via `addVoicings()` are truncated past 16 intervals.

## notes

`poly()` and `soundfont()` *sound* a chord; `notes()` and `freqs()` hand you the chord as **data** you can transform. They take a pattern and return a **dynamic array** — an array whose length is the current event's chord size, varying per event at runtime.

| Function   | Returns                                    |
|------------|--------------------------------------------|
| `notes(e)` | dynamic array of MIDI numbers              |
| `freqs(e)` | dynamic array of frequencies (Hz)          |

The method-style forms `e.notes` and `e.freqs` are equivalent and usually read better:

```akk
n"[c4,e4,g4] g3 [a3,c4]" as e
e.notes        // dynamic array: [60,64,67], then [55], then [57,60]
e.freqs        // same chords in Hz
len(e.notes)   // runtime signal: 3, then 1, then 2
```

`len()` is polymorphic — a constant for static arrays, a runtime signal for dynamic ones. Indexing wraps by default, so a free-running counter walks the chord regardless of its size. Combined with `step()` (see [State Cells](../builtins/state)) that is a complete arpeggiator:

```akk
fn step (arr, trig) -> arr[counter(trig)]

n"[c4,e4,g4] [a3,c4,e4] g3 [b3,d4,f#4]" as e
e.notes.step(trigger(8))     // walk the current chord on each 8th
  |> mtof(@) |> osc("saw", @) |> out(@)
```

`map()` over a dynamic array transposes every chord note and stays dynamic:

```akk
map(e.notes, (v) -> v + 7)   // the chord up a fifth — still a dynamic array
```

Dynamic arrays cannot auto-fan-out across a stateful UGen (the chord size is not known at compile time): `osc("sin", e.freqs)` is a compile error (**E181**) directing you to `poly()`. For a *fixed*-size transform, map over a **static** array instead — that fans out at compile time:

```akk
n"c4 e4 g4" as e
sum(osc("saw", mtof(map([0, 4, 7], (i) -> e.note + i))))   // every note → a triad
```

## anchor

`anchor(pattern, "c4")` sets the MIDI anchor note for chord voicing. Note names accept letter + optional accidental (`#` / `b`) + octave (`c4`, `F#3`, `Bb-1`).

```akk
// Voice the chords around c4
chord("Am C G F").anchor("c4")
```

## mode

`mode(pattern, "below")` sets the chord voicing mode:

- `below`: all chord notes ≤ anchor
- `above`: all chord notes ≥ anchor
- `duck`: closest to anchor, avoiding the anchor itself
- `root`: root in bass octave near anchor, rest stacked near anchor

```akk
// Voice-led progression, top note ≤ c4
chord("Am C G F").anchor("c4").mode("below")
  |> mtof(@)
  |> osc("saw", @)
  |> out(@)
```

## voicing

`voicing(pattern, "drop2")` applies a named voicing dictionary. Built-in dictionaries:

- `close`: all notes within an octave
- `open`: wide spacing, root on bottom
- `drop2`: second-from-top dropped one octave
- `drop3`: third-from-top dropped one octave

```akk
// Drop-2 voicing on a jazz progression
chord("Cmaj7 Am7 Dm7 G7").voicing("drop2")
```

## drop2

`drop2`: the second-highest note dropped down an octave. Classic guitar/piano jazz voicing.

## drop3

`drop3`: the third-highest note dropped down an octave. Wider spread than drop2.

## close

`close`: all notes packed within an octave. Tight, pad-like voicing.

## open

`open`: root in bass, upper notes stacked widely. Broad, brassy voicing.

## addVoicings

`addVoicings("name", {quality: [intervals], ...})` registers a custom voicing dictionary by chord-quality name.

```akk
// Custom piano-jazz voicing
addVoicings("piano-jazz", {
    M: [0, 4, 7, 11, 14],
    m: [0, 3, 7, 10, 14]
})
chord("CM Am Dm G").voicing("piano-jazz")
```

Related: [Mini-Notation Basics](basics), [Sequencing](../builtins/sequencing), [poly](../builtins/polyphony#poly)
