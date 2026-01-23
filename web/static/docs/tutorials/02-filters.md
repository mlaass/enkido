---
title: Shaping Sound with Filters
order: 2
category: tutorials
keywords: [filter, lowpass, highpass, cutoff, resonance, tutorial, lp, hp, moog]
---

# Shaping Sound with Filters

Filters are essential tools for sculpting your sounds. They remove or emphasize certain frequencies, transforming raw oscillators into musical timbres.

## What Filters Do

Imagine sound as a mix of many frequencies. A filter acts like a gate that lets some frequencies through while blocking others.

## Your First Lowpass Filter

The lowpass filter (`lp`) is the most common. It passes low frequencies and cuts high ones:

```akk
// Raw sawtooth - bright and buzzy
osc("saw", 110) |> out(%, %)
```

```akk
// Filtered sawtooth - warmer and darker
osc("saw", 110) |> lp(%, 800) |> out(%, %)
```

The `800` is the **cutoff frequency** - frequencies above this get quieter.

## The Pipe and Hole Pattern

Notice the pattern: `|> lp(%, 800)`

- The pipe (`|>`) sends the sawtooth into the filter
- The hole (`%`) receives that signal
- `800` sets the cutoff frequency

This pattern is how all signal processing works in Akkado.

## Moving the Cutoff

Lower cutoffs make darker sounds, higher cutoffs brighter:

```akk
// Very dark - cutoff at 200 Hz
osc("saw", 110) |> lp(%, 200) |> out(%, %)
```

```akk
// Bright - cutoff at 2000 Hz
osc("saw", 110) |> lp(%, 2000) |> out(%, %)
```

## Adding Resonance

The third parameter adds **resonance** - a boost at the cutoff frequency:

```akk
// Q of 0.707 (default) - no resonance
osc("saw", 110) |> lp(%, 800, 0.707) |> out(%, %)
```

```akk
// Q of 4 - noticeable peak
osc("saw", 110) |> lp(%, 800, 4) |> out(%, %)
```

```akk
// Q of 10 - strong resonance
osc("saw", 110) |> lp(%, 800, 10) |> out(%, %)
```

## Filter Sweeps

Make the cutoff change over time for classic synth sounds:

```akk
// Slow sweep using an LFO
osc("saw", 110) |> lp(%, 400 + osc("sin", 0.5) * 800) |> out(%, %)
```

The cutoff moves between 400 and 1200 Hz following a sine wave.

## Envelope-Controlled Filter

For percussive sounds, use an envelope to control the filter:

```akk
// Filter opens on each trigger, then closes
osc("saw", 110) |> lp(%, 200 + ar(trigger(2)) * 2000) |> out(%, %)
```

## Highpass Filter

The highpass (`hp`) does the opposite - it removes low frequencies:

```akk
// Remove the bass
osc("saw", 110) |> hp(%, 500) |> out(%, %)
```

Great for hi-hats and making sounds thinner:

```akk
// Hi-hat from filtered noise
osc("noise") |> hp(%, 8000) * ar(trigger(8), 0.001, 0.05) |> out(%, %)
```

## The Moog Filter

For a classic, creamy analog sound, use the Moog ladder filter:

```akk
// Classic Moog bass
osc("saw", 55) |> moog(%, 400, 2) |> out(%, %)
```

```akk
// Self-oscillating filter - acts as an oscillator!
osc("noise") * 0.01 |> moog(%, 440, 3.9) |> out(%, %)
```

## Chaining Filters

You can use multiple filters in series:

```akk
// Remove lows and highs
osc("saw", 110) |> hp(%, 200) |> lp(%, 2000) |> out(%, %)
```

## Next Steps

Now that you can shape your sounds with filters, move on to:
- [Building Synths](03-synthesis.md) to combine oscillators and envelopes
- Try the [Moog filter reference](../builtins/filters.md#moog) for more details

You're on your way to making real music with Akkado!
