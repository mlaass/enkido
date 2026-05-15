---
title: Runtime Controls
category: concepts
order: 5
keywords: [param, slider, button, toggle, dropdown, ui, interactive, controls, live, performance, knob]
---

# Runtime Controls

Akkado patches can declare named UI controls that the web IDE turns into live inputs — sliders, momentary buttons, toggles, and dropdowns. Move them while the patch plays; hot-swap preserves their values across reloads, so you can tune a sound by ear without dropping audio.

Four builtins create controls:

| Builtin    | Returns                       | UI widget             |
|------------|-------------------------------|-----------------------|
| `param`    | signal (continuous)           | Slider                |
| `button`   | trigger (1 while held)        | Momentary button      |
| `toggle`   | signal (0 or 1)               | On/off toggle         |
| `dropdown` | signal (0-based index)        | Dropdown menu         |

The controls are extracted at compile time. The IDE reads their metadata and renders the widgets next to the editor; values flow back through the runtime's `EnvMap`, so every read returns the latest user-set value with no allocation.

## param

**Continuous parameter (slider)** — exposes a named slider with a default, min, and max.

| Param   | Type   | Default | Description                       |
|---------|--------|---------|-----------------------------------|
| name    | string | —       | Display label and lookup key      |
| default | number | —       | Initial value when the patch loads |
| min     | number | 0       | Lower bound of the slider          |
| max     | number | 1       | Upper bound of the slider          |

```akk
cutoff = param("cutoff", 800, 100, 5000)
osc("saw", 220)
    |> lp(%, cutoff)
    |> out(%)
```

Names are display strings — spaces are fine (`param("lfo rate", 0.4, 0.05, 6)`). Each unique name produces one slider; calling `param` with the same name twice returns the same value.

## button

**Momentary trigger** — outputs `1` while the user holds the button, `0` otherwise. Ideal for one-shot triggers.

| Param | Type   | Default | Description                  |
|-------|--------|---------|------------------------------|
| name  | string | —       | Display label and lookup key |

```akk
hit = button("Hit!")
sample(hit, 1.0, "bd") |> out(%)
```

Press "Hit!" in the IDE to fire the kick sample on demand.

## toggle

**Boolean switch** — outputs `0` or `1`, click to flip. Use it as a gate or to switch between two signal paths.

| Param   | Type   | Default | Description                  |
|---------|--------|---------|------------------------------|
| name    | string | —       | Display label and lookup key |
| default | number | 0       | Initial state (0 or 1)       |

```akk
bypass = toggle("bypass", 0)
dry = osc("saw", 220)
wet = dry |> freeverb(%, 0.6, 0.5)
out(dry * bypass + wet * (1 - bypass))
```

## dropdown

**Selection menu** — outputs the 0-based index of the currently selected option. Up to five options after the name.

| Param   | Type   | Default | Description                  |
|---------|--------|---------|------------------------------|
| name    | string | —       | Display label and lookup key |
| opt1    | string | —       | First option (index 0)       |
| opt2…5  | string | —       | Additional options (indices 1–4) |

```akk
wave = dropdown("waveform", "sin", "saw", "sqr", "tri")
osc(wave, 220) |> out(%)
```

Switching the dropdown returns `0`, `1`, `2`, or `3`; `osc` accepts the index directly.

## Full example: live-tweakable bass-pad

This is the `interactive-params.akk` patch — a Moog-style bass with every knob exposed:

```akk
bpm = 100

cutoff    = param("cutoff", 800, 100, 5000)
resonance = param("resonance", 2.0, 0.0, 4.0)
drive     = param("drive", 1.5, 1.0, 6.0)
lfo_rate  = param("lfo rate", 0.4, 0.05, 6.0)
lfo_depth = param("lfo depth", 0.6, 0.0, 1.0)
note_freq = param("freq", 110, 55, 440)

hit_trigger = button("Hit!")
hit = sample(hit_trigger, 1.0, "bd")

// Detuned saw stack for body
voices = saw(note_freq) + saw(note_freq * 1.005) + saw(note_freq * 0.997)

// LFO modulates the cutoff around the slider value
swept = cutoff + osc("sin", lfo_rate) * cutoff * lfo_depth * 0.6

voices * 0.2
    |> moog(%, swept, resonance, _, drive)
    |> @ + freeverb(@ + hit, 0.5, 0.5) * 0.35
    |> out(%, %)
```

Load it in the web IDE, hit play, and ride the sliders. Edit the code and re-evaluate — slider positions stay put, audio crossfades seamlessly.

## How the IDE finds them

The compiler treats `param` / `button` / `toggle` / `dropdown` specially. Each call is rewritten to an `ENV_GET` opcode that looks the name up in the runtime's `EnvMap` at audio-rate. At compile time, the call site's name and (for `param`) min/max/default are exported to the IDE through `akkado_get_builtins_json()`. The IDE renders widgets from that metadata, and writes user input back into the same `EnvMap` — no extra registration step from the patch author.

Because lookups happen per-sample by name, you can declare the same control in multiple places without aliasing: every `param("cutoff", …)` reads the same slider.

## Related

- [Records as Builtin Options](/docs/concepts/record-as-options) — the broader convention for richly-configured builtins.
- [Hot-Swap](/docs/concepts/hot-swap) — how the runtime preserves slider values when you re-evaluate code.
