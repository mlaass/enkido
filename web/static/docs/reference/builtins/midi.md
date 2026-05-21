---
title: MIDI Input
category: builtins
order: 14
keywords: [midi, input, file, live, device, smf, .mid, note, velocity, channel, gate, freq, trig, loop, tempo, keyboard, controller, mpk, soundfont, poly, mono, legato, held-note, hotswap]
group: sequencing
subgroup: state-io
icon: Music
tagline: Live MIDI device or .mid file as an event source.
---

# MIDI Input

`midi()` exposes runtime MIDI events the same way typed pattern literals (`n"…"`/`v"…"`/`s"…"`/`c"…"`) expose baked pattern events. Drop it into the same pipe stages — `midi() |> poly(@, synth, 8) |> out(@)` for polyphony, or `midi() as e |> osc("saw", e.freq) ...` for monophonic field access. The source is either a live device (USB keyboard, virtual port) or a `.mid` file.

## midi

**MIDI event source** — Returns a runtime event stream that downstream `poly` / `mono` / `soundfont` / `as e` consumers read.

| Field | Type | Default | Description |
|---|---|---|---|
| `device` | string | `""` | Live device name (substring match against host port list). Empty = host default device. Mutually exclusive with `file:`. |
| `file` | string | `""` | `.mid` file path or URI. Mutually exclusive with `device:`. |
| `channel` | number | `0` | Channel filter, 1-16. `0` = accept all channels. |
| `loop` | bool | `false` | Loop file playback at end-of-track. Ignored for live devices. |
| `tempo` | enum | `"follow"` | `"follow"` plays the file at the engine's master clock; `"file"` honors the SMF's tempo meta events. File mode only. |

Bare `midi()` opens the default live device. All options are inside a single record argument: `midi({device: "MPK", channel: 1})`.

### Examples

Polyphonic synth from any plugged-in keyboard:

```akk
fn lead({freq, gate, vel}) ->
    saw(freq) * adsr(gate, 0.01, 0.2, 0.7, 0.3) * vel
        |> lp(@, 2000, 0.7)

midi() |> poly(@, lead, 8) |> out(@)
```

Named device (substring match — `"MPK"` matches `"Akai MPK Mini Play"`):

```akk
midi({device: "MPK"}) |> poly(@, lead, 8) |> out(@)
```

Channel-filtered (drum pad on channel 10):

```akk
midi({channel: 10}) |> soundfont(@, "gm", 128) |> out(@)
```

Loop a `.mid` file at the master tempo:

```akk
midi({file: "twinkle.mid", loop: true})
    |> soundfont(@, "gm", 0)
    |> out(@)
```

File at its own embedded tempo:

```akk
midi({file: "swing-groove.mid", tempo: "file"})
    |> poly(@, piano, 16)
    |> out(@)
```

### Monophonic field access (`midi() as e`)

`midi()` synthesises four per-block buffers for `as e` field access:

| Field | Meaning |
|---|---|
| `e.freq` | Hz of the latest held note (last-note-wins). |
| `e.gate` | Sustain signal, held high while any key is down. Drops to 0 for one sample on every new note-on so envelopes retrigger. |
| `e.vel`  | Velocity (0-1) of the latest note-on. |
| `e.trig` | One-sample pulse at every note-on. |

Voice policy is **last-note-wins with held-stack fallback**: pressing a new key takes over; releasing the latest key falls back to the most recent still-held key. Matches every analog mono synth.

```akk
// Mono lead, smoothed velocity, ADSR retriggers per note
fn lead(freq, gate, vel, trig) -> {
    v = interp(vel, 0.002)
    saw(glide(freq, 0.05)) * adsr(gate, 0.01, 0.15, 0.75, 0.35) * v
}

midi() |> lead(@freq, @gate, @vel, @trig)
    |> lp(@, 1800, 0.7)
    |> out(@)
```

See `static/patches/midi-cc-filtermono.akk` for a fuller worked patch with CC routing and pitch-bend.

Only `freq`, `gate`, `vel`, and `trig` are synthesised. Accessing pattern-only fields like `e.note`, `e.dur`, or `e.n` raises compile error `E136`.

### Polyphonic field access via `poly` / `soundfont`

`poly`, `mono`, `legato`, and `soundfont` read the full polyphonic note list directly off the MIDI state — they ignore the mono buffers and call the instrument function per voice:

```akk
chord_pad = ({freq, gate, vel}) ->
    osc("saw", freq) * adsr(gate, 0.05, 0.3, 0.7, 0.4) * vel
        |> lp(@, 1600)

midi() |> poly(@, chord_pad, 12) |> out(@)
```

Soundfonts work the same way:

```akk
midi() |> soundfont(@, "gm", 0) |> out(@)   // grand piano from your keyboard
```

### Held-note migration across hot-swap

Held keys survive recompiles. When you edit the instrument body and re-run, the new program inherits every still-pressed note and re-emits note-ons into the new voice allocator — chords stay sounding through code edits. The note only cuts if the `midi()` call disappears between programs. No user action required.

### Where the MIDI comes from

| Host | Source selection |
|------|-----------------|
| Web | Files panel → MIDI tab. Permission prompt on first use; requires a secure context (HTTPS or `localhost`). Drag a `.mid` onto the panel to register it for `midi({file: "..."})`. |
| CLI (`nkido-cli`) | `--list-midi-devices` enumerates ports; `--midi-in "Name"` (substring) selects one. Defaults to the first available port. |
| Godot | Extension wires `ctx.midi_event_callback` from the chosen Godot MIDI bus. |

When no MIDI is available (permission denied, device unplugged, file not yet uploaded), `midi()` silently produces no events. The pipe stays valid; downstream voices stay idle.

### `.mid` file URIs

`.mid` files use the same URI schemes as samples and soundfonts — there is no dedicated `midi://` scheme. Bare paths and `file://` resolve via `cedar::UriResolver`; on the web, drag-drop registers a transient `blob:nkido:...` handle the bare filename then looks up. See [URI Schemes](../uri-schemes) for the full table.

```akk
midi({file: "drums.mid"})                          // bundled / web-registered
midi({file: "file:///home/me/songs/intro.mid"})    // absolute native path
midi({file: "https://example.com/groove.mid"})     // remote, cached after first fetch
```

### Multiple midi() calls

Every `midi()` is an independent source with its own state and routing — two calls do not share data, even if both point at the same device. Use this to fan one keyboard into two voice managers:

```akk
midi() |> poly(@, lead, 8) +
midi() |> soundfont(@, "gm", 48) * 0.5
    |> out(@)
```

### `release:` for click-free note-off

Live MIDI exposes the gate-multiplied click on note-off that patterns mask with cycle-aligned timing. Add a `release:` window on the voice manager so the instrument's ADSR finishes its release stage:

```akk
midi() |> poly(@, lead, 8, release: 0.3) |> out(@)
```

See [Polyphony](polyphony) for details on `release:`.

### See also

- [`midi_cc`](midi-cc) — route incoming CC / pitch-bend / aftertouch to a `param()`.
- [`poly`](polyphony#poly), [`mono`](polyphony#mono), [`legato`](polyphony#legato) — voice managers that consume `midi()`.
- [`soundfont`](soundfonts) — GM/SF2 playback driven by `midi()` upstream.
- [Audio Input](audio-input) — sibling builtin for live audio.
- [URI Schemes](../uri-schemes) — resolving `.mid` file paths.
