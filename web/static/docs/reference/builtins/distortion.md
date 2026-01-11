---
title: Distortion
category: builtins
subcategory: distortion
keywords: [distortion, tanh, softclip, bitcrush, fold, wavefold, saturate, drive, crush, lofi]
---

# Distortion

Distortion effects add harmonic content by clipping, saturating, or otherwise mangling signals.

## tanh

**Tanh Saturation** - Smooth hyperbolic tangent waveshaping.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| drive | number | 2.0     | Drive amount (1-10+) |

Aliases: `distort`, `saturate`

Warm, tube-like saturation that adds odd harmonics while preventing harsh clipping. Higher drive values increase saturation intensity.

```akk
// Warm overdrive
saw(110) |> tanh(%, 3) |> out(%, %)
```

```akk
// Heavy saturation on bass
saw(55) |> tanh(%, 8) |> lp(%, 400) |> out(%, %)
```

```akk
// Subtle warming
sin(220) |> tanh(%, 1.5) |> out(%, %)
```

Related: [softclip](#softclip), [fold](#fold)

---

## softclip

**Soft Clipping** - Polynomial soft clipper with adjustable threshold.

| Param  | Type   | Default | Description |
|--------|--------|---------|-------------|
| in     | signal | -       | Input signal |
| thresh | number | 0.5     | Clipping threshold (0-1) |

Softer than hard clipping, rounds off peaks smoothly. Lower threshold values create more aggressive distortion.

```akk
// Gentle compression-like softclip
saw(220) |> softclip(%, 0.7) |> out(%, %)
```

```akk
// Aggressive softclip
sqr(110) |> softclip(%, 0.2) |> out(%, %)
```

Related: [tanh](#tanh)

---

## bitcrush

**Bit Crusher** - Reduces bit depth and sample rate.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| bits  | number | 8.0     | Bit depth (1-16) |
| rate  | number | 0.5     | Sample rate reduction (0-1) |

Aliases: `crush`

Creates lo-fi digital artifacts by quantizing amplitude and reducing sample rate. Lower values create more extreme degradation.

```akk
// Classic 8-bit sound
saw(220) |> bitcrush(%, 8, 0.5) |> out(%, %)
```

```akk
// Extreme lo-fi
saw(110) |> bitcrush(%, 4, 0.2) |> out(%, %)
```

```akk
// Subtle grit
saw(440) |> bitcrush(%, 12, 0.8) |> out(%, %)
```

Related: [fold](#fold)

---

## fold

**Wavefolding** - Folds signal back on itself when exceeding threshold.

| Param  | Type   | Default | Description |
|--------|--------|---------|-------------|
| in     | signal | -       | Input signal |
| thresh | number | 0.5     | Folding threshold (0-1) |

Aliases: `wavefold`

Classic West Coast synthesis technique. When the signal exceeds the threshold, it folds back creating complex harmonic spectra.

```akk
// Basic wavefold
sin(110) * 2 |> fold(%, 0.5) |> out(%, %)
```

```akk
// Animated wavefolding
sin(110) * (1.5 + sin(0.2)) |> fold(%, 0.4) |> out(%, %)
```

```akk
// Aggressive folding
tri(55) * 4 |> fold(%, 0.3) |> lp(%, 2000) |> out(%, %)
```

Related: [tanh](#tanh), [softclip](#softclip)
