---
title: Utility
category: builtins
order: 11
keywords: [utility, out, output, mtof, midi, frequency, dc, slew, sah, sample, hold, clock]
---

# Utility

Utility functions for common audio tasks like output, MIDI conversion, and signal processing helpers.

## out

**Audio Output** - Send signal to speakers.

| Param | Type   | Description |
|-------|--------|-------------|
| L     | signal | Left channel |
| R     | signal | Right channel (optional, defaults to L) |

Aliases: `output`

Every Akkado patch needs an `out()` to produce sound. Pass one signal for mono, two for stereo.

```akk
// Mono output (same signal to both speakers)
osc("sin", 440) |> out(%, %)
```

```akk
// Stereo output (different signals)
osc("sin", 440) |> out(%, osc("sin", 442))
```

```akk
// Panned signal
osc("sin", 440) * 0.7 |> out(%, % * 0.3)
```

---

## mtof

**MIDI to Frequency** - Convert MIDI note number to frequency in Hz.

| Param | Type   | Description |
|-------|--------|-------------|
| note  | signal | MIDI note number (0-127) |

Converts MIDI note numbers to frequencies using equal temperament. Middle C (C4) is note 60 = 261.6 Hz.

```akk
// Middle C
sin(mtof(60)) |> out(%, %)
```

```akk
// A4 (MIDI note 69 = 440 Hz)
sin(mtof(69)) |> out(%, %)
```

```akk
// Chromatic scale using modulation
sin(mtof(48 + osc("phasor", 2) * 12)) |> out(%, %)
```

---

## dc

**DC Offset** - Constant value generator.

| Param  | Type   | Description |
|--------|--------|-------------|
| offset | number | Constant value to output |

Outputs a constant value. Useful for mixing with signals or providing a static parameter.

```akk
// Use as constant multiplier
osc("sin", 440) * dc(0.5) |> out(%, %)
```

---

## slew

**Slew Rate Limiter** - Smoothly transitions between values.

| Param  | Type   | Description |
|--------|--------|-------------|
| target | signal | Target value |
| rate   | number | Slew rate (higher = faster) |

Limits how fast a signal can change, creating smooth transitions. Great for portamento or smoothing control signals.

```akk
// Portamento effect (smooth pitch changes)
sin(slew(mtof(48 + osc("sqr", 2) * 12), 10)) |> out(%, %)
```

```akk
// Smooth filter sweep
saw(110) |> lp(%, slew(200 + osc("sqr", 0.5) * 2000, 5)) |> out(%, %)
```

---

## sah

**Sample and Hold** - Captures a signal when triggered.

| Param | Type    | Description |
|-------|---------|-------------|
| in    | signal  | Input signal to sample |
| trig  | trigger | When to capture |

Samples the input signal each time the trigger fires and holds that value until the next trigger. Classic modular synth technique.

```akk
// Random pitches
sin(mtof(48 + sah(osc("noise") * 24, trigger(4)))) |> out(%, %)
```

```akk
// Stepped filter
saw(110) |> lp(%, 200 + sah(osc("noise") * 2000, trigger(2))) |> out(%, %)
```

---

## clock

**Master Clock** - Returns the global clock signal.

No parameters.

Returns the master clock signal synchronized to the BPM. Useful for building custom timing logic.

```akk
// Clock-synced modulation
sin(440 + clock() * 100) |> out(%, %)
```

Related: [trigger](#trigger), [lfo](#lfo)
