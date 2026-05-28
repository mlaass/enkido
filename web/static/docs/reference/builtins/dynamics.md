---
title: Dynamics
category: builtins
order: 9
keywords: [dynamics, comp, compressor, limiter, gate, noisegate, threshold, ratio, ceiling, compression]
group: effects
subgroup: dynamics
icon: Gauge
tagline: Compression, limiting, and gating.
subfeatures:
  - name: Compressor
    anchor: comp
    tagline: Feedforward compressor.
    snippet: 'saw(110) * ar(trigger(2)) |> comp(@, -12, 4)'
  - name: Limiter
    anchor: limiter
    tagline: Brick-wall ceiling.
    snippet: 'saw(110) * 2 |> limiter(@, -0.1, 0.1)'
  - name: Gate
    anchor: gate
    tagline: Threshold-based noise gate.
    snippet: 'noise() * ar(trigger(8), 0.001, 0.05) |> gate(@, -20, 10)'
---

# Dynamics

Dynamics processors control the volume envelope of signals, reducing dynamic range or removing unwanted quiet sections.

All effects in this file expose `dry` and `wet` as their last two parameters (Category B defaults `dry=0, wet=1` for transform effects). See [Effect Parameters Convention](/docs/concepts/effect-parameters) for the full spec.

## comp

**Compressor** - Reduces dynamic range above threshold.

| Param  | Type   | Default | Description |
|--------|--------|---------|-------------|
| in     | signal | -       | Input signal |
| thresh | number | -12.0   | Threshold in dB |
| ratio  | number | 4.0     | Compression ratio |
| dry   | number | 0.0     | Dry signal level (Category B) |
| wet   | number | 1.0     | Wet (processed) signal level (Category B) |
| attack  | number | 0.1   | Attack time in milliseconds |
| release | number | 10.0  | Release time in milliseconds |

Aliases: `compress`, `compressor`

Reduces the level of signals that exceed the threshold. Higher ratios create more aggressive compression. A ratio of 4:1 means 4dB of input above threshold becomes 1dB of output.

`attack` and `release` set the envelope-follower time constants — pass them as named arguments. Shorter attack clamps transients harder; longer release sustains gain reduction between hits.

```akk
// Basic compression
saw(110) * ar(trigger(2)) |> comp(@, -12, 4) |> out(@)
```

```akk
// Slow, transparent leveling
saw(110) * ar(trigger(2)) |> comp(@, -12, 4, attack: 30, release: 250) |> out(@)
```

```akk
// Heavy compression (limiting-like)
saw(55) * ar(trigger(4)) |> comp(@, -20, 10) |> out(@)
```

```akk
// Gentle leveling
noise() * ar(trigger(1)) |> comp(@, -6, 2) |> out(@)
```

Related: [limiter](#limiter), [gate](#gate)

---

## limiter

**Limiter** - Brickwall limiter that prevents signal from exceeding ceiling.

| Param   | Type   | Default | Description |
|---------|--------|---------|-------------|
| in      | signal | -       | Input signal |
| ceiling | number | -0.1    | Maximum output level in dB |
| release | number | 0.1     | Release time in seconds |
| dry   | number | 0.0     | Dry signal level (Category B) |
| wet   | number | 1.0     | Wet (processed) signal level (Category B) |
| lookahead | number | 0.0  | Lookahead time in ms (`0` = off; capped at ~1ms) |

Aliases: `limit`

A limiter is an extreme compressor (infinite ratio) that prevents the signal from ever exceeding the ceiling. Use to prevent digital clipping.

`lookahead` defaults to `0` (off). A positive value delays the signal so the limiter can react to a peak before it arrives, trading a small latency for cleaner transient control.

```akk
// Master limiter
saw(110) * 2 |> limiter(@, -0.1, 0.1) |> out(@)
```

```akk
// Limiter with lookahead for cleaner peak control
saw(110) * 2 |> limiter(@, -0.1, 0.1, lookahead: 1) |> out(@)
```

```akk
// Aggressive limiting for loudness
saw(55) * ar(trigger(4)) * 3 |> limiter(@, -1, 0.05) |> out(@)
```

Related: [comp](#comp)

---

## gate

**Noise gate** - Silences signal below threshold.

| Param  | Type   | Default | Description |
|--------|--------|---------|-------------|
| in     | signal | -       | Input signal |
| thresh | number | -40.0   | Threshold in dB |
| range  | number | -40.0   | Attenuation applied when the gate is closed, in dB |
| hyst   | number | 6.0     | Hysteresis in dB (open/close difference) |
| close_time | number | 5.0 | Fade-out time (ms) |
| dry   | number | 0.0     | Dry signal level (Category B) |
| wet   | number | 1.0     | Wet (processed) signal level (Category B) |
| attack  | number | 0.1   | Attack time in milliseconds |
| hold    | number | 0.0   | Hold time in milliseconds (gate stays open after signal drops) |
| release | number | 10.0  | Release time in milliseconds |

Aliases: `noisegate`

Cuts the signal when it falls below the threshold, useful for removing noise during quiet passages. Hysteresis prevents chattering at the threshold by requiring the signal to drop further below the threshold before the gate closes.

The `close_time` parameter controls how quickly the gate fades out when closing, preventing abrupt cuts. `attack`, `hold`, and `release` (named arguments, all in milliseconds) shape the open/close envelope — a longer `hold` keeps the gate open through short dips below threshold.

```akk
// Basic noise gate
(saw(110) + noise() * 0.1) * ar(trigger(2))
    |> gate(@, -30, 6)
    |> out(@)
```

```akk
// Tight gate for percussive sounds
noise() * ar(trigger(8), 0.001, 0.05)
    |> gate(@, -20, 10)
    |> out(@)
```

```akk
// Slow fade-out gate
saw(110) * ar(trigger(2)) |> gate(@, -30, 6, 20) |> out(@)
```

```akk
// Long hold keeps the gate open through short gaps
noise() * ar(trigger(4)) |> gate(@, -30, -40, hold: 120, release: 80) |> out(@)
```

Related: [comp](#comp)
