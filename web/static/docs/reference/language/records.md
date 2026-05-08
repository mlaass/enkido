---
title: Records
category: language
order: 5
keywords: [record, field, dot, access, destructure, spread, options, state, struct, named-fields, pattern-event]
group: language
subgroup: syntax
icon: Boxes
tagline: Group named fields, destructure, spread, and use as builtin options.
---

# Records

A record is a value that bundles named fields together. Records are immutable: each operation that "modifies" a record produces a new record. They show up in three places in akkado: as plain user values, as the events that flow through patterns, and as the option payload of certain builtins.

## Record literals

Build a record with `{field: value, ...}`:

```akk
pos = {x: 1, y: 2}
synth_cfg = {wave: "saw", cutoff: 2000, q: 0.7}
```

When the field name and the variable name match, use shorthand:

```akk
x = 1
y = 2
pos = {x, y}              // same as {x: x, y: y}
```

Field names are arbitrary identifiers; values can be numbers, strings, booleans, or other expressions evaluated at compile time. Mixing types is fine.

## Field access

Read a field with `record.field`:

```akk
pos = {x: 1, y: 2}
pos.x                     // 1
pos.y                     // 2
```

Inside a pipe, the hole `%` carries the upstream value. When the upstream is a record (e.g. a pattern event), `%.field` reads off it:

```akk
pat("c4 e4 g4") |> osc("sin", %.freq) |> out(%, %)
```

Nested access chains:

```akk
voice = {env: {a: 0.01, r: 0.5}, freq: 440}
voice.env.a               // 0.01
```

Reading an undeclared field is a compile error (`E135` / `E136`).

## Spread — record-from-record

Build a new record from an existing one with `{..base, ...overrides}`:

```akk
default = {wave: "saw", cutoff: 2000, q: 0.7}
loud    = {..default, q: 1.4}             // override q, keep the rest
shifted = {..default, cutoff: 800, q: 0.9}
```

Spread is positional: fields written after the spread override fields from the spread source. The base record is unchanged.

## Pipe binding

`as` binds the upstream value to a name so multiple downstream stages can read off it:

```akk
pat("c4 e4 g4") as e |> osc("sin", e.freq) |> % * e.vel |> out(%, %)
```

Inline destructure inside `as` lifts the named fields directly into scope:

```akk
pat("c4 e4 g4") as {freq, vel} |> osc("sin", freq) |> % * vel |> out(%, %)
```

Only fields that the upstream record actually exposes are bindable; unknown names raise `E141`.

## Statement-level destructure

Destructure a record into local bindings outside of a pipe:

```akk
config = {freq: 440, wave: "saw", cutoff: 2000}
{freq, wave, cutoff} = config

osc(wave, freq) |> lp(%, cutoff) |> out(%, %)
```

The right-hand side must be a record value (`E140`). Each declared field must exist on the source unless a default is provided (`E187`). Repeating a field name in the pattern is a parse error (`E188`).

## Function-parameter destructure

A function can destructure a record argument inline:

```akk
fn distance({x, y}) -> sqrt(x * x + y * y)
distance({x: 3, y: 4})           // 5
```

This composes with regular params:

```akk
fn lp_voice(freq, {cutoff, q}) ->
    osc("saw", freq) |> lp(%, cutoff, q)

lp_voice(440, {cutoff: 2000, q: 0.7})
```

## Defaults

Both the statement form and the parameter form accept default expressions per field. The default is evaluated only when the source record omits the field:

```akk
{x = 0, y = 0} = {x: 5}          // x = 5, y = 0

fn synth({freq = 440, wave = "saw", cutoff = 2000, q = 0.7}) ->
    osc(wave, freq) |> lp(%, cutoff, q)

synth({})                                  // all defaults
synth({freq: 220})                         // override freq, rest default
synth({freq: 220, cutoff: 800, q: 0.9})    // mix-and-match
```

A field without a default is required; missing it raises `E187`.

## Mutation: pure records are immutable

