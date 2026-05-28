---
title: Parameter Type Annotations
category: concepts
order: 11
keywords: [annotation, type, evs, sig, signal, num, rec, arr, str, fn, parameter, event_map, pattern, E160, E184, E185, E104, transpose, event-transform]
---

# Parameter Type Annotations

User `fn` parameters can carry an optional type annotation: `name: type`. Today the language ships seven annotation keywords — **`evs`**, **`sig`** (alias of `signal`), **`num`**, **`rec`**, **`arr`**, **`str`**, and **`fn`** — and one new error code, **`E184`**, that fires when an argument's type can't reach the annotation.

The annotation is opt-in. **Un-annotated parameters keep today's behavior exactly** — there is no auto-inference, no W-class nudge, and no migration burden. You only add an annotation when you want something the un-annotated path can't give you.

## When to reach for an annotation

Three practical reasons to annotate:

1. **You want to receive an event stream and transform it.** Without `: evs`, a polyphonic pattern (`c"Am"`, `n"…"` with chord stacks) passed to your `fn` is rejected with `E160` — the un-annotated path coerces patterns to voice-0 scalars and refuses to silently drop chord voices. With `: evs`, the parameter preserves the caller's full `TypedValue`, so you can pipe it straight into `event_map` / `event_filter` / `poly`.
2. **You want explicit documentation at the call site.** `: sig` makes today's implicit "Number / mono Pattern → voice-0 buffer" coerce explicit. Behavior is identical to un-annotated for the accepted cases, but `: sig` additionally rejects fundamentally incompatible types (`String`, `Record`, `Array`, …) with `E184`.
3. **You want a strict precondition.** The Phase 2 keywords `: num`, `: rec`, `: arr`, `: str`, `: fn` are tight type contracts — they accept only their exact `ValueType` (with `: rec` also accepting `Pattern`, since a pattern is structurally a record) and otherwise emit `E184`. Useful for fn parameters that specialise the body's behaviour at the call boundary: voice counts, dispatch keys, callback functions, structured event records, etc.

## Syntax

The annotation goes between the parameter name and its optional default:

```
fn_param   ::= identifier (':' type_name)? ('=' default_expr)?
            | '...' identifier                       // rest — no annotation
            | destructure_pattern                    // {x,y} — no annotation
type_name  ::= 'evs' | 'sig' | 'signal'
            | 'num' | 'rec' | 'arr' | 'str' | 'fn'
```

Examples:

```akkado
// Bare evs annotation — preserves Pattern through the boundary (incl. runtime event streams)
fn transpose(events: evs, n) ->
    event_map(events, (e) -> {note: e.note + n})

// Explicit sig annotation — makes today's voice-0 coerce explicit
fn wobble(rate: sig, depth) ->
    sine(rate) * depth

// num — strict compile-time numeric constant
fn unison(freq: sig, voices: num) =
    each(range(voices), (i) -> saw(freq * (1 + i * 0.01)))

// rec — Record or Pattern (Pattern is structurally a record)
fn arpinst(e: rec) =
    saw(e.freq) * adsr(e.gate, 0.01, 0.1, 0.5, 0.2) * e.vel

// arr — compile-time Array literal
fn mixer(channels: arr, gain: sig) = sum(channels) * gain

// str — compile-time String
fn osctype(kind: str, freq: sig) = osc(kind, freq)

// fn — Function reference (callbacks, higher-order fns)
fn each_voice(voices: arr, cb: fn) = each(voices, cb)

// Annotation precedes default value
fn velocity(events: evs, v: sig = 1.0) ->
    event_map(events, (e) -> {vel: e.vel * v})
```

**Reserved keywords:** `evs`, `sig`, `signal`, `num`, `rec`, `arr`, `str`, `fn` are reserved. If you had a variable or fn named after any of them, you'll need to rename it.

### Migration from `: stream`

Earlier development snapshots used `: stream` instead of `: evs`. The keyword has been renamed — `: stream` no longer parses. Existing `: stream` annotations should be rewritten as `: evs`. The semantics are unchanged.

`: signal` keeps working as a long-form alias of `: sig` — both forms are accepted.

## The `evs` type

`evs` is the **event-stream** annotation. It accepts `Pattern` values — including those produced by `n"…"`, `c"…"`, `s"…"`, `seq(...)`, pattern transforms, and runtime event sources like `midi(...)` (which ride on `Pattern` with the `is_runtime_event_source` flag set).

You never write `: evs` *expecting* the body to see a synthesised "Stream" value. Inside the `fn`, `events` resolves to the Pattern the caller actually passed — `events.freq` / `events.note` work, and `poly(events, ...)` works for runtime event sources. The annotation is a precondition check at the call boundary, not a runtime tag.

## The `sig` / `signal` type

`sig` (and its long-form alias `signal`) accepts:

