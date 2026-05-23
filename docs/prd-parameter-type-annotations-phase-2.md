> **Status: READY FOR IMPLEMENTATION.** Phase 1 of
> [`prd-parameter-type-annotations.md`](prd-parameter-type-annotations.md)
> shipped 2026-05-23 (commits `599a692`, `d579c3c`, `4e9b618`, `ecbd317`,
> `4c2ca4c`). This PRD is the deferred follow-up named in Phase 1 §9.

# PRD: Akkado Parameter Type Annotations — Phase 2 (Full ValueType Set)

## Executive Summary

Phase 1 shipped `: stream` and `: signal` parameter annotations on
user `fn` definitions. The grammar (`name: type = default`),
`ParamValueType` lookup, analyzer propagation, and
`handle_user_function_call` dispatch all landed and are exercised by
end-to-end tests. Phase 1 §9 explicitly deferred the rest of the type
set — `number`, `record`, `array`, `string`, `function` — to this
follow-up PRD.

Phase 2 adds those five annotation keywords. The mechanism is exactly
the one Phase 1 built; only the cases multiply. The bulk of
`ParamValueType` variants already exist for builtin use
(`Pattern`, `String`, `Function`, `Array`, `Record`), so the only new
variant is `ParamValueType::Number`. The dispatch in
`handle_user_function_call` gains five `else if` branches, each
mirroring the Phase 1 Stream/Signal pattern: precondition-check the
caller's `TypedValue`, fire `E184` on incompatible types, otherwise
fall through to the existing binding path with no new binding logic.

**Key design decisions (locked — see §11 for sourcing):**

- **Reserved keywords, hard cutover.** `number`, `record`, `array`,
  `string`, `function` become reserved keywords on Phase 2 landing,
  matching the Phase 1 R4-Q1 precedent. The in-tree grep confirms no
  `.ak` source file uses any of these as identifiers (see §8.5).
- **Strict `: number`.** Accepts only `ValueType::Number` (compile-time
  constant). `Signal` does not coerce. Per R5-Q2 (Phase 1): "coerce
  when defensible ≠ force-coerce regardless"; Number is the
  more-restrictive direction, the coercion is lossy.
- **Annotations mirror existing builtin precedent.** The Phase 1
  `type_compatible()` lookup table is the design's load-bearing
  reference. `: record` accepts Pattern (Pattern is structurally a
  Record); the others reject everything except their own ValueType.
- **No new error codes.** `E184` from Phase 1 covers type-mismatch
  cases; `E185` covers unknown type names. Phase 2's five new keywords
  remove five cases from `E185`'s firing surface — that's it.
- **No new W-class warnings.** Phase 2 follows Phase 1's choice of
  silent passes for defensible coercions and hard errors otherwise.
- **Closure-param annotations stay tolerated-but-unenforced.** The
  analyzer already copies `ClosureParamData::annotated_type` into
  `FunctionParamInfo` (`analyzer.cpp:377-389`); enforcement at the
  closure boundary is Phase 3 work.
- **No body-side type checking, no inference.** Both deferrals inherit
  unchanged from Phase 1 §2.2.

---

## 1. Problem Statement

### 1.1 What Phase 1 ships

The user-`fn` param-binding path
(`akkado/src/codegen_functions.cpp:539-612`) branches on the param's
`annotated_type`:

- `: stream` — preserves Pattern/EventSource `TypedValue` across the
  boundary; bypasses `E160`; fires `E184` on incompatible actuals.
- `: signal` — keeps today's voice-0 coerce for Signal/Number/mono
  Pattern; keeps `E160` for polyphonic non-sample Pattern; fires
  `E184` on EventSource/Record/Array/DynArray/String/Function/
  StateCell/Void.
- *(un-annotated)* — exactly today's behavior, bit-for-bit. Only the
  polyphonic-pattern `E160` guard fires.

The lattice has `ValueType::Stream` (abstract supertype) and
`ParamValueType::Stream`. `type_compatible()` covers all 8 existing
ParamValueType cases. The grammar is `name: type (= default)?`,
restricted in Phase 1 to `stream` and `signal` only.

### 1.2 What's missing

Five annotations specified in §9 of the Phase 1 PRD as Phase 2 work:

| Keyword     | Accepts                                  | Useful for                                        |
|-------------|------------------------------------------|---------------------------------------------------|
| `number`    | compile-time `Number` only               | param specialisation: voice count, array size, etc. |
| `record`    | `Record` or `Pattern`                    | structured field-set params (`{freq, vel}`-shaped) |
| `array`     | compile-time `Array`                     | spread/unroll params; positional fan-out          |
| `string`    | compile-time `String`                    | dispatch keys, file paths, mode selectors         |
| `function`  | `Function` reference                     | callback params, higher-order user fns            |

