---
title: Effect Parameters Convention
category: concepts
order: 6
keywords: [effect, dry, wet, mix, parallel, parameters, convention, category, additive, transform, drywet]
---

# Effect Parameters Convention

Every effect builtin in akkado — delays, reverbs, modulation, comb,
filters, distortion, dynamics — exposes the **same two mix parameters
at the end of its argument list**: `dry` and `wet`. The mix is always

```text
out = dry_in * dry + processed * wet
```

per channel. The convention means you can swap from "in-line" to
"parallel" mixing on any effect by changing one number, without
remembering each effect's bespoke parameter shape.

## Two defaults, by category

Effects fall into two musical categories with different sensible
defaults:

### Category A — Additive (parallel-mix effects)

**Delays, reverbs, modulation (chorus/flanger/phaser), comb.**
The dry signal is musically essential; the effect *adds character*
on top. Default: **`dry=1.0, wet=0.5`** — a balanced parallel mix
out of the box.

```akkado
// Reverb with default balanced mix
osc("saw", 220) * ar(trigger(2)) |> freeverb(@, 0.5, 0.5) |> out(@)

// Fully wet for send/return
osc("saw", 220) * ar(trigger(2)) |> freeverb(@, 0.5, 0.5, wet: 1.0, dry: 0.0) |> out(@)

// Subtle chorus over the dry signal
osc("saw", 220) |> chorus(@, 0.5, 0.5) |> out(@)
```

### Category B — Transform (in-line effects)

**Filters, distortion, dynamics.** The effect *replaces* the signal.
Default: **`dry=0.0, wet=1.0`** — pure processed output, back-compat
with how filters and distortion have always worked.

Parallel processing (NY compression, parallel distortion, blended
filter) is one keyword away:

```akkado
// Pure filter (default — dry=0, wet=1)
osc("saw", 220) |> lp(@, 800) |> out(@)

// Parallel ("New York") compression
osc("saw", 220) |> comp(@, -12, 4, dry: 0.5) |> out(@)

// Parallel distortion — mix some grit under the dry signal
osc("saw", 220) |> fold(@, 1.2, dry: 0.7, wet: 0.4) |> out(@)
```

## Quick reference

| Family               | Category | Default `dry` | Default `wet` |
|----------------------|----------|---------------|---------------|
| Delays (incl. `pingpong`) | A   | 1.0           | 0.5           |
| Reverbs              | A        | 1.0           | 0.5           |
| Modulation (chorus, flanger, phaser) | A | 1.0   | 0.5           |
| Comb                 | A        | 1.0           | 0.5           |
| Filters              | B        | 0.0           | 1.0           |
| Distortion           | B        | 0.0           | 1.0           |
| Dynamics (compressor, limiter, gate) | B | 0.0   | 1.0           |

## Breaking change — delays

Prior to this convention, every delay variant (`delay`, `delay_ms`,
`delay_smp`, `tap_delay`, `tap_delay_ms`, `tap_delay_smp`, `pingpong`)
defaulted to fully-wet output. They are now Category A
(`dry=1, wet=0.5`).

**If you have a patch that called `delay(in, time, fb)` and expected
100%-wet echoes**, you'll now hear the dry signal mixed in with the
echoes at half volume. Restore the prior behavior by passing `wet`
explicitly:

```akkado
// Old behavior (fully wet, no direct signal in the delay return)
osc("saw", 220) |> delay(@, 0.5, 0.4, dry: 0.0, wet: 1.0) |> out(@)
```

See the changelog entry under
[`## [Unreleased]`](https://github.com/mlaass/nkido/blob/master/CHANGELOG.md)
for the full list of affected builtins.

## Tips

- **Send/return architecture**: send to an effect with `dry=0, wet=1`,
  then mix the returned signal at the bus. Works the same way on every
  effect.
- **Wet-only chain element**: when stacking effects, set `dry=0`
  on the intermediate stages to avoid summing the unprocessed signal
  multiple times.
- **Parallel processing**: any Category-B effect becomes a parallel
  effect by setting `dry > 0`. There's no separate "wet/dry mix"
  knob — `dry` *is* that knob.

## Related

- [Delays](/docs/reference/builtins/delays)
- [Reverbs](/docs/reference/builtins/reverbs)
- [Modulation](/docs/reference/builtins/modulation)
- [Filters](/docs/reference/builtins/filters)
- [Distortion](/docs/reference/builtins/distortion)
- [Dynamics](/docs/reference/builtins/dynamics)
