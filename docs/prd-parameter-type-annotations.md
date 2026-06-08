> **Status: SHIPPED (2026-05-23; names updated 2026-06-08)** — both phases
> landed (`Stream`/`Signal` + full `Number`/`Record`/`Array`/`String`/`Function`
> coverage). **Naming update (2026-06-08):** the annotation surface now uses
> **uppercase PascalCase type names** mirroring the C++ `ValueType` enum —
> `Signal`, `Number`, `Pattern`, `Record`, `Array`, `String`, `Function`,
> `Stream`. The earlier lowercase abbreviations (`evs`/`sig`/`num`/`rec`/`arr`/
> `str`/`fn` and the long form `signal`) were **hard-removed** (pre-release, no
> external callers). Type names are NOT keywords — they lex as identifiers and
> are resolved contextually only in annotation position (after a `:`), so they
> stay usable as ordinary identifiers elsewhere. `Pattern` (strict Pattern) is
> newly annotatable. **Examples below predate the rename** — read every lowercase
> `: stream`/`: signal`/`: num`/… as its uppercase PascalCase form.
>
> _Original status:_ Phase 1 (`stream` + `signal`) landed via Commits A–E
> (599a692 → 4c2ca4c); Phase 2 (`number`/`record`/`array`/`string`/`function`)
> followed.

# PRD: Akkado Parameter Type Annotations

## Executive Summary

Akkado patterns are compiler "magic", not first-class typed values at the
user-`fn` boundary. A pattern passed as a user-`fn` parameter is silently
coerced to a voice-0 scalar signal, and the eager `E160` guard *rejects*
polyphonic patterns passed to a user fn outright
(`akkado/src/codegen_functions.cpp:546-555`). This blocks userspace (and
stdlib) functions that want to **transform** an event stream rather than
sample it — including the stdlib modifier one-liners that
[`prd-runtime-event-transforms.md`](prd-runtime-event-transforms.md) Phase
2b depends on.

This PRD introduces source-level **`param: type` annotation syntax** on
`fn` definitions. Phase 1 ships two annotation keywords: **`stream`** (a
new abstract supertype covering `Pattern` ⊆ `Stream` and `EventSource` ⊆
`Stream`) and **`signal`** (an explicit form of today's implicit
default-coerce behavior). Annotated `: stream` params **preserve the
caller's `TypedValue`** across the parameter boundary, bypassing the
eager `E160` reject; un-annotated params keep today's behavior exactly.

**Key design decisions (locked — see §11 for the interview-round
sourcing):**

- **Phased rollout.** Phase 1 = `stream` + `signal` only. Phase 2
  (`number`, `record`, `array`, `string`, `function`) is deferred to a
  follow-up PRD.
- **`Stream` is an abstract supertype, not a coercion target.**
  `Pattern` ⊆ `Stream` and `EventSource` ⊆ `Stream`. No codegen ever
  produces a `TypedValue{Stream, ...}` directly — inside the fn body,
  an `events: stream` param resolves to the caller's *actual* type
  (Pattern or EventSource), so existing field-access paths
  (`events.freq`, `as e |> osc(@, e.freq)`) work unchanged.
- **Coerce-don't-fail.** Per Akkado's live-coding philosophy, the
  default is to coerce toward the target type when a defensible
  coercion exists; only emit a hard error (new code `E184`) for the
  no-defensible-path cases. Diagnostics for lossy-but-doable coercions
  default to W-class warnings.
- **Un-annotated params are unchanged.** Existing `E160` still applies
  to un-annotated params receiving polyphonic non-sample patterns.
  Zero-regression-risk for existing code.
- **Reserved keywords.** `stream` and `signal` become reserved keywords
  on introduction (no current stdlib usage; minor breaking-change
  surface for user patches that used them as identifiers).
- **fn params only in v1.** Closure-param annotations
  (`(e: stream) -> ...`) are out of scope; closures already inline so
  type flows through naturally.
- **No body-side type checking.** The annotation is a precondition
  check at the call boundary. Inside the body, mis-uses surface via the
  existing downstream-builtin diagnostics.
- **No inference.** Un-annotated params used only in stream-typed
  positions are NOT inferred as `: stream`. Explicit annotation is
  required.

---

## 1. Problem Statement

### 1.1 What exists today

The user-`fn` parameter binding path in
`akkado/src/codegen_functions.cpp:539-568` visits each argument, captures
its `TypedValue`, then:

1. If the arg is a **polyphonic non-sample Pattern**, emit `E160` and
   reject the call.
2. Otherwise, store the arg's `buffer_index` as the param's binding —
   collapsing any structural information in the `TypedValue` to a single
   audio buffer (voice-0 for monophonic patterns).

The only exception today (`codegen_functions.cpp:707-719`) is the
**DynArray** branch, where a runtime-varying-length array argument
*does* propagate its full `TypedValue` through the symbol table
(per [`prd-pattern-event-arrays.md`](prd-pattern-event-arrays.md)). That
branch is the architectural template for the `stream`-annotated path
introduced here.

