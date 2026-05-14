---
title: Shaping Sound with Filters
order: 2
category: tutorials
keywords: [filter, lowpass, highpass, cutoff, resonance, tutorial, lp, hp, moog]
---

# Shaping Sound with Filters

Filters remove or emphasize certain frequencies, turning raw oscillators into something more musical.

## Your first lowpass filter

The lowpass filter (`lp`) is the most common. It passes low frequencies and cuts high ones:

```akk
// Raw sawtooth - bright and buzzy
osc("saw", 110) |> out(%)
```

```akk
// Filtered sawtooth - warmer and darker
osc("saw", 110) |> lp(%, 800) |> out(%)
```

The `800` is the **cutoff frequency**: frequencies above this get quieter.

## The pipe and hole pattern

Notice the pattern: `|> lp(%, 800)`

- The pipe (`|>`) sends the sawtooth into the filter
- The hole (`%`) receives that signal
- `800` sets the cutoff frequency

All signal processing in Akkado uses this shape.

## Moving the cutoff

Lower cutoffs make darker sounds, higher cutoffs brighter:

```akk
// Very dark - cutoff at 200 Hz
osc("saw", 110) |> lp(%, 200) |> out(%)
```

```akk
// Bright - cutoff at 2000 Hz
osc("saw", 110) |> lp(%, 2000) |> out(%)
```

## Adding resonance

The third parameter adds **resonance**, a boost at the cutoff frequency:

```akk
// Q of 0.707 (default) - no resonance
osc("saw", 110) |> lp(%, 800, 0.707) |> out(%)
```

```akk
// Q of 4 - noticeable peak
osc("saw", 110) |> lp(%, 800, 4) |> out(%)
```

```akk
// Q of 10 - strong resonance
osc("saw", 110) |> lp(%, 800, 10) |> out(%)
```

## Filter sweeps

Make the cutoff change over time for classic synth sounds:

```akk
// Slow sweep using an LFO
osc("saw", 110) |> lp(%, 400 + osc("sin", 0.5) * 800) |> out(%)
```

The cutoff moves between 400 and 1200 Hz following a sine wave.

## Envelope-controlled filter

For percussive sounds, use an envelope to control the filter:

```akk
// Filter opens on each trigger, then closes
osc("saw", 110) |> lp(%, 200 + ar(trigger(2)) * 2000) |> out(%)
```

## Highpass filter

The highpass (`hp`) does the opposite: it removes low frequencies:

```akk
// Remove the bass
osc("saw", 110) |> hp(%, 500) |> out(%)
```

Useful for hi-hats and thinning out sounds:

```akk
// Hi-hat from filtered noise
osc("noise") |> hp(%, 8000) * ar(trigger(8), 0.001, 0.05) |> out(%)
```

## The Moog filter

For a classic analog sound, use the Moog ladder filter:

```akk
// Classic Moog bass
osc("saw", 55) |> moog(%, 400, 2) |> out(%)
```

```akk
// Self-oscillating filter - acts as an oscillator
osc("noise") * 0.01 |> moog(%, 440, 3.9) |> out(%)
```

## Chaining filters

Multiple filters in series:

```akk
// Remove lows and highs
osc("saw", 110) |> hp(%, 200) |> lp(%, 2000) |> out(%)
```

## Next steps

- [Building Synths](03-synthesis.md): combine oscillators, filters, and envelopes
- [Moog filter reference](../builtins/filters.md#moog) for parameter details
