---
title: Oscilloscope
category: builtins
order: 20
keywords: [oscilloscope, visualization, viz, time-domain, trigger, triggerLevel, triggerEdge, rising, falling, display, monitor, probe, scope]
group: visualizations
icon: LineChart
tagline: Triggered time-domain probe.
---

# Oscilloscope

A time-domain visualizer that draws the signal as a waveform with trigger stabilization. It sits in the signal chain as a pass-through, so audio flows through unchanged.

## oscilloscope

**Oscilloscope** - Time-domain waveform display with trigger stabilization.

| Param | Type   | Description |
|-------|--------|-------------|
| in    | signal | Input signal |
| name  | string | Display label (optional) |
| opts  | record | Options (optional) |

**Options:**

| Option       | Type            | Default    | Description |
|--------------|-----------------|------------|-------------|
| width        | number / string | 200        | Width in pixels, or `"100%"` for full width |
| height       | number / string | 50         | Height in pixels, or `"100%"` for full height |
| triggerLevel | number          | 0          | Amplitude threshold for trigger |
| triggerEdge  | string          | `"rising"` | Trigger edge: `"rising"` or `"falling"` |

Displays a real-time oscilloscope view of the signal. The trigger stabilizes the waveform so it doesn't drift horizontally. Set `triggerLevel` to the amplitude where the waveform should "lock" and `triggerEdge` to choose which direction of crossing to trigger on.

```akk
// Basic oscilloscope
osc("sin", 440) |> oscilloscope(@) |> out(@)
```

## rising

Trigger on a rising edge (signal crossing the threshold from below to above). This is the default and gives the most stable picture for typical waveforms.

```akk
// Named with trigger at zero-crossing
osc("saw", 110) |> oscilloscope(@, "saw wave", {triggerLevel: 0, triggerEdge: "rising"}) |> out(@)
```

## falling

Trigger on a falling edge (signal crossing the threshold from above to below). Useful when the falling slope is the visually distinctive feature.

```akk
// Wide display with falling edge trigger
osc("sqr", 220) |> oscilloscope(@, "square", {width: 400, triggerEdge: "falling"}) |> out(@)
```

## triggerLevel

The amplitude threshold the signal must cross to start a frame. `0` is the typical zero-crossing trigger; non-zero values can isolate a specific point in a complex waveform.

```akk
// Trigger at +0.3 amplitude
osc("sin", 220) |> oscilloscope(@, {triggerLevel: 0.3}) |> out(@)
```

## triggerEdge

Which direction of threshold crossing fires the trigger. One of `"rising"` (default) or `"falling"`.

Related: [waveform](#waveform), [spectrum](spectrum)
