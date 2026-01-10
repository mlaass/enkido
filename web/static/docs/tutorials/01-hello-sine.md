---
title: Hello Sine
order: 1
category: tutorials
keywords: [getting started, first sound, sine, oscillator, output, beginner, tutorial]
---

# Your First Sound

Welcome to Akkado! In this tutorial, you'll make your first sound and learn the basics of how the language works.

## The Simplest Patch

Every Akkado program is a signal flow graph. Let's start with the simplest possible patch - a sine wave sent to the output.

```akk
sin(440) |> out(%, %)
```

Click **Run** above to hear a 440 Hz sine wave (concert A).

## Understanding the Code

Let's break down what each part means:

- `sin(440)` - Creates a sine wave oscillator at 440 Hz
- `|>` - The **pipe** operator, connecting nodes in the signal flow
- `%` - The **hole**, representing the signal from the left side of the pipe
- `out(%, %)` - Sends the signal to both left and right speakers

## Changing the Frequency

Try different frequencies by changing the number:

```akk
// Lower octave (220 Hz)
sin(220) |> out(%, %)
```

```akk
// Higher octave (880 Hz)
sin(880) |> out(%, %)
```

## Adding More Oscillators

You can combine multiple oscillators with math operators:

```akk
// Two detuned oscillators for a fatter sound
sin(440) + sin(442) |> out(%, %) * 0.5
```

The `* 0.5` at the end reduces the volume so it doesn't clip.

## Different Waveforms

Besides sine waves, Akkado has other oscillator types:

```akk
// Sawtooth - rich and buzzy
saw(220) |> out(%, %)
```

```akk
// Triangle - softer than sawtooth
tri(220) |> out(%, %)
```

```akk
// Square - hollow and punchy
sqr(220) * 0.3 |> out(%, %)
```

## Next Steps

Now that you can make basic sounds, try:
- Combining different waveforms
- Using math to modulate the frequency
- Moving on to the [Filters tutorial](02-filters.md) to shape your sounds

Congratulations - you've made your first sounds with Akkado!