The compile-time type system from
[`prd-compiler-type-system.md`](prd-compiler-type-system.md) already
defines `ValueType` (`akkado/include/akkado/typed_value.hpp:15-27`) and
`ParamValueType` (`akkado/include/akkado/builtins.hpp:29-49`); builtins
carry `param_types` arrays and emit type-mismatch diagnostics in
`visit_call()`. **But there is no source-level annotation syntax for
user-defined fns** — only builtins consume the mechanism.

### 1.2 The gap

| Today | Needed |
|---|---|
| User-fn params silently collapse Pattern → voice-0 | A way to preserve `Pattern` / `EventSource` across the param boundary |
| Polyphonic Pattern + user fn → `E160` reject | A way to opt into accepting any stream (mono or poly Pattern, or live MIDI) |
| Builtins are the only path to receive a typed Pattern | User-defined stdlib fns can also receive typed Patterns |
| `fn transpose(events, n) = event_map(events, ...)` doesn't compile | Stdlib event-transform modifiers as one-liners over `event_map` |

The third row is the immediate forcing function: every property modifier
queued for [`prd-runtime-event-transforms.md`](prd-runtime-event-transforms.md)
Phase 2b's `akkado/stdlib/event_transforms.ak` migration needs an
`events: stream`-shaped param.

---

## 2. Goals and Non-Goals

### 2.1 Goals (Phase 1)

1. Add **`param: type`** grammar to `fn` parameter lists, with
   interaction `name: type = default` (annotation precedes default).
2. Ship two keyword type names: **`stream`** (Pattern ⊆ Stream,
   EventSource ⊆ Stream) and **`signal`** (today's implicit-coerce
   behavior, made explicit).
3. Add `ValueType::Stream` as an abstract supertype (used only in the
   annotation and the type-compat table; never materialized as a runtime
   `TypedValue`).
4. Add `ParamValueType::Stream` matching the same supertype.
5. Extend `handle_user_function_call` so `: stream`-annotated params
   preserve the caller's `TypedValue` and bypass the eager `E160`
   reject (mirroring the existing DynArray branch).
6. Allocate **error code `E184`** for genuinely incompatible arg types
   passed to an annotated param (e.g. Number passed where `: stream`
   was expected). Use poison-value recovery per the existing pattern.
7. Unblock [`prd-runtime-event-transforms.md`](prd-runtime-event-transforms.md)
   Phase 2b by including an end-to-end verification example
   (`fn transpose(events: stream, n) = event_map(events, ...)`).
8. Document the annotation surface in `web/static/docs/concepts/`.

### 2.2 Non-Goals (deferred or out of scope)

- **Closure-param annotations** (`(e: stream) -> ...`). Closures inline
  in v1; types flow through naturally without grammar work.
- **Destructure-param annotations** (`{x, y}: record`). The destructure
  path already enforces Record/Pattern at the AST level
  (`bind_destructure_fields`); annotation adds no expressiveness.
- **Rest-param annotations** (`...args: signal`). Rest collects into a
  synthetic Array; per-element annotation is Phase 2.
- **Phase 2 type set** (`number`, `record`, `array`, `string`,
  `function`). Owned by a follow-up PRD.
- **Body-side type checking.** Per-statement usage of a `: stream` param
  inside the body is not validated by this PRD; misuse surfaces via
  downstream builtin param_types diagnostics.
- **Type inference.** An un-annotated param used only in stream-typed
  positions is NOT auto-annotated.
- **Builtin `param_types` coverage sweep.** Owned by
  [`prd-compiler-type-system.md`](prd-compiler-type-system.md) Phase 4.
  This PRD reuses the existing mechanism but does not annotate
  additional builtins.
- **Cedar VM bytecode changes.** Annotations are compile-time only.

---

## 3. Target Syntax

### 3.1 Phase 1 examples

```akkado
// Bare stream annotation — preserves Pattern/EventSource through the param boundary
fn transpose(events: stream, n) =
    event_map(events, (e) -> {note: e.note + n})

// Explicit signal annotation — makes today's default-coerce behavior explicit
fn wobble(rate: signal, depth) =
    sine(rate) * depth

// Combined with a default value (annotation precedes default)
fn velocity(events: stream, v: signal = 1.0) =
    event_map(events, (e) -> {vel: e.vel * v})

// Stream accepts both subtypes uniformly
n"c4 e4 g4".transpose(7)            // Pattern argument — OK
midi("ctrl1").transpose(12)         // EventSource argument — OK
```

Annotation positions, grammar:

```ebnf
fn_param   ::= identifier (':' type_name)? ('=' default_expr)?
            | '...' identifier                       // rest, no annotation in v1
            | destructure_pattern                    // destructure, no annotation in v1
type_name  ::= 'stream' | 'signal'                   // Phase 1 keywords; Phase 2 extends
```

