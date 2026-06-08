---
title: Parameter Type Annotations
category: concepts
order: 11
keywords: [annotation, type, Signal, Number, Pattern, Record, Array, String, Function, Stream, parameter, event_map, E160, E184, E185, E104, transpose, event-transform]
---

# Parameter Type Annotations

User `fn` parameters can carry an optional type annotation: `name: Type`. The type names are **uppercase PascalCase**, mirroring the compiler's `ValueType` enum — **`Signal`**, **`Number`**, **`Pattern`**, **`Record`**, **`Array`**, **`String`**, **`Function`**, and **`Stream`** — plus one error code, **`E184`**, that fires when an argument's type can't reach the annotation.

The type names are **not keywords** — they lex as ordinary identifiers and are recognised only in annotation position (immediately after a `:` in a parameter list). You can still use `Signal`, `Array`, etc. as variable or function names elsewhere.

The annotation is opt-in. **Un-annotated parameters keep today's behavior exactly** — there is no auto-inference, no W-class nudge, and no migration burden. You only add an annotation when you want something the un-annotated path can't give you.

## When to reach for an annotation

Three practical reasons to annotate:

1. **You want to receive an event stream and transform it.** Without `: Stream`, a polyphonic pattern (`c"Am"`, `n"…"` with chord stacks) passed to your `fn` is rejected with `E160` — the un-annotated path coerces patterns to voice-0 scalars and refuses to silently drop chord voices. With `: Stream`, the parameter preserves the caller's full `TypedValue`, so you can pipe it straight into `event_map` / `event_filter` / `poly`.
2. **You want explicit documentation at the call site.** `: Signal` makes today's implicit "Number / mono Pattern → voice-0 buffer" coerce explicit. Behavior is identical to un-annotated for the accepted cases, but `: Signal` additionally rejects fundamentally incompatible types (`String`, `Record`, `Array`, …) with `E184`.
3. **You want a strict precondition.** `: Number`, `: Record`, `: Array`, `: String`, `: Function` are tight type contracts — they accept only their exact `ValueType` (with `: Record` also accepting `Pattern`, since a pattern is structurally a record) and otherwise emit `E184`. Useful for fn parameters that specialise the body's behaviour at the call boundary: voice counts, dispatch keys, callback functions, structured event records, etc. `: Pattern` is the strict-Pattern form (like `: Stream` but spelled for the mini-notation case).

## Syntax

The annotation goes between the parameter name and its optional default:

```
fn_param   ::= identifier (':' Type)? ('=' default_expr)?
            | '...' identifier                       // rest — no annotation
            | destructure_pattern                    // {x,y} — no annotation
Type       ::= 'Signal' | 'Number' | 'Pattern' | 'Record'
            | 'Array' | 'String' | 'Function' | 'Stream'
```

Examples:

```akkado
// Stream — preserves Pattern through the boundary (incl. runtime event streams)
fn transpose(events: Stream, n) ->
    event_map(events, (e) -> {note: e.note + n})

// Signal — makes today's voice-0 coerce explicit
fn wobble(rate: Signal, depth) ->
    sine(rate) * depth

// Number — strict compile-time numeric constant
fn unison(freq: Signal, voices: Number) =
    each(range(voices), (i) -> saw(freq * (1 + i * 0.01)))

// Record — Record or Pattern (Pattern is structurally a record)
fn arpinst(e: Record) =
    saw(e.freq) * adsr(e.gate, 0.01, 0.1, 0.5, 0.2) * e.vel

// Array — compile-time Array literal
fn mixer(channels: Array, gain: Signal) = sum(channels) * gain

// String — compile-time String
fn osctype(kind: String, freq: Signal) = osc(kind, freq)

// Function — Function reference (callbacks, higher-order fns)
fn each_voice(voices: Array, cb: Function) = each(voices, cb)

// Annotation precedes default value
fn velocity(events: Stream, v: Signal = 1.0) ->
    event_map(events, (e) -> {vel: e.vel * v})
```

