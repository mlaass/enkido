---
title: Filters
category: builtins
order: 2
keywords: [filter, lp, lowpass, hp, highpass, bp, bandpass, moog, svf, cutoff, resonance, q]
---

# Filters

Filters shape the frequency content of signals by attenuating or boosting certain frequencies.

## lp

**Lowpass Filter** - Passes frequencies below the cutoff, attenuates above.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| cut   | signal | -       | Cutoff frequency in Hz |
| q     | number | 0.707   | Resonance (0.5-20) |

Aliases: `lowpass`, `svflp`

A state-variable lowpass filter. Higher Q values add resonance at the cutoff frequency.

```akk
// Basic lowpass at 800 Hz
osc("saw", 220) |> lp(%, 800) |> out(%, %)
```

```akk
// Envelope-controlled filter sweep
osc("saw", 110) |> lp(%, 200 + ar(trigger(4)) * 2000) |> out(%, %)
```

```akk
// Resonant filter
osc("saw", 110) |> lp(%, 500, 8) |> out(%, %)
```

Related: [hp](#hp), [bp](#bp), [moog](#moog)

---

## hp

**Highpass Filter** - Passes frequencies above the cutoff, attenuates below.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| cut   | signal | -       | Cutoff frequency in Hz |
| q     | number | 0.707   | Resonance (0.5-20) |

Aliases: `highpass`, `svfhp`

Removes low frequencies. Useful for thinning out sounds or creating transitions.

```akk
// Remove sub frequencies
osc("saw", 110) |> hp(%, 200) |> out(%, %)
```

```akk
// Hi-hat from noise
osc("noise") |> hp(%, 8000) * ar(trigger(8), 0.001, 0.05) |> out(%, %)
```

Related: [lp](#lp), [bp](#bp)

---

## bp

**Bandpass Filter** - Passes frequencies around the cutoff, attenuates others.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| cut   | signal | -       | Center frequency in Hz |
| q     | number | 0.707   | Bandwidth (higher = narrower) |

Aliases: `bandpass`, `svfbp`

Isolates a band of frequencies. Higher Q values create a narrower, more resonant band.

```akk
// Telephone effect
osc("saw", 220) |> bp(%, 1000, 4) |> out(%, %)
```

```akk
// Vocal formant
osc("saw", 110) |> bp(%, 500, 10) |> out(%, %)
```

Related: [lp](#lp), [hp](#hp)

---

## moog

**Moog Ladder Filter** - Classic 4-pole resonant lowpass filter.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| cut   | signal | -       | Cutoff frequency in Hz |
| res   | number | 1.0     | Resonance (0-4, self-oscillates near 4) |

Aliases: `moogladder`

The legendary Moog ladder filter with its characteristic warm, creamy sound. At high resonance values, it can self-oscillate.

```akk
// Classic Moog bass
osc("saw", 55) |> moog(%, 400, 2) |> out(%, %)
```

```akk
// Filter sweep with high resonance
osc("saw", 110) |> moog(%, 100 + osc("sin", 0.5) * 1000, 3.5) |> out(%, %)
```

```akk
// Self-oscillating filter (use as oscillator)
osc("noise") * 0.01 |> moog(%, 440, 3.9) |> out(%, %)
```

Related: [lp](#lp)
