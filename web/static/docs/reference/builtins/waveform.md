---
title: Waveform
category: builtins
order: 21
keywords: [waveform, visualization, viz, envelope, time-domain, scale, filled, line, display, monitor, probe]
group: visualizations
icon: AudioWaveform
tagline: Continuous waveform display.
---

# Waveform

A time-domain envelope visualizer that draws the min/max amplitude of the signal over time. It sits in the chain as a pass-through, so audio flows through unchanged.

## waveform

**Waveform** - Time-domain envelope display showing min/max amplitude.

| Param | Type   | Description |
|-------|--------|-------------|
| in    | signal | Input signal |
| name  | string | Display label (optional) |
| opts  | record | Options (optional) |

**Options:**

| Option | Type            | Default | Description |
|--------|-----------------|---------|-------------|
| width  | number / string | 200     | Width in pixels, or `"100%"` for full width |
| height | number / string | 50      | Height in pixels, or `"100%"` for full height |
| scale  | number          | 1.0     | Amplitude multiplier (2.0 = zoom in 2x) |
| filled | boolean         | true    | Filled envelope or line waveform |

Displays the signal as a min/max envelope over time. In filled mode (default), shows a shaded region between the minimum and maximum amplitude at each point. In line mode (`filled: false`), draws a single center-line waveform similar to the oscilloscope.

```akk
// Basic waveform
osc("saw", 110) |> waveform(@) |> out(@)
```

## scale

Amplitude multiplier for the displayed envelope. Useful when the signal is quiet and you want to see detail without changing the actual gain.

```akk
// Zoomed in for a quiet signal
osc("sin", 440) * 0.3 |> waveform(@, "quiet signal", {scale: 3.0}) |> out(@)
```

## filled

Toggle between filled-envelope mode (`true`, default) and line-waveform mode (`false`). Line mode looks similar to the oscilloscope but without trigger stabilization.

```akk
// Line waveform
osc("sin", 440) |> waveform(@, {filled: false}) |> out(@)
```

```akk
// Wide filled envelope
osc("saw", 55) |> lp(@, 400) |> waveform(@, "bass", {width: 400, height: 80}) |> out(@)
```

Related: [oscilloscope](oscilloscope), [spectrum](spectrum)
