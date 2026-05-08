---
title: Chords
category: mini-notation
order: 3
keywords: [chord, chords, voicing, voicings, anchor, mode, addVoicings, drop2, drop3, close, open, inversion, m7, maj7, dim, aug, sus2, sus4, triad, seventh, polyphony, poly, soundfont, E410]
group: sequencing
subgroup: patterns
icon: Music2
tagline: Triads, sevenths, voicings, and drops.
subfeatures:
  - name: Triads
    anchor: triad
    tagline: Major, minor, dim, aug.
    snippet: 'c"C"'
  - name: Sevenths
    anchor: seventh
    tagline: Maj7, m7, dom7, dim7.
    snippet: 'c"Cmaj7 Am7 Dm7 G7"'
  - name: Voicings
    anchor: voicing
    tagline: Inversions and spread voicings.
    snippet: '.voicing("open")'
---

# Chords

Two paths to chordal patterns: the `chord()` function with chord-symbol literals (`Am7`, `Cmaj7`), and inline chord brackets in `pat()` strings (`[c4 e4 g4]`). For voice leading, the `anchor`, `mode`, `voicing`, and `addVoicings` transforms reshape chord events into musical voicings.

## chord

**Chord pattern** - Parse a string of chord symbols into a polyphonic event stream.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| str   | string | -       | Chord symbols separated by spaces |

```akk
// Major chord
chord("C")      // C major triad

// Minor seventh
chord("Am7")    // A minor seventh

// Chord progression
chord("Am C Dm G")  // One chord per beat
```

## triad

A **triad** is a 3-note chord: root, third, fifth. Bare letter names (`C`, `Am`, `F`) parse as triads. Quality suffixes:

| Suffix    | Meaning |
|-----------|---------|
| (none)    | Major triad |
| `m`       | Minor triad |
| `dim`     | Diminished |
| `aug`     | Augmented |
| `sus2`    | Suspended 2 |
| `sus4`    | Suspended 4 |

## seventh

A **seventh** chord adds a 7th degree. Quality suffixes:

| Suffix    | Meaning |
|-----------|---------|
| `7`       | Dominant 7 |
| `maj7`    | Major 7 |
| `m7`      | Minor 7 |

```akk
// Jazz progression
chord("Cmaj7 Am7 Dm7 G7") |> mtof(%) |> osc("saw", %) |> out(%, %)
```

## inline

**Inline chords** in pattern strings use square brackets: every note plays simultaneously rather than in sequence.

```akk
// C major as inline chord
pat("[c4 e4 g4]")
```

```akk
// Mixed sequence and chord
pat("c4 [c4 e4 g4] e4 g4")
```

## polyphony

Chord patterns produce events with multiple voices per step. How those voices reach the audio output depends on what consumes the pattern:

- **Internally polyphonic instruments** (`soundfont`) accept chord patterns directly. Every chord voice is dispatched to a separate voice slot inside the instrument, and the per-voice outputs are summed automatically.

  ```akk
  c"CM Am Dm G" |> soundfont(@, "gm", 0) |> out(@, @)
  ```

- **Mono synths** (oscillators, filters, single-voice DSP) require an explicit `poly(N, instrument_fn)` wrapper to allocate voices. A chord patched directly into a mono synth raises **E410** because there is no implicit voice allocation:

  ```akk
  // ✗ E410 — chord into a mono synth has no voice allocator
  c"CM Am Dm G" |> osc("saw", @.freq) |> out(@, @)

  // ✓ poly() wraps the synth in N parallel voices
  fn lead(freq, gate, vel) =
    osc("saw", freq) |> lp(@, 2000 * adsr(gate)) |> @ * vel
  c"CM Am Dm G" |> poly(@, lead, 8) |> out(@, @)
  ```

> **Voice limit**: chord events carry up to **4 voices** per step (`MAX_VALUES_PER_EVENT`). Quality dictionaries with more than 4 intervals (e.g. a 5-note maj9 voicing) are truncated. Bumping this constant is tracked as a follow-up.

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
  |> mtof(%)
  |> osc("saw", %)
  |> out(%, %)
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
addVoicings("piano-jazz", {M: [0, 4, 7, 11, 14], m: [0, 3, 7, 10, 14]})
chord("CM Am Dm G").voicing("piano-jazz")
```

Related: [Mini-Notation Basics](basics), [Sequencing](../builtins/sequencing), [poly](../builtins/polyphony#poly)
