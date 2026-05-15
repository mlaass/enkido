---
title: Visualizations
category: builtins
order: 12
keywords: [visualization, viz, oscilloscope, waveform, spectrum, waterfall, pianoroll, piano, roll, display, monitor, probe]
---

# Visualizations

Inline visualizations render directly in the editor. Each visualization is a pass-through node in the signal chain: audio flows through unchanged.

All visualization functions accept an optional name string and an options record. Common options shared by all types:

| Option | Type            | Default | Description |
|--------|-----------------|---------|-------------|
| width  | number / string | 200     | Width in pixels, or `"100%"` for full width |
| height | number / string | 50      | Height in pixels, or `"100%"` for full height |

## oscilloscope

Time-domain waveform display with trigger stabilization. See [oscilloscope](oscilloscope) for the full reference.

```akk
osc("sin", 440) |> oscilloscope(@) |> out(@)
```

## waveform

Time-domain envelope display showing min/max amplitude. See [waveform](waveform) for the full reference.

```akk
osc("saw", 110) |> waveform(@) |> out(@)
```

## spectrum

Frequency-domain FFT display. See [spectrum](spectrum) for the full reference.

```akk
osc("saw", 220) |> spectrum(@) |> out(@)
```

## waterfall

Scrolling spectrogram. Time-frequency display with color-mapped amplitude. See [waterfall](waterfall) for the full reference.

```akk
osc("saw", 220) |> waterfall(@) |> out(@)
```

## pianoroll

Pattern-event visualization on a pitch-time grid. See [pianoroll](pianoroll) for the full reference.

```akk
pat("c4 e4 g4 b4") |> pianoroll(@)
```

Related: [oscilloscope](oscilloscope), [waveform](waveform), [spectrum](spectrum), [waterfall](waterfall), [pianoroll](pianoroll)