Each is independently useful at the user-fn call boundary as a
**precondition documenter + enforcer**, surfacing the fn's contract
in source. Without them, user fns either accept everything silently
(today's un-annotated default, which means "anything that doesn't
trigger `E160`") or simulate the contract via downstream
builtin-mismatch diagnostics.

### 1.3 The gap, summarised

| Today                                                                            | Phase 2                                                          |
|----------------------------------------------------------------------------------|------------------------------------------------------------------|
| Only `stream`/`signal` annotations available on user fns                         | Five more: `number`, `record`, `array`, `string`, `function`     |
| `ParamValueType::Number` doesn't exist                                           | Added                                                            |
| Builtin-side `type_compatible()` covers the five types (used by builtin checks)  | Same table — Phase 2 only adds a `Number` case                   |
| User fns accept anything un-annotated, with `E160`-only guard                    | Opt-in precondition checks via the five new annotations          |

---

## 2. Goals and Non-Goals

### 2.1 Goals (Phase 2)

1. Add five lexer keywords: `number`, `record`, `array`, `string`,
   `function`. Each becomes a `TokenType` variant.
2. Extend `parse_optional_annotation()` to recognise the five new
   tokens.
3. Add `ParamValueType::Number` (the only new variant — the other
   four already exist for builtin use).
4. Extend `type_compatible()` with the Number case.
5. Add five dispatch branches in `handle_user_function_call`. Each
   branch follows the Phase 1 Stream/Signal template: precondition
   check the actual `TypedValue`, fire `E184` on mismatch, fall
   through to the existing binding path on success.
6. Tests: parser cases for the five new keywords; codegen cases for
   each annotation × each ValueType actual.
7. Update `web/static/docs/concepts/parameter-type-annotations.md` to
   cover all seven annotation keywords (the two from Phase 1 + the
   five from Phase 2).
8. Rebuild the doc lookup index (`bun run build:docs`).

### 2.2 Non-Goals (deferred or out of scope)

- **`: dynarray` annotation.** DynArray flows through un-annotated
  today via the typed-value-preserving branch at
  `codegen_functions.cpp:781-793`. An explicit `: dynarray`
  annotation would surface its contract but is not in the
  "round out the missing five" remit. Phase 3 candidate if demand
  appears.
- **Closure-param annotation enforcement.** The analyzer copies
  `ClosureParamData::annotated_type` already; enforcement at the
  closure boundary stays deferred (Phase 1 §2.2).
- **Body-side type checking.** Inherits Phase 1 R3-Q2.
- **Type inference.** Inherits Phase 1 R3-Q3.
- **Builtin `param_types` coverage sweep.** Owned by
  `prd-compiler-type-system.md` Phase 4. Orthogonal.
- **`Number → Signal` coerce on `: number`.** R5-Q2 precedent rules
  this out (lossy direction; not defensible). Use `: signal` if
  Signal-shaped is acceptable.
- **`DynArray → Array` coerce on `: array`.** Same reasoning —
  semantically different (runtime-varying vs. compile-time unrolled).
- **Cedar VM bytecode changes.** Annotations stay compile-time only.
  No opcode work.

---

## 3. Target Syntax

### 3.1 Phase 2 examples

```akkado
// : number — strict compile-time constant
fn unison_spread(freq: signal, voices: number, detune: signal) =
    each(range(voices), (i) -> osc("saw", freq * (1 + (i - voices/2) * detune)))

unison_spread(220, 5, 0.01)         // OK — voices is a constant
unison_spread(220, lfo, 0.01)       // E184 — Number expected, got Signal

// : record — Record or Pattern (matches builtin precedent)
fn arpinst(e: record) =
    osc("saw", e.freq) * adsr(e.gate, 0.01, 0.1, 0.5, 0.2) * e.vel

n"c4 e4 g4" |> arpinst(@) |> out(@)   // Pattern argument — OK
arpinst({freq: 440, gate: 1, vel: 0.8})  // Record argument — OK

// : array — compile-time Array only
fn mixer(channels: array, gain: signal) = sum(channels) * gain

mixer([osc("sin", 220), osc("sin", 330)], 0.5)   // OK
mixer(notes(e), 0.5)                              // E184 — DynArray, not Array

// : string — compile-time String only
fn osctype(kind: string, freq: signal) = osc(kind, freq)

osctype("saw", 440)                  // OK
osctype(input_kind, 440)             // E184 if input_kind is a Signal

// : function — Function reference
fn each_voice_call(fns: array, fn: function) = each(fns, fn)

each_voice_call(voices, (v) -> v * 0.5)   // OK
```

Grammar (unchanged from Phase 1; only the token set widens):

```ebnf
fn_param   ::= identifier (':' type_name)? ('=' default_expr)?
            | '...' identifier                       // rest, no annotation
            | destructure_pattern                    // destructure, no annotation
type_name  ::= 'stream' | 'signal'                   // Phase 1
            | 'number' | 'record' | 'array' | 'string' | 'function'   // Phase 2
```

### 3.2 Error examples

```akkado
fn pick(opts: array, idx: number) = opts[idx]

pick([220, 440, 880], lfo)
// E184: parameter 'idx' of fn 'pick' expects Number, got Signal —
//       no coercion path
//   --> at call site, arg 1
```

```akkado
fn echo_chain(stages: number, in: signal) = ...

echo_chain("3", osc("sin", 220))
// E184: parameter 'stages' of fn 'echo_chain' expects Number, got String —
//       no coercion path
```

```akkado
fn each_record(things: array, fn: function) = each(things, fn)

each_record(things, "saw")
// E184: parameter 'fn' of fn 'each_record' expects Function, got String —
//       no coercion path
```

---

## 4. Architecture / Technical Design

### 4.1 Type lattice (delta from Phase 1 §4.1)

Add a single `ParamValueType` variant:

```cpp
// akkado/include/akkado/typed_value.hpp:41-50  (current state, post-Phase-1)
enum class ParamValueType : std::uint8_t {
    Any = 0,
    Signal,
    Pattern,
    String,
    Function,
    Array,
    Record,
    Stream,
    Number,           // NEW — strict compile-time-constant numeric
};
```

No `ValueType` changes — `Number` already exists at
`typed_value.hpp:15-31`.

### 4.2 Compatibility table (Phase 2)

| Annotation   | Actual ValueType                                        | Behavior                                                                                                |
|--------------|---------------------------------------------------------|---------------------------------------------------------------------------------------------------------|
| `: number`   | `Number`                                                | Pass-through; bind buffer via existing default-case binding (`codegen_functions.cpp:800-802`).          |
| `: number`   | `Signal`, `Pattern`, `EventSource`, `Record`, `Array`, `DynArray`, `String`, `Function`, `StateCell`, `Stream`, `Void` | **E184** — no defensible coercion path.                                                                 |
| `: record`   | `Record`                                                | Field extraction via existing path (`codegen_functions.cpp:768-780`).                                   |
| `: record`   | `Pattern`                                               | Bind via existing default-case path. Matches `type_compatible(Record, Pattern) = true` precedent.       |
| `: record`   | other                                                   | **E184**.                                                                                               |
| `: array`    | `Array`                                                 | Multi-buffer bind via existing path (`codegen_functions.cpp:794-799`).                                  |
| `: array`    | `DynArray`                                              | **E184**. Mirrors Phase 1 §4.2's `: stream`/DynArray reject.                                            |
| `: array`    | other                                                   | **E184**.                                                                                               |
| `: string`   | `String`                                                | Existing `param_string_defaults_` map path (`codegen_functions.cpp:510-512`).                           |
| `: string`   | other                                                   | **E184**.                                                                                               |
| `: function` | `Function`                                              | Existing `param_function_refs_` map path (`codegen_functions.cpp:523-526`).                             |
| `: function` | other                                                   | **E184**.                                                                                               |

The `type_compatible()` extension:

```cpp
case ParamValueType::Number:
    return actual == ValueType::Number;
```

The other four annotations reuse the existing cases verbatim:

```cpp
// Already present at builtins.hpp:56-68 (Phase 1 state):
case ParamValueType::Record:   return actual == ValueType::Record || actual == ValueType::Pattern;
case ParamValueType::Array:    return actual == ValueType::Array;
case ParamValueType::String:   return actual == ValueType::String;
case ParamValueType::Function: return actual == ValueType::Function;
```

### 4.3 Codegen change point

`akkado/src/codegen_functions.cpp:539-612` (`handle_user_function_call`
param-binding loop) gains five `else if` branches following the Phase
1 Stream/Signal pattern verbatim. Sketch:

```cpp
} else if (expected == ParamValueType::Number) {
    if (arg_tv.type != ValueType::Number) {
        error("E184",
              "parameter '" + func.params[i].name +
              "' of fn '" + func.name +
              "' expects Number, got " +
              std::string(value_type_name(arg_tv.type)) +
              " — no coercion path",
              ast_->arena[args[i]].location);
    }
} else if (expected == ParamValueType::Record) {
    if (arg_tv.type != ValueType::Record &&
        arg_tv.type != ValueType::Pattern) {
        error("E184",
              "parameter '" + func.params[i].name +
              "' of fn '" + func.name +
              "' expects Record, got " +
              std::string(value_type_name(arg_tv.type)) +
              " — no coercion path",
              ast_->arena[args[i]].location);
    }
} else if (expected == ParamValueType::Array) {
    if (arg_tv.type != ValueType::Array) {
        std::string msg = "parameter '" + func.params[i].name +
                          "' of fn '" + func.name +
                          "' expects Array, got " +
                          std::string(value_type_name(arg_tv.type)) +
                          " — no coercion path";
        if (arg_tv.type == ValueType::DynArray) {
            msg += " (DynArray is a runtime-varying numeric array — "
                   "use the un-annotated path or pass an unrolled "
                   "array literal)";
        }
        error("E184", msg, ast_->arena[args[i]].location);
    }
} else if (expected == ParamValueType::String) {
    if (arg_tv.type != ValueType::String) {
        error("E184",
              "parameter '" + func.params[i].name +
              "' of fn '" + func.name +
              "' expects String, got " +
              std::string(value_type_name(arg_tv.type)) +
              " — no coercion path",
              ast_->arena[args[i]].location);
    }
} else if (expected == ParamValueType::Function) {
    if (arg_tv.type != ValueType::Function) {
        error("E184",
              "parameter '" + func.params[i].name +
              "' of fn '" + func.name +
              "' expects Function, got " +
              std::string(value_type_name(arg_tv.type)) +
              " — no coercion path",
              ast_->arena[args[i]].location);
    }
}
```

On the success path, control falls through to the existing
binding-selection block at `codegen_functions.cpp:745-802`, which
already routes Record/Array/Function/String to their type-specific
binding paths. **No new binding logic is needed** — Phase 2 only adds
precondition checks at the call boundary; the post-check binding
machinery exists already from un-annotated and Phase 1 paths.

An optional refactor (not required): the five Phase 2 branches plus
Phase 1's Stream branch all share the shape `if (actual != expected
ValueType) error("E184", ...)`. A small dispatch helper could collapse
them, but it adds indirection for marginal gain. The PRD recommends
leaving the five `else if` branches verbatim for clarity, matching
Phase 1's style.

### 4.4 Parser & AST changes

- **Lexer** (`akkado/src/lexer.cpp:12-22`): add five entries to
  `keywords`:
  ```cpp
  {"number",   TokenType::Number},
  {"record",   TokenType::Record},
  {"array",    TokenType::Array},
  {"string",   TokenType::String},
  {"function", TokenType::Function},
  ```
- **Token enum** (`akkado/include/akkado/token.hpp`): add `Number`,
  `Record`, `Array`, `String`, `Function` to `TokenType`, following
  the Phase 1 `Stream`/`Signal` convention.
- **Parser** (`akkado/src/parser.cpp:1220-1231`): extend
  `parse_optional_annotation()` lambda with the five new token-→-
  ParamValueType mappings.
- **Analyzer** (`akkado/src/analyzer.cpp:377-389`, `:500-502`,
  `:779-789`): **no changes**. The annotation propagation path is
  type-agnostic — it copies `ParamValueType` end to end.

### 4.5 Compile-time vs runtime

Annotations stay compile-time only. The Cedar VM remains untyped; no
bytecode format changes; no new opcodes. Inherits Phase 1 §4.5.

---

## 5. Impact Assessment

| Component                                          | Status        | Notes                                                                                  |
|----------------------------------------------------|---------------|----------------------------------------------------------------------------------------|
| Akkado lexer                                       | **Modified**  | Five new keyword entries                                                               |
| Akkado token enum                                  | **Modified**  | Five new `TokenType` variants                                                          |
| Akkado parser                                      | **Modified**  | Five new cases in `parse_optional_annotation()`                                        |
| Akkado analyzer                                    | **Stays**     | Annotation propagation is type-agnostic                                                |
| `handle_user_function_call`                        | **Modified**  | Five new `else if` branches following the Phase 1 Stream/Signal template               |
| `ValueType` enum                                   | **Stays**     | `Number` already exists                                                                |
| `ParamValueType` enum                              | **Modified**  | Add `Number`                                                                           |
| `type_compatible()` lookup                         | **Modified**  | One new case for `Number`                                                              |
| `param_value_type_name()` printer                  | **Modified**  | One new case for `Number`                                                              |
| Symbol table                                       | **Stays**     | Existing binding paths cover all five Phase 2 types                                    |
| Cedar VM                                           | **Stays**     | No bytecode/opcode/instruction-format change                                           |
| `prd-compiler-type-system.md` Phase 4              | **Stays**     | Orthogonal                                                                             |
| Existing stdlib fns                                | **Stays**     | No annotations added in Phase 2                                                        |
| Existing user patches                              | **Stays** *(except identifier collision)* | Reserved keywords may collide with user variable names; documented breaking change (see §8.5) |
| Hot-swap path                                      | **Stays**     | `annotated_type` is a static AST field                                                 |
| `prd-runtime-event-transforms.md` Phase 2b         | **Unblocked already by Phase 1** | Phase 2 is additive, not load-bearing for event-transforms                |

---

## 6. File-Level Changes

| File                                                       | Change                                                                                          |
|------------------------------------------------------------|-------------------------------------------------------------------------------------------------|
| `akkado/include/akkado/token.hpp`                          | Add `Number`, `Record`, `Array`, `String`, `Function` to `TokenType` enum                       |
| `akkado/src/lexer.cpp`                                     | Add five entries to `keywords` lookup table (lexer.cpp:12-22)                                   |
| `akkado/include/akkado/typed_value.hpp`                    | Add `ParamValueType::Number` to enum (typed_value.hpp:41-50)                                    |
| `akkado/include/akkado/builtins.hpp`                       | Extend `param_value_type_name()` (builtins.hpp:32-44) and `type_compatible()` (builtins.hpp:56-68) for `Number` |
| `akkado/src/parser.cpp`                                    | Extend `parse_optional_annotation()` lambda (parser.cpp:1220-1231) with five new token cases    |
| `akkado/src/codegen_functions.cpp`                         | Five new `else if` branches in `handle_user_function_call` (codegen_functions.cpp:539-612)      |
| `akkado/tests/test_lexer.cpp`                              | Cases for each new keyword token                                                                |
| `akkado/tests/test_parser.cpp`                             | Annotation-grammar cases for each new keyword (see §10.1)                                       |
| `akkado/tests/test_fn_annotations.cpp`                     | Codegen cases for each annotation × each ValueType actual (see §10.2)                           |
| `web/static/docs/concepts/parameter-type-annotations.md`   | Update to cover all seven annotation keywords; add examples for each                            |
| `web/src/lib/docs/lookup-index.ts`                         | Rebuilt via `bun run build:docs` after the concept-doc update                                   |

No changes required in:

- `cedar/` (no VM work).
- `akkado/stdlib/` (no existing stdlib fn is annotated in Phase 2).
- `experiments/` (no DSP-level testing).

---

## 7. Error Codes

| Code | Site                                              | Meaning                                                                              |
|------|---------------------------------------------------|--------------------------------------------------------------------------------------|
| `E104` (existing) | `parse_param_list()`                  | Annotation not allowed on destructure or rest param (Phase 1 restriction; covers Phase 2 keywords too) |
| `E160` (existing, **unchanged**) | `handle_user_function_call` un-annotated and `: signal` paths | User function parameter cannot accept a polyphonic non-sample pattern as scalar |
| `E184` (existing, **reused**) | `handle_user_function_call` annotated paths | Argument type incompatible with parameter annotation — no defensible coercion path. Phase 2 reuses the same code; the diagnostic body names the new ValueType. |
| `E185` (existing, **scope narrows**) | `parse_param_list()`             | Unknown type name in annotation. Phase 2 removes five cases from `E185`'s firing surface (the new keywords). |

No new error codes are introduced in Phase 2.

---

## 8. Edge Cases

### 8.1 `: number = 5` default

Numeric default + numeric annotation composes cleanly. The existing
numeric-default branch (`codegen_functions.cpp:628-653`) emits a
`PUSH_CONST` buffer and binds the param verbatim, without consulting
`annotated_type` (per Phase 1 §8.9). The resulting buffer is a
constant `5` — exactly what `: number` requires. No new diagnostic
needed.

### 8.2 `: record = {a: 1}` default

The existing default-eval path rejects non-trivial Record literals as
defaults with `E105`. The Phase 2 `: record` annotation doesn't change
this — `E105` fires first. No new diagnostic needed.

### 8.3 `: string = "x"` default

The existing string-default path (`codegen_functions.cpp:510-512`)
emits `BUFFER_UNUSED` and populates `param_string_defaults_`. The
binding works as today. The annotation type-check at the call site
doesn't run for omitted args (per Phase 1 §8.9), so the default is
implicitly trusted. No new diagnostic needed.

### 8.4 `: array = [1, 2]` and `: function = (x) -> x` defaults

Both rejected by `E105` at compile-time const-eval (arrays of
non-trivial elements and lambda literals are not const-evaluable).
The `: array`/`: function` annotation doesn't see them. No new
diagnostic needed.

### 8.5 Reserved-keyword collision surface

`number`, `record`, `array`, `string`, `function` become reserved
keywords on Phase 2 landing.

**In-tree breakage check (run 2026-05-23):**

```bash
find . -name "*.ak" -not -path "./build/*" \
  | xargs grep -lE "\b(number|record|array|string|function)\b" 2>/dev/null
