---
title: Piano Roll
category: builtins
order: 24
keywords: [pianoroll, piano, roll, visualization, viz, beats, showGrid, scale, chromatic, pentatonic, octave, pattern, events, grid, midi]
group: visualizations
icon: Grid3x3
tagline: Note-event grid for pattern visualization.
---

# Piano Roll

A pattern-event visualizer that draws notes as rectangles on a pitch-time grid. It sits in the signal chain as a pass-through.

## pianoroll

**Piano Roll** - Pattern event visualization on a pitch-time grid.

| Param | Type   | Description |
|-------|--------|-------------|
| in    | signal | Pattern signal |
| name  | string | Display label (optional) |
| opts  | record | Options (optional) |

**Options:**

| Option   | Type            | Default       | Description |
|----------|-----------------|---------------|-------------|
| width    | number / string | 200           | Width in pixels, or `"100%"` for full width |
| height   | number / string | 50            | Height in pixels, or `"100%"` for full height |
| beats    | number          | auto          | Number of beats visible in the window |
| showGrid | boolean         | true          | Show beat grid lines |
| scale    | string          | `"chromatic"` | Scale filter: `"chromatic"`, `"pentatonic"`, or `"octave"` |

Displays pattern events as rectangles on a piano-roll grid. The playhead scrolls through the pattern in real time.

```akk
// Basic piano roll
pat("c4 e4 g4 b4") |> pianoroll(@)
```

## beats

Number of beats visible in the scrolling window. By default, the pianoroll auto-fits the pattern's cycle length. Set explicitly to show more or less of the timeline at once.

```akk
// Wide view with 8 beats visible
pat("c4 e4 g4 c5 ~ e4 g4 b4") |> pianoroll(@, "melody", {beats: 8, width: 400, height: 80})
```

## showGrid

Toggle the beat-grid overlay. Off can be useful for screenshots or when overlaying multiple pianorolls.

## scale

Highlight notes that fall inside a named scale; notes outside are dimmed. Helps eye-check whether a melody stays in key.

## chromatic

`"chromatic"` (default): every note is highlighted; no filtering applied.

## pentatonic

`"pentatonic"`: highlights C major pentatonic (C, D, E, G, A). Notes outside this set are dimmed.

```akk
// Pentatonic scale filter, no grid
pat("c4 d4 e4 f4 g4 a4 b4 c5") |> pianoroll(@, "scale check", {scale: "pentatonic", showGrid: false})
```

## octave

`"octave"`: highlights only root notes (C across octaves). Useful for verifying tonal centers in a long progression.

Related: [oscilloscope](oscilloscope), [spectrum](spectrum)
