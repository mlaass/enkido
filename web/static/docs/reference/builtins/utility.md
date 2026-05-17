---
title: Utility
category: builtins
order: 11
keywords: [utility, out, output, mtof, midi, frequency, dc, slew, glide, interp, interp_ease_in, interp_ease_out, interp_cos, interpolation, portamento, ease-in, ease-out, cosine, time-based, sah, sample, hold, clock]
group: tools
subgroup: audio-plumbing
icon: Wrench
tagline: Output, MIDI-to-frequency, slew, glide, DC, and more.
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
osc("sin", 440) |> out(@)
```

```akk
// Stereo output (different signals)
osc("sin", 440) |> out(@, osc("sin", 442))
```

```akk
// Panned signal
osc("sin", 440) * 0.7 |> out(@, @ * 0.3)
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
sin(mtof(60)) |> out(@)
```

```akk
// A4 (MIDI note 69 = 440 Hz)
sin(mtof(69)) |> out(@)
```

```akk
// Chromatic scale using modulation
sin(mtof(48 + osc("phasor", 2) * 12)) |> out(@)
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
osc("sin", 440) * dc(0.5) |> out(@)
```

---

## slew

**Slew Rate Limiter** - Caps how fast a signal can change, in units per second.

| Param  | Type   | Description |
|--------|--------|-------------|
| target | signal | Target value |
| rate   | number | Slew rate (units per second; higher = faster) |

Rate-limited smoothing. Good for taming param sliders, slewing CVs, and audio-rate slew effects. For note-pitch glide between pattern events with a fixed *duration* regardless of interval size, reach for [`glide`](#glide) instead.

| Aspect          | `slew(target, rate)`           | `glide(sig, time, …)`   |
|-----------------|--------------------------------|-------------------------|
| Time meaning    | units per second               | total ramp duration     |
| Interval indep. | No — fast slides on big jumps  | Yes — same time always  |
| Shape           | Linear                         | Linear / ease / cosine  |
| Value-space     | Linear                         | Linear or log           |
| Best for        | CV / knob smoothing, audio slew| Note glide, portamento  |

```akk
// Smoothed pitch sweep (rate-limited)
sin(slew(mtof(48 + osc("sqr", 2) * 12), 10)) |> out(@)
```

```akk
// Smooth filter sweep
saw(110) |> lp(@, slew(200 + osc("sqr", 0.5) * 2000, 5)) |> out(@)
```

See also: [glide](#glide), [interp](#interp).

---

## glide

**Time-based glide / portamento** - Slides to a new target over a fixed *duration*, regardless of interval size. Stereo-native.

| Param | Type   | Default    | Description                                       |
|-------|--------|------------|---------------------------------------------------|
| sig   | signal | —          | Target value (sample-and-hold patterns work well) |
| time  | number | `0.05`     | Ramp duration in seconds                          |
| curve | string | `"linear"` | `"linear"`, `"ease_in"`, `"ease_out"`, `"cosine"` |
| space | string | `"linear"` | `"linear"` or `"log"` (musical pitch glide)       |

Detects target changes by exact value compare and ramps from the current emitted value to the new target over `time` seconds. Pattern fields (`@freq`, `@note`, `@vel`) feed in cleanly because they arrive as sample-and-hold buffers. Use `space: "log"` for perceptually uniform glide over wide intervals — or feed `@note` through `mtof`, which is already log-pitch by construction.

`glide` is stereo-native: it auto-widens a mono target to stereo. To compose it with a mono-only slot (like `saw(freq)`), either feed via `mtof(glide(@note, …))` (`mtof` keeps the chain mono) or wrap with `mono(...)`. See the [Glide & Interpolation](../../concepts/glide-and-interpolation.md) concept page for the full discussion.

```akk
// Default 50 ms portamento between notes
n"c4 c5" |> saw(mtof(glide(@note, 0.05))) |> out(@)
```

```akk
// 100 ms cosine S-curve glide
n"c4 c5" |> saw(mtof(glide(@note, 0.1, "cosine"))) |> out(@)
```

```akk
// Wide-interval portamento — log space keeps the slide pitch-uniform
n"c2 c6" |> saw(mono(glide(@freq, 0.2, "ease_out", "log"))) |> out(@)
```

```akk
// Smoother param-slider response
cutoff = param("cutoff", 1000, 100, 8000)
osc("saw", 220) |> lp(@, mono(glide(cutoff, 0.03))) |> out(@)
```

See also: [interp](#interp) (primitive form), [slew](#slew) (rate-based), [Glide & Interpolation](../../concepts/glide-and-interpolation.md).

---

## interp

**Time-based interpolator (linear)** - Lower-level primitive used by [`glide`](#glide). Stereo-native, no value-space conversion.

| Param  | Type   | Description                       |
|--------|--------|-----------------------------------|
| target | signal | Target value                      |
| time   | number | Ramp duration in seconds          |

Same change-detection and ramp behavior as `glide(sig, time, "linear", "linear")`, with fewer characters. Call directly when you don't need curve or value-space options.

```akk
// 100 ms linear ramp between targets
n"c4 c5" |> saw(mtof(interp(@note, 0.1))) |> out(@)
```

See also: [glide](#glide), [interp_ease_in](#interp_ease_in), [interp_ease_out](#interp_ease_out), [interp_cos](#interp_cos).

---

## interp_ease_in

**Time-based interpolator (ease-in)** - Quadratic-in shape: `t²`. Starts slow, accelerates into the target.

| Param  | Type   | Description                       |
|--------|--------|-----------------------------------|
| target | signal | Target value                      |
| time   | number | Ramp duration in seconds          |

```akk
// Notes "settle into" their targets
n"c4 c5" |> saw(mtof(interp_ease_in(@note, 0.15))) |> out(@)
```

See also: [glide](#glide) with `curve: "ease_in"`.

---

## interp_ease_out

**Time-based interpolator (ease-out)** - Quadratic-out shape: `1 - (1-t)²`. Starts fast, decelerates as it approaches the target.

| Param  | Type   | Description                       |
|--------|--------|-----------------------------------|
| target | signal | Target value                      |
| time   | number | Ramp duration in seconds          |

```akk
// Notes "spring toward" their targets
n"c4 c5" |> saw(mtof(interp_ease_out(@note, 0.15))) |> out(@)
```

See also: [glide](#glide) with `curve: "ease_out"`.

---

## interp_cos

**Time-based interpolator (cosine S-curve)** - Shape `½(1 − cos πt)`. Smooth in *and* out; the classic "tape-stop"-style ramp.

| Param  | Type   | Description                       |
|--------|--------|-----------------------------------|
| target | signal | Target value                      |
| time   | number | Ramp duration in seconds          |

```akk
// Slow S-shaped pitch sweep
n"c4 c5" |> saw(mtof(interp_cos(@note, 0.4))) |> out(@)
```

See also: [glide](#glide) with `curve: "cosine"`.

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
sin(mtof(48 + sah(osc("noise") * 24, trigger(4)))) |> out(@)
```

```akk
// Stepped filter
saw(110)
    |> lp(@, 200 + sah(osc("noise") * 2000, trigger(2)))
    |> out(@)
```

---

## clock

**Master Clock** - Returns the global clock signal.

No parameters.

Returns the master clock signal synchronized to the BPM. Useful for building custom timing logic.

```akk
// Clock-synced modulation
sin(440 + clock() * 100) |> out(@)
```

Related: [trigger](#trigger), [lfo](#lfo)
