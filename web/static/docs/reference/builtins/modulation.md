---
title: Modulation Effects
category: builtins
order: 7
keywords: [modulation, chorus, flanger, phaser, comb, effect, rate, depth, sweep, lfo_phase, stereo, decorrelation, extended-params]
group: effects
subgroup: time-based
icon: Wand2
tagline: Modulated short delay lines for movement and color.
subfeatures:
  - name: Chorus
    anchor: chorus
    tagline: Stereo chorus, lush detune.
    snippet: 'osc("saw", 220) |> chorus(@, 0.5, 0.5)'
  - name: Flanger
    anchor: flanger
    tagline: Swept short delay with feedback.
    snippet: 'osc("saw", 110) |> flanger(@, 0.5, 0.7)'
  - name: Phaser
    anchor: phaser
    tagline: All-pass cascade phaser.
    snippet: 'osc("saw", 110) |> phaser(@, 0.3, 0.8)'
  - name: Comb
    anchor: comb
    tagline: Comb filter modulation.
    snippet: 'osc("noise") |> comb(@, 1/220, 0.95)'
---

# Modulation Effects

Modulation effects use time-varying delays to add movement and spatial width to sounds.

**Note:** All modulation effects output 100% wet signal. For dry/wet mixing, blend manually:

```akk
// 30% dry, 70% wet chorus
dry = osc("saw", 220)
dry * 0.3 + chorus(dry, 0.5, 0.5) * 0.7 |> out(@)
```

## chorus

**Chorus** - Creates copies with slight pitch/time variations.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| rate  | number | 0.5     | LFO rate in Hz |
| depth | number | 0.5     | Modulation depth (0-1) |
| base_delay | number | 20.0 | Base chorus delay (ms) |
| depth_range | number | 10.0 | Modulation depth range (ms) |
| lfo_phase | number | 0.25 | R-channel LFO offset, in **turns** (0.0–1.0). 0 = mono-equivalent; 0.25 = 90° (default stereo widening); 0.5 = anti-phase. |

Mixes the input with delayed copies that are slightly pitch-shifted by an LFO, producing a thicker, wider sound.

The `base_delay` parameter sets the center delay time, while `depth_range` controls how far the modulation sweeps from the base. Larger values produce more pronounced detuning.

Chorus is stereo-native: mono input widens to a true stereo image with the right channel reading the LFO at the `lfo_phase` offset; stereo input gets independent per-channel processing.

```akk
// Classic chorus
osc("saw", 220) |> chorus(@, 0.5, 0.5) |> out(@)
```

```akk
// Slow deep chorus
osc("tri", 110) |> chorus(@, 0.2, 0.8) |> out(@)
```

```akk
// Fast shimmer
osc("sin", 440) |> chorus(@, 2, 0.3) |> out(@)
```

```akk
// Wide chorus with longer delay
osc("saw", 220) |> chorus(@, 0.3, 0.6, 30, 15) |> out(@)
```

```akk
// Anti-phase R LFO for maximum stereo width
osc("saw", 220) |> chorus(@, 0.5, 0.6, lfo_phase: 0.5) |> out(@)
```

```akk
// lfo_phase: 0 = mono-equivalent (L=R)
osc("saw", 220) |> chorus(@, 0.5, 0.6, lfo_phase: 0) |> out(@)
```

