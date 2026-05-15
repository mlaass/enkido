---
title: Samplers
category: builtins
order: 15
keywords: [sampler, sample, sample_loop, drum, kit, "808", "909", bd, sd, hh, oh, cp, percussion, playback, one-shot, loop, button, trigger]
group: instruments
subgroup: sample-based
icon: Drum
tagline: One-shot and looping playback with kit shortcuts.
---

# Samplers

Samplers play recorded audio. The common path is a **sample pattern** like `pat("bd sd hh sd")`, which triggers events from the loaded sample bank. nkido ships with a default 808 drum kit so common drum names work out of the box. For trigger-driven one-shots — e.g. firing a kick on a button press — `sample()` accepts the same names as patterns. `sample_loop()` plays a sample looped while a gate is held.

## sample

**One-shot sampler** - Trigger a sample on every rising edge.

| Param | Type            | Default | Description |
|-------|-----------------|---------|-------------|
| trig  | signal          | -       | Trigger signal (rising edge plays sample) |
| pitch | signal / number | -       | Playback rate (1.0 = original, 2.0 = octave up, 0.5 = octave down) |
| id    | string / number | -       | Sample name (recommended) or numeric sample-bank ID |

The third argument accepts either a **sample-name string** (preferred) or a numeric sample-bank ID:

- `"bd"` — load and play the default kit's bass drum
- `"bd:3"` — variant `3` of `bd` (i.e. `bd3` from the kit)
- `"Dirt-Samples/amencutup:0"` — sample from a named bank loaded via `samples("...")`

When you pass a string, the compiler registers the name in the required-samples ledger so the host loads it before playback; the bank-assigned numeric ID is patched into the bytecode at load time.

The numeric form is reserved for advanced use where you already know the bank's internal ID. Most patches should use the string form.

```akk
// Fire the default kit's kick on a button press
hit = button("Hit!")
sample(hit, 1.0, "bd") |> out(@)

// Pitch-shifted snare on a clock pulse
sample(beat(1), 0.5, "sd") |> out(@)

// Variant from a named external bank
samples("github:tidalcycles/Dirt-Samples")
sample(beat(2), 1.0, "Dirt-Samples/amencutup:0") |> out(@)
```

For sequenced playback (different sample on each step), use the [mini-notation pattern](sequencing) form `s"bd sd hh"` instead — it drives the same opcode but lets you express a pattern.

## sample_loop

**Looping sampler** - Loop a sample while the gate is held.

| Param | Type            | Default | Description |
|-------|-----------------|---------|-------------|
| trig  | signal          | -       | Gate signal (>0 plays, 0 stops) |
| pitch | signal / number | -       | Playback rate |
| id    | string / number | -       | Sample name (recommended) or numeric sample-bank ID |

Plays the sample looped. Useful for sustained material like chords or pads recorded as samples. Same name/ID semantics as `sample()`.

```akk
// Looped pad sample on a slow clock
sample_loop(beat(0.25), 1.0, "pad") |> out(@)
```

## bd

The **bass drum**. Default kit slot for `bd` patterns; variants `bd2` through `bd16` cover different kicks from the 808 bank.

```akk
// Four-on-the-floor kick
pat("bd bd bd bd") |> out(@)
```

## sd

The **snare drum**. Default kit slot for `sd`; variants `sd2`–`sd8` give alternate snares.

```akk
// Backbeat
pat("~ sd ~ sd") |> out(@)
```

## hh

The **hi-hat closed**. Variants like `oh` (open hat), `cp` (clap), `rim` (rimshot) round out the percussion set.

```akk
// 16th-note hats
pat("hh*16") |> out(@)
```

## oh

The **open hi-hat**. Use it to vary the closed-hat pattern; typical syncopation drops an `oh` on the off-beat.

```akk
// Closed/open hat variation
pat("hh hh oh hh") |> out(@)
```

## cp

The **clap**. Layered on the backbeat for a snare reinforcement.

```akk
// Snare + clap layered
pat("~ [sd cp] ~ [sd cp]") |> out(@)
```

Related: [samples-loading](samples-loading), [soundfonts](soundfonts), [sequencing](sequencing)