### 3.2 Error examples

```akkado
fn transpose(events: stream, n) = event_map(events, (e) -> {note: e.note + n})

transpose(440, 7)
// E184: parameter 'events' of fn 'transpose' expects Stream, got Number —
//       no coercion path
//   --> at call site, arg 0
```

```akkado
fn wobble(rate: signal, depth) = sine(rate) * depth

wobble(n"c4 e4 g4 c5", 0.5)         // polyphonic pattern — keeps existing E160 reject
// E160: user function parameter 'rate' cannot accept a polyphonic pattern
//       as scalar; use poly() to consume it, or pick a voice/field
//       explicitly (e.g. p.freq)
```

```akkado
fn wobble(rate: signal, depth) = sine(rate) * depth

wobble(n"c4 e4 g4", 0.5)            // monophonic pattern — silently voice-0 (unchanged)
```

---

## 4. Architecture / Technical Design

### 4.1 Type lattice

Add `Stream` as a new abstract `ValueType` and as a new
`ParamValueType`:

```cpp
// akkado/include/akkado/typed_value.hpp:15-27
enum class ValueType : std::uint8_t {
    Signal, Number, Pattern, Record, Array, String,
    Function, StateCell, EventSource, DynArray,
    Stream,           // NEW — abstract supertype (annotation surface only)
    Void
};
```

```cpp
// akkado/include/akkado/builtins.hpp:29-49
enum class ParamValueType : std::uint8_t {
    Any, Signal, Pattern, String, Function, Array, Record,
    Stream,           // NEW — accepts Pattern OR EventSource
};
```

**Abstract supertype rule:** No codegen path ever produces a
`TypedValue{type = Stream, ...}`. The `Stream` variant exists only for:

- the annotation surface (`fn f(x: stream)`),
- the `type_compatible(actual, expected)` lookup table,
- printing the annotation in diagnostics.

Inside the fn body, an `events: stream`-annotated param **resolves to
the caller's actual `TypedValue`** (Pattern or EventSource), so existing
field-access paths (`events.freq`, `as e |> osc(@, e.freq)`) compose
without changes.

**MIDI-as-Pattern parity.** `midi(...)` produces an `EventSource`
TypedValue; but a MIDI-driven payload that has already been routed
through pattern transforms also surfaces as a `Pattern` TypedValue
carrying `is_runtime_event_source = true`
(`typed_value.hpp:97-105`). The `: stream` check is a pure tag check —
both forms pass without inspecting `is_runtime_event_source`. The
field is consulted only by downstream poly()/voice-allocating
builtins, not by the param-boundary check introduced here.

### 4.2 Compatibility table (Phase 1)

| Annotation | Actual ValueType | Behavior |
|---|---|---|
| `: stream` | `Pattern` (incl. `is_runtime_event_source` MIDI-pattern) | Pass-through. Bind param symbol with full `TypedValue`. Bypass `E160`. |
| `: stream` | `EventSource` | Pass-through. Bind param symbol with full `TypedValue`. |
| `: stream` | `DynArray` | **E184** (no defensible coercion path). A `DynArray` is a runtime-varying numeric array (e.g. `notes(e)`); semantically unrelated to event streams. The implementer must add a guard *before* the catch-all so the diagnostic clearly cites DynArray. Poison. |
| `: stream` | `Signal`, `Number`, `Record`, `Array`, `String`, `Function`, `StateCell`, `Void` | **E184** (no defensible coercion path). Poison. |
| `: signal` | `Signal`, `Number`, monophonic `Pattern` | Today's voice-0 coerce. Unchanged. |
| `: signal` | polyphonic non-sample `Pattern` | **E160** (preserved from today). |
| `: signal` | `EventSource`, `Record`, `Array`, `DynArray`, `String`, `Function`, `StateCell`, `Void` | **E184** (no defensible coercion path). Poison. |
| *(un-annotated)* | *(any)* | Today's behavior. Unchanged. `E160` for polyphonic non-sample Pattern; voice-0 coerce for the rest. |

The compatibility entry for `ParamValueType::Stream` in
`akkado/include/akkado/builtins.hpp:62-72`:

```cpp
case ParamValueType::Stream:
    return actual == ValueType::Pattern || actual == ValueType::EventSource;
```

### 4.3 Codegen change point

`akkado/src/codegen_functions.cpp:539-568` (`handle_user_function_call`
param binding) is the single critical site. Sketch (real diff to be
worked out in implementation):

