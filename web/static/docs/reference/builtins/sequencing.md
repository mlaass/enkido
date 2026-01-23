---
title: Sequencing & Timing
category: builtins
order: 4
keywords: [sequencing, timing, lfo, trigger, euclid, euclidean, seq_step, timeline, clock, rhythm, pattern]
---

# Sequencing & Timing

Timing and sequencing functions create rhythmic patterns, triggers, and automation curves synchronized to the global clock.

## clock

**Clock** - Returns the current clock position.

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| -     | -    | -       | No parameters |

Returns the current position in the clock cycle. Use with other timing functions or for sync.

```akk
// Use clock for tempo-synced effects
osc("saw", 110) |> delay(%, clock() / 4, 0.4) |> out(%, %)
```

Related: [trigger](#trigger), [lfo](#lfo)

---

## lfo

**LFO** - Low Frequency Oscillator with optional duty cycle.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| rate  | signal | -       | Rate in Hz |
| duty  | number | 0.5     | Duty cycle for pulse (0-1) |

A low-frequency oscillator for modulation. The duty parameter controls the pulse width.

```akk
// Vibrato
osc("sin", 220 + lfo(5) * 10) |> out(%, %)
```

```akk
// Tremolo
osc("saw", 220) * (0.5 + lfo(4) * 0.5) |> out(%, %)
```

```akk
// Filter sweep
osc("saw", 110) |> lp(%, 500 + lfo(0.2) * 1500) |> out(%, %)
```

Related: [clock](#clock), [trigger](#trigger)

---

## trigger

**Trigger** - Generates trigger pulses at division of the beat.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| div   | number | -       | Triggers per beat |

Generates short impulses at regular intervals. A div of 4 means 4 triggers per beat (16th notes at 4/4).

```akk
// Kick drum on quarter notes
osc("sin", 55) * ar(trigger(1), 0.01, 0.2) |> out(%, %)
```

```akk
// Hi-hat on 8th notes
osc("noise") |> hp(%, 8000) * ar(trigger(2), 0.001, 0.05) |> out(%, %)
```

```akk
// Fast arpeggio triggers
osc("saw", mtof(48 + seq_step(1) * 12)) * ar(trigger(8)) |> out(%, %)
```

Related: [euclid](#euclid), [lfo](#lfo)

---

## euclid

**Euclidean Rhythm** - Generates Euclidean rhythm patterns.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| hits  | number | -       | Number of hits in pattern |
| steps | number | -       | Total steps in pattern |
| rot   | number | 0       | Rotation offset |

Creates rhythms by distributing hits as evenly as possible across steps. Classic patterns: (3,8) = Cuban tresillo, (5,8) = West African bell.

```akk
// Tresillo pattern
osc("sin", 55) * ar(euclid(3, 8), 0.01, 0.15) |> out(%, %)
```

```akk
// West African bell
osc("noise") |> hp(%, 6000) * ar(euclid(5, 8), 0.001, 0.03) |> out(%, %)
```

```akk
// Rotated pattern
osc("saw", 110) * ar(euclid(5, 16, 2)) |> lp(%, 800) |> out(%, %)
```

Related: [trigger](#trigger), [seq_step](#seq_step)

---

## seq_step

**Step Sequencer** - Returns current step index.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| speed | number | -       | Steps per beat |

Returns the current step position as an integer, useful for indexing into arrays or creating melodic patterns.

```akk
// 4-note pattern
osc("saw", mtof(48 + seq_step(2) * 3)) * ar(trigger(2)) |> out(%, %)
```

```akk
// Fast arpeggio
osc("tri", mtof(36 + seq_step(8) * 7)) * ar(trigger(8)) |> out(%, %)
```

Related: [trigger](#trigger), [euclid](#euclid), [timeline](#timeline)

---

## timeline

**Timeline** - Breakpoint automation envelope.

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| -     | -    | -       | Configured via pattern syntax |

Creates smooth automation curves between breakpoints. Used for complex parameter automation synced to the clock.

```akk
// Volume automation
osc("saw", 220) * timeline() |> out(%, %)
```

Related: [lfo](#lfo), [seq_step](#seq_step)
