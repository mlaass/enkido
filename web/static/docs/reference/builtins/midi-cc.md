---
title: MIDI CC
category: builtins
order: 15
keywords: [midi_cc, cc, controller, control-change, control change, pitch-bend, pitchbend, pb, aftertouch, channel-pressure, at, param, route, envmap, slew, channel, modwheel]
group: sequencing
subgroup: state-io
icon: Sliders
tagline: Route MIDI CC, pitch-bend, or aftertouch to a param() slot.
---

# MIDI CC

`midi_cc()` is a **compile-time directive** that maps an incoming MIDI controller onto an existing `param()` slot. There is no audio-thread work and no extra opcode — the host MIDI callback evaluates the route table and calls `vm.set_param(name, value, slew_ms)` whenever a matching event arrives. The slider remains the visible, savable default; the controller overwrites it whenever the wheel or knob moves.

## midi_cc

**Route a MIDI controller to a param() slot** — Compile-time directive. Emits no bytecode.

| Param | Type | Default | Description |
|---|---|---|---|
| `name` (positional) | string | — | Target `param()` slot name. The slot must already be declared with `param("name", default, min, max)`. |
| `cc` | number | `-128` (unset) | CC number 0-127. Omit when `pb:` or `at:` is set. |
| `pb` | bool | `false` | Route 14-bit pitch-bend (status byte `0xEn`) instead of a CC. |
| `at` | bool | `false` | Route channel aftertouch (status byte `0xDn`) instead of a CC. |
| `channel` | number | `0` | Channel filter, 1-16. `0` = accept all channels. |
| `min` | number | `0` (`-1` if `pb:`) | Output range minimum. |
| `max` | number | `1` | Output range maximum. |
| `slew` | number | `5` | EnvMap slew, in milliseconds (linear interpolation toward the new value). |

Exactly one of `cc:`, `pb:`, `at:` must be set. Setting two or none raises a compile error.

### Examples

Mod-wheel (CC 1) to filter cutoff:

```akk
cutoff = param("cutoff", 800, 100, 8000)
midi_cc("cutoff", {cc: 1, min: 100, max: 8000})

osc("saw", 220) |> lp(@, cutoff, 0.7) |> out(@)
```

Pitch-bend wheel to ±2 semitones:

```akk
bend = param("bend", 0, -2, 2)
midi_cc("bend", {pb: true, min: -2, max: 2})

osc("saw", 440 * pow(2, bend / 12)) |> out(@)
```

Channel-aftertouch to filter resonance:

```akk
res = param("res", 0.5, 0, 1)
midi_cc("res", {at: true, min: 0, max: 1})

osc("saw", 220) |> lp(@, 2000, res) |> out(@)
```

Channel-filtered CC (only listen to channel 1):

```akk
verb = param("verb", 0.3, 0, 1)
midi_cc("verb", {cc: 91, channel: 1, min: 0, max: 1})
```

See `static/patches/midi-cc-filtermono.akk` for a full mono-synth patch with five CC routes (cutoff, resonance, PWM, glide, pitch-bend).

### Routing and slew

EnvMap performs linear interpolation toward the latest target value over `slew` milliseconds, recomputed once per block. The slew is internally capped at a 0.1 ms minimum so a `slew: 0` is still spike-free. Set higher values (50-200 ms) when smoothing a sticky controller like an expression pedal, or lower (1-5 ms) for snappy keyboard knobs.

The `param()` slider's saved value is the default until the controller first moves; from that point on, the controller is the source of truth until the slider is dragged or the patch is hot-swapped.

### `.mid` file CC playback

File CCs are dispatched through the same route table as live CCs — the parser stores them in `MidiSequence::ccs[]` and the audio engine fires them as the play head crosses each event. No extra wiring is required; the same `midi_cc()` directive picks them up whether the bytes came from rtmidi or from an SMF file.

### Common CC numbers

| CC | Convention | Notes |
|---:|---|---|
| 1 | Mod wheel | Nearly universal. |
| 2 | Breath / joystick Y | Korg-style controllers. |
| 7 | Volume | GM main volume. |
| 10 | Pan | GM pan. |
| 11 | Expression | Continuous "swell" pedal. |
| 64 | Sustain pedal | 0-63 = off, 64-127 = on. |
| 71 | Resonance | GM2 spec; only sent by mappable knobs. |
| 74 | Brightness / cutoff | GM2 spec; same caveat. |
| 91 | Reverb send | GM. |
| 93 | Chorus send | GM. |

CC numbers vary by gear. If a route does not move, change `cc:` to match what your controller actually sends — most MIDI keyboards include a CC reference in their manual.

### See also

- [`midi`](midi) — the event source that produces the CCs.
- [`param`](../language/builtins) — the slot `midi_cc` writes into.
- [Runtime Controls](../../concepts/runtime-controls) — how `param()` slots, sliders, and EnvMap interact.
