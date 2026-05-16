---
title: Polyphony
category: builtins
order: 18
keywords: [polyphony, polyphonic, poly, mono, legato, spread, voice, voices, chord, instrument, allocation, retrigger, voice-stealing, stereo, unison, release, release-window, note-off, click, midi]
group: sequencing
subgroup: voicing
icon: Layers
tagline: Voice allocation, legato, and stereo spread.
subfeatures:
  - name: Poly
    anchor: poly
    tagline: Polyphonic voice allocation.
    snippet: 'pat("c4 e4 g4 b4") |> poly(@, (f, g, v) -> osc("sin", f) * v, 8)'
  - name: Mono
    anchor: mono
    tagline: Monophonic last-note priority.
  - name: Legato
    anchor: legato
    tagline: Held-note legato playing.
  - name: Spread
    anchor: spread
    tagline: Stereo spread across voices.
    snippet: 'osc("saw", spread(5, [220, 220.7, 219.3, 221.4, 218.6]))'
---

# Polyphony

Voice allocation for patterns. `poly()` runs an instrument function per voice and sums the outputs. `mono()` and `legato()` are single-voice variants with different retrigger behavior. `spread()` distributes an array across N voices for unison stacks.

## poly

**Polyphonic Voice Manager** - Allocates voices for a pattern and runs an instrument function per voice.

| Param      | Type     | Default | Description |
|------------|----------|---------|-------------|
| input      | pattern  | -       | Pattern or `chord(...)` producing events (notes or chords) |
| instrument | function | -       | A function `(freq, gate, vel) -> signal` run per voice |
| voices     | number   | 64      | Voice count (1-128, must be a literal) |
| `release:` | number   | 0.0     | Seconds of voice-mix tail past note-off. Holds the gate high for the window so the instrument's ADSR finishes its release stage instead of being multiplied to zero. |

`poly()` reads pattern events at runtime and assigns each note to its own voice slot. The instrument function receives per-voice frequency, gate, and velocity, and the outputs of all active voices are summed. When the same note appears in consecutive events, the voice slot is reused (preserving phase continuity); when all voices are busy, the oldest is stolen.

The instrument must be a 3-parameter function; the names don't matter but the order is fixed: `(freq, gate, vel)`. The instrument may return a **mono** or **stereo** signal — `poly` always produces stereo output, broadcasting mono bodies to both channels.

```akk
// Polyphonic chord progression with the default 64 voices
stab = (freq, gate, vel) ->
    saw(freq) * ar(gate, 0.05, 0.4) * vel
    |> lp(@, 1100)

chord("C Em Am G") |> poly(@, stab) |> out(@)
```

```akk
// Lower voice count if you want predictable stealing
pat("c4 e4 g4 b4")
    |> poly(@, (f, g, v) -> osc("sin", f) * v, 8)
    |> out(@)
```

```akk
// Stereo instrument: each voice returns L/R, poly sums per-channel.
pat("c4 e4 g4")
    |> poly(@, (f, g, v) -> stereo(saw(f), saw(f * 1.01)) * v, 4)
    |> out(@)
```

For *unison* per voice — fanning each chord note into a stereo-spread,
detuned cluster — wrap [`unison`](unison) inside the 3-arg `poly`
instrument.

### `release:` — click-free note-off

By default `poly()` accumulates `voice * gate` per sample, so when a
note-off drops the gate to zero the instrument's ADSR release tail is
multiplied out instantly — audible as a click on every key-up with live
[`midi()`](midi) input. Cycle-aligned pattern note-offs hide this; live
keyboards expose it on every note.

`release: 0.3` holds the gate high for 300 ms past note-off so the
instrument's own ADSR runs its release to completion. The voice slot
isn't reusable until the window expires, so set the window roughly equal
to your longest ADSR release.

```akk
fn lead(freq, gate, vel) ->
    saw(freq) * adsr(gate, 0.01, 0.2, 0.7, 0.3) * vel

midi() |> poly(@, lead, 8, release: 0.3) |> out(@)
```

Same option is available on [`mono`](#mono) and [`legato`](#legato).

## mono

**Monophonic Voice Manager** - Single-voice manager with retrigger on every new note.

| Param      | Type     | Default | Description |
|------------|----------|---------|-------------|
| input      | pattern  | -       | Pattern producing events |
| instrument | function | -       | A function `(freq, gate, vel) -> signal` |
| `release:` | number   | 0.0     | Seconds of mix tail past note-off (voice-manager mode). See [`poly`](#poly) for mechanics. |

`mono()` is `poly()` with one voice and last-note priority. Every new note retriggers the gate so envelopes restart cleanly, the classic hardware-mono behavior.

`mono(stereo_signal)` is a different builtin (stereo-to-mono downmix). The compiler routes based on argument type: a function instrument gets the voice manager; a stereo signal gets the downmix.

```akk
// Mono lead with retrigger on every note
pat("c4 e4 g4 c5")
    |> mono((f, g, v) -> saw(f) * adsr(g, 0.01, 0.1, 0.6, 0.3))
    |> out(@)
```

## legato

**Legato Voice Manager** - Single-voice with no retrigger between connected notes.

| Param      | Type     | Default | Description |
|------------|----------|---------|-------------|
| input      | pattern  | -       | Pattern producing events |
| instrument | function | -       | A function `(freq, gate, vel) -> signal` |
| `release:` | number   | 0.0     | Seconds of mix tail past note-off. See [`poly`](#poly) for mechanics. |

Like `mono()`, but the gate stays high while notes overlap, so envelopes don't restart on every note. Frequency and velocity update but the AR/ADSR keeps decaying through the phrase. Best for legato leads and bass lines.

```akk
// Smooth bassline, gate stays high across notes
pat("c2 e2 g2 c3")
    |> legato((f, g, v) -> saw(f) * adsr(g, 0.01, 0.2, 0.8, 0.4))
    |> out(@)
```

## spread

**Spread** - Distribute an array across N voices evenly.

| Param  | Type   | Default | Description |
|--------|--------|---------|-------------|
| n      | number | -       | Number of voices to spread across |
| source | array  | -       | Source array of values |

`spread()` is a compile-time helper that takes an array and distributes its values across `n` slots, repeating or reducing as needed. Useful for unison voices and detuned stacks.

```akk
// Detuned saw stack, 5 oscillators across the array
osc("saw", spread(5, [220, 220.7, 219.3, 221.4, 218.6])) |> out(@)
```

## voice

A **voice** is one independent instance of the instrument function. `poly()` runs N voices in parallel; `mono()` and `legato()` use one. Voice slots are pre-allocated at compile time; runtime allocation is just a slot lookup.

## voices

The **voice count** parameter to `poly()` (default 64). It must be a literal so the compiler can statically allocate. Lower voice counts give predictable voice stealing; higher counts handle complex patterns without dropouts.

Related: [unison](unison), [sequencing](sequencing), [chord](../mini-notation/chords), [pat](../mini-notation/basics)