# (no output)
```

No `.ak` source file uses any of these as identifiers. The C++
test/source tree has `record` as a C++ local variable name and
`array`/`function` inside C++ TEST_CASE strings — none of which are
Akkado source. **No in-tree migration needed.**

External user patches using any of these five names as Akkado
identifiers (variables, function names, fn parameters) will get a
parse error after Phase 2 lands and need to rename. The concept-doc
update at `parameter-type-annotations.md` will warn users explicitly.

### 8.6 `: record` accepting a Pattern argument

Existing `type_compatible(Record, Pattern) = true`. A Pattern passed
to a `: record`-annotated param passes the precondition check. The
binding falls through to the default-case symbol path
(`codegen_functions.cpp:800-802`) — *not* the Record field-extraction
path (`:768-780`), because that path is gated on
`arg_tv.type == ValueType::Record`. Inside the body, the
param resolves to a Pattern `TypedValue`, so field-access paths like
`p.freq` resolve to the Pattern's per-field buffers (as they do today
for any Pattern symbol). The body author writes Record-shaped code
(`p.freq`, `p.vel`); Pattern provides those fields natively.

### 8.7 `: array` rejecting DynArray with E184

DynArray is the `notes(e)` / `freqs(e)` accessor's return type
(prd-pattern-event-arrays). It is runtime-varying-length; `: array`
expects compile-time-unrolled. The diagnostic body explicitly names
DynArray as the rejected case (see §4.3's `: array` branch sketch)
and points users at the un-annotated path. This mirrors Phase 1
§4.2's `: stream`/DynArray reject behavior.

### 8.8 `: function` and closure values

A Function value is either a top-level `fn` reference (passed via the
existing `param_function_refs_` map at `codegen_functions.cpp:523-526`)
or a closure literal that has been promoted to a Function symbol.
`: function` checks `arg_tv.type == ValueType::Function`; the
existing call-time dispatch path handles binding verbatim. The
closure-param-annotation deferral (Phase 1 §2.2) is unrelated —
`: function` is a *parameter type*, not a closure's own param's type.

### 8.9 `: string` and pattern strings (`"c4 e4"`)

A bare double-quoted string parses to `ValueType::String`. The mini-
notation literals `n"c4 e4"` and `p"c4 e4"` parse to
`ValueType::Pattern`, not String — they have prefixed sigils. So
`: string` accepts plain strings only, never mini-notation literals.
This is the correct behavior — a `: string`-annotated param is for
dispatch keys and mode selectors, not patterns.

### 8.10 Hot-swap: annotation changes between revisions

Inherits Phase 1 §8.5 verbatim. `annotated_type` is a static field on
`ParsedParam` / `FunctionParamInfo`; hot-swap re-runs the analyzer
from the new source and the new annotation simply applies. No special
path-hash logic.

### 8.11 Polymorphic call: same fn called with multiple actual subtypes

Only `: record` has multiple accepted ValueType actuals (Record and
Pattern). Each call inlines the body with the caller's `TypedValue`
propagated, and any downstream field-access uses the inlined-symbol's
type. Both paths are exercised by §10's tests.

### 8.12 Nested annotation

```akkado
fn outer(things: array, fn: function) = inner_user_fn(things, fn)
```

If `inner_user_fn`'s params are also `: array` / `: function`, both
boundary checks succeed (pass-through). No special case.

---

## 9. Phasing

Phase 2 is one phase — no sub-split. Steps mirror Phase 1 §9 lifecycle:

| Step | Deliverable                                                                                                            | Tests                                                          |
|------|------------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------|
| 2.1  | Lexer keywords + TokenType variants: `number`, `record`, `array`, `string`, `function`                                  | `test_lexer.cpp` keyword cases                                 |
| 2.2  | `ParamValueType::Number`; extend `type_compatible()` + `param_value_type_name()`                                        | unit test on `type_compatible(Number, X)`                      |
| 2.3  | Parser `parse_optional_annotation()` extension                                                                          | `test_parser.cpp` annotation-grammar cases (see §10.1)         |
| 2.4  | `handle_user_function_call`: five new `else if` branches with `E184` precondition checks                                | `test_fn_annotations.cpp` (see §10.2)                          |
| 2.5  | End-to-end verification examples                                                                                        | see §10.3                                                      |
| 2.6  | Concept-doc update + lookup index rebuild                                                                               | `bun run build:docs`                                           |

---

## 10. Testing / Verification

### 10.1 Parser tests (`akkado/tests/test_parser.cpp`)

| Case                                                                  | Expectation                                                   |
|-----------------------------------------------------------------------|---------------------------------------------------------------|
| `fn f(x: number) = x * 2`                                             | parses; `params[0].annotated_type == Number`                  |
| `fn f(r: record) = r.freq`                                            | parses; `params[0].annotated_type == Record`                  |
| `fn f(a: array) = a[0]`                                               | parses; `params[0].annotated_type == Array`                   |
| `fn f(s: string) = s`                                                 | parses; `params[0].annotated_type == String`                  |
| `fn f(cb: function) = cb(1)`                                          | parses; `params[0].annotated_type == Function`                |
| `fn f(x: number = 5) = x * 2`                                         | parses; annotation + default compose                          |
| `fn f({x, y}: record) = x + y`                                        | `E104` — annotation on destructure                            |
| `fn f(...args: array) = sum(args)`                                    | `E104` — annotation on rest                                   |
| `fn f(x: bogustype) = x`                                              | `E185` — unknown type name                                    |
| `array = 5` (top-level assignment using reserved name)                | parse error — `array` is now a reserved keyword               |
| `fn array(...) = ...` (using reserved name as fn name)                | parse error                                                   |
| `fn f(array) = array` (using reserved name as param name)             | parse error                                                   |

### 10.2 Codegen tests (`akkado/tests/test_fn_annotations.cpp`)

For each annotation, the canonical accepted-type case plus 2–3
rejected-type cases:

| Annotation   | Case                                                              | Expectation                                                          |
|--------------|-------------------------------------------------------------------|----------------------------------------------------------------------|
| `: number`   | `fn f(x: number) = x` called with `5`                             | no `E184`; body binds `x` to constant-5 buffer                       |
| `: number`   | `fn f(x: number) = x` called with `osc("sin", 1)` (Signal)        | `E184` "expects Number, got Signal"                                  |
| `: number`   | `fn f(x: number) = x` called with `n"c4 e4"` (Pattern)            | `E184` "expects Number, got Pattern"                                 |
| `: number`   | `fn f(x: number) = x` called with `"text"` (String)               | `E184` "expects Number, got String"                                  |
| `: record`   | `fn f(r: record) = r.freq` called with `{freq: 440}`              | no `E184`; field-extraction binds `freq`                             |
| `: record`   | `fn f(r: record) = r.freq` called with `n"c4 e4"` (Pattern)       | no `E184`; body's `r.freq` resolves via Pattern's per-field buffer   |
| `: record`   | `fn f(r: record) = r.freq` called with `220` (Number)             | `E184` "expects Record, got Number"                                  |
| `: record`   | `fn f(r: record) = r.freq` called with `[1, 2]` (Array)           | `E184` "expects Record, got Array"                                   |
| `: array`    | `fn f(a: array) = a[0]` called with `[220, 440]`                  | no `E184`; multi-buffer bind; `a[0]` emits `ARRAY_INDEX`             |
| `: array`    | `fn f(a: array) = a[0]` called with `notes(e)` (DynArray)         | `E184` with the DynArray-specific hint                               |
| `: array`    | `fn f(a: array) = a[0]` called with `{x: 1}` (Record)             | `E184` "expects Array, got Record"                                   |
| `: string`   | `fn f(s: string) = s` called with `"saw"`                         | no `E184`; binding via `param_string_defaults_`                      |
| `: string`   | `fn f(s: string) = s` called with `220` (Number)                  | `E184` "expects String, got Number"                                  |
| `: string`   | `fn f(s: string) = s` called with `n"c4"` (Pattern)               | `E184` "expects String, got Pattern"                                 |
| `: function` | `fn f(cb: function) = cb(1)` called with `(x) -> x * 2`           | no `E184`; binding via `param_function_refs_`                        |
| `: function` | `fn f(cb: function) = cb(1)` called with `220` (Number)           | `E184` "expects Function, got Number"                                |
| `: function` | `fn f(cb: function) = cb(1)` called with `"saw"` (String)         | `E184` "expects Function, got String"                                |

Plus a smoke test verifying un-annotated params still behave bit-for-
bit identically (no regression of Phase 1's R2-Q4 invariant).

### 10.3 End-to-end verification examples

**Multi-annotation fn (one of each):**

```akkado
fn unison_arp(p: stream, voices: number, kind: string, mix: signal) =
    poly(p, (f, g, v) ->
            each(range(voices), (i) -> osc(kind, f * (1 + i * 0.01))) |> sum() * v * mix,
         voices)

