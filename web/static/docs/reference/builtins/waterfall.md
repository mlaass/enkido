---
title: Waterfall
category: builtins
order: 23
keywords: [waterfall, spectrogram, fft, visualization, viz, gradient, magma, viridis, inferno, thermal, grayscale, time-frequency, scrolling, angle, speed, minDb, maxDb]
group: visualizations
icon: AreaChart
tagline: Scrolling time-frequency spectrogram.
---

# Waterfall

A scrolling spectrogram visualizer: time on one axis, frequency on the other, color for amplitude. It sits in the chain as a pass-through, so audio flows through unchanged.

## waterfall

**Waterfall / Spectrogram** - Time-frequency display showing spectrum evolution over time.

| Param | Type   | Description |
|-------|--------|-------------|
| in    | signal | Input signal |
| name  | string | Display label (optional) |
| opts  | record | Options (optional) |

**Options:**

| Option   | Type            | Default   | Description |
|----------|-----------------|-----------|-------------|
| width    | number / string | 300       | Width in pixels, or `"100%"` for full width |
| height   | number / string | 150       | Height in pixels, or `"100%"` for full height |
| fft      | number          | 1024      | FFT size: 256, 512, 1024, or 2048 |
| gradient | string          | `"magma"` | Color gradient preset |
| angle    | number          | 180       | Scroll direction in degrees |
| speed    | number          | 40        | Scroll speed in pixels per second |
| minDb    | number          | -90       | Minimum dB for color mapping |
| maxDb    | number          | 0         | Maximum dB for color mapping |

The default size is 300×150 (larger than the other visualizations).

```akk
// Basic waterfall
osc("saw", 220) |> waterfall(@) |> out(@)
```

## gradient

Color gradient preset. Available presets: `"magma"` (default), `"viridis"`, `"inferno"`, `"thermal"`, `"grayscale"`.

```akk
// Viridis gradient
osc("saw", 110) |> lp(@, 1000 + osc("sin", 0.5) * 2000) |> waterfall(@, "sweep", {gradient: "viridis"}) |> out(@)
```

## magma

Default warm-to-cool gradient with deep purple at low energy and yellow at high. Good general-purpose contrast.

## viridis

Perceptually-uniform gradient (purple-green-yellow). Easy to read for color-vision-deficient viewers.

## inferno

Black-red-yellow gradient. High contrast for finding peaks.

## thermal

Black-blue-cyan-yellow-red. Mimics a thermal camera readout.

## grayscale

Pure black-to-white gradient. Useful for prints or when overlaying with other visualizations.

## angle

Scroll direction in degrees. `180` (default) scrolls left to right; `0` scrolls right to left; `90` and `270` give vertical scroll.

## speed

Scroll speed in pixels per second. Lower values give a longer history; higher values respond faster to recent activity.

```akk
// Slow scroll, large display
osc("saw", 55) |> waterfall(@, "bass", {speed: 15, width: 500, height: 200, gradient: "thermal"}) |> out(@)
```

Related: [spectrum](spectrum), [oscilloscope](oscilloscope)
