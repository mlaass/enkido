---
title: Delays
category: builtins
order: 5
keywords: [delay, echo, feedback, time, fb, effect]
---

# Delays

Delay effects create copies of a signal offset in time, enabling echoes, rhythmic effects, and spatial depth.

## delay

**Delay** - Creates a delayed copy of the input signal with feedback.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| time  | signal | -       | Delay time in seconds |
| fb    | number | -       | Feedback amount (0-1) |

A simple delay line with feedback. Use short times (< 50ms) for comb filtering effects, medium times for slapback, and longer times for distinct echoes.

```akk
// Simple echo at quarter note (120 BPM = 0.5s)
saw(220) |> delay(%, 0.5, 0.4) |> out(%, %)
```

```akk
// Slapback delay for thickening
saw(110) |> delay(%, 0.08, 0.3) * 0.5 + % |> out(%, %)
```

```akk
// Ping-pong style stereo delay
saw(110) |> (delay(%, 0.3, 0.5), delay(%, 0.45, 0.5)) |> out(%, %)
```

Related: [freeverb](#../reverbs#freeverb), [comb](#../modulation#comb)
