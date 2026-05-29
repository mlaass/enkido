---
title: Audio Input
category: builtins
order: 13
keywords: [in, input, microphone, mic, tab, file, live, audio, capture, getUserMedia]
group: tools
subgroup: state-io
icon: Mic
tagline: Capture mic, tab audio, or file as a signal source.
---

# Audio Input

The `in()` builtin exposes external audio as a signal source (microphone, tab/system audio, or an uploaded file) so any Cedar effect can process it. The host populates the input buffer each block.

## in

**Live audio input** - Returns a stereo signal from the host's selected input source.

| Param  | Type   | Default | Description |
|--------|--------|---------|-------------|
| source | string | (UI default) | Optional override: `"mic"`, `"tab"`, or `"file:NAME"` |

Output: **Stereo** signal. Every audio effect is stereo-native, so `in() |> lp(@, 2000)` is a stereo lowpass with independent state per side.

### Examples

Filter the microphone:

```akk
in() |> lp(@, 2000, 0.7) |> out(@)
```

Granular delay on the live source:

```akk
in() |> delay(@, 0.25, 0.5, 0.5, 0.5) |> out(@)
```

Mix live input with a synth voice:

```akk
sig = in()
osc = saw(220) * 0.3
sig + stereo(osc) |> out(@)
```

Override the source per-program (a string literal, resolved at compile time):

```akk
in("mic")              // microphone
in("tab")              // tab / system audio (browser only)
in("file:voice.wav")   // an uploaded file, looped
```

The argument is a compile-time string. The compiler does not interpret it semantically; it forwards it to the host as metadata. The host (web UI dropdown, CLI `--input-device`, Godot binding) decides what to do with it.

### Multiple in() calls

All `in()` calls in a single program share the same input buffer. There is one stereo input source per execution context. Calling `in()` twice does not allocate a second source; it reads the same data.

### Silent fallback

When no input is available (permission denied, device unplugged, file not yet uploaded, host without input support), `in()` silently returns zeros. There is no compile error and no crash. The UI shows the current input status so you can see whether `in()` is actually live.

### Where the audio comes from

| Host | Source selection |
|------|-----------------|
| Web | Settings panel → Audio Input. Pick **Mic** (browser permission prompt), **Tab** (`getDisplayMedia`, requires "Share tab audio"), or **File** (drag/drop a WAV). |
| CLI (`nkido`) | `--list-devices` enumerates capture devices; `--input-device "Name"` selects one. Defaults to the system default. |
| Godot | Extension wires `ctx.input_left/right` from the chosen Godot bus or `AudioStreamMicrophone`. |

### Feedback warning

Mic-in → speakers-out without headphones causes feedback. Use headphones, or attenuate the wet signal until you understand the loop. Feedback-loop detection is not built-in; the loud screech is on you.

### See also

- [Stereo](stereo): `stereo()`, `mono()`, `left()`, `right()` for routing the live input.
- [Filters](filters): typical first effect for vocal / instrument processing.
- [Delays](delays): for live looping and dub effects on the mic.
- [MIDI Input](midi): sibling builtin for live MIDI devices and `.mid` file playback.
