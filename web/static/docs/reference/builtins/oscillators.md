---
title: Oscillators
category: builtins
order: 1
keywords: [oscillator, osc, sin, sine, tri, triangle, saw, sawtooth, sqr, square, phasor, ramp, noise, waveform, tone, frequency]
group: instruments
subgroup: synthesis
icon: Waves
tagline: Sine, saw, square, triangle, phasor, ramp, and noise sources.
---

# Oscillators

Oscillators are the primary sound sources in synthesis. The `osc()` function is the unified interface for all oscillator types.

## osc

**Generic Oscillator** - Generates a waveform of the specified type.

| Param | Type   | Description |
|-------|--------|-------------|
| type  | string | Waveform type (see below) |
| freq  | signal | Frequency in Hz (not required for `"noise"`) |

### Waveform types

| Type | Description |
|------|-------------|
| `"sin"` | Sine wave - pure tone, no harmonics |
| `"saw"` | Sawtooth wave - all harmonics, bright and buzzy |
| `"tri"` | Triangle wave - odd harmonics, softer than saw |
| `"sqr"` | Square wave - odd harmonics, hollow and punchy |
| `"phasor"` | 0-1 ramp, useful for modulation and wavetables |
| `"ramp"` | Alias for phasor |
| `"noise"` | White noise (freq parameter ignored) |

### Pitched waveforms

```akk
// Sine wave (440 Hz)
sine(440) |> out(@)
```

```akk
// Sawtooth wave
saw(220) |> out(@)
```

```akk
// Triangle wave
tri(110) |> out(@)
```

```akk
// Square wave
sqr(110) * 0.3 |> out(@)
```

### Phasor / Ramp

A phasor outputs a value that ramps from 0 to 1 over each cycle. Useful for driving wavetables, custom waveshaping, or as a modulation source.

```akk
// Use phasor for wavetable position
phasor(2) |> out(@)
```

```akk
// Create a sine from phasor using math sin()
sin(phasor(440) * 2 * 3.14159) |> out(@)
```

### Noise

White noise contains equal energy at all frequencies. Useful for percussion, wind sounds, and as a modulation source. The frequency parameter is ignored.

```akk
// Raw noise
noise() * 0.3 |> out(@)
```

```akk
// Filtered noise for hi-hats
noise() |> hp(@, 8000) * ar(trigger(8), 0.001, 0.05) |> out(@)
```

```akk
// Noise sweep
noise() |> lp(@, 200 + sine(0.5) * 1000) |> out(@)
```

### FM synthesis

FM synthesis modulates the carrier's frequency input with another oscillator. See [fm-synthesis](fm-synthesis) for the full treatment.

```akk
// Simple FM
sine(440 + sine(5) * 10) |> out(@)
```

Related: [fm-synthesis](fm-synthesis), [Math Functions](math), [Filters](filters)
