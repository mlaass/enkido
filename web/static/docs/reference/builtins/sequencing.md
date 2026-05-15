---
title: Sequencing & Timing
category: builtins
order: 4
keywords: [sequencing, timing, lfo, trigger, euclid, euclidean, clock, rhythm, pattern, early, late, palindrome, compress, ply, linger, zoom, segment, swing, swingBy, iter, iterBack, run, binary, binaryN]
group: sequencing
subgroup: timing
icon: ListMusic
tagline: Clocks, LFOs, triggers, and pattern transforms.
subfeatures:
  - name: Clock
    anchor: clock
    tagline: Master tempo source.
    snippet: 'osc("saw", 110) |> delay(@, clock() / 4, 0.4)'
  - name: LFO
    anchor: lfo
    tagline: Bipolar low-frequency oscillator.
    snippet: 'osc("sin", 220 + lfo(5) * 10)'
  - name: Trigger
    anchor: trigger
    tagline: Edge-triggered gate generator.
    snippet: 'osc("sin", 55) * ar(trigger(1), 0.01, 0.2)'
  - name: Euclidean
    anchor: euclid
    tagline: Euclidean rhythm generator.
    snippet: 'osc("sin", 55) * ar(euclid(3, 8), 0.01, 0.15)'
  - name: Pattern Transforms
    anchor: pattern-transforms
    tagline: early, late, swing, palindrome, ply.
    snippet: 'pat("c4 e4 g4 b4").early(0.25)'
  - name: Pattern Generators
    anchor: pattern-generators
    tagline: binary, run, iter, iterBack.
    snippet: 'run(8) |> mtof(@ + 60) |> osc("saw", @)'
---

# Sequencing & Timing

Timing and sequencing functions create rhythmic patterns, triggers, and automation curves synchronized to the global clock.

For voice allocation across notes (`poly`, `mono`, `legato`, `spread`), see [polyphony](polyphony). For breakpoint envelopes (`timeline`), see [timelines](timelines). For chord voicings (`anchor`, `mode`, `voicing`, `addVoicings`), see [chords](../mini-notation/chords).

## clock

**Clock** - Returns the current clock position.

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| -     | -    | -       | No parameters |

Returns the current position in the clock cycle. Use with other timing functions or for sync.

```akk
// Use clock for tempo-synced effects
osc("saw", 110) |> delay(@, clock() / 4, 0.4) |> out(@)
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
osc("sin", 220 + lfo(5) * 10) |> out(@)
```

```akk
// Tremolo
osc("saw", 220) * (0.5 + lfo(4) * 0.5) |> out(@)
```

```akk
// Filter sweep
osc("saw", 110) |> lp(@, 500 + lfo(0.2) * 1500) |> out(@)
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
osc("sin", 55) * ar(trigger(1), 0.01, 0.2) |> out(@)
```

```akk
// Hi-hat on 8th notes
osc("noise") |> hp(@, 8000) * ar(trigger(2), 0.001, 0.05) |> out(@)
```

```akk
// Fast arpeggio triggers
pat("c4 e4 g4 c5") |> ((f) -> osc("saw", f) * ar(trigger(8))) |> out(@)
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
osc("sin", 55) * ar(euclid(3, 8), 0.01, 0.15) |> out(@)
```

```akk
// West African bell
osc("noise") |> hp(@, 6000) * ar(euclid(5, 8), 0.001, 0.03) |> out(@)
```

```akk
// Rotated pattern
osc("saw", 110) * ar(euclid(5, 16, 2)) |> lp(@, 800) |> out(@)
```

Related: [trigger](#trigger), [timelines](timelines)

## Pattern transforms

Compile-time event-list rewriters that wrap a pattern. All compose via dot-call (`pat(...).slow(2).rev()`) and the equivalent functional form.

### early

`early(pattern, amount)` shifts events earlier by `amount` cycles, wrapping within `[0, 1)`.

```akk
pat("c4 e4 g4 b4").early(0.25)  // each event plays 1/4 cycle earlier
```

### late

`late(pattern, amount)` shifts events later by `amount` cycles, wrapping.

### palindrome

`palindrome(pattern)` plays the pattern forward then reversed, doubling `cycle_length`.

```akk
pat("c4 e4 g4 b4").palindrome()  // c4 e4 g4 b4 b4 g4 e4 c4 over 2× cycles
```

### compress

`compress(pattern, start, end)` squashes the entire pattern into the sub-window `[start, end)` of the cycle (silence outside).

> Note: this name was previously aliased to the audio compressor; the audio compressor is now reachable as `comp(...)` or `compressor(...)`.

### ply

`ply(pattern, n)` repeats each event `n` times within its slot.

### linger

`linger(pattern, frac)` keeps the first `frac` of the pattern and loops it across the cycle (equivalent to `zoom(0, frac).fast(1/frac)`).

### zoom

`zoom(pattern, start, end)` plays only the `[start, end)` portion of the pattern, stretched to fill the full cycle. Events that straddle the window are clipped.

### segment

`segment(pattern, n)` samples the pattern at `n` evenly-spaced points and emits `n` equal-duration events carrying the value active at each sample.

### swing / swingBy

`swing(pattern, n=4)` applies a 1/3 swing on an `n`-slice grid (default 4). `swingBy(pattern, amount, n=4)` lets you set the swing amount explicitly.

```akk
pat("bd hh sd hh").swing()           // default 1/3 on 4 slices
pat("bd hh sd hh").swingBy(0.5, 8)   // half-amount on 8 slices
```

### iter / iterBack

`iter(pattern, n)` rotates the pattern's start by `1/n` per cycle (forward); `iterBack` rotates the opposite way. Implemented as a runtime rotation on the SequenceState; no compile-time event explosion.

```akk
pat("c4 e4 g4 b4").iter(4)  // advance start by 1/4 each cycle
```

## Pattern generators

Pattern constructors that emit an event stream directly from numeric input. Compose with transforms via dot-call.

### run

`run(n)` produces `n` events at times `i/n` carrying values `0, 1, ..., n-1` each of duration `1/n`. Useful as a rising/integer index pattern.

```akk
run(8) |> mtof(@ + 60) |> osc("saw", @)  // ascending chromatic from C4
```

### binary

`binary(n)` produces a trigger pattern from the binary representation of `n` (MSB first; `bits = floor(log2(n)) + 1`). Set bits emit triggers, unset bits emit rests.

```akk
binary(178) |> sampler(@, "hh")  // 0b10110010 = 8-step rhythm
```

### binaryN

`binaryN(n, bits)` is the zero-padded fixed-width form. Truncates `n` to the lower `bits` bits.

```akk
binaryN(5, 8)  // 00000101 = 8 events with bits at positions 5 and 7
```
