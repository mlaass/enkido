---
title: MIDI Input
category: tutorials
order: 7
keywords: [tutorial, midi, keyboard, controller, cc, pitch-bend, soundfont, .mid, file, live, mono, poly, lead]
---

# MIDI Input

Plug in a keyboard and play through your patch. This tutorial walks from a one-line synth to a full mono lead with CC routing and `.mid` file playback.

## Plug in a keyboard

The shortest playable MIDI patch is one line:

```akk
midi() |> poly(@, ({freq, gate, vel}) -> sine(freq) * adsr(gate, 0.01, 0.2, 0.7, 0.3) * vel, 8) |> out(@)
```

Bare `midi()` opens the host's default MIDI input. On the web, the Files panel has a MIDI tab that lists every visible device and asks for browser permission on first use. On the CLI, run `nkido --list-midi-devices` to see what's connected, then pass `--midi-in "Name"` (substring match) to pick one.

## A velocity-aware lead

A real instrument reacts to how hard you play. A saw/square hybrid with a velocity-modulated filter envelope sounds rounder soft, brighter hard:

```akk
cutoff_base = param("cutoff", 600, 100, 4000)
env_amount  = param("env",   1800, 0, 6000)
res         = param("res",    0.4, 0, 1)

fn lead({freq, gate, vel}) ->
    saw(freq) * 0.6 + sqr(freq * 0.5) * 0.3
        |> lp(@, cutoff_base + env_amount * vel * ar(gate, 0.005, 0.4), res)
        |> @ * adsr(gate, 0.05, 0.2, 0.7, 0.3) * vel

midi() |> poly(@, lead, 8) |> out(@)
```

The three sliders are global — drag them while playing to dial in the sound. `vel` scales both the filter envelope amount and the final amplitude, so quiet notes are quiet *and* dark.

## Mono lead with field access

For solo lines, swap `poly()` for monophonic field access:

```akk
midi() as e
    |> saw(e.freq) * adsr(e.gate, 0.01, 0.2, 0.7, 0.3) * e.vel
    |> lp(@, 1800, 0.7)
    |> out(@)
```

`midi() as e` exposes four per-block signals: `e.freq`, `e.gate`, `e.vel`, and `e.trig` (a one-sample pulse on every note-on). The voice policy is last-note-wins with held-stack fallback — release the latest key and the pitch drops back to the most recent still-held one, like every analog mono synth.

`e.gate` drops to zero for one sample on every new note-on so ADSR retriggers cleanly between adjacent notes. For sliding portamento, wrap the frequency with `glide()`:

```akk
glide_time = param("glide", 0.05, 0, 0.5)
midi() as e
    |> saw(glide(e.freq, glide_time)) * adsr(e.gate, 0.01, 0.2, 0.7, 0.3)
    |> out(@)
```

Pattern-only fields like `e.note` or `e.dur` are not synthesised — accessing them raises compile error `E136`.

## Hook up the mod wheel

`midi_cc()` is a compile-time directive that maps a controller stream onto an existing `param()` slot. There's no extra opcode and no audio-thread overhead — the host MIDI callback writes the slot directly.

```akk
cutoff = param("cutoff", 1200, 100, 8000)
midi_cc("cutoff", {cc: 1, min: 100, max: 8000})

midi() as e
    |> saw(e.freq) * adsr(e.gate, 0.01, 0.2, 0.7, 0.3)
    |> lp(@, cutoff, 0.7)
    |> out(@)
```

The slider still shows the saved default; the wheel takes over as soon as it moves. Add pitch-bend the same way, with `pb: true`:

```akk
bend = param("bend", 0, -2, 2)
midi_cc("bend", {pb: true, min: -2, max: 2})

saw(440 * pow(2, bend / 12)) |> out(@)
```

See `static/patches/midi-cc-filtermono.akk` for a five-route patch (cutoff, resonance, PWM, glide, pitch-bend) you can drop into the editor and play.

## Play a `.mid` file

`midi({file: "..."})` swaps live input for a SMF (Standard MIDI File). On the web, drag a `.mid` onto the Files panel — the registered name is the bare filename. On the CLI, the path resolves through the same URI resolver as samples (relative path, `file://`, or `https://`).

```akk
midi({file: "twinkle.mid", loop: true})
    |> soundfont(@, "gm", 0)
    |> out(@)
```

Two tempo modes:

- `tempo: "follow"` (default) — the file plays at the engine's master clock. Speed it up with `bpm = 180` at the top of your patch.
- `tempo: "file"` — honor the SMF's embedded tempo meta events. Useful when the file has its own swing / rubato.

```akk
midi({file: "swing-groove.mid", tempo: "file"})
    |> poly(@, lead, 12)
    |> out(@)
```

File CCs route through `midi_cc()` exactly like live CCs — automation in your `.mid` will move whatever knobs you've mapped.

## Live coding while playing

Hold a chord. Edit the instrument body. Recompile. The notes stay sounding through the swap — the new program inherits every still-pressed key.

```akk
midi() |> poly(@, lead, 8) |> out(@)
```

Change `saw(...)` to `sqr(...)` in `lead`, recompile, and the held chord switches timbre without dropping. Notes only cut when the `midi()` call itself disappears between programs.

If you hear a click on note-off, the gate is multiplying out the instrument's ADSR release before it can finish. Add a `release:` window on the voice manager so the tail decays naturally:

```akk
midi() |> poly(@, lead, 8, release: 0.3) |> out(@)
```

The same option works on `mono` and `legato`. See [Polyphony](../reference/builtins/polyphony) for the full mechanics.

## Next steps

- [`midi`](../reference/builtins/midi) — full reference for the event source.
- [`midi_cc`](../reference/builtins/midi-cc) — CC / pitch-bend / aftertouch routing.
- [`soundfont`](../reference/builtins/soundfonts) — playing General MIDI presets from a live keyboard or `.mid` file.
- [Polyphony](../reference/builtins/polyphony) — `poly`, `mono`, `legato`, and the `release:` option.
