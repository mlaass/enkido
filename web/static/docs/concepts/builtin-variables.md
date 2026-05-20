---
title: Builtin Variables
category: concepts
order: 7
keywords: [bpm, sr, spb, tempo, sample rate, builtin variable, tempo-relative, env]
---

# Builtin Variables

A few identifiers in Akkado are not ordinary variables — they are **builtin
variables**, backed directly by the synth engine's runtime state. You read
them like any other name, and one of them (`bpm`) you can also assign.

| Name  | Access     | Default | Meaning                                      |
|-------|------------|---------|----------------------------------------------|
| `bpm` | read-write | 120     | Project tempo in beats per minute (1–999)    |
| `sr`  | read-only  | 48000   | Audio sample rate in Hz                      |
| `spb` | read-only  | 0.5     | Seconds per beat — equals `60 / bpm`         |

## Reading a builtin variable

Reading `bpm`, `sr`, or `spb` gives you a **live** value. The read tracks
the running engine, so when you move the Transport's BPM control the value
updates without recompiling:

```akk
// A delay locked to one beat — follows the Transport tempo live
osc("saw", 220)
    |> delay(@, spb, 0.5)
    |> out(@)
```

`spb` is the convenient primitive for tempo-relative timing. Use it
wherever a duration should scale with tempo:

```akk
dotted8 = spb * 0.75   // dotted-eighth note in seconds
quarter = spb          // one beat
```

`sr` is useful when a calculation needs the sample rate:

```akk
nyquist = sr / 2
```

## Setting the tempo

Assigning `bpm` sets the project tempo. The value is extracted at compile
time and applied to the Transport, so the BPM control in the UI updates to
match:

```akk
bpm = 140
```

The right-hand side must be a **compile-time constant**. Arithmetic on
constants is fine; runtime controls are not:

```akk
bpm = 60 * 2              // OK — evaluates to 120
bpm = param("t", 120)     // Error — not a constant
```

Values are clamped to the valid range 1–999. Multiple `bpm =` lines are
allowed; the last one wins.

## Rules

- `sr` and `spb` are **read-only** — assigning them is a compile error.
- `bpm` must be assigned a constant; a non-constant or non-numeric
  right-hand side is rejected.
- A builtin variable cannot be declared `const` (`const bpm = 120` is an
  error) — its read/write behavior is fixed by the language.
