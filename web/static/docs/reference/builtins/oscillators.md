---
title: Oscillators
category: builtins
subcategory: oscillators
keywords: [oscillator, sin, sine, tri, triangle, saw, sawtooth, sqr, square, phasor, ramp, noise, waveform, tone, frequency]
---

# Oscillators

Oscillators are the fundamental sound sources in synthesis. They generate periodic waveforms at specified frequencies.

## sin

**Sine Oscillator** - Generates a pure sine wave.

| Param | Type   | Description |
|-------|--------|-------------|
| freq  | signal | Frequency in Hz |

Aliases: `sine`

The sine wave is the purest tone, containing only the fundamental frequency with no harmonics. Useful for sub-bass, FM synthesis, and LFOs.

```akk
// Concert A (440 Hz)
sin(440) |> out(%, %)
```

```akk
// Vibrato using FM
sin(440 + sin(5) * 10) |> out(%, %)
```

```akk
// Sub-bass with envelope
sin(55) * ar(trigger(4), 0.01, 0.3) |> out(%, %)
```

Related: [tri](#tri), [saw](#saw), [sqr](#sqr)

---

## tri

**Triangle Oscillator** - Generates a triangle wave.

| Param | Type   | Description |
|-------|--------|-------------|
| freq  | signal | Frequency in Hz |

Aliases: `triangle`

Triangle waves have odd harmonics that roll off at -12dB/octave, giving a softer, mellower tone than sawtooth or square waves.

```akk
// Mellow lead sound
tri(330) |> out(%, %)
```

```akk
// Filtered triangle bass
tri(110) |> lp(%, 800) |> out(%, %)
```

Related: [sin](#sin), [saw](#saw), [sqr](#sqr)

---

## saw

**Sawtooth Oscillator** - Generates a sawtooth wave.

| Param | Type   | Description |
|-------|--------|-------------|
| freq  | signal | Frequency in Hz |

Aliases: `sawtooth`

Sawtooth waves contain all harmonics, making them rich and buzzy. They're the foundation of many classic synth sounds.

```akk
// Classic synth lead
saw(440) |> out(%, %)
```

```akk
// Detuned supersaw
saw(440) + saw(442) + saw(438) |> out(%, %) * 0.3
```

```akk
// Filtered pad
saw(220) |> lp(%, 1000, 2) |> out(%, %)
```

Related: [sin](#sin), [tri](#tri), [sqr](#sqr)

---

## sqr

**Square Oscillator** - Generates a square wave.

| Param | Type   | Description |
|-------|--------|-------------|
| freq  | signal | Frequency in Hz |

Aliases: `square`

Square waves have only odd harmonics, producing a hollow, woody tone. Great for basses and retro sounds.

```akk
// 8-bit style sound
sqr(440) * 0.3 |> out(%, %)
```

```akk
// Octave bass
sqr(110) + sqr(55) * 0.5 |> out(%, %) * 0.3
```

Related: [sin](#sin), [tri](#tri), [saw](#saw)

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
sin(440 + phasor(5) * 100) |> out(%, %)
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
noise() |> lp(%, 200 + sin(0.5) * 1000) |> out(%, %)
```

Related: [lp](#lp), [hp](#hp)
