---
title: Building Synth Voices
order: 3
category: tutorials
keywords: [synthesis, synth, envelope, adsr, ar, subtractive, tutorial, voice]
---

# Building Synth Voices

Now that you know oscillators and filters, let's combine them into complete synthesizer voices with amplitude envelopes.

## The Problem with Raw Oscillators

A raw oscillator plays constantly:

```akk
saw(220) |> out(%, %)
```

For musical notes, we need the sound to start and stop - that's what envelopes do.

## Attack-Release Envelopes

The `ar` envelope creates a simple shape triggered by a clock:

```akk
// Envelope only (no sound)
ar(trigger(2)) |> out(%, %)
```

Multiply your oscillator by the envelope:

```akk
// Plucky synth - envelope controls volume
saw(220) * ar(trigger(2)) |> out(%, %)
```

## Shaping the Envelope

The `ar` function takes attack and release times:

```akk
// Fast attack, short release - percussive
saw(220) * ar(trigger(4), 0.001, 0.1) |> out(%, %)
```

```akk
// Slow attack, long release - pad-like
saw(220) * ar(trigger(1), 0.3, 1.0) |> out(%, %)
```

## A Complete Synth Voice

Combine oscillator, filter, and envelope:

```akk
// Classic subtractive synth
saw(110)
    |> lp(%, 800)
    * ar(trigger(2), 0.01, 0.3)
    |> out(%, %)
```

## Filter Envelope

Make the filter open and close with each note:

```akk
// Filter follows its own envelope
saw(110)
    |> lp(%, 200 + ar(trigger(2), 0.01, 0.2) * 2000)
    * ar(trigger(2), 0.01, 0.5)
    |> out(%, %)
```

## Layering Oscillators

Combine multiple oscillators for richer sounds:

```akk
// Two detuned saws
(saw(110) + saw(110.5)) * 0.5
    |> lp(%, 1000)
    * ar(trigger(2))
    |> out(%, %)
```

```akk
// Octave layering
(saw(110) + saw(220) * 0.5) * 0.5
    |> moog(%, 600, 2)
    * ar(trigger(2))
    |> out(%, %)
```

## Adding Sub Bass

Layer a sine wave an octave below for weight:

```akk
// Main oscillator plus sub
osc = saw(110) + sin(55) * 0.5
osc |> lp(%, 800) * ar(trigger(2)) |> out(%, %)
```

## ADSR Envelopes

For more control, use `adsr` with attack, decay, sustain, and release:

```akk
// Sustained pad with ADSR
saw(220) * adsr(trigger(0.5), 0.1, 0.2) |> out(%, %)
```

## Building a Bass Patch

Let's build a complete bass sound:

```akk
// Punchy bass
bass_freq = 55
filter_env = ar(trigger(2), 0.01, 0.15)
amp_env = ar(trigger(2), 0.005, 0.3)

saw(bass_freq)
    |> moog(%, 200 + filter_env * 1500, 2)
    * amp_env
    |> tanh(%, 2)
    |> out(%, %)
```

## Building a Lead Patch

A bright, cutting lead sound:

```akk
// Screaming lead
lead_freq = 440
(saw(lead_freq) + sqr(lead_freq) * 0.3)
    |> lp(%, 2000 + ar(trigger(4)) * 3000, 4)
    * ar(trigger(4), 0.01, 0.2)
    |> out(%, %)
```

## Storing Voices as Variables

Keep your patches organized:

```akk
// Define the voice
synth = saw(220) |> lp(%, 800) * ar(trigger(2))

// Use it
synth |> out(%, %)
```

## Next Steps

You now know the fundamentals of synthesis! Continue to:
- [Rhythm & Patterns](04-rhythm.md) to create beats and sequences
- Explore [Effects](../builtins/reverbs.md) to add space and character

You're making real synthesizer sounds with Akkado!
