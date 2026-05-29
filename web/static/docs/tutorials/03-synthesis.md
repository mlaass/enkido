---
title: Building Synth Voices
order: 3
category: tutorials
keywords: [synthesis, synth, envelope, adsr, ar, subtractive, tutorial, voice]
---

# Building Synth Voices

Oscillators and filters give you tone. Envelopes give you notes — they decide when the sound starts and when it dies away.

## The problem with raw oscillators

A raw oscillator plays constantly:

```akk
saw(220) |> out(@)
```

That's a drone, not a note. To play notes you need an envelope on the amplitude.

## Attack-release envelopes

The `ar` envelope creates a simple shape triggered by a clock:

```akk
// Envelope only (no sound)
ar(trigger(2)) |> out(@)
```

Multiply your oscillator by the envelope:

```akk
// Plucky synth - envelope controls volume
saw(220) * ar(trigger(2)) |> out(@)
```

## Shaping the envelope

The `ar` function takes attack and release times:

```akk
// Fast attack, short release - percussive
saw(220) * ar(trigger(4), 0.001, 0.1) |> out(@)
```

```akk
// Slow attack, long release - pad-like
saw(220) * ar(trigger(1), 0.3, 1.0) |> out(@)
```

## A complete synth voice

Combine oscillator, filter, and envelope:

```akk
// Classic subtractive synth
saw(110)
    |> lp(@, 800)
    * ar(trigger(2), 0.01, 0.3)
    |> out(@)
```

## Filter envelope

Make the filter open and close with each note:

```akk
// Filter follows its own envelope
saw(110)
    |> lp(@, 200 + ar(trigger(2), 0.01, 0.2) * 2000)
    * ar(trigger(2), 0.01, 0.5)
    |> out(@)
```

## Layering oscillators

Combine multiple oscillators for thicker sounds:

```akk
// Two detuned saws
(saw(110) + saw(110.5)) * 0.5
    |> lp(@, 1000)
    * ar(trigger(2))
    |> out(@)
```

```akk
// Octave layering
(saw(110) + saw(220) * 0.5) * 0.5
    |> moog(@, 600, 2)
    * ar(trigger(2))
    |> out(@)
```

## Adding sub bass

A sine wave an octave below adds weight without muddying the mids:

```akk
// Main oscillator plus sub
osc = saw(110) + sine(55) * 0.5
osc
    |> lp(@, 800)
    * ar(trigger(2))
    |> out(@)
```

## ADSR envelopes

For more control, use `adsr` with attack, decay, sustain, and release:

```akk
// Sustained pad with ADSR
saw(220) * adsr(trigger(0.5), 0.1, 0.2) |> out(@)
```

## Building a bass patch

A complete bass sound:

```akk
// Punchy bass
bass_freq = 55
filter_env = ar(trigger(2), 0.01, 0.15)
amp_env = ar(trigger(2), 0.005, 0.3)

saw(bass_freq)
    |> moog(@, 200 + filter_env * 1500, 2)
    * amp_env
    |> saturate(@, 2)
    |> out(@)
```

## Building a lead patch

A lead that cuts through a busy mix — saw plus a touch of square, big filter env:

```akk
// Screaming lead
lead_freq = 440
(saw(lead_freq) + sqr(lead_freq) * 0.3)
    |> lp(@, 2000 + ar(trigger(4)) * 3000, 4)
    * ar(trigger(4), 0.01, 0.2)
    |> out(@)
```

## Storing voices as variables

Once a patch has more than two lines, bind it to a name:

```akk
// Define the voice
synth = saw(220) |> lp(@, 800) * ar(trigger(2))

// Use it
synth |> out(@)
```

## Next steps

- [Rhythm & Patterns](04-rhythm.md) — beats, sequences, and Euclidean patterns.
- [Effects](../builtins/reverbs.md) — reverbs and delays.
