---
title: Distortion
category: builtins
order: 8
keywords: [distortion, tanh, softclip, bitcrush, fold, wavefold, saturate, drive, crush, lofi, tube, valve, smooth, adaa, tape, warmth, xfmr, transformer, excite, exciter, harmonics]
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

---

## tube

**Tube Saturation** - Asymmetric waveshaping with even harmonics.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| drive | number | 5.0     | Drive amount (1-20) |
| bias  | number | 0.1     | Asymmetry bias (0-0.3) |

Aliases: `valve`, `triode`

Emulates triode tube saturation with an asymmetric transfer function. Unlike symmetric saturators like tanh which produce only odd harmonics, tube adds even harmonics (especially 2nd) for a warmer, vintage character. Uses 2x oversampling internally.

```akk
// Warm vintage drive
saw(110) |> tube(%, 5, 0.1) |> out(%, %)
```

```akk
// Aggressive tube distortion
saw(55) |> tube(%, 15, 0.2) |> lp(%, 800) |> out(%, %)
```

```akk
// Subtle 2nd harmonic enhancement
sin(220) |> tube(%, 2, 0.15) |> out(%, %)
```

Related: [tanh](#tanh), [tape](#tape)

---

## smooth

**ADAA Saturation** - Alias-free saturation using antiderivative antialiasing.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| drive | number | 5.0     | Drive amount (1-20) |

Aliases: `adaa`

High-quality tanh saturation with built-in antialiasing. Uses the ADAA (Antiderivative Antialiasing) algorithm to eliminate harsh aliasing artifacts without oversampling. Best for clean, high-fidelity saturation on full mixes or high-frequency content.

```akk
// Clean master bus saturation
saw(220) |> smooth(%, 3) |> out(%, %)
```

```akk
// Heavy saturation without harshness
sqr(440) |> smooth(%, 10) |> out(%, %)
```

```akk
// High-frequency content stays clean
saw(880) |> smooth(%, 8) |> out(%, %)
```

Related: [tanh](#tanh), [tube](#tube)

---

## tape

**Tape Saturation** - Soft compression with high-frequency warmth.

| Param  | Type   | Default | Description |
|--------|--------|---------|-------------|
| in     | signal | -       | Input signal |
| drive  | number | 3.0     | Drive amount (1-10) |
| warmth | number | 0.3     | HF rolloff (0-1) |

Emulates magnetic tape characteristics: a wide linear region, soft compression at extremes, and subtle high-frequency rolloff. The warmth parameter controls how much the high frequencies are smoothed. Uses 2x oversampling internally.

```akk
// Tape-style glue
saw(110) |> tape(%, 4, 0.4) |> out(%, %)
```

```akk
// Lo-fi tape warmth
sqr(220) |> tape(%, 6, 0.8) |> out(%, %)
```

```akk
// Subtle tape coloration
sin(440) |> tape(%, 2, 0.2) |> out(%, %)
```

Related: [tube](#tube), [tanh](#tanh)

---

## xfmr

**Transformer Saturation** - Bass-heavy saturation.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| drive | number | 3.0     | Drive amount (1-10) |
| bass  | number | 5.0     | Bass saturation (1-10) |

Aliases: `transformer`, `console`

Emulates transformer/console saturation where bass frequencies saturate more heavily than highs (magnetic core saturation). Creates thick, punchy low-end while keeping highs relatively clean. Great for drums and bass. Uses 2x oversampling internally.

```akk
// Punchy bass
saw(55) |> xfmr(%, 4, 8) |> out(%, %)
```

```akk
// Console warmth on synths
saw(220) |> xfmr(%, 3, 4) |> out(%, %)
```

```akk
// Thick transformer saturation
sqr(110) |> xfmr(%, 5, 6) |> lp(%, 2000) |> out(%, %)
```

Related: [tube](#tube), [tape](#tape)

---

## excite

**Harmonic Exciter** - Adds controlled harmonics to high frequencies.

| Param  | Type   | Default | Description |
|--------|--------|---------|-------------|
| in     | signal | -       | Input signal |
| amount | number | 0.5     | Exciter intensity (0-1) |
| freq   | number | 3000    | Corner frequency (1000-10000 Hz) |

Aliases: `exciter`, `aural`

Adds harmonic content to frequencies above the corner frequency only, similar to an Aphex Aural Exciter. Creates presence and sparkle without adding low-frequency mud. Uses 2x oversampling internally.

```akk
// Add presence to synths
saw(220) |> excite(%, 0.5, 3000) |> out(%, %)
```

```akk
// Brighten with higher corner
tri(440) |> excite(%, 0.7, 5000) |> out(%, %)
```

```akk
// Subtle air and sparkle
sin(110) |> excite(%, 0.3, 4000) |> out(%, %)
```

Related: [tube](#tube), [tanh](#tanh)
