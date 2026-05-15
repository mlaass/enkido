---
title: Hello Sine
order: 1
category: tutorials
keywords: [getting started, first sound, sine, oscillator, output, beginner, tutorial, osc]
---

# Your First Sound

## The simplest patch

Every Akkado program is a signal flow graph. The smallest one is a sine wave sent to the output:

```akk
osc("sin", 440) |> out(@)
```

Click **Run** above to hear a 440 Hz sine wave (concert A).

## Understanding the code

What each part means:

- `osc("sin", 440)` - a sine wave oscillator at 440 Hz
- `|>` - the **pipe** operator, connecting nodes in the signal flow
- `@` - the **hole**, representing the signal from the left side of the pipe
- `out(@)` - sends the signal to both left and right speakers

## Changing the frequency

Try different frequencies by changing the number:

```akk
// Lower octave (220 Hz)
osc("sin", 220) |> out(@)
```

```akk
// Higher octave (880 Hz)
osc("sin", 880) |> out(@)
```

## Adding more oscillators

Combine multiple oscillators with math operators:

```akk
// Two detuned oscillators for a fatter sound
osc("sin", 440) + osc("sin", 442) |> out(@) * 0.5
```

The `* 0.5` at the end reduces the volume so it doesn't clip.

## Different waveforms

`osc()` covers all the basic waveforms; pass the name as the first argument:

```akk
// Sawtooth - rich and buzzy
osc("saw", 220) |> out(@)
```

```akk
// Triangle - softer than sawtooth
osc("tri", 220) |> out(@)
```

```akk
// Square - hollow and punchy
osc("sqr", 220) * 0.3 |> out(@)
```

## Next steps

- Combine different waveforms
- Use math to modulate the frequency
- Move on to the [Filters tutorial](02-filters.md) to shape your sounds