```cpp
TypedValue arg_tv = visit(args[i]);
param_buf = arg_tv.buffer;

ParamValueType expected = func.params[i].annotated_type;  // NEW (Any if no annotation)

if (expected == ParamValueType::Stream) {
    // Preserve the caller's TypedValue across the boundary; bypass E160.
    if (arg_tv.type != ValueType::Pattern &&
        arg_tv.type != ValueType::EventSource) {
        error("E184",
              "parameter '" + func.params[i].name +
              "' of fn '" + func.name +
              "' expects Stream, got " + value_type_name(arg_tv.type) +
              " — no coercion path",
              ast_->arena[args[i]].location);
        arg_tv = TypedValue::poison(ValueType::Stream);
    }
    // Bind via the DynArray-branch template (lines 707-719): preserve
    // the full TypedValue on the symbol.

} else if (expected == ParamValueType::Signal) {
    // Today's behavior. The existing E160 check applies for polyphonic
    // non-sample patterns; mono Pattern silently voice-0; Signal/Number
    // pass through.
    // (Additional E184 for genuinely incompatible types — Record,
    // Array, EventSource, etc.)
    if (arg_tv.type == ValueType::Pattern && arg_tv.pattern &&
        arg_tv.pattern->max_voices > 1 && !arg_tv.pattern->is_sample_pattern) {
        error("E160", /* unchanged */);
    }
    // ... E184 cases ...

} else {
    // Un-annotated — exactly today's branch. No changes.
    if (arg_tv.type == ValueType::Pattern && arg_tv.pattern &&
        arg_tv.pattern->max_voices > 1 && !arg_tv.pattern->is_sample_pattern) {
        error("E160", /* unchanged */);
    }
}
```

The DynArray-style symbol-table binding (`codegen_functions.cpp:707-719`)
is the architectural template:

```cpp
// Existing DynArray branch — verbatim template for `: stream`
Symbol sym{};
sym.kind         = SymbolKind::Variable;
sym.name_hash    = fnv1a_hash(func.params[i].name);
sym.name         = func.params[i].name;
sym.buffer_index = param_bufs[i];
sym.typed_value  = type_it->second;     // ← preserves the typed payload
symbols_->define(sym);
```

For a `: stream`-annotated param, the same shape applies: the
`typed_value` field carries the caller's Pattern/EventSource payload,
keeping field-access (`events.freq`), pipe-binding (`as e |> ...`), and
nested-call type propagation working unchanged.

### 4.4 Parser & AST changes

- **Lexer** (`akkado/include/akkado/lexer.hpp`,
  `akkado/src/lexer.cpp`): `stream` and `signal` become reserved
  keywords. (No current Akkado stdlib uses these as identifiers; user
  patches that do will see a parse error and need to rename.)
- **Parameter structs** (`akkado/include/akkado/parser.hpp` —
  `ParsedParam`; `akkado/include/akkado/symbol_table.hpp` —
  `FunctionParamInfo`; `akkado/src/parser.cpp` — `parse_param_list()`):
  both `ParsedParam` and `FunctionParamInfo` gain
  `ParamValueType annotated_type = ParamValueType::Any;`. The parser
  sets it on `ParsedParam`; the analyzer copies it onto
  `FunctionParamInfo` so the codegen pass can read it via
  `func.params[i].annotated_type`. The grammar accepts
  `identifier (':' type_name)? ('=' default_expr)?` in
  `parse_param_list()`. (The AST itself stores params as Identifier
  child nodes of `FunctionDef`; the annotation lives on the parameter
  *info* structs, not on a per-node AST struct.)
- **Analyzer** (`akkado/src/analyzer.cpp`): in the loop that builds
  `FunctionParamInfo` from `ParsedParam` (analyzer.cpp:~370/494/etc.),
  copy `annotated_type` over. No body-side checking.

### 4.5 Compile-time vs runtime

Type annotations are **compile-time only**. The Cedar VM remains
untyped; no bytecode format changes. No new opcodes. No runtime type
tags.

---

## 5. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| Akkado lexer | **Modified** | `stream`, `signal` become reserved keywords |
| Akkado parser | **Modified** | New `name: type = default` token sequence in `parse_param_list()` |
| Akkado analyzer | **Modified** | Store `annotated_type` on `FunctionDef.params` |
| `handle_user_function_call` | **Modified** | New branches for `: stream` and `: signal`; un-annotated path unchanged |
| `ValueType` enum | **Modified** | Add abstract `Stream` variant |
| `ParamValueType` enum | **Modified** | Add `Stream` variant; extend `type_compatible()` |
| Symbol table | **Stays** | Existing `Symbol::typed_value` field carries the binding (DynArray branch is the template) |
| Cedar VM | **Stays** | No bytecode/opcode/instruction-format change |
| `prd-compiler-type-system.md` Phase 4 (builtin coverage) | **Stays** | Orthogonal; not touched by this PRD |
| Existing stdlib fns | **Stays** | No annotations added in Phase 1 |
| Existing user patches | **Stays** *(except identifier collision)* | Reserved keywords `stream`/`signal` may collide with user variable names; documented breaking change |
| Hot-swap path | **Stays** | `annotated_type` is a static AST field; flows through hot-swap normally |
| [`prd-runtime-event-transforms.md`](prd-runtime-event-transforms.md) Phase 2b | **Unblocked** | This PRD's deliverable lets the deferred stdlib migration proceed |

