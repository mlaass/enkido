---
title: Unison
category: builtins
order: 18
keywords: [unison, supersaw, fatten, fat, detune, voices, width, phase, stack, stereo, spread, voice, cluster]
group: sequencing
subgroup: voicing
icon: Copy
tagline: Multiply a single note into a stack of detuned, panned, phase-spread voices.
subfeatures:
  - name: Unison
    anchor: unison
    tagline: Fatten a single note into N stereo-spread, detuned voices.
    snippet: 'unison(440, 1, 1, (f, g, v, e) -> saw(f, e.phase), voices: 5, detune: 0.3)'
---

# Unison

Unison takes a single note and multiplies it across N voices that play the
same pitch with slightly different detune, stereo pan, and starting phase.
Classic supersaw, fattened pads, doubled leads. It's a pure userspace stdlib
function — no special opcode — built on top of `linspace`, `map`,
`pan`, and the variadic stereo `sum`.

Unlike [`poly`](polyphony#poly), which allocates a *new* voice per incoming
note, unison plays one note at a time and spreads it across voices. For
polyphonic unison, wrap unison inside a 3-arg `poly` instrument (see the last
example).

## unison

**Unison** — Stack N detuned, panned, phase-spread copies of a single note.
Returns a stereo signal.

| Param      | Type     | Default | Description |
|------------|----------|---------|-------------|
| freq       | signal   | -       | Note frequency in Hz |
| gate       | signal   | -       | Envelope gate (0/1 or amplitude pulse) |
| vel        | signal   | -       | Velocity (0–1) |
| instrument | function | -       | A 4-arg closure `(freq, gate, vel, ext) -> signal` invoked per voice |
| voices     | literal  | 2       | Voice count, compile-time integer (sane range 1–16) |
| detune     | signal   | 0.5     | Per-voice frequency spread in **semitones** (audio-rate) |
| width      | signal   | 0.5     | Per-voice stereo pan spread, ±width (audio-rate) |
| phase      | signal   | 0       | Per-voice initial-phase spread, in cycles (audio-rate) |

`voices` is statically unrolled at compile time, so it must be an integer
literal — it cannot come from a `param()` slider. Voice counts above ~16
work but grow the bytecode linearly; if you need more, drop to a builtin
opcode (future PRD).

`detune`, `width`, and `phase` are ordinary audio-rate signals. Modulating
them with a slider, an LFO, or a pattern works without recompilation.

The per-voice offsets come from a symmetric linear spread of
`linspace(-1, +1, voices)`. With `voices = 4`, the unit positions are
`[-1, -1/3, 1/3, 1]`; multiplying by `detune` / `width` / `phase` gives the
per-voice values. The special case `voices = 1` returns a single centered,
undetuned, in-phase voice.

The voice sum is scaled by `1 / sqrt(voices)` so the overall RMS stays
roughly constant as the voice count grows — the supersaw convention. A
4-voice unison is about 2× louder than a single voice (not 4×), and you
can sweep `voices` from 2 up to 16 without re-staging gain.

### The instrument convention

The `instrument` argument is a 4-arg function:

```akk
fn voice(freq, gate, vel, ext) -> ...
```

The 4th `ext` argument is a record describing the voice's position in the
stack, so the instrument can vary its sound per voice without unison having
to grow more parameters:

| Field          | Description                                            |
|----------------|--------------------------------------------------------|
| `ext.idx`      | Voice index, `0 .. voices - 1`                          |
| `ext.count`    | Total voice count (= `voices`)                          |
| `ext.detune_st`| This voice's detune offset in semitones                 |
| `ext.pan`      | This voice's pan position, `-1 .. +1`                   |
| `ext.phase`    | This voice's initial phase offset, `0 .. phase` cycles |

Pass `ext.phase` straight into an oscillator's `phase` input to get the
per-voice phase staggering for free:

```akk
fn voice(freq, gate, vel, ext) -> saw(freq, ext.phase)
```

### Examples

**Minimal pattern-driven fat lead:**

```akk
fn voice(freq, gate, vel, ext) ->
    saw(freq, ext.phase) * adsr(gate, 0.01, 0.2, 0.7, 0.3) * vel

n"c4 e4 g4" as e
  |> unison(e.freq, e.gate, e.vel, voice, voices: 5, detune: 0.3)
  |> out(@)
```

**Using the `ext` record to vary per voice:**

```akk
fn rich(freq, gate, vel, ext) -> {
    // Different attack per voice — the stack swells in.
    env = adsr(gate, 0.02 + ext.idx * 0.01, 0.3, 0.6, 0.4)
    saw(freq, ext.phase) * env * vel
}

unison(440, 1, 1, rich,
    voices: 6, detune: 0.4, width: 0.9, phase: 0.5)
    |> out(@)
```

**Bridging a 3-arg `poly` instrument:**

Unison expects a 4-arg instrument, but you might already have a 3-arg one
you wrote for `poly`. Wrap it with an underscore for the unused `ext`:

```akk
fn lead(freq, gate, vel) ->
    sqr(freq) * adsr(gate, 0.01, 0.1, 0.7, 0.3) * vel

fn fat(f, g, v, _) -> lead(f, g, v)

unison(440, 1, 1, fat, voices: 5, detune: 0.4) |> out(@)
```

**Polyphonic clusters via composition with `poly`:**

`unison` is monophonic by construction. For polyphonic unison, wrap unison
inside a 3-arg `poly` instrument — each incoming pattern note triggers its
own detuned cluster:

```akk
fn pad_voice(freq, gate, vel, ext) ->
    saw(freq, ext.phase) * adsr(gate, 0.4, 0.6, 0.7, 1.2) * vel

fn fat(freq, gate, vel) ->
    unison(freq, gate, vel, pad_voice,
        voices: 4, detune: 0.25, width: 0.7)

chord("Cmaj9 Am11 Fmaj7 G13")
    |> poly(@, fat, 4)
    |> out(@)
```

CPU scales with `poly_voices × unison_voices` — a 4-voice `poly` × 4-voice
`unison` runs up to 16 oscillators alive simultaneously.

Related: [poly](polyphony#poly), [pan](stereo#pan), [map](../language/arrays#map), [sum](../language/arrays#sum)
