---
title: Shaping Sound with Filters
order: 2
category: tutorials
keywords: [filter, lowpass, highpass, cutoff, resonance, tutorial, lp, hp, moog]
---

# Shaping Sound with Filters

Filters remove or emphasize certain frequencies. A raw saw or square is harmonically dense — bright, buzzy, hard to listen to for long. A filter is how you carve that into a tone that sits in a mix.

## Your first lowpass filter

The lowpass filter (`lp`) is the most common. It passes low frequencies and cuts high ones:

```akk
// Raw sawtooth - bright and buzzy
osc("saw", 110) |> out(@)
```

```akk
// Filtered sawtooth - warmer and darker
osc("saw", 110)
    |> lp(@, 800)
    |> out(@)
```

The `800` is the **cutoff frequency**: frequencies above this get quieter.

## The pipe and hole pattern

`|> lp(@, 800)` reads: pipe the saw into `lp`, drop it into the `@` slot, set cutoff to 800. Every effect in Akkado uses the same shape — pipe in, name the hole, pass parameters.

## Moving the cutoff

Lower cutoffs sound darker. Higher cutoffs sound brighter:

```akk
// Very dark - cutoff at 200 Hz
osc("saw", 110)
    |> lp(@, 200)
    |> out(@)
```

```akk
// Bright - cutoff at 2000 Hz
osc("saw", 110)
    |> lp(@, 2000)
    |> out(@)
```

## Adding resonance

The third parameter adds **resonance**, a boost at the cutoff frequency:

```akk
// Q of 0.707 (default) - no resonance
osc("saw", 110)
    |> lp(@, 800, 0.707)
    |> out(@)
```

```akk
// Q of 4 - noticeable peak
osc("saw", 110)
    |> lp(@, 800, 4)
    |> out(@)
```

```akk
// Q of 10 - strong resonance
osc("saw", 110)
    |> lp(@, 800, 10)
    |> out(@)
```

## Filter sweeps

Modulate the cutoff with an LFO for the classic synth sweep:

```akk
// Slow sweep using an LFO
osc("saw", 110)
    |> lp(@, 400 + osc("sin", 0.5) * 800)
    |> out(@)
```

The cutoff moves between 400 and 1200 Hz following a sine wave.

## Envelope-controlled filter

For percussive sounds, use an envelope to control the filter:

```akk
// Filter opens on each trigger, then closes
osc("saw", 110)
    |> lp(@, 200 + ar(trigger(2)) * 2000)
    |> out(@)
```

## Highpass filter

The highpass (`hp`) does the opposite: it removes low frequencies:

```akk
// Remove the bass
osc("saw", 110)
    |> hp(@, 500)
    |> out(@)
```

Useful for hi-hats and thinning out sounds:

```akk
// Hi-hat from filtered noise
osc("noise")
    |> hp(@, 8000)
    * ar(trigger(8), 0.001, 0.05)
    |> out(@)
```

## The Moog filter

The Moog ladder is the classic 24-dB analog filter — fatter than `lp`, with characteristic resonance:

```akk
// Classic Moog bass
osc("saw", 55)
    |> moog(@, 400, 2)
    |> out(@)
```

```akk
// Self-oscillating filter - acts as an oscillator
osc("noise") * 0.01
    |> moog(@, 440, 3.9)
    |> out(@)
```

## Chaining filters

Multiple filters in series:

```akk
// Remove lows and highs
osc("saw", 110)
    |> hp(@, 200)
    |> lp(@, 2000)
    |> out(@)
```

## Next steps

- [Building Synths](03-synthesis.md) wraps oscillators and filters in envelopes to make actual notes.
- [Moog filter reference](../builtins/filters.md#moog) has the full parameter list.
