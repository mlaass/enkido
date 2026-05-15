---
title: Delays
category: builtins
order: 5
keywords: [delay, delay_ms, delay_smp, tap_delay, tap_delay_ms, tap_delay_smp, echo, feedback, time, fb, effect, comb]
group: effects
subgroup: time-based
icon: Repeat2
tagline: Single- and multi-tap delay lines.
subfeatures:
  - name: Delay
    anchor: delay
    tagline: Beat-synced feedback delay.
    snippet: 'osc("saw", 220) |> delay(@, 0.5, 0.4)'
  - name: Delay (ms)
    anchor: delay_ms
    tagline: Time-domain delay with millisecond control.
    snippet: 'osc("saw", 220) |> delay_ms(@, 300, 0.4)'
  - name: Tap Delay
    anchor: tap_delay
    tagline: Multi-tap with feedback processor.
    snippet: 'osc("saw", 110) |> tap_delay(@, 0.375, 0.7, (x) -> lp(x, 1500))'
---

# Delays

Delay effects create copies of a signal offset in time for echoes, rhythmic effects, and spatial depth.

## delay

**Delay** - Creates a delayed copy of the input signal with feedback.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| time  | signal | -       | Delay time in seconds (0-10) |
| fb    | number | -       | Feedback amount (0-1) |
| dry   | number | 0.0     | Dry signal level |
| wet   | number | 1.0     | Wet (delayed) signal level |

A simple delay line with feedback. Use short times (< 50ms) for comb filtering effects, medium times for slapback, and longer times for distinct echoes.

The optional `dry` and `wet` parameters control the output mix. Defaults (dry=0, wet=1) output 100% wet signal for backward compatibility. Set dry=1, wet=1 for classic delay pedal behavior.

```akk
// Simple echo at quarter note (120 BPM = 0.5s)
osc("saw", 220) |> delay(@, 0.5, 0.4) |> out(@)
```

```akk
// Classic delay pedal: dry passthrough with wet echoes
osc("saw", 220) |> delay(@, 0.5, 0.4, 1.0, 0.6) |> out(@)
```

```akk
// Slapback delay for thickening
osc("saw", 110) |> delay(@, 0.08, 0.3, 0.7, 0.5) |> out(@)
```

```akk
// Ping-pong style stereo delay
osc("saw", 110) |> (delay(@, 0.3, 0.5), delay(@, 0.45, 0.5)) |> out(@)
```

## delay_ms

**Delay (milliseconds)** - Same as delay, but with time in milliseconds.

| Param   | Type   | Default | Description |
|---------|--------|---------|-------------|
| in      | signal | -       | Input signal |
| time_ms | signal | -       | Delay time in milliseconds (0-10000) |
| fb      | number | -       | Feedback amount (0-1) |
| dry     | number | 0.0     | Dry signal level |
| wet     | number | 1.0     | Wet (delayed) signal level |

```akk
// 300ms delay
osc("saw", 220) |> delay_ms(@, 300, 0.4) |> out(@)
```

```akk
// 50/50 mix
osc("saw", 220) |> delay_ms(@, 300, 0.4, 0.5, 0.5) |> out(@)
```

## delay_smp

**Delay (samples)** - Same as delay, but with time in samples for precise control.

| Param    | Type   | Default | Description |
|----------|--------|---------|-------------|
| in       | signal | -       | Input signal |
| time_smp | signal | -       | Delay time in samples |
| fb       | number | -       | Feedback amount (0-1) |
| dry      | number | 0.0     | Dry signal level |
| wet      | number | 1.0     | Wet (delayed) signal level |

Use sample-based delay for comb filtering and Karplus-Strong synthesis effects. The resonant frequency is `sample_rate / delay_samples`.

```akk
// Comb filter at ~480Hz (48000 / 100 = 480)
noise() |> delay_smp(@, 100, 0.9) |> out(@)
```

```akk
// Karplus-Strong style pluck (109 samples at 48kHz ≈ 440Hz)
noise() * ar(button("pluck"), 0.001, 0.001) |> delay_smp(@, 109, 0.995) |> out(@)
```

## tap_delay

**Tap delay** - Delay with configurable feedback processing chain.

| Param     | Type    | Default | Description |
|-----------|---------|---------|-------------|
| in        | signal  | -       | Input signal |
| time      | signal  | -       | Delay time in seconds (0-10) |
| fb        | number  | -       | Feedback amount (0-1) |
| processor | closure | -       | `(x) -> ...` feedback processing |
| dry       | number  | 0.0     | Dry signal level |
| wet       | number  | 1.0     | Wet (delayed) signal level |

Tap delay runs the feedback signal through any DSP chain (filters, distortion, etc.). Used for tape delay (high-frequency rolloff), dub delay (filtering + saturation), and shimmer (pitch shifting).

The optional `dry` and `wet` parameters control the output mix. Defaults (dry=0, wet=1) output 100% wet signal for backward compatibility.

```akk
// Dub-style delay with lowpass filter in feedback
osc("saw", 110) |> tap_delay(@, 0.375, 0.7, (x) -> lp(x, 1500)) |> out(@)
```

```akk
// Degrading tape delay with dry/wet mix
osc("saw", 110) |> tap_delay(@, 0.4, 0.65, (x) ->
    lp(x, 3000) |> saturate(@, 0.1),
    0.3, 0.7
) |> out(@)
```

```akk
// Classic delay pedal: full dry signal with softer echoes
osc("saw", 110) |> tap_delay(@, 0.375, 0.7, (x) -> lp(x, 1500), 1.0, 0.5) |> out(@)
```

## tap_delay_ms

**Tap delay (milliseconds)** - Same as tap_delay, but with time in milliseconds.

| Param     | Type    | Default | Description |
|-----------|---------|---------|-------------|
| in        | signal  | -       | Input signal |
| time_ms   | signal  | -       | Delay time in milliseconds (0-10000) |
| fb        | number  | -       | Feedback amount (0-1) |
| processor | closure | -       | `(x) -> ...` feedback processing |
| dry       | number  | 0.0     | Dry signal level |
| wet       | number  | 1.0     | Wet (delayed) signal level |

```akk
// 300ms delay with feedback filtering
osc("saw", 110) |> tap_delay_ms(@, 300, 0.7, (x) -> lp(x, 1000)) |> out(@)
```

```akk
// 300ms delay with mix control
osc("saw", 110) |> tap_delay_ms(@, 300, 0.7, (x) -> lp(x, 1000), 0.5, 0.5) |> out(@)
```

## tap_delay_smp

**Tap delay (samples)** - Same as tap_delay, but with time in samples.

| Param     | Type    | Default | Description |
|-----------|---------|---------|-------------|
| in        | signal  | -       | Input signal |
| time_smp  | signal  | -       | Delay time in samples |
| fb        | number  | -       | Feedback amount (0-1) |
| processor | closure | -       | `(x) -> ...` feedback processing |
| dry       | number  | 0.0     | Dry signal level |
| wet       | number  | 1.0     | Wet (delayed) signal level |

Use sample-based tap delay for comb filtering with feedback processing.

```akk
// Resonant comb filter with dampening
noise() |> tap_delay_smp(@, 100, 0.95, (x) -> lp(x, 2000)) |> out(@)
```

```akk
// Comb filter with dry/wet blend
noise() |> tap_delay_smp(@, 100, 0.95, (x) -> lp(x, 2000), 0.3, 0.7) |> out(@)
```

Related: [freeverb](#../reverbs#freeverb), [comb](#../modulation#comb)
