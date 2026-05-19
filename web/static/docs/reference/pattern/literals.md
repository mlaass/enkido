---
title: Pattern Literals
category: pattern
order: 1
keywords: [pattern, literal, prefix, value, note, sample, chord, scalar, coerce, v"…", n"…", s"…", c"…", t"…", typed prefix]
---

# Pattern Literals

Patterns can be written as typed string prefixes that disambiguate parse semantics at the literal site:

| Prefix | Atom semantics                              | Coerces to Signal? |
|--------|---------------------------------------------|--------------------|
| `v"…"` | Numeric only: `0`, `0.5`, `-0.5`, `1e3`     | Yes                |
| `n"…"` | Note names + bare MIDI ints (both → Hz)     | Yes                |
| `s"…"` | Sample names: `bd`, `sd`, `kick:2`          | Audio output       |
| `c"…"` | Chord symbols: `Am`, `C7`, `F#m7b5`         | No (use `poly()`)  |
| `t"…"` | Timeline curve breakpoints: `0 1 0.5`       | Yes                |

## v"…" value patterns

A numeric pattern produces stepped raw scalars at each event boundary. Atoms must be numeric literals; note names, sample names, and chord symbols are rejected with `E163`.

```akk
osc("sin", v"<220 440 880>")        // raw Hz, no mtof
lp(sig, v"<200 800 2000>", 0.7)     // pattern-driven cutoff
sig * v"<0.2 0.5 1.0 0.5>"          // amplitude envelope
```

Negative numbers, decimals, and scientific notation all parse:

```akk
v"-0.5 0.25 1e3 -1.25e-2"
```

## n"…" note patterns

Note names and bare integers both map to Hz via `mtof`. Rejects sample-name fallback.

```akk
osc("sin", n"c4 e4 g4")
osc("sin", n"60 64 67")            // bare ints = MIDI notes
```

## s"…" sample patterns

Atoms are sample names. Use with the sampler / out() audio path:

```akk
s"bd ~ bd ~" |> out(@)
s"hh*8 [bd sd]" |> out(@)
```

## c"…" chord patterns

Atoms are chord symbols (`Am`, `C7`, `Fmaj7`, …). Multi-voice; consume via `poly()`:

```akk
c"Am C G Em"
    |> poly(4, fn (e) -> osc("saw", e.freq) * ar(e.trig) )
    |> out(@)
// `e.trig` is the per-note pulse for AR. Use `e.gate` instead
// if the instrument is an ADSR that should sustain across the
// note's duration.
```

A chord pattern in a scalar slot errors `E160`. Silently dropping voices is not implicit.

## Pattern → Signal coercion

When a `Pattern` value reaches a slot that expects a `Signal`, the compiler implicitly extracts the pattern's primary value buffer (the `freq` field). This makes patterns first-class as scalar values:

```akk
osc("sin", n"c4 e4 g4")          // Pattern freq → osc freq (Signal)
lp(sig, v"<200 800>", 0.7)       // Pattern → cutoff slot (Signal)
osc("saw", n"c4 e4" + v"<0 12>") // Pattern + Pattern arithmetic
```

The coerce only fires for monophonic, non-sample patterns. Polyphonic patterns (chord, multi-voice) error `E160` with a hint to `poly()`, since silently emitting voice 0 is rarely what you want. Sample patterns route through `SAMPLE_PLAY` and produce audio output rather than a scalar; use `out()` directly.

## scalar()

The explicit cast version. `scalar(p)` unwraps a monophonic pattern's freq buffer as a Signal, identical to what auto-coerce produces. Useful when you want to be explicit, or when feeding into a context that the auto-coerce wouldn't fire on:

```akk
scalar(n"c4 e4 g4")                // Signal carrying mtof'd freqs
scalar(v"<220 440>") * 2           // Signal * 2 → Signal
```

`scalar()` is idempotent on Signal: `scalar(scalar(p))` is safe. It errors `E161` on sample or polyphonic patterns.

## Pipe-binding stays a pattern

The `as e` pipe binding does not coerce. `e` remains a Pattern, and `e.freq`, `e.vel`, `e.trig`, `e.gate` plus any custom record-suffix keys (`e.cutoff`, `e.bend`, …) are accessible as fields:

```akk
n"c4{cutoff:0.3} e4{cutoff:0.7} g4{cutoff:0.5}" as e
  |> osc("saw", e.freq)
  |> lp(@, 200 + e.cutoff * 4000)
  |> out(@)
```
