---
title: Distortion
category: builtins
order: 8
keywords: [distortion, saturate, softclip, bitcrush, fold, wavefold, drive, crush, lofi, tube, valve, smooth, adaa, tape, warmth, xfmr, transformer, excite, exciter, harmonics]
group: effects
subgroup: nonlinear
icon: Zap
tagline: Soft to extreme nonlinearity — saturation, folding, and lo-fi.
subfeatures:
  - name: Saturate
    anchor: saturate
    tagline: Tape-style tanh saturation.
    snippet: 'osc("saw", 110) |> saturate(@, 3)'
  - name: Softclip
    anchor: softclip
    tagline: Cubic soft clipper.
    snippet: 'osc("saw", 220) |> softclip(@, 0.7)'
  - name: Bitcrush
    anchor: bitcrush
    tagline: 8-bit lo-fi color.
    snippet: 'osc("saw", 220) |> bitcrush(@, 8, 0.5)'
    icon: Binary
  - name: Fold
    anchor: fold
    tagline: Wavefolder for harmonic chaos.
    snippet: 'osc("sin", 110) * 2 |> fold(@, 0.5)'
  - name: Tube
    anchor: tube
    tagline: Asymmetric valve warmth.
    snippet: 'osc("saw", 110) |> tube(@, 5, 0.1)'
    icon: Cylinder
  - name: Smooth
    anchor: smooth
    tagline: Anti-aliased smooth distortion.
    snippet: 'osc("saw", 220) |> smooth(@, 3)'
  - name: Tape
    anchor: tape
    tagline: Tape compression and saturation.
    snippet: 'osc("saw", 110) |> tape(@, 4, 0.4)'
  - name: XFMR
    anchor: xfmr
    tagline: Transformer-style coloration.
    snippet: 'osc("saw", 55) |> xfmr(@, 4, 8)'
  - name: Excite
    anchor: excite
    tagline: Harmonic exciter.
    snippet: 'osc("saw", 220) |> excite(@, 0.5, 3000)'
---

# Distortion

Distortion effects add harmonic content by clipping, saturating, or otherwise mangling signals.

## saturate

**Tanh saturation** - Smooth hyperbolic tangent waveshaping with drive control.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| drive | number | 2.0     | Drive amount (1-10+) |

Aliases: `distort`

Warm, tube-like saturation that adds odd harmonics while preventing harsh clipping. Higher drive values increase saturation intensity.

