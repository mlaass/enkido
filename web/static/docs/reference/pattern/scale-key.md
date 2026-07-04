---
title: Scale & Key
category: pattern
order: 2
keywords: [scale, key, quantize, quantization, degree, degree-mapping, scale-degree, tonality, mode, major, minor, dorian, mixolydian, ionian, aeolian, harmonic_minor, pentatonic, major_pentatonic, minor_pentatonic, chromatic, snap, in-key, music-theory, root, tonic, octave, user-defined, intervals, interval-list, custom-scale, key_deltas, note_num]
---

# Scale & Key

Two pattern transforms constrain melodies to a musical scale. They have
clearly separated jobs:

- **`key("d:minor")`** — *quantization*. Snaps each event's note to the
  nearest pitch in the scale. Use it to tame chromatic, generative, or
  MIDI input.
- **`scale("d3:minor")`** — *degree mapping*. Reinterprets each event's
  number as a zero-indexed scale degree: `0` is the root, `1` the second
  scale tone, and so on. Use it to write melodies as numbers.

```akkado
// key — snap a chromatic run into D minor
n"[c4 c#4 d4 d#4 e4 f4]" |> key(@, "d:minor") as e
    |> sine(e.freq) |> out(@)

// key — keep a randomly transposed melody in key
n"[c4 e4 g4]".transpose(random(12)) |> key(@, "a:minor") as e
    |> saw(e.freq) |> out(@)

// scale — integers become scale tones: D3 F3 A3 C4
n"[0 2 4 6]" |> scale(@, "d3:minor") as e
    |> sine(e.freq) |> out(@)

// method-call form works too
n"[0 2 4]".scale("c:dorian") |> sine(@.freq) |> out(@)
```

Both accept the pipe form (`|> key(@, "…")` — note the explicit `@` hole)
and the method form (`.key("…")`).

## The name string

`"root[octave]:type"`, lowercase, sharp-spelled roots
(`c c# d d# e f f# g g# a a# b`). Multi-word types use underscores:
`"c:harmonic_minor"`.

| | `key` | `scale` |
|---|---|---|
| Octave digit | Accepted and **ignored** — `"d2:minor"` = `"d:minor"` | **Used** — sets where degree 0 lands; supported octaves 2–4 |
| Omitted octave | n/a | Degree 0 lands in octave 3 (`"d:minor"` → D3) |

### Available scale types

`major` (alias `ionian`), `minor` (alias `aeolian`), `dorian`,
`mixolydian`, `harmonic_minor`, `major_pentatonic`, `minor_pentatonic`,
`chromatic` — each × 12 chromatic roots.

An **unknown name is not an error**: the transform passes events through
unchanged. If a `key`/`scale` call seems to do nothing, check the name
string for typos (e.g. a flat spelling like `"eb:minor"` — use
`"d#:minor"`).

## `key` — quantization rules

- Snaps to the nearest scale tone in semitones; an exact tie rounds
  **down** (in C major, `c#4` → `c4`, `d#4` → `d4`).
- In-scale notes pass through unchanged.
- Octave-agnostic — the result lands in whichever octave is closest.
- **Chord events pass through untouched** — only single-note events are
  quantized.
- Fractional (microtonal) input notes round half-up to an integer MIDI
  note before snapping.

## `scale` — degree-mapping rules

- Degrees beyond the scale length wrap the octave: with a 7-tone scale,
  degree `7` is the root one octave up.
- Negative degrees map below the root (degree `-1` of `"d3:minor"` is
  C3).
- Fractional degrees round to the nearest integer degree — no microtonal
  degrees.
- `scale` never snaps; quantization is `key`'s job.

## User-defined scales

Both transforms also take an explicit root + semitone interval list in
place of a name string, for scales outside the built-in catalog:

```akkado
// hand-rolled minor pentatonic rooted at D2
n"[0 1 2 3 4]" |> scale(@, "d2", [0,3,5,7,10]) as e
    |> sine(e.freq) |> out(@)

// custom pentatonic quantizer (octave in the root ignored, as always)
n"[c4 c#4 d4]" |> key(@, "e", [0,2,4,7,9]) as e
    |> saw(e.freq) |> out(@)
```

The root is a note-name string (`"d2"`, `"f#"`) or a plain MIDI number
(`50`). The interval list must be a compile-time-constant array of
semitone offsets. Values are not validated — anything is taken mod 12,
so an unusual list simply produces the scale it describes.

## How they work

Both are generated stdlib `fn`s in `akkado/stdlib/scale_quantize.ak`
built on [`event_map`](../../concepts/event-transforms) — no special
opcode. They compose with every other event transform:

```akkado
n"[0 2 4 7]" |> scale(@, "a2:minor_pentatonic") as e
    |> @ .transpose(12).velocity(0.7)
    |> saw(e.freq) * e.vel |> out(@)
```

The scale/key name must be a **string literal** at the call site — a
runtime-computed name won't select a scale (the dispatcher folds at
compile time).
