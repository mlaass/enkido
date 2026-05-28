---
title: Stereo
category: builtins
order: 12
keywords: [stereo, mono, left, right, pan, width, ms_encode, ms_decode, pingpong, downmix, channels, stereo-native]
group: tools
subgroup: audio-plumbing
icon: Headphones
tagline: Pan, width, M/S encoding, and stereo plumbing.
---

# Stereo

Stereo signal operations. Akkado tracks channel count (Mono vs Stereo) on every
signal; every audio DSP op is **stereo-native** — one instruction handles both
channels with independent per-channel state, and a mono input widens
automatically, so you don't have to duplicate a chain for L and R.

## stereo

**Create a stereo signal** from mono or two separate signals.

| Param | Type   | Description |
|-------|--------|-------------|
| L     | signal | Mono signal (duplicated to both channels), or left channel if `R` given |
| R     | signal | Right channel (optional) |

```akk
// Duplicate a mono signal into stereo
saw(220) |> stereo() |> out(@)

// Build a stereo image from two oscillators
stereo(saw(218), saw(222)) |> out(@)
```

---

## mono

**Sum-to-mono downmix** of a stereo signal: `out = (L + R) * 0.5`.

| Param | Type   | Description |
|-------|--------|-------------|
| sig   | stereo | Stereo signal to downmix |

Standard sum-to-mono convention: correlated content stays at 0 dB;
uncorrelated content sums at roughly -3 dB RMS.

```akk
// Downmix a stereo reverb tail before sidechain detection
wet = src |> stereo() |> freeverb(@, 0.8, 0.5)
env = wet |> mono() |> env_follower(@)
```

Calling `mono()` on a mono signal is a compile-time error; call it only on
stereo signals. (When passed a function instead of a signal, `mono()` falls
back to the monophonic voice-manager form: `mono(instrument)`.)

---

## left, right

**Channel extraction** from a stereo signal.

```akk
s = stereo(sine(220), sine(330))
left(s)   // → Mono
right(s)  // → Mono
```

---

## pan

**Panning** with equal-power (constant-power) law.

| Signature | Behaviour |
|-----------|-----------|
| `pan(mono, pos)` | Position a mono signal in the stereo field. Emits `PAN`. |
| `pan(stereo, pos)` | Balance an already-stereo signal. Emits `PAN_STEREO`. |

Position is `-1` (hard left) … `0` (centre, -3 dB) … `+1` (hard right).

```akk
// Mono → stereo pan
saw(220) |> pan(@, lfo(0.25)) |> out(@)

// Stereo balance on a stereo bus
stereo(saw(218), saw(222))
  |> pan(@, 0.3)
  |> out(@)
```

The stereo form is DAW-style balance, not re-panning each channel. At
`pos = -1`, the right channel is silenced and the left passes through at
unity; at `pos = 0`, both channels are scaled by `cos(π/4) ≈ 0.707`.

---

## width

**Stereo width** control using mid/side processing internally.

```akk
// Narrow to mono (width=0), original (1), wide (>1)
stereo_sig |> width(@, 1.4) |> out(@)
```

---

## ms_encode, ms_decode

**Mid/side processing**: encode stereo to M/S, process, decode back.

```akk
// M/S processing: boost sides (wider stereo) while preserving centre
stereo_sig
  |> ms_encode(@)
  |> @ * stereo(1.0, 2.0)   // scale mid 1.0, side 2.0
  |> ms_decode(@)
  |> out(@)
```

---

## pingpong

**True stereo ping-pong delay** where echoes cross between L and R.

Convenience: `pingpong(stereo, time, fb, width?)`
Explicit:    `pingpong(L, R, time, fb, width?)`

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| sig   | stereo | —       | Stereo input (or explicit L, R) |
| time  | signal | —       | Delay time (seconds) |
| fb    | signal | —       | Feedback (0..1) |
| width | signal | 1.0     | Pan width (0 = centre, 1 = full ping-pong). Optional. |
| dry   | signal | 1.0     | Dry-mix coefficient (Category A). Kwarg only. |
| wet   | signal | 0.5     | Wet-mix coefficient (Category A). Kwarg only. |

The output applies the unified mix line per channel: `out = dry_in * dry +
delayed * wet`. Category A defaults give a balanced parallel mix where the
echo sits 6 dB below the dry signal. Pass `wet: 1.0` to recover the
pre-0.5.0 behaviour where the wet signal was added at full level on top of
the dry source.

```akk
saw(110)
  |> stereo()
  |> pingpong(@, 0.375, 0.6)            // defaults: dry=1, wet=0.5
  |> out(@)

saw(110)
  |> stereo()
  |> pingpong(@, 0.375, 0.6, wet: 1.0)  // legacy-loud echo tail
  |> out(@)
```

---

## Stereo-native effects

Every audio DSP op (`lp`, `hp`, `delay`, `freeverb`, `saturate`, ...) is
**stereo-native**: the compiler emits one instruction flagged
`STEREO_OUTPUT`, and the opcode handles both channels in a single dispatch
with independent per-channel DSP state. A mono input widens automatically —
no `stereo()` wrapper needed.

```akk
// One instruction per effect; each handles L and R in one dispatch.
saw(220)               // mono — widens automatically
    |> lp(@, 500, 0.7)        // stereo lowpass, per-channel state
    |> delay(@, 0.25, 0.5)    // stereo delay lines per channel
    |> freeverb(@, 0.85, 0.5) // stereo reverb with cross-coupling
    |> out(@)
```

Scalar / control-rate parameters (cutoff, Q, feedback, ...) are shared
between the L and R channels. If you want independent parameters per
channel, split the chain manually with `stereo(lp(left(s), cutL), lp(right(s), cutR))`.