---

## 6. File-Level Changes

| File | Change |
|---|---|
| `akkado/include/akkado/lexer.hpp` | Add `TokenType::Stream`, `TokenType::Signal` to the `TokenType` enum (existing convention; see `TokenType::True`/`Match`/etc.) |
| `akkado/src/lexer.cpp` | Add `{"stream", TokenType::Stream}` and `{"signal", TokenType::Signal}` to the `keywords` lookup table (lexer.cpp:11-20) |
| `akkado/include/akkado/parser.hpp` | `ParsedParam::annotated_type: ParamValueType` (parser-side intermediate, parser.hpp:29-40) |
| `akkado/include/akkado/symbol_table.hpp` | `FunctionParamInfo::annotated_type: ParamValueType` (analyzer/codegen-side resolved info, symbol_table.hpp:17-29) |
| `akkado/include/akkado/typed_value.hpp` | Add `ValueType::Stream`; update `value_type_name()` |
| `akkado/include/akkado/builtins.hpp` | Add `ParamValueType::Stream`; update `param_value_type_name()`; extend `type_compatible()` |
| `akkado/src/parser.cpp` | Parse `name: type = default` in `parse_param_list()`; reject annotation on destructure/rest with `E104` |
| `akkado/src/analyzer.cpp` | Propagate `annotated_type` into `FunctionDef.params` |
| `akkado/src/codegen_functions.cpp` | `Stream`/`Signal` branches in `handle_user_function_call` (line 539-568) — preserve TypedValue for `: stream`, emit `E184` for incompatible types, keep `E160` for un-annotated and `: signal` polyphonic-pattern |
| `akkado/tests/test_parser.cpp` | Parse-level tests for the new grammar (see §10.1) |
| `akkado/tests/test_fn_annotations.cpp` | Codegen tests for stream/signal binding (see §10.2) |
| `web/static/docs/concepts/parameter-type-annotations.md` | NEW concept doc — annotation surface, lattice, error codes, examples |
| `web/src/lib/docs/lookup-index.ts` | Rebuilt via `bun run build:docs` after the concept doc lands |
| `web/src/lib/components/Editor/` | (Optional, polish) syntax-highlight `: stream` / `: signal` in param position |

No changes required in:

- `cedar/` (VM is untyped; no opcode/format work).
- `akkado/stdlib/` (no existing stdlib fn is annotated in Phase 1; the
  event-transforms stdlib migration is owned by that PRD).
- `experiments/` (compile-time mechanism; no DSP-level testing).

---

## 7. Error Codes

| Code | Site | Meaning |
|---|---|---|
| `E104` (existing) | `parse_param_list()` | Annotation not allowed on destructure or rest param (Phase 1 restriction) |
| `E160` (existing, **unchanged**) | `handle_user_function_call` un-annotated and `: signal` paths | User function parameter cannot accept a polyphonic non-sample pattern as scalar |
| `E184` (**NEW**) | `handle_user_function_call` annotated paths | Argument type incompatible with parameter annotation — no defensible coercion path |
| `E185` (**NEW**) | `parse_param_list()` | Unknown type name in annotation (e.g. `: bogustype`). Distinct from `E184` so IDE/autocomplete can offer a "did you mean stream/signal?" suggestion. |

**`E184` recovery:** emit the diagnostic at the arg's source location,
substitute a poison `TypedValue` (`type = expected, error = true`), and
let downstream codegen short-circuit per the existing poison-value
pattern. No cascading errors.

---

## 8. Edge Cases

### 8.1 `: stream` with `= default`

```akkado
fn transpose(events: stream = ???, n) = ...
```

No literal stream-shaped expression exists in Akkado today
(`n"..."` parses to a Pattern but only at module scope; you can't
embed a `n"..."` literal as a function default expression that
evaluates lazily per call). For Phase 1: parser accepts the grammar
(annotation precedes default) but the `ConstEvaluator` will reject any
non-trivial default with the existing `E105`. No special diagnostic
needed; `E105` is sufficient.

### 8.2 `: stream` on a destructure or rest param

Disallowed in Phase 1. Parser emits `E104` at the annotation token.
Example:

```akkado
fn f({x, y}: stream) = ...    // E104: annotation not allowed on destructure
fn f(...args: stream) = ...    // E104: annotation not allowed on rest param
```

### 8.3 `stream` / `signal` as an identifier in existing code

These become reserved keywords. Any user patch using `stream` or
`signal` as a variable, parameter, or function name will get a parse
error and need to rename. None of the in-tree stdlib uses these names;
the migration note in §6 covers the user-facing breakage.

### 8.4 `: stream`-annotated param used in a Signal-expecting builtin

```akkado
fn bad(events: stream) = sine(events)    // events is a Pattern, osc expects Signal
```

No new diagnostic from this PRD. The existing `param_types` check on
`osc` will emit the builtin-mismatch error at the inner call site. Per
Round 3 of the interview, body-side validation is intentionally out of
scope.