- `Signal` — audio-rate buffers (the output of `osc(...)`, `lp(...)`, etc.).
- `Number` — compile-time constants (`220`, `0.5`).
- monophonic `Pattern` — the per-event freq stream is read as a voice-0 buffer (today's silent coerce).

Polyphonic patterns (`max_voices > 1`, the chord-pattern form) still fire **`E160`** — the user should wrap with `poly()` or pick a voice/field explicitly.

## The `num` type

`num` is strict — it accepts only compile-time `Number` constants. `Signal`, `Pattern`, and everything else fire `E184`. Use `: num` for parameters that need to be known at compile time: voice counts for `each(range(n), ...)`, array sizes, fixed integer indices, etc. Use `: sig` instead if a runtime audio signal is acceptable.

## The `rec` type

`rec` accepts `Record` values (built with `{field: value, …}` literals) and `Pattern` values (which are structurally records — each event carries `.freq`, `.note`, `.vel`, `.gate`, etc.). Inside the body, field access (`r.freq`) resolves cleanly in both cases.

## The `arr` type

`arr` accepts a compile-time `Array` — typically an array literal `[a, b, c]`. It does **not** accept `DynArray` (the runtime-varying array type returned by `notes(e)` / `freqs(e)`); use the un-annotated path or convert to a fixed-size literal. The `E184` diagnostic for an `arr`/`DynArray` mismatch carries an explicit hint.

## The `str` type

`str` accepts compile-time `String` literals — useful for dispatch keys (`match(kind)` on the body), file paths, mode selectors. Mini-notation literals (`n"…"`, `c"…"`, etc.) are *not* strings — they parse to `Pattern`, not `String`, so they are rejected.

## The `fn` type

`fn` accepts `Function` references — top-level user fns passed by name or closure literals like `(x) -> x * 2`. Useful for callback parameters and higher-order fns.

## Compatibility table

| Annotation | Argument type | Behavior |
|---|---|---|
| `: evs` | `Pattern` (incl. MIDI-pattern via `is_runtime_event_source`) | Pass-through. Bind the param with the full `TypedValue`. Bypass `E160`. |
| `: evs` | `DynArray` | **`E184`** — a `DynArray` is a runtime-varying *numeric* array (e.g. `notes(e)`), semantically unrelated to event streams. |
| `: evs` | `Signal`, `Number`, `Record`, `Array`, `String`, `Function`, `StateCell`, `Void` | **`E184`** — no defensible coercion path. |
| `: sig` / `: signal` | `Signal`, `Number`, monophonic `Pattern` | Today's voice-0 coerce, unchanged. |
| `: sig` / `: signal` | polyphonic non-sample `Pattern` | **`E160`** (preserved). |
| `: sig` / `: signal` | `Record`, `Array`, `DynArray`, `String`, `Function`, `StateCell`, `Void` | **`E184`** — no defensible coercion path. |
| `: num` | `Number` | Pass-through; bind buffer as a constant. |
| `: num` | anything else | **`E184`** — no coercion path. `: num` is strict. |
| `: rec` | `Record` | Field-extraction bind; `r.freq`/`r.vel` resolve from record fields. |
| `: rec` | `Pattern` | Pass-through with TypedValue preserved; field access (`r.freq`) resolves via the Pattern's per-field buffers. |
| `: rec` | anything else | **`E184`** — no coercion path. |
| `: arr` | `Array` (literal) | Multi-buffer bind; `a[i]` emits `ARRAY_INDEX`. |
| `: arr` | `DynArray` | **`E184`** — with a DynArray-specific hint pointing at the un-annotated path. |
| `: arr` | anything else | **`E184`** — no coercion path. |
| `: str` | `String` | Bound via `param_string_defaults_`; available for `match(s)`. |
| `: str` | anything else | **`E184`** — no coercion path. |
| `: fn` | `Function` (named fn ref or closure literal) | Bound via `param_function_refs_`; callable inside the body. |
| `: fn` | anything else | **`E184`** — no coercion path. |
| *(un-annotated)* | *(any)* | Today's behavior, bit-for-bit. `E160` for polyphonic non-sample `Pattern`; voice-0 coerce otherwise. |

## Error codes

| Code | Site | Meaning |
|---|---|---|
| `E104` | parser | Annotation not allowed on a destructure (`{x,y}: rec`) or rest (`...args: arr`) parameter in this release. |
| `E160` | codegen (un-annotated and `: sig` paths) | Polyphonic non-sample pattern cannot be coerced to scalar — wrap with `poly()` or pick a voice. |
| `E184` | codegen (annotated paths) | Argument type incompatible with the annotation — no defensible coercion. |
| `E185` | parser | Unknown type name in annotation (e.g. `events: bogustype`). The diagnostic suggests the valid keywords. Also fires when a reserved type-name keyword (`num`, `str`, …) leaks into expression position. |

## End-to-end example

```akkado
// User-defined transpose modifier — accepts mono notes OR a chord pattern.
fn xp(events: evs, n) ->
    event_map(events, (e) -> {note: e.note + n})

// Mono path — three transposed notes, c4+7=g4, e4+7=b4, g4+7=d5.
n"c4 e4 g4".xp(7) |> sine(@.freq) |> out(@)

// Polyphonic chord path — Am transposed up a fifth.
c"Am".xp(7)
  |> poly(@, (f, g, v) -> sine(f) * v, 3)
  |> out(@)
```

Without the `: evs` annotation, the second line fires `E160` (polyphonic pattern in a scalar-shaped parameter slot). With the annotation, the call passes through and the chord is transposed event-by-event before `poly` allocates voices.

## What's intentionally out of scope right now

- **Closure parameters** (`(e: evs) -> …`). Closures inline; types flow through naturally without grammar work, so the closure-arrow form doesn't enforce annotations yet. Use a named `fn` if you want a typed boundary.
- **Destructure and rest parameter annotations** (`{x,y}: rec`, `...args: sig`). Both fire `E104` today. Track demand in a follow-up PRD.
- **Body-side type checking.** The annotation is a precondition at the call boundary. Misuse inside the body (`fn f(e: evs) -> sine(e)`) is caught downstream by the builtin's own `param_types` diagnostic, not by this mechanism.
- **Inference.** A parameter used only in stream-shaped positions is NOT auto-annotated. Explicit `: evs` is required.
- **`: dynarr` annotation.** `DynArray` flows through un-annotated today; an explicit annotation is filed as a Phase 3 candidate.
- **Long-form aliases for the Phase 2 keywords.** `number`, `record`, `array`, `string`, `function` are NOT reserved — only the abbreviated forms work. If demand emerges, long-form aliases can be added in a follow-up PRD.
