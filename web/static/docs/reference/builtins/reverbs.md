---
title: Reverbs
category: builtins
order: 6
keywords: [reverb, freeverb, dattorro, fdn, room, plate, space, decay, damping, wet, dry]
---

# Reverbs

Reverbs simulate acoustic spaces by creating many delayed, filtered reflections. Different algorithms offer different sonic characteristics.

## freeverb

**Freeverb** - Classic Schroeder-style algorithmic reverb.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| room  | number | 0.5     | Room size (0-1) |
| damp  | number | 0.5     | High frequency damping (0-1) |

Aliases: `reverb`

A classic reverb algorithm with a smooth, natural sound. Higher room values create larger spaces with longer decay.

```akk
// Medium room reverb
osc("saw", 220) * ar(trigger(2)) |> freeverb(%, 0.5, 0.5) |> out(%, %)
```

```akk
// Large hall
osc("saw", 110) * ar(trigger(1)) |> freeverb(%, 0.9, 0.3) |> out(%, %)
```

```akk
// Damped small room
osc("noise") * ar(trigger(4), 0.001, 0.05) |> freeverb(%, 0.2, 0.8) |> out(%, %)
```

Related: [dattorro](#dattorro), [fdn](#fdn)

---

## dattorro

**Dattorro Reverb** - High-quality plate reverb algorithm.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| decay | number | 0.7     | Decay time (0-1) |
| predelay | number | 20.0  | Predelay in milliseconds |

Aliases: `plate`

The Dattorro plate reverb produces lush, shimmering tails perfect for vocals and synth pads. The predelay separates the dry signal from the reverb onset.

```akk
// Lush plate reverb
osc("saw", 220) * ar(trigger(2)) |> dattorro(%, 0.8, 30) |> out(%, %)
```

```akk
// Short bright plate
osc("tri", 440) * ar(trigger(4)) |> dattorro(%, 0.5, 10) |> out(%, %)
```

Related: [freeverb](#freeverb), [fdn](#fdn)

---

## fdn

**FDN Reverb** - Feedback Delay Network reverb.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| decay | number | 0.8     | Decay time (0-1) |
| damp  | number | 0.3     | High frequency damping (0-1) |

Aliases: `room`

A matrix-based reverb using multiple delay lines with cross-feedback. Creates dense, even decay patterns suitable for ambient textures.

```akk
// Dense ambient reverb
osc("saw", 55) * ar(trigger(0.5)) |> fdn(%, 0.9, 0.4) |> out(%, %)
```

```akk
// Tight room
osc("noise") * ar(trigger(8), 0.001, 0.02) |> fdn(%, 0.4, 0.6) |> out(%, %)
```

Related: [freeverb](#freeverb), [dattorro](#dattorro)
