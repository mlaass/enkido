---
title: Curve Notation
category: reference
keywords: [curve, timeline, automation, envelope, LFO, t", breakpoint]
---

# Curve Notation

Curve notation is an ASCII-art syntax for defining automation curves that compile to Cedar's `TIMELINE` opcode breakpoints.

Accessed via `timeline("...")` or the `t"..."` string prefix, curve notation integrates with the existing mini-notation system. All grouping, subdivision, alternation, and modifier features work identically.

## Value levels

Five characters map to fixed values in the 0.0–1.0 range:

| Char | Value | Mnemonic |
|------|-------|------------|
| `_`  | 0.00  | floor/ground |
| `.`  | 0.25  | low dot |
| `-`  | 0.50  | middle dash |
| `^`  | 0.75  | high caret |
| `'`  | 1.00  | top/peak |

Each character occupies one equal-time slot within its containing group. Adjacent same-value characters extend the hold duration.

## Ramp characters

Two characters express linear interpolation between adjacent levels:

| Char | Visual | Behavior |
|------|--------|------------|
| `/`  | Up     | Linear ramp from preceding level to following level |
| `\`  | Down   | Linear ramp from preceding level to following level |

Both `/` and `\` produce identical interpolation behavior. They ramp linearly from the value of the preceding atom to the value of the following atom. The visual distinction serves readability only.

**Multiple consecutive ramps** spread the transition over more time slots:

```akkado
t"__/'''"    // quick ramp: _ to ' over 1 slot
t"__///'''"  // slow ramp: _ to ' over 3 slots
```

## Smooth modifier `~`

The `~` prefix converts a hard level change into a smooth (linear) interpolation:

```akkado
t"__''"      // hard step from 0.0 to 1.0
t"__~''"     // smooth ramp from 0.0 to 1.0 over 1 slot
t"__~-~^''"  // smooth ramp through 0.5, 0.75, then hard step to 1.0
```

Without `~`, a level character holds its value for the entire slot (type = hold). With `~`, the slot linearly interpolates from the previous value to the new value (type = linear).

## Integration syntax

**Function call**: extends existing `timeline()`:

```akkado
timeline("___/''''\\___")
```

**String prefix**: syntactic sugar (like `p"..."` for `pat()`):

```akkado
t"___/''''\\___"
```

Both forms are equivalent. The output is always 0.0–1.0. Users scale with math:

```akkado
t"__/''\\__" * 1800 + 200  // ramp from 200 to 2000 Hz
t"_/'\\_" * 0.8 + 0.1      // ramp from 0.1 to 0.9
```

## Mini-notation compatibility

All mini-notation modifiers and grouping work identically with curve atoms:

| Feature | Syntax | Effect |
|---------|--------|--------|
| Subdivision | `t"__[/''''] __"` | Ramp+hold subdivides into one slot |
| Alternation | `t"<__/' ''\\__>"` | Different curves per cycle |
| Repeat | `t"[__/']*4"` | Repeat sub-curve 4 times |
| Slow | `t"__''/2"` | Stretch over 2 cycles |
| Weight | `t"_@3 '"` | Underscore gets 3x time |
| Replicate | `t"'!4"` | Equivalent to `t"'''''"` |
| Chance | `t"_?0.5 '"` | 50% probability of this segment vs holding previous value |

## `/` ambiguity resolution

`/` is both a ramp atom and the slow modifier prefix. The disambiguation rule:

- `/` followed by a **digit** = slow modifier (e.g., `/2`, `/4`)
- `/` followed by a **non-digit** or at end of input = ramp atom

The mini-notation parser already performs lookahead for modifier detection, so this integrates naturally.

## Examples

### Basic curves

```akkado
t"''''"           // hold at 1.0 for entire cycle
t"____"           // hold at 0.0 for entire cycle
t"_/'\\_"         // ramp up then ramp down (triangle LFO)
t"_///''"         // slow ramp up
t"'//_"           // slow ramp down
t"__~.~-~^''"     // gradual ramp through 0.25, 0.5, 0.75
```

### Musical use cases

```akkado
// Filter sweep on a pad
osc("saw", 220) |> lp(@, t"__/''\\__" * 3000 + 200) |> out(@)

// Tremolo
osc("sin", 440) * t"['^]*8" |> out(@)

// Sidechain-style ducking (4 pumps per cycle)
drums = pat("bd _ _ _")
synth = osc("saw", C4') * t"[_/'']*4" |> out(@)

// Panning automation
sig = osc("saw", 220)
sig |> out(sig * t"_/'\\__", sig * t"__/'\\_ ")

// Envelope on pluck
pat("c4 e4 g4") as e |>
    osc("sin", e.freq) * t"'/\\___" * e.vel |>
    out(@)
```

### With mini-notation features

```akkado
t"<_/'' ''\\_ >"       // up on odd cycles, down on even
t"__ [/''''] __"        // ramp+hold compressed into one beat
t"[_/'\\_ ]*4"          // 4 attack-release envelopes per cycle
t"____/''''\\____/4"    // entire curve stretches over 4 cycles
t"_@3 /"                // hold at 0 for 3/4, ramp for 1/4
```

## Edge cases

- **Empty string** `t""`: compiles to empty timeline (no breakpoints)
- **Single character** `t"'"` or `t"_"`: constant value for entire cycle
- **Ramp at start** `t"/''"`: no preceding level defaults to 0.0
- **Ramp at end** `t"___/"`: no following level defaults to 0.0 (wraps if looping)
- **`~` on first character** `t"~'"`: smooth ramp from 0.0 to 1.0
- **`~` on ramp** `t"~/"`: compile error (`~` only applies to level characters)
- **Breakpoint limit**: `TimelineState::MAX_BREAKPOINTS = 64`. If exceeded, emits compile warning and truncates

## Implementation details

- Curve notation compiles to `TIMELINE` opcode breakpoints at compile time
- Integrates with existing mini-notation features (grouping, alternation, modifiers)
- Always outputs 0.0–1.0 (user scales with math)
- Works alongside existing `pat()` and `seq()` patterns