Plain record values cannot be reassigned in place:

```akk
r = {x: 1}
r.x = 5                   // E150 — value records are immutable
```

The idiomatic ways to "change" a record are spread:

```akk
r2 = {..r, x: 5}          // r2 = {x: 5}, r unchanged
```

…and, when you genuinely need persistent state, a record-valued state cell. State cells gain `cell.field` read sugar and `cell.field = expr` write sugar. See [State Cells](../builtins/state.md#record-valued-state-cells) for the full surface.

```akk
voice = state({freq: 440, gate: 0})
osc("sin", voice.freq) * voice.gate |> out(%, %)
voice.gate = 1            // sugar over set(voice, {..get(voice), gate: 1})
```

## Pattern events are records

Every event a pattern produces is a record. Five fixed fields ship today; six more are reserved for the polyphony / extended-event work and surface as soon as they're populated.

| Field | Aliases | Meaning |
|---|---|---|
| `freq` | `pitch`, `f`, `frequency` | Frequency in Hz |
| `vel` | `velocity`, `v` | Velocity, 0–1 |
| `trig` | `trigger`, `t` | Trigger pulse |
| `gate` | — | Gate signal (sustain) |
| `type` | — | Event-type tag |
| `note` | `midi`, `n` | MIDI note number |
| `dur` | — | Duration in beats |
| `chance` | — | Probability multiplier |
| `time` | — | Local time within the cycle |
| `phase` | `p` | Cycle phase (0–1) |
| `sample_id` | `sample`, `s` | Sampler bank index |

Custom fields attached via `.set("name", value)` chains live alongside these and are visible in autocomplete:

```akk
beat = pat("c4 e4").set("cutoff", saw(0.5)).set("res", 0.7)
beat |> lp(osc("sin", %.freq), %.cutoff, %.res) |> out(%, %)
```

A custom field that collides with one of the fixed names is silently shadowed by the fixed slot — `pat("c4").set("freq", x)` will not change what `%.freq` reads. The editor's autocomplete deduplicates by name so the suggestions never mislead.

## Records as builtin options

When a builtin needs more than three or four parameters, the convention is to take a record literal in the last positional slot. Each option is a named field with a declared type and default. This is how the visualizers work today; the same convention is recommended for samplers, filters, and delays.

```akk
osc("saw", 220) |> waterfall(%, "scope", {fft: 1024, gradient: "viridis"}) |> out(%, %)
```

See [Records as Builtin Options](../../concepts/record-as-options.md) for the full convention, the editor's autocomplete behaviour, and guidance for builtin authors.

## Diagnostics

| Code | When |
|---|---|
| `E122` | `state()` / `set()` initial value is not a number, signal, or record |
| `E135` | Field accessed on a non-record value |
| `E136` | Unknown field on a record (or state cell) |
| `E140` | Destructure source is not a record |
| `E141` | Pipe-binding destructure references a field the upstream record does not expose |
| `E150` | Assignment to an immutable binding — including `r.x = v` on a value record (hint: declare with `state({...})` to allow mutation) |
| `E187` | Statement / fn-param destructure source is missing a required field (no default declared) |
| `E188` | Duplicate field name inside a destructure pattern |
| `E189` | `set()` called on a record-valued state cell with a record whose field set differs from the cell's declared shape |
| `E204` | Nested-field write on a state cell — `cell.outer.inner = v` is deferred; rewrite with explicit `set` + nested spread |
| `E205` | Field assignment used in pipe (expression) position — hoist the `cell.field = expr` to a top-level statement |
| `W160` | (reserved) Caller passed an option-record field not declared on the builtin's schema |

## Related

- [State Cells](../builtins/state.md) — record-valued cells with `cell.field` sugar.
- [Records as Builtin Options](../../concepts/record-as-options.md) — convention for builtin authors.
- [Pipes & Holes](pipes.md) — `|>` and `%` semantics.
- [Mini-notation reference](../mini-notation/) — how patterns produce events.