**Note:** For pure math `tanh(x)` (hyperbolic tangent), see [Math Functions](math#tanh). The `saturate()` function wraps `tanh` with drive control for convenient distortion.

```akk
// Warm overdrive
osc("saw", 110) |> saturate(@, 3) |> out(@)
```

```akk
// Heavy saturation on bass
osc("saw", 55) |> saturate(@, 8) |> lp(@, 400) |> out(@)
```

```akk
// Subtle warming
osc("sin", 220) |> saturate(@, 1.5) |> out(@)
```

Related: [softclip](#softclip), [fold](#fold), [tanh (math)](math#tanh)

---

## softclip

**Soft clipping** - Polynomial soft clipper with adjustable threshold.

| Param  | Type   | Default | Description |
|--------|--------|---------|-------------|
| in     | signal | -       | Input signal |
| thresh | number | 0.5     | Clipping threshold (0-1) |

Softer than hard clipping, rounds off peaks smoothly. Lower threshold values create more aggressive distortion.

```akk
// Gentle compression-like softclip
osc("saw", 220) |> softclip(@, 0.7) |> out(@)
```

```akk
// Aggressive softclip
osc("sqr", 110) |> softclip(@, 0.2) |> out(@)
```

Related: [saturate](#saturate)

---

## bitcrush

**Bit crusher** - Reduces bit depth and sample rate.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| bits  | number | 8.0     | Bit depth (1-16) |
| rate  | number | 0.5     | Sample rate reduction (0-1) |

Aliases: `crush`

Creates lo-fi digital artifacts by quantizing amplitude and reducing sample rate. Lower values create more extreme degradation.

```akk
// Classic 8-bit sound
osc("saw", 220) |> bitcrush(@, 8, 0.5) |> out(@)
```

```akk
// Extreme lo-fi
osc("saw", 110) |> bitcrush(@, 4, 0.2) |> out(@)
```

```akk
// Subtle grit
osc("saw", 440) |> bitcrush(@, 12, 0.8) |> out(@)
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

Classic West Coast synthesis technique. When the signal exceeds the threshold, it folds back, producing complex harmonic spectra.

```akk
// Basic wavefold
osc("sin", 110) * 2 |> fold(@, 0.5) |> out(@)
```

```akk
// Animated wavefolding
osc("sin", 110) * (1.5 + osc("sin", 0.2)) |> fold(@, 0.4) |> out(@)
```

```akk
// Aggressive folding
osc("tri", 55) * 4 |> fold(@, 0.3) |> lp(@, 2000) |> out(@)
```

Related: [saturate](#saturate), [softclip](#softclip)

---

## tube

**Tube saturation** - Asymmetric waveshaping with even harmonics.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| drive | number | 5.0     | Drive amount (1-20) |
| bias  | number | 0.1     | Asymmetry bias (0-0.3) |

Aliases: `valve`, `triode`

Emulates triode tube saturation with an asymmetric transfer function. Unlike symmetric saturators like tanh which produce only odd harmonics, tube adds even harmonics (especially 2nd) for a warmer, vintage character. Uses 2x oversampling internally.

```akk
// Warm vintage drive
osc("saw", 110) |> tube(@, 5, 0.1) |> out(@)
```

```akk
// Aggressive tube distortion
osc("saw", 55) |> tube(@, 15, 0.2) |> lp(@, 800) |> out(@)
```

```akk
// Subtle 2nd harmonic enhancement
osc("sin", 220) |> tube(@, 2, 0.15) |> out(@)
```

Related: [saturate](#saturate), [tape](#tape)

---

## smooth

**ADAA saturation** - Alias-free saturation using antiderivative antialiasing.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| drive | number | 5.0     | Drive amount (1-20) |

Aliases: `adaa`

Tanh saturation with built-in antialiasing. Uses the ADAA (Antiderivative Antialiasing) algorithm to eliminate aliasing artifacts without oversampling. Suited for saturation on full mixes or high-frequency content.

```akk
// Clean master bus saturation
osc("saw", 220) |> smooth(@, 3) |> out(@)
```

```akk
// Heavy saturation without harshness
osc("sqr", 440) |> smooth(@, 10) |> out(@)
```

```akk
// High-frequency content stays clean
osc("saw", 880) |> smooth(@, 8) |> out(@)
```

Related: [saturate](#saturate), [tube](#tube)

---

## tape

**Tape saturation** - Soft compression with high-frequency warmth.

| Param  | Type   | Default | Description |
|--------|--------|---------|-------------|
| in     | signal | -       | Input signal |
| drive  | number | 3.0     | Drive amount (1-10) |
| warmth | number | 0.3     | HF rolloff (0-1) |
| soft_thresh | number | 0.5 | Saturation onset threshold |
| warmth_scale | number | 0.7 | HF rolloff amount |

Emulates magnetic tape characteristics: a wide linear region, soft compression at extremes, and subtle high-frequency rolloff. The warmth parameter controls how much the high frequencies are smoothed. Uses 2x oversampling internally.

The advanced `soft_thresh` parameter controls where saturation begins (lower = earlier saturation). The `warmth_scale` parameter controls the intensity of the high-frequency rolloff effect.

```akk
// Tape-style glue
osc("saw", 110) |> tape(@, 4, 0.4) |> out(@)
```

```akk
// Lo-fi tape warmth
osc("sqr", 220) |> tape(@, 6, 0.8) |> out(@)
```

```akk
// Subtle tape coloration
osc("sin", 440) |> tape(@, 2, 0.2) |> out(@)
```

```akk
// More aggressive saturation with stronger HF rolloff
osc("saw", 110) |> tape(@, 5, 0.5, 0.3, 0.9) |> out(@)
```

Related: [tube](#tube), [saturate](#saturate)

---

## xfmr

**Transformer saturation** - Bass-heavy saturation.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| drive | number | 3.0     | Drive amount (1-10) |
| bass  | number | 5.0     | Bass saturation (1-10) |
| bass_freq | number | 60.0 | Bass extraction cutoff (Hz) |

Aliases: `transformer`, `console`

Emulates transformer/console saturation where bass frequencies saturate more heavily than highs (magnetic core saturation). Produces thick, punchy low-end while keeping highs relatively clean. Suited for drums and bass. Uses 2x oversampling internally.

The `bass_freq` parameter controls the cutoff frequency for bass extraction. Higher values include more of the signal in the bass saturation path.

```akk
// Punchy bass
osc("saw", 55) |> xfmr(@, 4, 8) |> out(@)
```

```akk
// Console warmth on synths
osc("saw", 220) |> xfmr(@, 3, 4) |> out(@)
```

```akk
// Thick transformer saturation
osc("sqr", 110) |> xfmr(@, 5, 6) |> lp(@, 2000) |> out(@)
```

```akk
// Extended bass range saturation
osc("saw", 110) |> xfmr(@, 4, 6, 120) |> out(@)
```

Related: [tube](#tube), [tape](#tape)

---

## excite

**Harmonic exciter** - Adds controlled harmonics to high frequencies.

| Param  | Type   | Default | Description |
|--------|--------|---------|-------------|
| in     | signal | -       | Input signal |
| amount | number | 0.5     | Exciter intensity (0-1) |
| freq   | number | 3000    | Corner frequency (1000-10000 Hz) |
| harm_odd | number | 0.4   | Odd harmonic mix (0-1) |
| harm_even | number | 0.6  | Even harmonic mix (0-1) |

Aliases: `exciter`, `aural`

Adds harmonic content to frequencies above the corner frequency only, similar to an Aphex Aural Exciter. Adds presence without low-frequency mud. Uses 2x oversampling internally.

The `harm_odd` and `harm_even` parameters control the mix of odd and even harmonics. Even harmonics sound warmer; odd harmonics add edge and presence.

```akk
// Add presence to synths
osc("saw", 220) |> excite(@, 0.5, 3000) |> out(@)
```

```akk
// Brighten with higher corner
osc("tri", 440) |> excite(@, 0.7, 5000) |> out(@)
```

```akk
// Subtle air and sparkle
osc("sin", 110) |> excite(@, 0.3, 4000) |> out(@)
```

```akk
// Warmer excitation (more even harmonics)
osc("saw", 220) |> excite(@, 0.5, 3000, 0.2, 0.8) |> out(@)
```

Related: [tube](#tube), [saturate](#saturate)