**Not reserved:** the type names are ordinary identifiers — only meaningful after a `:` in a parameter list. `Signal`, `Array`, `String`, etc. remain usable as variable / fn / parameter names everywhere else. Type names are **case-sensitive**: only the uppercase PascalCase forms are recognised; `signal`, `array`, `stream`, … are not annotation types and surface `E185` in annotation position.

### Migration from earlier snapshots

Earlier development snapshots used lowercase abbreviations (`evs`, `sig`/`signal`, `num`, `rec`, `arr`, `str`, `fn`) and, before that, `: stream`. Those were **hard-removed** — only the uppercase PascalCase names parse now. Rewrite any old annotation to its uppercase form: `evs`/`stream` → `Stream`, `sig`/`signal` → `Signal`, `num` → `Number`, `rec` → `Record`, `arr` → `Array`, `str` → `String`, `fn` → `Function`. The semantics are unchanged.

## The `Stream` type

`Stream` is the **event-stream** annotation. It accepts `Pattern` values — including those produced by `n"…"`, `c"…"`, `s"…"`, `seq(...)`, pattern transforms, and runtime event sources like `midi(...)` (which ride on `Pattern` with the `is_runtime_event_source` flag set).

You never write `: Stream` *expecting* the body to see a synthesised "Stream" value. Inside the `fn`, `events` resolves to the Pattern the caller actually passed — `events.freq` / `events.note` work, and `poly(events, ...)` works for runtime event sources. The annotation is a precondition check at the call boundary, not a runtime tag.

`: Pattern` is the strict-Pattern spelling: behaviourally identical here (both accept `Pattern` only), it just reads as "this is a mini-notation pattern" rather than the abstract event-stream supertype.

## The `Signal` type

`Signal` accepts:

