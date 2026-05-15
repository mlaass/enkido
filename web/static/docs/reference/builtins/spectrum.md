---
title: Spectrum Analyzer
category: builtins
order: 22
keywords: [spectrum, fft, frequency, visualization, viz, analyzer, logScale, minDb, maxDb, frequency-domain, display, monitor]
group: visualizations
icon: BarChart
tagline: Real-time FFT magnitude display.
---

# Spectrum Analyzer

A frequency-domain visualizer that runs an FFT on the signal. It sits in the chain as a pass-through, so audio flows through unchanged.

## spectrum

**Spectrum Analyzer** - Frequency-domain FFT display.

| Param | Type   | Description |
|-------|--------|-------------|
| in    | signal | Input signal |
| name  | string | Display label (optional) |
| opts  | record | Options (optional) |

**Options:**

| Option   | Type            | Default | Description |
|----------|-----------------|---------|-------------|
| width    | number / string | 200     | Width in pixels, or `"100%"` for full width |
| height   | number / string | 50      | Height in pixels, or `"100%"` for full height |
| fft      | number          | 1024    | FFT size: 256, 512, 1024, or 2048 |
| logScale | boolean         | false   | Logarithmic frequency axis |
| minDb    | number          | -90     | Minimum dB for display range |
| maxDb    | number          | 0       | Maximum dB for display range |

Displays a real-time frequency spectrum. Larger FFT sizes give better frequency resolution but slower updates.

```akk
// Basic spectrum
osc("saw", 220) |> spectrum(@) |> out(@)
```

## fft

FFT window size. Larger values resolve closely-spaced frequencies better but slow down the update rate. Valid sizes: 256, 512, 1024 (default), 2048.

```akk
// High resolution FFT
osc("saw", 55) |> spectrum(@, "bass detail", {fft: 2048, width: 400}) |> out(@)
```

## logScale

Use a logarithmic frequency axis. Linear (default) spaces frequencies evenly; logarithmic gives more detail to low frequencies, matching how we perceive pitch.

```akk
// Log scale matches human pitch perception
osc("saw", 110) |> lp(@, 2000) |> spectrum(@, "filtered", {logScale: true}) |> out(@)
```

## minDb

Lower bound of the dB display range. Values below this are clamped to the bottom of the plot. Useful for cutting visual noise floor.

## maxDb

Upper bound of the dB display range. Loud signals above this are clamped to the top.

```akk
// Tight dB window for a hot signal
osc("saw", 110) |> spectrum(@, {minDb: -40, maxDb: 0}) |> out(@)
```

Related: [waterfall](waterfall), [oscilloscope](oscilloscope)
