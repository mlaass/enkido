---
title: Modulation Effects
category: builtins
order: 7
keywords: [modulation, chorus, flanger, phaser, comb, effect, rate, depth, sweep]
---

# Modulation Effects

Modulation effects use time-varying delays to create movement and spatial interest in sounds.

## chorus

**Chorus** - Creates copies with slight pitch/time variations.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| rate  | number | 0.5     | LFO rate in Hz |
| depth | number | 0.5     | Modulation depth (0-1) |

Creates a thicker, wider sound by mixing the input with delayed copies that are slightly pitch-shifted by an LFO.

```akk
// Classic chorus
osc("saw", 220) |> chorus(%, 0.5, 0.5) |> out(%, %)
```

```akk
// Slow deep chorus
osc("tri", 110) |> chorus(%, 0.2, 0.8) |> out(%, %)
```

```akk
// Fast shimmer
osc("sin", 440) |> chorus(%, 2, 0.3) |> out(%, %)
```

Related: [flanger](#flanger), [phaser](#phaser)

---

## flanger

**Flanger** - Comb filtering with swept delay time.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| rate  | number | 1.0     | LFO rate in Hz |
| depth | number | 0.7     | Modulation depth (0-1) |

Similar to chorus but with shorter delay times and feedback, creating the characteristic "jet plane" sweep effect.

```akk
// Classic flanger
osc("saw", 110) |> flanger(%, 0.5, 0.7) |> out(%, %)
```

```akk
// Slow metallic sweep
osc("sqr", 220) |> flanger(%, 0.1, 0.9) |> out(%, %)
```

```akk
// Fast subtle movement
osc("tri", 440) |> flanger(%, 3, 0.3) |> out(%, %)
```

Related: [chorus](#chorus), [phaser](#phaser), [comb](#comb)

---

## phaser

**Phaser** - Creates notches in frequency spectrum via allpass filters.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| rate  | number | 0.5     | LFO rate in Hz |
| depth | number | 0.8     | Modulation depth (0-1) |

Sweeps a series of notch filters through the spectrum, creating a distinctive swirling effect different from chorus or flanger.

```akk
// Classic phaser
osc("saw", 110) |> phaser(%, 0.3, 0.8) |> out(%, %)
```

```akk
// Fast space phaser
osc("sqr", 220) |> phaser(%, 2, 0.5) |> out(%, %)
```

```akk
// Slow deep sweep
noise() |> lp(%, 2000) |> phaser(%, 0.1, 0.9) |> out(%, %)
```

Related: [flanger](#flanger), [chorus](#chorus)

---

## comb

**Comb Filter** - Fixed delay with feedback for resonant coloring.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| time  | signal | -       | Delay time in seconds |
| fb    | number | -       | Feedback amount (0-1) |

A comb filter creates a series of peaks and notches at harmonics of the delay frequency. The fundamental frequency is approximately 1/time Hz.

```akk
// Tuned resonator at ~220 Hz
noise() |> comb(%, 1/220, 0.95) |> out(%, %)
```

```akk
// Metallic coloring
osc("saw", 110) |> comb(%, 0.01, 0.7) |> out(%, %)
```

```akk
// Karplus-Strong style pluck
noise() * ar(trigger(4), 0.001, 0.01) |> comb(%, 1/440, 0.99) |> out(%, %)
```

Related: [flanger](#flanger), [delay](#../delays#delay)
