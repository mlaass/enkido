---
title: Oscillators
category: builtins
order: 1
keywords: [oscillator, osc, sin, sine, tri, triangle, saw, sawtooth, sqr, square, phasor, ramp, noise, waveform, tone, frequency]
---

# Oscillators

Oscillators are the fundamental sound sources in synthesis. They generate periodic waveforms at specified frequencies.

## osc

**Generic Oscillator** - Generates a waveform of the specified type.

| Param | Type   | Description |
|-------|--------|-------------|
| type  | string | Waveform type: `"sin"`, `"tri"`, `"saw"`, `"sqr"` |
| freq  | signal | Frequency in Hz |

The primary oscillator function. Use this for all waveform types, especially for sine waves since `sin()` is now a pure math function.

```akk
// Sine wave (440 Hz)
osc("sin", 440) |> out(%, %)
```

```akk
// Sawtooth wave
osc("saw", 220) |> out(%, %)
```

```akk
// Triangle wave
osc("tri", 110) |> out(%, %)
```

```akk
// Square wave
osc("sqr", 110) * 0.3 |> out(%, %)
```

```akk
// FM synthesis
osc("sin", 440 + osc("sin", 5) * 10) |> out(%, %)
```

Related: [phasor](#phasor), [noise](#noise), [Math Functions](math)

---

## phasor

**Phasor Oscillator** - Generates a 0-1 ramp at the specified frequency.

| Param | Type   | Description |
|-------|--------|-------------|
| freq  | signal | Frequency in Hz |

A phasor outputs a value that ramps from 0 to 1 over each cycle. Useful for driving other processes, wavetable playback, or as a modulation source.

```akk
// Use phasor for wavetable position
phasor(2) |> out(%, %)
```

```akk
// FM using phasor as modulator
osc("sin", 440 + phasor(5) * 100) |> out(%, %)
```

```akk
// Create a sine from phasor using math sin()
sin(phasor(440) * 2 * 3.14159) |> out(%, %)
```

Related: [ramp](#ramp), [lfo](#lfo)

---

## ramp

**Ramp Oscillator** - Generates a ramp wave (alias for phasor).

| Param | Type   | Description |
|-------|--------|-------------|
| freq  | signal | Frequency in Hz |

Functionally identical to phasor. Outputs a 0-1 ramp at the specified frequency.

```akk
ramp(1) |> out(%, %)
```

Related: [phasor](#phasor)

---

## noise

**White Noise Generator** - Generates random noise.

No parameters - just outputs white noise.

White noise contains equal energy at all frequencies. Useful for percussion, wind sounds, and as a modulation source.

```akk
// Raw noise
noise() * 0.3 |> out(%, %)
```

```akk
// Filtered noise for hi-hats
noise() |> hp(%, 8000) * ar(trigger(8), 0.001, 0.05) |> out(%, %)
```

```akk
// Noise sweep
noise() |> lp(%, 200 + osc("sin", 0.5) * 1000) |> out(%, %)
```

Related: [lp](#lp), [hp](#hp)
