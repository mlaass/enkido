---
title: Pattern Modulation
category: tutorials
order: 6
keywords: [tutorial, pattern, modulation, value pattern, v"…", bend, aftertouch, dur, custom property, cutoff, scalar, coerce]
---

# Pattern Modulation

So far patterns have driven note pitches. They're also plain **values** you can plug into any DSP slot, attach to per-event properties, or combine with arithmetic.

## Patterns are values

A pattern is a stream of events that steps through values over time. When you write:

```akk
sine(n"c4 e4 g4")
```

the compiler takes the pattern's frequency buffer and feeds it to `osc`'s freq slot. Patterns coerce to scalar Signals automatically.

## Numeric patterns with v"…"

Sometimes you want raw numbers, not mtof'd notes. Use `v"…"`:

```akk
// Three frequencies, no mtof, just the literal Hz values
sine(v"<220 440 880>") |> out(@)

// Filter cutoff sweeping through three values per cycle
sig = saw(440)
lp(sig, v"<200 800 2000>", 0.7) |> out(@)

// Amplitude scrubber
saw(220) * v"<0.2 0.5 1.0 0.5>" |> out(@)
```

Atoms in `v"…"` must be numeric; `v"c4"` is a parse error.

## Per-event modulation

Pattern-valued bend depth: each note bends by the corresponding pattern value.

```akk
n"c4 e4 g4" |> bend(@, v"<0 0.5 -0.5>") as e
    |> sine(e.freq + e.bend * 12)
    |> out(@)
```

The same shape works for `aftertouch()` and `dur()`:

```akk
// crescendo
n"c4 e4 g4 b4" |> aftertouch(@, v"<0 0.25 0.5 1.0>")

// pattern-driven note length
n"c4 e4 g4"    |> dur(@, v"<0.25 0.5 1.0>")
```

Constant args still work; `bend(notes, 0.5)` is unchanged.

## Custom-property accessor

A note can carry arbitrary record-suffix keys; the binding `as e` exposes each as a Signal:

```akk
n"c4{cutoff:0.3} e4{cutoff:0.7} g4{cutoff:0.5}" as e
    |> saw(e.freq)
    |> lp(@, 200 + e.cutoff * 4000)
    |> out(@)
```

This is more compact than calling `bend()` / `aftertouch()` separately for each property.

## Scalar arithmetic

Patterns combine naturally with numbers and signals:

```akk
v"<60 64 67>" + 12          // still a Pattern (Pattern + Number)
v"<0 0.5>" + sig            // Signal (Pattern + Signal coerces)
n"c4 e4 g4" + v"<0 0 12>"   // Pattern + Pattern (combined)
```

## When coerce fails

Polyphonic chord patterns can't silently degrade to one voice:

```akk
sine(c"Am")     // ❌ E160: chord pattern in scalar slot
```

The fix is to consume them with `poly()`, which fans out per voice:

```akk
c"Am C G Em"
    |> poly(@, ({freq, trig}) -> saw(freq) * ar(trig), 4)
    |> out(@)
// AR + `trig` (per-note pulse).
// Swap to `gate` for ADSR-style sustain.
```

Sample patterns route through `SAMPLE_PLAY` and produce audio. Pipe them to `out()` directly:

```akk
s"bd ~ bd ~" |> out(@)
```

## scalar(): explicit cast

`scalar(p)` is the explicit form of auto-coerce. Useful for clarity, or for arithmetic outside a DSP slot:

```akk
let detune = scalar(v"<0 -10 10 0>")
saw(n"c4 e4 g4 b4" + detune)
```

`scalar()` errors on sample / polyphonic patterns (E161). It's idempotent on Signals: `scalar(scalar(p))` is safe.