Related: [flanger](#flanger), [phaser](#phaser)

---

## flanger

**Flanger** - Comb filtering with swept delay time.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| rate  | number | 1.0     | LFO rate in Hz |
| depth | number | 0.7     | Modulation depth (0-1) |
| min_delay | number | 0.1 | Minimum sweep delay (ms) |
| max_delay | number | 10.0 | Maximum sweep delay (ms) |
| lfo_phase | number | 0.25 | R-channel LFO offset, in **turns** (0.0–1.0). 0 = mono-equivalent; 0.25 = 90° (default); 0.5 = anti-phase. |

Similar to chorus but with shorter delay times and feedback, creating the characteristic "jet plane" sweep effect. Flanger is stereo-native: mono input widens via the `lfo_phase` offset on the right channel.

The `min_delay` and `max_delay` parameters define the sweep range. Shorter delays create more metallic tones, longer delays sound more like chorus.

```akk
// Classic flanger
osc("saw", 110) |> flanger(@, 0.5, 0.7) |> out(@)
```

```akk
// Slow metallic sweep
osc("sqr", 220) |> flanger(@, 0.1, 0.9) |> out(@)
```

```akk
// Fast subtle movement
osc("tri", 440) |> flanger(@, 3, 0.3) |> out(@)
```

```akk
// Tight metallic flanger
osc("saw", 110) |> flanger(@, 0.5, 0.8, 0.05, 2.0) |> out(@)
```

Related: [chorus](#chorus), [phaser](#phaser), [comb](#comb)

---

## phaser

**Phaser** - Creates notches in frequency spectrum via allpass filters.

| Param    | Type   | Default | Description |
|----------|--------|---------|-------------|
| in       | signal | -       | Input signal |
| rate     | number | 0.5     | LFO rate in Hz |
| depth    | number | 0.8     | Modulation depth (0-1) |
| min_freq | number | 200.0   | Sweep range low (Hz) |
| max_freq | number | 4000.0  | Sweep range high (Hz) |
| feedback | number | 0.5     | Feedback amount (0–0.99). Higher = sharper resonance. |
| stages   | number | 4       | Number of allpass stages (2–12). Each pair of stages = one notch. |
| lfo_phase | number | 0.25 | R-channel LFO offset, in **turns** (0.0–1.0). 0 = mono-equivalent; 0.25 = 90° (default stereo notch sweep); 0.5 = anti-phase. |

Sweeps a chain of allpass-derived notches through the spectrum, producing a
swirling effect distinct from chorus or flanger.

More stages give more notches; higher feedback sharpens the resonance. Both
parameters are full runtime params — you can wire a signal to either one.

The phaser output is the canonical Bode/MXR sum (dry + allpass cascade), so
expect up to +6 dB peak gain at constructive interference points.

```akk
// Classic phaser
osc("saw", 110) |> phaser(@, 0.3, 0.8) |> out(@)
```

```akk
// Fast space phaser
osc("sqr", 220) |> phaser(@, 2, 0.5) |> out(@)
```

```akk
// Slow deep sweep
osc("noise") |> lp(@, 2000) |> phaser(@, 0.1, 0.9) |> out(@)
```

```akk
// Extended high-frequency sweep
osc("saw", 110) |> phaser(@, 0.2, 0.8, 100, 8000) |> out(@)
```

```akk
// Deep, resonant 6-stage phaser with strong feedback
osc("saw", 110)
    |> phaser(@, 0.5, 0.9, 100, 5000, feedback: 0.7, stages: 6)
    |> out(@)
```

```akk
// Wide stereo phaser — anti-phase R LFO
osc("saw", 110) |> phaser(@, 0.3, 0.8, lfo_phase: 0.5) |> out(@)
```

Related: [flanger](#flanger), [chorus](#chorus)

---

## comb

**Comb Filter** - Fixed delay with feedback for resonant coloring.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| time  | signal | -       | Delay time in seconds |
| fb    | number | -       | Feedback amount (0-1) |

A comb filter creates a series of peaks and notches at harmonics of the delay frequency. The fundamental frequency is approximately 1/time Hz.

```akk
// Tuned resonator at ~220 Hz
osc("noise") |> comb(@, 1/220, 0.95) |> out(@)
```

```akk
// Metallic coloring
osc("saw", 110) |> comb(@, 0.01, 0.7) |> out(@)
```

```akk
// Karplus-Strong style pluck
osc("noise") * ar(trigger(4), 0.001, 0.01)
    |> comb(@, 1/440, 0.99)
    |> out(@)
```

Related: [flanger](#flanger), [delay](#../delays#delay)
