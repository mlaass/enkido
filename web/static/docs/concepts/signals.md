---
title: Mono & Stereo Signals
category: concepts
order: 3
keywords: [mono, stereo, channels, stereo-native, conversion, downmix, upmix, signal types]
---

# Mono & Stereo Signals

Akkado has two kinds of audio signals: **Mono** (one channel) and **Stereo** (two channels, L and R). Every expression the compiler sees has a known channel count — not a runtime property, but a type the compiler tracks through the program. That lets the compiler diagnose mismatches at compile time, while every audio effect is stereo-native: it processes both channels in one pass and a mono input automatically widens to stereo, so you never duplicate a chain by hand.

## Defaults

Oscillators, filters, and most DSP is **mono** by default. You go stereo explicitly:

```akkado
// Mono — single channel
saw(220)

// Stereo — duplicate mono across both channels
saw(220) |> stereo()

// Stereo — distinct L and R
stereo(saw(218), saw(222))
```

`out()` accepts either:

```akkado
saw(220) |> out(%)              // Mono  → duplicated to L and R
stereo(saw(218), saw(222))
  |> out(%)                     // Stereo → split into L and R
out(saw(218), saw(222))         // Two mono signals → L, R explicitly
```

## Conversions

There are exactly two canonical conversions between the representations:

| Call | Direction | Effect |
|---|---|---|
| `stereo(x)` | Mono → Stereo | Duplicate `x` to both L and R |
| `stereo(l, r)` | two Mono → Stereo | Pair `l` as L, `r` as R |
| `mono(s)` | Stereo → Mono | Sum-to-mono with 0.5 gain: `(L + R) * 0.5` |
| `left(s)` | Stereo → Mono | Extract the left channel only |
| `right(s)` | Stereo → Mono | Extract the right channel only |

Calling a conversion on a signal that's already the target channel count is a **warning**, not an error — it almost always means something upstream didn't do what you thought, but the expression still compiles and evaluates to the input unchanged. Some typical hits:

- `stereo(already_stereo_signal)` → `W182`
- `mono(already_mono_signal)` → `W181`
- `left(mono_signal)` / `right(mono_signal)` → `W183` / `W184`
- `out(stereo_signal, other)` as *two-arg* `out()` → `W185` (auto-escalates)

A genuine *type* mismatch — an audio-rate signal in a non-signal slot, for example — is still a hard error (`E186`).

## Stereo-native effects

Every audio effect is **stereo-native**: it processes both channels in a single dispatch with one per-channel state struct. A **mono** input automatically widens — the opcode reads it once and uses it for both the L and R lanes — so you never have to duplicate a chain or insert `stereo()` to get a stereo result.

```akkado
bus = osc("saw", 220)         // Mono — widens automatically

bus
  |> filter_lp(%, 500, 0.7)   // Stereo: per-channel filter state
  |> delay(%, 0.25, 0.5)      // Stereo: per-channel delay line
  |> out(%)                   // Stereo out
```

For channel-independent effects (filters, distortion, EQ, plain delays) this is exactly equivalent to writing:

```akkado
sig = osc("saw", 220)
left_out  = sig |> filter_lp(%, 500, 0.7) |> delay(%, 0.25, 0.5)
right_out = sig |> filter_lp(%, 500, 0.7) |> delay(%, 0.25, 0.5)
out(left_out, right_out)
```

Identical state handling, identical audio. Stateless effects (`saturate`, `softclip`, `fold`, `distort`) work the same way. Spatializing effects (reverbs, `chorus`, `phaser`, `flanger`) go further: a mono input widens into a genuinely *decorrelated* stereo output (cross-coupled reverb tanks, offset L/R LFO phases).

### What stereo-native processing doesn't change