n"c4 e4 g4".transpose(0)
    |> unison_arp(@, 3, "saw", 0.7)
    |> out(@)
```

Acceptance:
1. Compiles with no errors.
2. `cmake --build build && ./build/akkado/tests/akkado_tests` passes.
3. Renders via `nkido-cli render` and produces three notes with 3-
   voice unison arrays each.

**Record-annotated callback inside a higher-order fn:**

```akkado
fn play_event(e: record, mix: signal) =
    osc("saw", e.freq) * adsr(e.gate, 0.01, 0.1, 0.5, 0.2) * e.vel * mix

n"c4 e4 g4" |> play_event(@, 0.7) |> out(@)
```

Acceptance:
1. Compiles with no errors.
2. Pattern argument satisfies `: record` (via Pattern ⊆ Record).
3. Renders three notes at c4/e4/g4 with full ADSR envelope.

**Negative case (E184 surfaces cleanly):**

```akkado
fn pick(opts: array, idx: number) = opts[idx]

pick([220, 440, 880], osc("sin", 1))   // idx is Signal, not Number
```

Acceptance: emits `E184` at the second argument's source location,
diagnostic body reads "parameter 'idx' of fn 'pick' expects Number,
got Signal — no coercion path". No cascading errors. Compilation
fails cleanly with `success = false`.

### 10.4 Build / run commands

```bash
# Configure + build
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target akkado_tests

