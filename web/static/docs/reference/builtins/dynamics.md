---
title: Dynamics
category: builtins
order: 9
keywords: [dynamics, comp, compressor, limiter, gate, noisegate, threshold, ratio, ceiling, compression]
---

# Dynamics

Dynamics processors control the volume envelope of signals, reducing dynamic range or removing unwanted quiet sections.

## comp

**Compressor** - Reduces dynamic range above threshold.

| Param  | Type   | Default | Description |
|--------|--------|---------|-------------|
| in     | signal | -       | Input signal |
| thresh | number | -12.0   | Threshold in dB |
| ratio  | number | 4.0     | Compression ratio |

Aliases: `compress`, `compressor`

Reduces the level of signals that exceed the threshold. Higher ratios create more aggressive compression. A ratio of 4:1 means 4dB of input above threshold becomes 1dB of output.

```akk
// Basic compression
saw(110) * ar(trigger(2)) |> comp(%, -12, 4) |> out(%, %)
```

```akk
// Heavy compression (limiting-like)
saw(55) * ar(trigger(4)) |> comp(%, -20, 10) |> out(%, %)
```

```akk
// Gentle leveling
noise() * ar(trigger(1)) |> comp(%, -6, 2) |> out(%, %)
```

Related: [limiter](#limiter), [gate](#gate)

---

## limiter

**Limiter** - Brickwall limiter preventing signal from exceeding ceiling.

| Param   | Type   | Default | Description |
|---------|--------|---------|-------------|
| in      | signal | -       | Input signal |
| ceiling | number | -0.1    | Maximum output level in dB |
| release | number | 0.1     | Release time in seconds |

Aliases: `limit`

A limiter is an extreme compressor (infinite ratio) that prevents the signal from ever exceeding the ceiling. Essential for preventing digital clipping.

```akk
// Master limiter
saw(110) * 2 |> limiter(%, -0.1, 0.1) |> out(%, %)
```

```akk
// Aggressive limiting for loudness
saw(55) * ar(trigger(4)) * 3 |> limiter(%, -1, 0.05) |> out(%, %)
```

Related: [comp](#comp)

---

## gate

**Noise Gate** - Silences signal below threshold.

| Param  | Type   | Default | Description |
|--------|--------|---------|-------------|
| in     | signal | -       | Input signal |
| thresh | number | -40.0   | Threshold in dB |
| hyst   | number | 6.0     | Hysteresis in dB |

Aliases: `noisegate`

Cuts the signal when it falls below the threshold, useful for removing noise during quiet passages. Hysteresis prevents chattering at the threshold.

```akk
// Basic noise gate
(saw(110) + noise() * 0.1) * ar(trigger(2)) |> gate(%, -30, 6) |> out(%, %)
```

```akk
// Tight gate for percussive sounds
noise() * ar(trigger(8), 0.001, 0.05) |> gate(%, -20, 10) |> out(%, %)
```

Related: [comp](#comp)
