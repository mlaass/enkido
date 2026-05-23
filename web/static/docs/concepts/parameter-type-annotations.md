---
title: Parameter Type Annotations
category: concepts
order: 11
keywords: [annotation, type, stream, signal, fn, parameter, event_map, pattern, EventSource, E160, E184, E185, E104, transpose, event-transform]
---

# Parameter Type Annotations

User `fn` parameters can carry an optional type annotation: `name: type`. Today the language ships two annotation keywords — **`stream`** and **`signal`** — and one new error code, **`E184`**, that fires when an argument's type can't reach the annotation.

The annotation is opt-in. **Un-annotated parameters keep today's behavior exactly** — there is no auto-inference, no W-class nudge, and no migration burden. You only add `: stream` / `: signal` when you want something the un-annotated path can't give you.

## When to reach for an annotation

Two practical reasons to annotate:

1. **You want to receive an event stream and transform it.** Without `: stream`, a polyphonic pattern (`c"Am"`, `n"…"` with chord stacks) passed to your `fn` is rejected with `E160` — the un-annotated path coerces patterns to voice-0 scalars and refuses to silently drop chord voices. With `: stream`, the parameter preserves the caller's full `TypedValue`, so you can pipe it straight into `event_map` / `event_filter` / `poly`.
2. **You want explicit documentation at the call site.** `: signal` makes today's implicit "Number / mono Pattern → voice-0 buffer" coerce explicit. Behavior is identical to un-annotated for the accepted cases, but `: signal` additionally rejects fundamentally incompatible types (`String`, `Record`, `Array`, …) with `E184`.

## Syntax

The annotation goes between the parameter name and its optional default:

```
fn_param   ::= identifier (':' type_name)? ('=' default_expr)?
            | '...' identifier                       // rest — no annotation
            | destructure_pattern                    // {x,y} — no annotation
type_name  ::= 'stream' | 'signal'
```

Examples:

```akkado
// Bare stream annotation — preserves Pattern / EventSource through the boundary
fn transpose(events: stream, n) ->
    event_map(events, (e) -> {note: e.note + n})

// Explicit signal annotation — makes today's voice-0 coerce explicit
fn wobble(rate: signal, depth) ->
    osc("sin", rate) * depth

// Annotation precedes default value
fn velocity(events: stream, v: signal = 1.0) ->
    event_map(events, (e) -> {vel: e.vel * v})
```

`stream` and `signal` are **reserved keywords** as of this release. If you had a variable or fn named `stream` / `signal`, you'll need to rename it.

## The `stream` type

`stream` is an **abstract supertype**. Two concrete types inhabit it:

- `Pattern` — anything that came out of `n"…"`, `c"…"`, `s"…"`, `seq(...)`, or a pattern transform.
- `EventSource` — anything that came out of `midi(...)` in its raw, non-pattern-routed form.

You never write `: stream` *expecting* the body to see a Stream value. Inside the `fn`, `events` resolves to whatever the caller actually passed: a Pattern stays a Pattern (and `events.freq` / `events.note` work), an EventSource stays an EventSource (and `poly(events, ...)` works). The annotation is a precondition check at the call boundary, not a runtime tag.

## The `signal` type

`signal` annotation accepts:

- `Signal` — audio-rate buffers (the output of `osc(...)`, `lp(...)`, etc.).
- `Number` — compile-time constants (`220`, `0.5`).
- monophonic `Pattern` — the per-event freq stream is read as a voice-0 buffer (today's silent coerce).

Polyphonic patterns (`max_voices > 1`, the chord-pattern form) still fire **`E160`** — the user should wrap with `poly()` or pick a voice/field explicitly.

## Compatibility table

| Annotation | Argument type | Behavior |
|---|---|---|
| `: stream` | `Pattern` (incl. MIDI-pattern via `is_runtime_event_source`) | Pass-through. Bind the param with the full `TypedValue`. Bypass `E160`. |
| `: stream` | `EventSource` | Pass-through. |
| `: stream` | `DynArray` | **`E184`** — a `DynArray` is a runtime-varying *numeric* array (e.g. `notes(e)`), semantically unrelated to event streams. |
| `: stream` | `Signal`, `Number`, `Record`, `Array`, `String`, `Function`, `StateCell`, `Void` | **`E184`** — no defensible coercion path. |
| `: signal` | `Signal`, `Number`, monophonic `Pattern` | Today's voice-0 coerce, unchanged. |
| `: signal` | polyphonic non-sample `Pattern` | **`E160`** (preserved). |
| `: signal` | `EventSource`, `Record`, `Array`, `DynArray`, `String`, `Function`, `StateCell`, `Void` | **`E184`** — no defensible coercion path. |
| *(un-annotated)* | *(any)* | Today's behavior, bit-for-bit. `E160` for polyphonic non-sample `Pattern`; voice-0 coerce otherwise. |

## Error codes

| Code | Site | Meaning |
|---|---|---|
| `E104` | parser | Annotation not allowed on a destructure (`{x,y}: stream`) or rest (`...args: signal`) parameter in this release. |
| `E160` | codegen (un-annotated and `: signal` paths) | Polyphonic non-sample pattern cannot be coerced to scalar — wrap with `poly()` or pick a voice. |
| `E184` | codegen (annotated paths) | Argument type incompatible with the annotation — no defensible coercion. |
| `E185` | parser | Unknown type name in annotation (e.g. `events: bogustype`). The diagnostic suggests `stream` / `signal`. |

## End-to-end example

```akkado
// User-defined transpose modifier — accepts mono notes OR a chord pattern.
fn xp(events: stream, n) ->
    event_map(events, (e) -> {note: e.note + n})

// Mono path — three transposed notes, c4+7=g4, e4+7=b4, g4+7=d5.
n"c4 e4 g4".xp(7) |> osc("sin", @.freq) |> out(@)

// Polyphonic chord path — Am transposed up a fifth.
c"Am".xp(7)
  |> poly(@, (f, g, v) -> osc("sin", f) * v, 3)
  |> out(@)
```

Without the `: stream` annotation, the second line fires `E160` (polyphonic pattern in a scalar-shaped parameter slot). With the annotation, the call passes through and the chord is transposed event-by-event before `poly` allocates voices.

## What's intentionally out of scope right now

- **Closure parameters** (`(e: stream) -> …`). Closures inline; types flow through naturally without grammar work, so the closure-arrow form doesn't accept annotations yet. Use a named `fn` if you want a typed boundary.
- **Destructure and rest parameter annotations** (`{x,y}: record`, `...args: signal`). Both fire `E104` today. Track demand in a follow-up PRD.
- **Body-side type checking.** The annotation is a precondition at the call boundary. Misuse inside the body (`fn f(e: stream) -> osc("sin", e)`) is caught downstream by the builtin's own `param_types` diagnostic, not by this mechanism.
- **Inference.** A parameter used only in stream-shaped positions is NOT auto-annotated. Explicit `: stream` is required.
- **Phase 2 type set** (`number`, `record`, `array`, `string`, `function`). A follow-up PRD will pick those up once Phase 1 has soaked.
