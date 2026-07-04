> **Status: REFERENCE** — User-facing syntax reference. Current and accurate.

# Mini-Notation Reference

This document provides a comprehensive reference for Akkado's mini-notation syntax, used in typed pattern literals like `n"…"`, `v"…"`, `s"…"`, `c"…"` and `timeline()`. The formal EBNF (including lexer disambiguation rules and curve mode) lives in [docs/grammar/mini-notation-grammar.md](grammar/mini-notation-grammar.md).

## Typed Prefixes

Pattern strings carry a parse-mode prefix that selects per-atom rules:

| Prefix  | Mode    | Atom rules                                         | Result type           |
|---------|---------|----------------------------------------------------|-----------------------|
| `v"…"`  | Value   | Numeric only — `0`, `0.5`, `-0.5`, `1e3`           | Raw scalars (no mtof) |
| `n"…"`  | Note    | Note names + bare MIDI ints (both → Hz)            | mtof'd freq           |
| `s"…"`  | Sample  | Sample names — `bd`, `sd`, `kick:2`                | Sample IDs            |
| `c"…"`  | Chord   | Chord symbols — `Am`, `C7`, `F#m7b5`               | Multi-voice chord     |
| *(plain string)* | Note | Pattern-taking builtins re-parse bare strings in note mode | mtof'd freq |
| `t"…"`  | Curve   | `_`, `.`, `-`, `^`, `'` for breakpoints; `~` smooth| Timeline curve        |

The Value mode adds a numeric-atom path to the lexer: signed decimals, scientific notation, and finite reals are accepted; note names / sample names / chord symbols error E163.

## Quick Reference

| Syntax | Name | Example | Description |
|--------|------|---------|-------------|
| `a b c` | Top-level alternation | `c4 e4 g4` | One element per cycle (synonym of `<…>`) |
| `[a b]` | Group / Subdivision | `[c4 e4] g4` | Pack elements into one cycle |
| `<a b>` | Alternation | `<c4 e4>` | Synonym of top-level — one element per cycle |
| `[a, b]` | Polyrhythm | `[c4 e4, g4]` | Simultaneous layers |
| `{a b}%n` | Polymeter | `{c4 e4}%3` | Fixed step count |
| `*n` | Speed | `c4*2` | Repeat n times |
| `/n` | Slow | `c4/2` | Stretch duration |
| `@n` | Weight | `c4@2` | Temporal weight |
| `!n` | Repeat | `c4!3` | Replicate n times |
| `?n` | Chance | `c4?0.5` | Probability (0-1) |
| `a\|b` | Choice | `c4\|e4` | Random selection |
| `a(k,n)` | Euclidean | `bd(3,8)` | k hits in n steps |
| `~` | Rest | `c4 ~ e4` | Silent step |
| `_` | Elongate | `c4 _ e4` | Extend previous note |

## Atoms

### Pitches

Pitches follow the pattern: `note[accidental][octave]`

| Component | Values | Notes |
|-----------|--------|-------|
| Note | `a`-`g` (lowercase) or `A`-`G` (uppercase) | Case affects parsing |
| Accidental | `#` (sharp), `b` (flat), `x` (double sharp) | Optional, stackable |
| Micro-step | `^`/`+` (up), `v`/`\` (down) | Optional microtonal offset, stackable |
| Octave | `0`-`9` (two digits accepted; MIDI clamped 0–127) | Optional, defaults to 4 |

**Examples:**
```
c4      // C in octave 4 (middle C)
f#3     // F sharp in octave 3
Bb5     // B flat in octave 5
a       // A in octave 4 (lowercase defaults)
```

**Case sensitivity:**
- Lowercase (`c4`, `a3`): Always parsed as pitches
- Uppercase without octave (`A`, `C`): Parsed as chords (A major, C major)
- Uppercase with octave (`A4`, `C5`): Parsed as pitches

### Samples

Sample names are identifiers optionally followed by a variant number.

| Syntax | Example | Description |
|--------|---------|-------------|
| `name` | `bd`, `sd`, `hh` | Sample name |
| `name:n` | `bd:2`, `kick:0` | Sample with variant |

**Examples:**
```
bd          // Bass drum, default variant
sd:1        // Snare drum, variant 1
kick:2      // Kick sample, variant 2
```

**External banks via URI:** declare any Strudel-format sample manifest with the top-level `samples("uri")` directive, then route events to that bank with `.bank("Name")`:

```akkado
samples("github:tidalcycles/Dirt-Samples")