- `Signal` — audio-rate buffers (the output of `osc(...)`, `lp(...)`, etc.).
- `Number` — compile-time constants (`220`, `0.5`).
- monophonic `Pattern` — the per-event freq stream is read as a voice-0 buffer (today's silent coerce).

Polyphonic patterns (`max_voices > 1`, the chord-pattern form) still fire **`E160`** — the user should wrap with `poly()` or pick a voice/field explicitly.

## The `Number` type

`Number` is strict — it accepts only compile-time `Number` constants. `Signal`, `Pattern`, and everything else fire `E184`. Use `: Number` for parameters that need to be known at compile time: voice counts for `each(range(n), ...)`, array sizes, fixed integer indices, etc. Use `: Signal` instead if a runtime audio signal is acceptable.

## The `Record` type

`Record` accepts `Record` values (built with `{field: value, …}` literals) and `Pattern` values (which are structurally records — each event carries `.freq`, `.note`, `.vel`, `.gate`, etc.). Inside the body, field access (`r.freq`) resolves cleanly in both cases.

## The `Array` type

`Array` accepts a compile-time `Array` — typically an array literal `[a, b, c]`. It does **not** accept `DynArray` (the runtime-varying array type returned by `notes(e)` / `freqs(e)`); use the un-annotated path or convert to a fixed-size literal. The `E184` diagnostic for an `Array`/`DynArray` mismatch carries an explicit hint.

## The `String` type

`String` accepts compile-time `String` literals — useful for dispatch keys (`match(kind)` on the body), file paths, mode selectors. Mini-notation literals (`n"…"`, `c"…"`, etc.) are *not* strings — they parse to `Pattern`, not `String`, so they are rejected.

## The `Function` type

`Function` accepts `Function` references — top-level user fns passed by name or closure literals like `(x) -> x * 2`. Useful for callback parameters and higher-order fns.

## Compatibility table

| Annotation | Argument type | Behavior |
|---|---|---|
| `: Stream` / `: Pattern` | `Pattern` (incl. MIDI-pattern via `is_runtime_event_source`) | Pass-through. Bind the param with the full `TypedValue`. Bypass `E160`. |
| `: Stream` / `: Pattern` | `DynArray` | **`E184`** — a `DynArray` is a runtime-varying *numeric* array (e.g. `notes(e)`), semantically unrelated to event streams. |
| `: Stream` / `: Pattern` | `Signal`, `Number`, `Record`, `Array`, `String`, `Function`, `StateCell`, `Void` | **`E184`** — no defensible coercion path. |
| `: Signal` | `Signal`, `Number`, monophonic `Pattern` | Today's voice-0 coerce, unchanged. |
| `: Signal` | polyphonic non-sample `Pattern` | **`E160`** (preserved). |
| `: Signal` | `Record`, `Array`, `DynArray`, `String`, `Function`, `StateCell`, `Void` | **`E184`** — no defensible coercion path. |
| `: Number` | `Number` | Pass-through; bind buffer as a constant. |
| `: Number` | anything else | **`E184`** — no coercion path. `: Number` is strict. |
| `: Record` | `Record` | Field-extraction bind; `r.freq`/`r.vel` resolve from record fields. |
| `: Record` | `Pattern` | Pass-through with TypedValue preserved; field access (`r.freq`) resolves via the Pattern's per-field buffers. |
| `: Record` | anything else | **`E184`** — no coercion path. |
| `: Array` | `Array` (literal) | Multi-buffer bind; `a[i]` emits `ARRAY_INDEX`. |
| `: Array` | `DynArray` | **`E184`** — with a DynArray-specific hint pointing at the un-annotated path. |
| `: Array` | anything else | **`E184`** — no coercion path. |
| `: String` | `String` | Bound via `param_string_defaults_`; available for `match(s)`. |
| `: String` | anything else | **`E184`** — no coercion path. |
| `: Function` | `Function` (named fn ref or closure literal) | Bound via `param_function_refs_`; callable inside the body. |
| `: Function` | anything else | **`E184`** — no coercion path. |
| *(un-annotated)* | *(any)* | Today's behavior, bit-for-bit. `E160` for polyphonic non-sample `Pattern`; voice-0 coerce otherwise. |

## Error codes

| Code | Site | Meaning |
|---|---|---|
| `E104` | parser | Annotation not allowed on a destructure (`{x,y}: Record`) or rest (`...args: Array`) parameter in this release. |
| `E160` | codegen (un-annotated and `: Signal` paths) | Polyphonic non-sample pattern cannot be coerced to scalar — wrap with `poly()` or pick a voice. |
| `E184` | codegen (annotated paths) | Argument type incompatible with the annotation — no defensible coercion. |
| `E185` | parser | Unknown type name in annotation (e.g. `events: bogustype`, or a lowercase `events: signal`). The diagnostic lists the valid PascalCase type names. |

## End-to-end example

```akkado
// User-defined transpose modifier — accepts mono notes OR a chord pattern.
fn xp(events: Stream, n) ->
    event_map(events, (e) -> {note: e.note + n})

// Mono path — three transposed notes, c4+7=g4, e4+7=b4, g4+7=d5.
n"c4 e4 g4".xp(7) |> sine(@.freq) |> out(@)

// Polyphonic chord path — Am transposed up a fifth.
c"Am".xp(7)
  |> poly(@, (f, g, v) -> sine(f) * v, 3)
  |> out(@)
```

Without the `: Stream` annotation, the second line fires `E160` (polyphonic pattern in a scalar-shaped parameter slot). With the annotation, the call passes through and the chord is transposed event-by-event before `poly` allocates voices.

## What's intentionally out of scope right now

- **Closure parameters** (`(e: Stream) -> …`). Closures inline; types flow through naturally without grammar work, so the closure-arrow form doesn't enforce annotations yet. Use a named `fn` if you want a typed boundary.
- **Destructure and rest parameter annotations** (`{x,y}: Record`, `...args: Signal`). Both fire `E104` today. Track demand in a follow-up PRD.
- **Body-side type checking.** The annotation is a precondition at the call boundary. Misuse inside the body (`fn f(e: Stream) -> sine(e)`) is caught downstream by the builtin's own `param_types` diagnostic, not by this mechanism.
- **Inference.** A parameter used only in stream-shaped positions is NOT auto-annotated. Explicit `: Stream` is required.