- **Scalar parameters (cutoff, resonance, time, feedback) are shared between L and R.** Both channels see the same value. If you want independent per-channel modulation, split the stream explicitly.
- **Pattern events (`pat`, `seq`, `timeline`) are always mono** — a stereo synth driven by a mono pattern is the normal case.
- **Generators stay mono.** `osc`, `noise`, `pulse`, and mono-file `sample` return Mono; the widening happens at the boundary into the first effect.
- **Cross-channel effects** like `width`, `pingpong`, `ms_encode`/`ms_decode` have their own explicit stereo signatures.

## Mixed-channel arithmetic

`+`, `-`, `*`, `/` on a **mono** and a **stereo** operand broadcast the mono side across both channels:

```akkado
dry = osc("saw", 220)                          // Mono
wet = dry |> stereo() |> freeverb(%, 0.9, 0.5) // Stereo
dry * 0.3 + wet * 0.7 |> out(%)                // Stereo out
```

The `dry * 0.3` stays mono, `wet * 0.7` stays stereo, and `mono + stereo` promotes to stereo by dual-reading the mono buffer. No extra instructions.

Mono on mono stays mono. Stereo on stereo stays stereo (L op L, R op R). There's no implicit mono-to-stereo promotion anywhere else — the compiler will complain if it can't make sense of a mismatch.

## Patterns and Signals

Patterns (`pat()`, `n"…"`, `v"…"`, `c"…"`, `s"…"`) carry a primary value buffer that doubles as a Signal. When you pass a pattern to a slot that expects a Signal — `osc("sin", n"c4 e4 g4")`, `lp(sig, v"<200 800>", 0.7)` — the compiler implicitly extracts that buffer.

The **Pattern → Signal coerce** rule:

| Pattern shape           | Coerces to Signal? | Where the buffer points                  |
|-------------------------|--------------------|------------------------------------------|
| Monophonic note (`n"…"`)| Yes                | `freq` field — Hz post-mtof              |
| Numeric (`v"…"`)        | Yes                | `freq` field — raw scalar                |
| Sample (`s"…"`)         | Yes                | Audio output (post-`SAMPLE_PLAY`)        |
| Polyphonic chord (`c"…"`) | **No — E160**    | Use `poly()` to expand voices            |
| Polyphonic note (e.g. `n"[c4,e4]"`) | **No — E160** | Use `poly()`                       |

### Operator type rules

Arithmetic between patterns and other types follows these rules:

| LHS     | RHS     | Result   | Notes                                        |
|---------|---------|----------|----------------------------------------------|
| Pattern | Pattern | Pattern  | Pointwise op on `freq` buffers, longest wins |
| Pattern | Signal  | Signal   | Pattern coerces; sample-rate result          |
| Pattern | Number  | Pattern  | `n"c4 e4" + 12` is still a Pattern           |
| Signal  | Signal  | Signal   | Standard                                     |
| Pattern | Stereo  | E165     | Wrap with `stereo(scalar(...))` explicitly   |

### `scalar()` — the explicit cast

`scalar(p)` unwraps a monophonic pattern's `freq` buffer. It's idempotent on Signals (`scalar(scalar(p))` is safe) and errors **E161** on sample / polyphonic patterns. See the [Pattern Literals reference](../reference/pattern/literals.md).

## See also

- [Pattern Literals](../reference/pattern/literals.md) — typed prefixes (`v"…"`, `n"…"`, `s"…"`, `c"…"`) and `scalar()`.
- [Pattern Modulation tutorial](../tutorials/06-pattern-modulation.md) — flagship walkthrough of `bend(notes, v"…")` and custom-property accessors.
- [Stereo builtins reference](/docs/reference/builtins/stereo) — signatures and behaviour for `stereo`, `mono`, `left`, `right`, `pan`, `width`, `ms_encode`, `ms_decode`, `pingpong`.
- [Cedar architecture — STEREO_OUTPUT / STEREO_INPUT flags](../../../docs/cedar-architecture.md#bytecode-format) — the VM-level mechanism behind stereo-native opcodes.