s"amencutup:0 amencutup:1".bank("Dirt-Samples").out()
```

URI schemes (`file://`, `http(s)://`, `github:user/repo`, `bundled://`) are resolved by the unified URI resolver — see [URI Schemes](uri-schemes.md) for the full list and caching behaviour.

**Trigger-driven one-shots:** for samples fired by an arbitrary trigger source (a button, an envelope, a custom edge detector) rather than the cycle clock, use `sample(trig, pitch, "name")` directly — it accepts the same `[bank/]name[:variant]` syntax as the mini-notation forms above. See [`sample()`](../web/static/docs/reference/builtins/samplers.md#sample) for details.

### Chords

Chords start with an uppercase note letter followed by an optional accidental and quality.

| Syntax | Example | Description |
|--------|---------|-------------|
| `Root` | `C`, `Am` | Root note + quality |
| `Root#quality` | `F#m7` | With sharp |
| `Rootbquality` | `Bbmaj7` | With flat |

**Supported qualities** (full list, with aliases):

| Triad                   | Seventh                        | Sixth / Extended                   |
|-------------------------|--------------------------------|------------------------------------|
| (none), `M`, `maj`      | `7`, `dom7`                    | `6`, `m6`, `min6`                  |
| `m`, `min`, `-`         | `M7`, `maj7`, `^`, `^7`        | `9`, `M9`, `maj9`, `m9`, `min9`    |
| `dim`, `o`              | `m7`, `min7`, `-7`             | `add9`, `add2`                     |
| `aug`, `+`              | `dim7`, `o7`                   | `11`, `m11`                        |
| `sus2`, `sus4`, `sus`   | `m7b5`, `0` (half-dim)         | `13`                               |
| `5` (power, no 3rd)     | `aug7`, `+7`                   |                                    |
|                         | `mM7`, `m^7`, `minmaj7`        |                                    |

**Examples:**
```
C           // C major
Am          // A minor
F#m7        // F# minor 7th
Bbmaj7      // Bb major 7th
Gdim        // G diminished
Esus4       // E suspended 4th
Cmaj9       // C major 9th — 5 voices
D^7         // D major 7th (Strudel-style)
Am11        // A minor 11th — 6 voices
G13         // G dominant 13th — 6 voices
```

The same quality table backs the `chord()` builtin and the `c"..."` mini-notation prefix — the formal symbol grammar lives in [docs/grammar/chord-grammar.md](grammar/chord-grammar.md). See the [Chords reference](../web/static/docs/reference/mini-notation/chords.md) for voicing transforms and the canonical interval lists. (The old apostrophe chord literals `Am'` / `Cmaj7_4'` were removed 2026-05-21.)

### Rests

| Symbol | Description |
|--------|-------------|
| `~` | Silent step (rest) |

**Example:**
```
c4 ~ e4 ~   // C, rest, E, rest
```

### Elongation (Tidal-compatible)

| Symbol | Description |
|--------|-------------|
| `_` | Extend previous note's duration |

The `_` symbol extends the duration of the previous note to cover its time slot, following Tidal Cycles behavior.

**Example:**
```
c4 _ e4     // C takes 2/3 cycle (elongated), E takes 1/3
c4 _ _ e4   // C takes 3/4 cycle, E takes 1/4
bd _ _ sd   // bd extended, then sd
```

If `_` appears at the start (with no previous note), it has no effect.

## Grouping Constructs

### Top-Level Sequence (implicit alternation)

At the top level, whitespace-separated elements alternate **one per
cycle** (synonym of `<…>`) — they do *not* subdivide the cycle. Wrap in
`[…]` to pack elements into one cycle. (Deliberate divergence from
Strudel/Tidal — see CLAUDE.md "Clock System".)

```
c4 e4 g4     // c4 in cycle 0, e4 in cycle 1, g4 in cycle 2
[c4 e4 g4]   // three equal parts within one cycle
```

### Group `[...]`

Explicit grouping for subdivision control.

```
[c4 e4] g4       // First half: c4+e4, second half: g4
[[c4 e4] g4] a4  // Nested grouping
```

### Alternation `<...>`

Selects one element per cycle, rotating through the list.

```
<c4 e4 g4>   // Cycle 1: c4, Cycle 2: e4, Cycle 3: g4, Cycle 4: c4...
```

### Polyrhythm `[..., ...]`

Comma-separated elements play simultaneously, each subdividing independently.

```
[c4 e4, g4 a4 b4]   // Layer 1: c4 e4, Layer 2: g4 a4 b4
```

When every comma-separated member is a bare sample atom (or rest), the
polyrhythm collapses to a single simultaneous trigger — i.e. a drum chord.
`[bd, hh]` fires the kick and hi-hat together on the same beat; `[bd, sd, hh]`
fires all three. Up to 4 voices per stack; extra voices fall back to the
older per-child interleave so nothing is silently dropped.

```
s"[[bd, hh] hh [sd, hh] hh]"    // Standard rock groove
```

### Polymeter `{...}%n`

Fixed step count that wraps around. The `%n` specifies how many steps constitute one cycle.

```
{c4 e4 g4}%4    // 3 elements over 4 steps, wrapping
{bd sd}%3       // 2 elements over 3 steps
```

## Modifiers

Modifiers follow atoms and adjust their timing or behavior.

### Speed `*n`

Repeats the element n times within its original duration.

```
c4*2        // Play c4 twice in the time of one
c4*4        // Play c4 four times
[c4 e4]*2   // Repeat the group twice
```

### Slow `/n`

Stretches the element to span n cycles.

```
c4/2        // c4 spans 2 cycles
c4/4        // c4 spans 4 cycles
```

### Weight `@n`

Allocates temporal weight for unequal subdivisions.

```
c4@2 e4@1   // c4 gets 2/3 of the time, e4 gets 1/3
c4@3 e4     // c4 gets 3/4, e4 gets 1/4 (default weight is 1)
```

### Repeat `!n`

Replicates the element n times as separate events (default: 2).

```
c4!3        // Same as c4 c4 c4
c4!         // Same as c4 c4 (default is 2)
```

### Chance `?n`

Probability of the element playing (0-1 range, default: 0.5).

```
c4?0.5      // 50% chance of playing
c4?0.25     // 25% chance of playing
c4?         // 50% chance (default)
```

### Modifier Stacking

Multiple modifiers can be combined:

```
c4*2?0.5    // Repeat twice, each with 50% chance
c4:0.8@3    // Velocity 0.8, temporal weight 3
```

### Record Suffix `{key:value, ...}` (Phase 2 PRD)

Atoms (pitch / sample / chord) can carry a record-style property suffix that
must immediately follow the atom (no whitespace) before any modifiers:

```
c4{vel:0.8}                     // velocity = 0.8
c4{vel:0.8, dur:0.5}            // velocity 0.8, duration override 0.5
bd{vel:0.6}                     // sample with reduced velocity
Am{vel:0.7}                     // chord with velocity
c4{vel:0.5, cutoff:0.3}         // custom keys held on the atom
```

**Recognized short-form keys** populate fixed pattern-event fields:

| Key   | Effect                                                       |
|-------|--------------------------------------------------------------|
| `vel` | Sets velocity (0..1, clamped). Overrides any `:0.x` suffix.  |
| `dur` | Multiplies the event's duration by `value`.                  |
| `bend` | Stored on the atom for future MIDI-out exposure.            |
| `aftertouch` | Same — held on atom; runtime exposure deferred.       |

**Custom keys** (e.g., `cutoff`, `freq`, anything not listed above) are kept
on the atom AST. The `as e |> e.<key>` pipe-binding accessor that surfaces
them at runtime is deferred to a follow-up; for now, use the short forms or
positional `:0.x` syntax for runtime-effective velocity.

**Disambiguation from polymeter:**
- `c4{vel:0.8}` — record suffix (no whitespace before `{`).
- `{c4 e4 g4}%3` — polymeter (no preceding completed note token).
- `c4 {a b}%3` — polymeter (whitespace before `{`).

**Order with positional shorthand:**
- `c4:0.5` — positional velocity 0.5.
- `c4{vel:0.5}` — record suffix; equivalent.
- The two forms can be combined; the record-suffix value wins on overlap.

## Random Choice `|`

Selects randomly between alternatives.

```
c4|e4           // Randomly play c4 or e4
c4|e4|g4        // Random choice of three
[c4 e4]|[g4 a4] // Random choice between groups
```

## Euclidean Rhythms

Distribute k hits across n steps using the Euclidean algorithm.

| Syntax | Description |
|--------|-------------|
| `atom(k,n)` | k hits in n steps |
| `atom(k,n,r)` | With r-step rotation |

**Examples:**
```
bd(3,8)         // 3 hits in 8 steps: x..x..x.
bd(3,8,1)       // Same, rotated 1 step: ..x..x.x
sd(5,8)         // 5 hits in 8 steps: x.xx.xx.
hh(7,16)        // 7 hits in 16 steps
```

## Event Fields

When using patterns with pipe binding (`as`), event fields are accessible.

| Field | Aliases | Type | Description |
|-------|---------|------|-------------|
| `@.freq` | `@.f`, `@.pitch` | Hz | Frequency |
| `@.vel` | `@.v`, `@.velocity` | 0-1 | Velocity |
| `@.trig` | `@.trigger` | pulse | 1-sample pulse at every event onset (per-note retrigger) |
| `@.gate` | `@.g` | 0/1 | Held high during the event's duration, with a 1-sample drop at each onset |
| `@.note` | `@.n`, `@.midi` | 0-127 | MIDI note number |
| `@.dur` | - | cycles | Duration |
| `@.chance` | - | 0-1 | Probability |
| `@.time` | - | cycles | Event time |
| `@.phase` | - | 0-1 | Phase within cycle |

### Trigger vs gate

`@.trig` and `@.gate` are **two different signals**, not aliases. Pick the one
that matches your envelope:

- **`@.trig`** is a single-sample pulse fired exactly once at the start of each
  event. Pair it with `ar()`, sample players, or anything that listens for an
  impulse.
- **`@.gate`** stays high for the full duration of each event but drops to
  `0` for one sample at every event onset, so retrigger-style envelopes
  (`ar`, `adsr`) see a rising edge per note. Pair it with `adsr()` when you
  want true sustain between attack and release.

```akkado
// AR + .trig — pulse-driven, clean per-note attack/release
n"c4 e4 g4 b4" |> saw(@.freq) * ar(@.trig, 0.005, 0.15) |> out(@)

// ADSR + .gate — sustains while the note is held; per-note retrigger
n"c4 e4 g4 b4" |> saw(@.freq) * adsr(@.gate, 0.01, 0.1, 0.7, 0.2) |> out(@)
```

#### Legato via overlapping durations

The 1-sample drop in `.gate` happens at every event onset **unless** another
event is still sustaining at that moment. Overlap durations with `dur()` to
get true legato (no retrigger between notes):

```akkado
// Each note's duration is 2 steps long, so events overlap
// → gate stays continuously high → ADSR sustains across the whole phrase.
n"c4 e4 g4 b4".dur(2)
  |> saw(@.freq) * adsr(@.gate, 0.01, 0.1, 0.7, 0.5) |> out(@)
```

**Example:**
```akkado
n"c4 e4 g4" as e |> sine(e.freq) |> @ * e.vel |> out(@)
```

## Chord Qualities

### Triads

| Quality | Intervals | Example |
|---------|-----------|---------|
| (empty) | 0, 4, 7 | `C` (C major) |
| `m` | 0, 3, 7 | `Am` |
| `maj` | 0, 4, 7 | `Cmaj` |
| `min` | 0, 3, 7 | `Cmin` |
| `dim` | 0, 3, 6 | `Bdim` |
| `aug` | 0, 4, 8 | `Caug` |
| `sus2` | 0, 2, 7 | `Dsus2` |
| `sus4` | 0, 5, 7 | `Dsus4` |
| `5` | 0, 7 | `C5` (power chord) |

### Seventh Chords

| Quality | Intervals | Example |
|---------|-----------|---------|
| `7` | 0, 4, 7, 10 | `G7` (dominant) |
| `maj7` | 0, 4, 7, 11 | `Cmaj7` |
| `m7` | 0, 3, 7, 10 | `Am7` |
| `min7` | 0, 3, 7, 10 | `Amin7` |
| `dom7` | 0, 4, 7, 10 | `Cdom7` |
| `dim7` | 0, 3, 6, 9 | `Bdim7` |
| `m7b5` | 0, 3, 6, 10 | `Bm7b5` (half-dim) |
| `minmaj7` | 0, 3, 7, 11 | `Cminmaj7` |

### Extended Chords

| Quality | Intervals | Example |
|---------|-----------|---------|
| `9` | 0, 4, 7, 10, 14 | `G9` |
| `maj9` | 0, 4, 7, 11, 14 | `Cmaj9` |
| `min9` | 0, 3, 7, 10, 14 | `Am9` |
| `add9` | 0, 4, 7, 14 | `Cadd9` |
| `6` | 0, 4, 7, 9 | `C6` |
| `min6` | 0, 3, 7, 9 | `Am6` |

## Strudel/Tidal Compatibility

Akkado's mini-notation is designed to be compatible with Strudel and Tidal:

| Feature | Akkado | Strudel/Tidal |
|---------|--------|---------------|
| Rest symbol | `~` | `~` (same) |
| Elongation | `_` = extend previous note | `_` = extend previous note (same) |
| Sample variant | `bd:2` | `bd:2` (same) |
| Native chords | `Am` in pattern | `chord("Am")` function |
| Uppercase pitch | Requires octave (`A4`) | Often defaults to octave 4 |
| `.` shorthand | Not supported | `][` equivalent |

### Key Differences

1. **Chord notation**: Akkado parses `Am`, `C7`, `Fmaj7` directly in mini-notation. Strudel typically uses `chord("Am")` or similar functions.

2. **Case handling**: Uppercase letters without octaves (`A`, `C`, `G`) are parsed as chords in Akkado. Add an octave (`A4`) to force pitch interpretation.

## Examples

### Simple Melody
```
n"c4 e4 g4 e4"
```

### With Rests
```
n"c4 ~ e4 ~ g4 ~ e4 ~"
```

### Chord Progression
```
n"Am F C G"
```

### Euclidean Drum Pattern
```
s"bd(3,8) sd(2,8) hh(5,8)"
```

### Polyrhythm
```
n"[c4 e4 g4, d4 f4]"
```

### Complex Pattern
```
n"<c4 e4>*2 [g4 a4]?0.5 b4/2"
```

### With Alternation
```
n"<[c4 e4] [d4 f4] [e4 g4]>"
```
