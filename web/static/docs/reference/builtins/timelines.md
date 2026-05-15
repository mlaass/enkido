---
title: Timelines
category: builtins
order: 19
keywords: [timeline, breakpoint, automation, envelope, curve, curve-notation, scheduled, clock, sync, breakpoints, parameter]
group: sequencing
subgroup: timing
icon: Clock
tagline: Time-driven automation with breakpoints and curves.
subfeatures:
  - name: Timeline
    anchor: timeline
    tagline: Sample-accurate parameter automation.
    snippet: "osc(\"saw\", 220) * timeline(\"__/''\")"
  - name: Breakpoints
    anchor: breakpoint
    tagline: Linear and curved segments.
  - name: Curves
    anchor: curve
    tagline: Curve interpolation between values.
  - name: Automation
    anchor: automation
    tagline: Synced parameter automation.
---

# Timelines

Breakpoint automation envelopes synced to the clock. Use `timeline()` to define smooth parameter curves with arbitrary shapes: slower than an `lfo()` and more expressive than a static value. Curves are written in **curve notation** (see the mini-notation reference), which encodes breakpoint amplitudes and segment shapes as a string of characters.

## timeline

**Timeline** - Breakpoint automation envelope from curve notation.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| curve | string | -       | Curve notation string (e.g. `"__/''"`) |

Creates smooth automation curves between breakpoints. The string is parsed at compile time into a fixed list of (time, value, segment-shape) breakpoints, then loaded into a state cell that interpolates between them in sync with the clock.

```akk
// Volume swell synced to one cycle
osc("saw", 220) * timeline("__/''") |> out(@)
```

## breakpoint

A **breakpoint** is one (time, value) pair in the curve. Each character in the curve string is one breakpoint at an evenly-spaced position; the value is the character's amplitude (`_` low, `'` high, `/` and `\` for ramps).

```akk
// Four breakpoints over one cycle
osc("sin", 220) * timeline("_/''\\") |> out(@)
```

## curve

The **curve string** is an ASCII sketch of the envelope shape. See [Curve Notation](../mini-notation/curve-notation) for the full character vocabulary; the most common shapes are step (`_`/`'`), ramp (`/`/`\`), and held value (`-`).

```akk
// Filter sweep envelope
osc("saw", 110) |> lp(@, 200 + timeline("__/''__\\__") * 2000) |> out(@)
```

## automation

A timeline is **automation**: a parameter value over time. Useful for amp envelopes longer than `ar()` allows, filter sweeps that span a phrase, or evolving modulation depths.

```akk
// Long amp envelope
osc("saw", 220) * timeline("_/''----\\__") |> out(@)
```

## sync

Timelines are **clock-synced**. The full curve length is `4 × cycle_span` beats, where the cycle span is derived from the curve string length. This means doubling the string length doubles the duration; the timeline loops in lockstep with the global clock.

## scheduled

Timelines are **scheduled** at compile time, not driven by audio-rate signals. The breakpoints are baked into a state cell and the runtime interpolates between them based on the clock position. This is cheaper than running an LFO at audio rate.

## clock

The **clock** position drives timeline playback. Stopping the transport stops the timeline; resetting the clock resets the timeline.

Related: [lfo](sequencing#lfo), [trigger](sequencing#trigger), [Curve Notation](../mini-notation/curve-notation)