### 8.5 Hot-swap: annotation changes between revisions

`annotated_type` is a static field on `ParsedParam` /
`FunctionParamInfo`. Hot-swap rebuilds the AST and re-runs the analyzer
from the new source; the new annotation simply applies. No special
path-hash logic.

### 8.6 Polymorphic call: same fn called with Pattern and EventSource

```akkado
fn t(e: stream, n) = event_map(e, (ev) -> {note: ev.note + n})

n"c4 e4".t(7)         // Pattern path
midi("ctrl1").t(12)   // EventSource path
```

Each call inlines the body once (existing inline-expansion model) with
the caller's `TypedValue` propagated into the body's scope. The
`event_map` builtin's `param_types` check accepts both Pattern and
EventSource (via `ParamValueType::Stream`).

### 8.7 Chord-wide event transforms

Per [`prd-runtime-event-transforms.md`](prd-runtime-event-transforms.md)
§11 OQ-1, the chord-wide form
`(e) -> {notes: map(e.notes, (v) -> v + 7)}` depends on
[`prd-pattern-event-arrays.md`](prd-pattern-event-arrays.md) for the
`DynArray` type. **This PRD does not gate that work.** A `: stream`
param already preserves the Pattern's `voice_freqs[]`; if/when
`prd-pattern-event-arrays.md` lands, chord-wide closures compose with
this PRD's `: stream` annotation without modification.

### 8.8 Nested annotation: `fn outer(events: stream) = inner(events)` where `inner` also takes `: stream`

Pass-through. Both boundary checks succeed. No special case.

### 8.9 Default value taken for an annotated param

When the caller omits a positional argument for an annotated param and
its declared default kicks in, the default-value branches at
`codegen_functions.cpp:571-644` (numeric default, string default,
expression default) emit a synthesized buffer **without** consulting
`annotated_type`. This is intentional:

- For `: signal` params, the existing numeric / expression default
  paths already produce a Signal-shaped buffer — exactly what the
  annotation requires.
- For `: stream` params, no source-level expression evaluates to a
  Pattern/EventSource at compile time (per §8.1), so a non-trivial
  default already fires `E105` before the annotation branch sees it.
  Numeric defaults like `events: stream = 0` would emit a Signal-shaped
  buffer that the annotation wants to reject — but reaching that state
  requires the user to explicitly write a number-shaped default after
  `: stream`, which the parser does accept syntactically.

