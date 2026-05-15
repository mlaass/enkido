---
title: Glide & Interpolation
category: concepts
order: 4
keywords: [glide, portamento, interp, interpolation, slew, ease, ease-in, ease-out, cosine, log, pitch, transition, time-based]
---

# Glide & Interpolation

Pattern fields like `@.freq` and `@.note` jump *step-wise* from one event to the next — they're sample-and-hold buffers. `glide` smooths those jumps over a configurable **duration**: 100 ms between c4 and c5 takes 100 ms, and 100 ms between c2 and c6 also takes 100 ms. That's the headline difference from [`slew`](../reference/builtins/utility.md#slew), which limits how fast the signal can change *per second* — so a wide interval takes proportionally longer.

## `slew` vs `glide`

| Aspect          | `slew(target, rate)`           | `glide(sig, time, …)`   |
|-----------------|--------------------------------|-------------------------|
| Time meaning    | units per second               | total ramp duration     |
| Interval indep. | No — fast slides on big jumps  | Yes — same time always  |
| Shape           | Linear                         | Linear / ease / cosine  |
| Value-space     | Linear                         | Linear or log           |
| State           | `SlewState`                    | `InterpTimeState`       |

| Use case                            | Reach for     |
|-------------------------------------|---------------|
| Note pitch / portamento             | `glide`       |
| Velocity / pattern field smoothing  | `glide`       |
| Param slider response (knob lag)    | `glide`       |
| Audio-rate signal smoothing         | `slew` or LP  |
| Limiting how fast a CV can change   | `slew`        |
| Anti-click ramp on amp envelopes    | `slew` or LP  |

Rule of thumb: **if you can answer "how many seconds should the slide take?" reach for `glide`. If you can only answer "how fast can it change per second?" reach for `slew`.**

## Two paths to musical glide

Linear interpolation in **Hz** sounds non-uniform over wide intervals — a c4→c5 glide rushes through the low frequencies and slows at the top, because equal frequency steps don't sound equal-sized to the ear. There are two ways to get a pitch-perceptual (log) glide; both produce identical audio:

### Path A — `space: "log"` on `glide`

```akkado
n"c2 c6" |> saw(glide(@.freq, 0.2, "linear", "log")) |> out(@)
```

Internally pipes the target through `log + scale` and `pow(2, …)`. Use this when you only have a frequency signal — e.g. a `param`, an LFO, or a non-pattern source where MIDI note isn't naturally available.

### Path B — glide the MIDI note, then `mtof`

```akkado
n"c2 c6" |> saw(mtof(glide(@.note, 0.2))) |> out(@)
```

`@.note` carries the MIDI note number (linear in semitones), so a plain linear glide is already log-pitch. More transparent, fewer characters. **Recommended for pattern sources** — but both paths produce identical audio.

## Curve shapes

Four shapes ship in v1, selected via the `curve` argument or the corresponding `interp*` builtin:

| `curve`      | Builtin             | Shape `t → shaped_t`     | Feel                                       |
|--------------|---------------------|--------------------------|--------------------------------------------|
| `"linear"`   | `interp`            | `t`                      | Even motion through the slide              |
| `"ease_in"`  | `interp_ease_in`    | `t²`                     | Starts slow; "settles into" the target     |
| `"ease_out"` | `interp_ease_out`   | `1 − (1 − t)²`           | Starts fast; "springs toward" the target   |
| `"cosine"`   | `interp_cos`        | `½(1 − cos πt)`          | Smooth in *and* out; classic S-curve       |

The `interp*` family is the lower-level primitive. `glide` is the recommended high-level entry point because it adds value-space conversion and gives you a sensible default (50 ms linear).

## Channel width follows the input

`glide`, every `interp*` builtin, and `slew` preserve the channel width of their primary input. Mono in → mono out, so a pattern field like `@.freq` flows straight into a mono parameter slot:

```akkado
n"c4 c5" |> saw(glide(@.freq, 0.1)) |> out(@)
```

Feed a stereo signal and you get independent per-channel ramps — useful for stereo CV processing:

```akkado
s = stereo(saw(218), saw(222))
glide(s, 0.05) |> out(@)
```

See [Mono & Stereo Signals](./signals.md#stereo-native-effects) for the bigger picture.

## Edge cases

- **`time = 0`** is passthrough — output tracks the target sample-exact, no ramp.
- **Repeated identical notes** don't retrigger a ramp. Change detection uses an exact float compare on the target, and a held note emits the same value every sample. If you want every note onset to glide regardless, future versions may add an explicit `trig:` option; for now, perturb the source.
- **Mid-ramp retargeting** anchors smoothly: the new ramp starts from the *currently emitted* value, not from the original ramp start. No value jumps when notes change rapidly.
- **NaN / infinite targets** hold the last good value. Useful when upstream sources may briefly go invalid.
- **Audio-rate noise as a target** retargets every sample. The output behaves like a heavy lowpass — almost certainly not what you wanted. Pre-sample-and-hold the source first (`sah` with a trigger) or use `slew` instead.

## See also

- [`glide`, `interp*` reference](../reference/builtins/utility.md#glide) — full signatures and inline examples.
- [`slew` reference](../reference/builtins/utility.md#slew) — the rate-based companion.
- [Mono & Stereo Signals](./signals.md) — why `mono(...)` / `mtof(...)` show up in the canonical examples.