# Run Phase 2 tests
./build/akkado/tests/akkado_tests "[parser]"
./build/akkado/tests/akkado_tests "[type-annotation]"     # Phase 1 + Phase 2 share this tag
./build/akkado/tests/akkado_tests "[fn_annotations]"

# Rebuild concept-doc lookup index after the doc update
cd web && bun run build:docs

# End-to-end manual check
bun run dev
# Paste §10.3 examples; listen.
```

---

## 11. Resolved Design Decisions

All decisions below were resolved by the clarification round on
2026-05-23 (this session). They are final and locked; implementation
follows directly.

**P2-Q1. Deliverable shape. → RESOLVED: standalone Phase 2 PRD file.**
This PRD lives at `docs/prd-parameter-type-annotations-phase-2.md`,
independent of the Phase 1 PRD. Phase 1 §9 explicitly named "a
follow-up PRD authored once Phase 1 ships" — this is it. The Phase 1
PRD is not re-edited.

**P2-Q2. Reserved keyword vs context-only recognition. → RESOLVED: reserved keywords.**
`number`, `record`, `array`, `string`, `function` become reserved
keywords on Phase 2 landing, matching Phase 1's R4-Q1 precedent. The
in-tree `.ak` grep (§8.5) confirms no current Akkado source uses any
of these as identifiers. External user patches using these names need
to rename — documented in the concept-doc update. Context-only
recognition was considered and rejected: it adds parser complexity
without proportional benefit given the empty in-tree collision
surface.

**P2-Q3. `: number` strictness. → RESOLVED: strict — Number only.**
`: number` accepts only `ValueType::Number` (compile-time constant).
Signal does not coerce. Matches Phase 1 R5-Q2 precedent ("coerce when
defensible ≠ force-coerce regardless"). Use `: signal` if the param
is OK with runtime audio. A future "strict + W-warning suggesting
`: signal`" option was considered and rejected for Phase 2 — silent
hard-error keeps the surface predictable; the warning can land in
Phase 3 if demand emerges.

**P2-Q4. `: record` and Pattern. → RESOLVED: accept (matches builtin precedent).**
`type_compatible(Record, Pattern) = true` is the existing precedent
for builtin param-type checks. `: record` annotation respects the
same rule — Pattern passes the precondition check; the body sees a
Pattern `TypedValue` (not a synthesised Record), and field-access
paths resolve via the Pattern's per-field buffers as they do for any
Pattern symbol.

**P2-Q5. `: array` and DynArray. → RESOLVED: reject with E184 + DynArray-specific hint.**
Mirrors Phase 1 §4.2's `: stream`/DynArray reject. DynArray is
runtime-varying-length; `: array` is compile-time-unrolled. No
defensible coercion path. Diagnostic body explicitly names DynArray
to help users discover the un-annotated DynArray-passing path.

**P2-Q6. No new W-class warnings. → RESOLVED: silent passes for defensible coercions, hard E184 otherwise.**
Phase 2 inherits Phase 1's choice. Lossy-but-doable coercions (Phase
1's mono Pattern → Signal under `: signal`) stay silent. Phase 2's
five new annotations have no lossy-but-doable coercion paths (the
strict choices in §4.2 are all "exactly this type or E184"), so the
warning surface is empty.

**P2-Q7. No new error codes. → RESOLVED: reuse E184 and E185.**
`E184` covers type-mismatch; `E185` covers unknown type names. The
Phase 1 catalog at §7 already names both. Phase 2 adds no codes.

**P2-Q8. Closure-param annotation enforcement. → RESOLVED: stays deferred.**
The analyzer's site-1 propagation
(`analyzer.cpp:377-389`) copies `ClosureParamData::annotated_type`
end-to-end already; the AST tolerates closure-param annotations.
Enforcement at the closure boundary is Phase 3 work. Phase 2 keeps
the Phase 1 §2.2 deferral.

**P2-Q9. No body-side type checking, no inference. → RESOLVED: inherit Phase 1.**
Phase 1 R3-Q2 (no body-side check) and R3-Q3 (no inference) carry
forward unchanged.

**P2-Q10. `: dynarray` annotation. → RESOLVED: out of scope.**
DynArray flows through un-annotated today via the typed-value-
preserving branch at `codegen_functions.cpp:781-793`. An explicit
annotation would surface the contract but is not in Phase 2's
"round out the missing five" remit. Filed as Phase 3 candidate.

**P2-Q11. Refactor `handle_user_function_call` dispatch into a helper. → RESOLVED: no.**
The five Phase 2 branches plus the two Phase 1 branches share a
shape, but extracting a helper trades clarity for indirection. Phase
2 keeps the verbatim `else if` style, matching Phase 1. Refactor can
happen later if a sixth/seventh case lands.

---

## 12. Next Step

All design-blocking prerequisites are cleared:

1. ✅ Phase 1 of `prd-parameter-type-annotations.md` SHIPPED
   (`stream`/`signal` annotations, `ParamValueType::Stream`, dispatch,
   tests, doc).
2. ✅ All eleven Phase 2 design decisions resolved (§11).
3. ✅ In-tree breakage check clean (§8.5).
4. ✅ Existing builtin `type_compatible()` already covers the four
   non-Number types — only the Number case is new.

**Implementation may begin at Phase 2 step 2.1** (lexer keywords),
proceeding through the steps in §9. Each step is independently
testable; the §10.3 end-to-end examples are the final acceptance
check.