**Resolution for Phase 1:** the default-value branches do not run the
new `: stream` / `: signal` type-check branches; they emit the default
buffer and bind the param verbatim. A `: stream = 0` mis-write
therefore compiles but binds the param to a Signal buffer, which any
downstream stream-shaped use will diagnose via the existing
`param_types` mechanism. If demand emerges, Phase 2 can add a
parse-time check ("annotated `: stream`/`: signal` param cannot have
a numeric default") at `parse_param_list()` — but this PRD leaves it
implicit.

---

## 9. Phasing

### Phase 1 — Stream + Signal (this PRD)

| Step | Deliverable | Tests |
|---|---|---|
| 1.1 | Lexer keywords `stream`, `signal` | `test_lexer.cpp` keyword cases |
| 1.2 | `ValueType::Stream`, `ParamValueType::Stream`, `type_compatible` extension | unit test on `type_compatible` |
| 1.3 | Grammar `name: type = default` in `parse_param_list()` | `test_parser.cpp` annotation grammar |
| 1.4 | `ParsedParam.annotated_type` → `FunctionParamInfo.annotated_type` propagation through analyzer | analyzer round-trip test |
| 1.5 | `handle_user_function_call` branching: `: stream` preserves TypedValue, bypasses `E160`; `: signal` keeps today's behavior; `E184` for incompatible types | `test_fn_annotations.cpp` (see §10.2) |
| 1.6 | E184 diagnostic + poison-value recovery | mismatch test cases |
| 1.7 | End-to-end verification example | see §10.3 |
| 1.8 | Web concept doc | `parameter-type-annotations.md` |

### Phase 2 — Full ValueType set (separate PRD)

Adds `: number`, `: record`, `: array`, `: string`, `: function`
annotations. Defines coercion rules for each, per the
coerce-don't-fail philosophy. **Not in scope here.** Owned by a
follow-up PRD authored once Phase 1 ships.

---

## 10. Testing / Verification

### 10.1 Parser tests (`akkado/tests/test_parser.cpp`)

| Case | Expectation |
|---|---|
| `fn f(events: stream) = events` | parses; `params[0].annotated_type == Stream` |
| `fn f(rate: signal, depth) = sine(rate) * depth` | parses; mixed annotated + un-annotated |
| `fn f(events: stream = ???)` | parses (annotation precedes default); compile-time default-eval will error later via existing `E105` |
| `fn f({x, y}: record) = x + y` | `E104` — annotation on destructure |
| `fn f(...args: signal) = sum(args)` | `E104` — annotation on rest |
| `fn f(events: bogustype) = events` | `E185` — unknown type name (with "did you mean stream/signal?" hint) |
| `stream = 5` (Akkado top-level assignment using the now-reserved name) | parse error — `stream` is now a reserved keyword |

### 10.2 Codegen tests (`akkado/tests/test_fn_annotations.cpp`)

| Case | Expectation |
|---|---|
| `fn id(e: stream) = e` called with `n"c4 e4 g4"` (monophonic Pattern) | no `E160`; body's `e` resolves to a Pattern TypedValue; downstream `e.freq` access works |
| `fn id(e: stream) = e` called with `n"[c4,e4,g4]"` (polyphonic Pattern) | no `E160` (the bypass); body sees the full polyphonic Pattern with `voice_freqs[]` populated |
| `fn id(e: stream) = e` called with `midi("ctrl1")` | no `E160`; body's `e` resolves to an EventSource TypedValue |
| `fn id(e: stream) = e` called with `440` (Number) | `E184` at the call site; poison `TypedValue{Stream, error=true}` propagated |
| `fn id(e: stream) = e` called with `"text"` (String) | `E184` |
| `fn id(p) = p` (un-annotated) called with `n"[c4,e4,g4]"` (polyphonic) | `E160` (unchanged) |
| `fn id(p) = p` called with `n"c4 e4"` (monophonic) | no error, voice-0 coerce (unchanged) |
| `fn w(rate: signal) = sine(rate)` called with `220` (Number) | no error, behaves like today's un-annotated default |
| `fn w(rate: signal) = sine(rate)` called with `n"[c4,e4]"` (polyphonic) | `E160` (preserved) |
| `fn w(rate: signal) = sine(rate)` called with `midi("ctrl1")` (EventSource) | `E184` — no coercion path Signal ← EventSource |

### 10.3 End-to-end verification examples

**Happy path (monophonic Pattern):**

```akkado
fn transpose(events: stream, n) =
    event_map(events, (e) -> {note: e.note + n})

n"c4 e4 g4".transpose(7) |> sine(@.freq) |> out(@)
```

Acceptance:

1. Compiles with no `E160`, no `E184`, no warnings.
2. `cmake --build build && ./build/akkado/tests/akkado_tests` passes.
3. Renders via `nkido render` and the resulting WAV contains three
   notes at the transposed frequencies (c4+7 = g4, e4+7 = b4, g4+7 = d5)
   — a quick scope check.
4. Manual paste in `bun run dev` web IDE: green compile, audible
   transposed playback.

**Counter-example (polyphonic Pattern — the E160 bypass):**

```akkado
fn transpose(events: stream, n) =
    event_map(events, (e) -> {note: e.note + n})

n"[c4,e4,g4]".transpose(7) |> poly(@, (f, g, v) -> sine(f) * adsr(g, 0.01, 0.1, 0.5, 0.2) * v, 3) |> out(@)
```

Acceptance:

1. Compiles with no `E160` (this is the headline behavior — without
   the `: stream` annotation today's compiler rejects the call).
2. The `transpose` body sees a Pattern `TypedValue` with
   `max_voices = 3` and `voice_freqs[0..2]` populated; `event_map`
   transforms each note in the chord.
3. Rendering produces a transposed major chord (G4 / B4 / D5) playing
   simultaneously per onset.

This pair validates the integration end-to-end (happy path + the
specific case the un-annotated path rejects), without performing the
[`prd-runtime-event-transforms.md`](prd-runtime-event-transforms.md)
Phase 2b stdlib migration itself.

### 10.4 Build / run commands

```bash
# Configure + build
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target akkado_tests

# Run new tests
./build/akkado/tests/akkado_tests "[parser]"           # parser grammar
./build/akkado/tests/akkado_tests "[codegen]"          # codegen binding
./build/akkado/tests/akkado_tests "[type-annotation]"  # new tag for §10.2 cases

# End-to-end manual check
cd web && bun run dev
# Paste §10.3 example, listen.
```

---

## 11. Resolved Design Decisions

All decisions below were resolved across five interview rounds during
prd-create on 2026-05-23. They are final and locked; implementation
follows directly.

**R1-Q1. Phase 1 type set. → RESOLVED: `stream` + `signal`.**
Phased rollout. `stream` ships first to unblock event-transforms;
`signal` makes today's implicit-coerce explicit. Phase 2 covers
`number`/`record`/`array`/`string`/`function`.

**R1-Q2. Relationship to `prd-compiler-type-system.md`. → RESOLVED: syntax + new ValueType.**
This PRD adds user-facing `param: type` syntax and extends `ValueType`
with `Stream`. It does NOT sweep the builtin `param_types` coverage gap
(that's Phase 4 of the type-system PRD, orthogonal).

**R1-Q3. Mismatch behavior. → RESOLVED: hard error + poison.**
Genuinely incompatible types emit `E184` and substitute a poisoned
TypedValue. Lossy-but-coercible cases (mono Pattern → Signal) coerce
silently, per the live-coding philosophy.

**R1-Q4. Closure-param annotations. → RESOLVED: fn params only in v1.**
`(e: stream) -> ...` is out of scope. Closures inline; types flow
through naturally.

**R2-Q1. Type lattice. → RESOLVED: Stream is an abstract supertype.**
`Pattern` ⊆ `Stream`, `EventSource` ⊆ `Stream`. No codegen materializes
`TypedValue{Stream}`; inside a body, the param resolves to the caller's
actual subtype. Future `: pattern` (Phase 2) would be a narrower
annotation.

**R2-Q2. Grammar interactions. → RESOLVED: `name: type = default` only.**
Phase 1 supports `name: type = default` (annotation precedes default).
Destructure (`{x,y}: type`) and rest (`...args: type`) are rejected
with `E104` in Phase 1.

**R2-Q3. Error-code allocation. → RESOLVED: new E184 + narrow E160.**
`E184` is new and covers annotated-param type mismatch. `E160` stays
exactly as today (un-annotated polyphonic-pattern guard, also fires
for `: signal` polyphonic-pattern).

**R2-Q4. Un-annotated param behavior. → RESOLVED: zero change.**
Un-annotated params keep today's coerce + `E160` behavior bit-for-bit.
The annotation is strictly opt-in; no W-class warning suggesting
annotation in v1.

**R3-Q1. In-body type for `: stream` params. → RESOLVED: caller's actual type.**
`events` inside the body resolves to whatever `TypedValue` the caller
passed (Pattern or EventSource). Not an abstract Stream value. Existing
field-access paths work unchanged.

**R3-Q2. Body-side validation. → RESOLVED: no body-side check.**
Only the call-boundary precondition is checked. Body-side misuse
surfaces via the existing downstream builtin `param_types` mechanism.

**R3-Q3. Inference. → RESOLVED: no inference in Phase 1.**
Un-annotated params used only in stream-typed positions are NOT
auto-annotated. Explicit `: stream` is required. Inference is filed as
a non-blocking future PRD if demand emerges.

**R3-Q4. Phasing. → RESOLVED: Phase 1 = stream + signal; Phase 2 = rest.**
See §9.

**R4-Q1. Reserved keyword vs context-only. → RESOLVED: reserved keywords, hard cutover.**
`stream` and `signal` become reserved everywhere the moment Phase 1
lands. No deprecation period. Breakage surface checked: no current
stdlib usage; user-patch breakage surfaces as a parse error.

**R4-Q2. Coordination with event-transforms Phase 2b. → RESOLVED: independent + verification example.**
This PRD ships standalone; the verification example (§10.3) proves the
unblock without performing the stdlib migration itself. The actual
`akkado/stdlib/event_transforms.ak` migration remains owned by
[`prd-runtime-event-transforms.md`](prd-runtime-event-transforms.md).

**R4-Q3. Phase 1 test bar. → RESOLVED: parser + codegen + E184 + e2e.**
All four bullets adopted. See §10.

**R5-Q1. `: signal` + Pattern. → RESOLVED: mono coerce; poly keeps E160.**
Monophonic Pattern → voice-0 silently (today's behavior). Polyphonic
non-sample Pattern → `E160` (today's reject preserved). The `: signal`
annotation is essentially documentation in Phase 1 — same runtime
behavior as un-annotated, but explicit at the call site.

**R5-Q2. `: stream` + Signal/Number. → RESOLVED: reject.**
No defensible coercion path. Signals are continuous samples; wrapping
them as one-event-per-block streams is contrived. Number → constant
stream is similarly contrived. Both emit `E184`. Per the
coerce-don't-fail philosophy, "coerce when defensible" does NOT mean
"force-coerce regardless".

**R5-Q3. E184 scope. → RESOLVED: only for genuinely incompatible types.**
E184 fires when no defensible coercion exists (String → Signal,
Number → Stream, etc.). For coercible cases (mono Pattern → Signal,
Number → Signal) the system coerces silently. The Phase 1 inventory of
E184-triggering cases is the §4.2 "no defensible coercion path" rows.

---

## 12. Next Step

Phase 1 is shipped (see header). Follow-ups:

1. **`prd-runtime-event-transforms.md` Phase 2b** is now unblocked —
   stdlib modifier one-liners (`.transpose(n)`, `.velocity(v)`, etc.)
   can be authored with `events: stream` parameters and the eager
   `E160` no longer rejects polyphonic patterns at the boundary.
2. **Phase 2** (full ValueType coverage: `number`, `record`, `array`,
   `string`, `function`) is specified in
   [`prd-parameter-type-annotations-phase-2.md`](prd-parameter-type-annotations-phase-2.md)
   and is READY FOR IMPLEMENTATION.
