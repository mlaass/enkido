> **Status: READY FOR IMPLEMENTATION.** Phase 1 of
> [`prd-parameter-type-annotations.md`](prd-parameter-type-annotations.md)
> landed on `master` 2026-05-23 (commits `599a692`, `d579c3c`,
> `4e9b618`, `ecbd317`, `4c2ca4c`) — local only, not released. This
> PRD is the deferred follow-up named in Phase 1 §9, **plus a small
> Phase 1 retrofit**: it renames the `: stream` keyword to `: evs`
> and adds a `: sig` alias for `: signal`. Both retrofits are in
> scope because Phase 1 hasn't shipped externally and the
> abbreviation policy resolved in P2-Q13 applies to the entire
> annotation surface.
>
> **Caveat — Phase 5 Commit I (2026-05-22, `00a8d34`) collapsed
> `ValueType::EventSource` into `ValueType::Pattern`.** Runtime
> event streams (`midi(...)`, etc.) are now represented as a
> `Pattern` `TypedValue` whose `PatternPayload::is_runtime_event_source`
> flag is set, rather than a separate discriminator. Wherever this
> PRD says "Pattern or EventSource" (notably §1.1, §3.1, §4.2),
> read it as "Pattern (possibly with the runtime-event-source
> flag)". The annotation surface is unaffected — `: evs` /
> `ParamValueType::Stream` accepts `Pattern`, which now covers both
> the mini-notation and runtime-event-stream cases. The rejected-
> actuals row for `: num`/`: rec`/`: arr`/`: str`/`: fn` in §4.2
> can drop `EventSource` from its enumeration (it is no longer a
> distinct ValueType to reject); the row is otherwise unchanged.

# PRD: Akkado Parameter Type Annotations — Phase 2 (Full ValueType Set, Abbreviated)

## Executive Summary

Phase 1 landed `: stream` and `: signal` parameter annotations on
user `fn` definitions. The grammar (`name: type = default`),
`ParamValueType` lookup, analyzer propagation, and
`handle_user_function_call` dispatch all landed and are exercised by
end-to-end tests. Phase 1 §9 explicitly deferred the rest of the type
set — `number`, `record`, `array`, `string`, `function` — to this
follow-up PRD.

Phase 2 adds those five types under abbreviated keywords (`num`,
`rec`, `arr`, `str`, `fn`) and retrofits Phase 1 for naming
consistency (rename `stream` → `evs`; add `sig` alias for `signal`).
The mechanism is exactly the one Phase 1 built; only the cases
multiply. The bulk of `ParamValueType` variants already exist for
builtin use (`Pattern`, `String`, `Function`, `Array`, `Record`), so
the only new variant is `ParamValueType::Number`. The dispatch in
`handle_user_function_call` gains five `else if` branches, each
mirroring the Phase 1 Stream/Signal pattern: precondition-check the
caller's `TypedValue`, fire `E184` on incompatible types, otherwise
fall through to the existing binding path with no new binding logic.

**Key design decisions (locked — see §11 for sourcing):**

- **Abbreviated keywords.** Phase 2 ships `num`, `rec`, `arr`, `str`,
  `fn`. Phase 1's `: stream` is renamed to `: evs` (local-only
  rename since Phase 1 hasn't been released externally); `: signal`
  gains a `: sig` alias (both forms work). Per P2-Q13: "use
  abbreviations for the types in general."
- **Reserved keywords, hard cutover.** `num`, `rec`, `arr`, `str`,
  `sig`, `evs` become reserved keywords on Phase 2 landing. `fn` is
  already reserved (declaration keyword); the Phase 2 grammar reuses
  the existing `TokenType::Fn` in annotation position. The in-tree
  grep confirms no `.ak` source file uses any of these as identifiers
  (see §8.5).
- **Token reuse for literal-named keywords.** `TokenType::Number` and
  `TokenType::String` already exist for numeric/string literals.
  Phase 2 reuses these tokens for the `num` / `str` keywords —
  `parse_optional_annotation()` disambiguates by inspecting
  `tok.lexeme`. Only `TokenType::Record`, `TokenType::Array`, and
  `TokenType::Evs` (rename of `TokenType::Stream`) are genuinely new
  variants. `TokenType::Signal` keeps its current name and adds `sig`
  as a second keyword-table entry. (P2-Q12.)
- **Strict `: num`.** Accepts only `ValueType::Number` (compile-time
  constant). `Signal` does not coerce. Per R5-Q2 (Phase 1): "coerce
  when defensible ≠ force-coerce regardless"; Number is the
  more-restrictive direction, the coercion is lossy.
- **Annotations mirror existing builtin precedent.** The Phase 1
  `type_compatible()` lookup table is the design's load-bearing
  reference. `: rec` accepts Pattern (Pattern is structurally a
  Record); the others reject everything except their own ValueType.
- **No new error codes.** `E184` from Phase 1 covers type-mismatch
  cases; `E185` covers unknown type names. Phase 2's five new
  keywords remove five cases from `E185`'s firing surface — that's
  it.
- **No new W-class warnings.** Phase 2 follows Phase 1's choice of
  silent passes for defensible coercions and hard errors otherwise.
- **Closure-param annotations stay tolerated-but-unenforced.** The
  analyzer already copies `ClosureParamData::annotated_type` into
  `FunctionParamInfo` (`analyzer.cpp:378-391`, `:525-531`, `:779-791`);
  enforcement at the closure boundary is Phase 3 work.
- **No body-side type checking, no inference.** Both deferrals inherit
  unchanged from Phase 1 §2.2.

---

## 1. Problem Statement

### 1.1 What Phase 1 ships

The user-`fn` param-binding path
(`akkado/src/codegen_functions.cpp:539-623`) branches on the param's
`annotated_type`:

- `: stream` — preserves Pattern `TypedValue` across the boundary
  (including the runtime-event-source flag, pre-Phase-5-Commit-I
  EventSource); bypasses `E160`; fires `E184` on incompatible actuals.
- `: signal` — keeps today's voice-0 coerce for Signal/Number/mono
  Pattern; keeps `E160` for polyphonic non-sample Pattern; fires
  `E184` on Record/Array/DynArray/String/Function/StateCell/Void.
- *(un-annotated)* — exactly today's behavior, bit-for-bit. Only the
  polyphonic-pattern `E160` guard fires.

The lattice has `ValueType::Stream` (abstract supertype) and
`ParamValueType::Stream`. `type_compatible()` covers all 8 existing
ParamValueType cases. The grammar is `name: type (= default)?`,
restricted in Phase 1 to `stream` and `signal` only.

### 1.2 What's missing

Five annotation types specified in §9 of the Phase 1 PRD as Phase 2
work, here under abbreviated keywords:

| Keyword | Accepts                                  | Useful for                                          |
|---------|------------------------------------------|-----------------------------------------------------|
| `num`   | compile-time `Number` only               | param specialisation: voice count, array size, etc. |
| `rec`   | `Record` or `Pattern`                    | structured field-set params (`{freq, vel}`-shaped)  |
| `arr`   | compile-time `Array`                     | spread/unroll params; positional fan-out            |
| `str`   | compile-time `String`                    | dispatch keys, file paths, mode selectors           |
| `fn`    | `Function` reference                     | callback params, higher-order user fns              |

Each is independently useful at the user-fn call boundary as a
**precondition documenter + enforcer**, surfacing the fn's contract
in source. Without them, user fns either accept everything silently
(today's un-annotated default, which means "anything that doesn't
trigger `E160`") or simulate the contract via downstream
builtin-mismatch diagnostics.

### 1.3 The gap, summarised

| Today                                                                            | Phase 2                                                          |
|----------------------------------------------------------------------------------|------------------------------------------------------------------|
| Only `stream`/`signal` annotations available on user fns                         | Five more abbreviated: `num`, `rec`, `arr`, `str`, `fn`          |
| `ParamValueType::Number` doesn't exist                                           | Added                                                            |
| Builtin-side `type_compatible()` covers the five types (used by builtin checks)  | Same table — Phase 2 only adds a `Number` case                   |
| User fns accept anything un-annotated, with `E160`-only guard                    | Opt-in precondition checks via the five new annotations          |
| `: stream` keyword landed in Phase 1 (full word, local only)                     | Renamed to `: evs`; abbreviation convention extends Phase 1      |
| `: signal` landed in Phase 1                                                     | `: sig` alias added; both `: signal` and `: sig` work            |

---

## 2. Goals and Non-Goals

### 2.1 Goals (Phase 2)

1. **Phase 2 keywords.** Add four lexer keywords and reuse the
   existing `TokenType::Fn`:
   - `num` → reuses existing `TokenType::Number` (literal token,
     disambiguated by lexeme in `parse_optional_annotation()`)
   - `rec` → new `TokenType::Record`
   - `arr` → new `TokenType::Array`
   - `str` → reuses existing `TokenType::String` (literal token,
     disambiguated by lexeme)
   - `fn` → reuses existing `TokenType::Fn` (declaration keyword)
2. **Phase 1 retrofit.**
   - Rename `: stream` → `: evs`. Update lexer keyword table, rename
     `TokenType::Stream` → `TokenType::Evs`, update the parser case,
     update Phase 1 tests, and update the Phase 1 concept doc. Local
     rename only — Phase 1 hasn't been released externally, so no
     downstream users to migrate.
   - Add `sig` alias for `signal`. Add a second keyword-table entry
     `{"sig", TokenType::Signal}` mapping the abbreviation to the
     existing token. Both `: signal` and `: sig` work.
3. Extend `parse_optional_annotation()` to recognise the four new
   tokens, plus `TokenType::Fn` in annotation position. For
   `TokenType::Number` / `TokenType::String`, check `tok.lexeme` to
   distinguish keyword from literal.
4. Add `ParamValueType::Number` (the only new variant — the other
   four already exist for builtin use).
5. Extend `type_compatible()` with the Number case.
6. Add five dispatch branches in `handle_user_function_call`. Each
   branch follows the Phase 1 Stream/Signal template: precondition
   check the actual `TypedValue`, fire `E184` on mismatch, fall
   through to the existing binding path on success.
7. Tests: parser cases for the new keywords; codegen cases for each
   annotation × each ValueType actual. All Phase 2 cases land in
   `akkado/tests/test_param_type_annotations.cpp` (where Phase 1
   tests already live), under the existing `[type-annotation]` tag.
8. Update `web/static/docs/concepts/parameter-type-annotations.md` to
   cover all seven annotation keywords (Phase 1 `evs`/`sig` after
   rename + Phase 2's five abbreviated forms).
9. Rebuild the doc lookup index (`bun run build:docs`).

### 2.2 Non-Goals (deferred or out of scope)

- **`: dynarr` annotation.** DynArray flows through un-annotated
  today via the typed-value-preserving branch at
  `codegen_functions.cpp:792-804`. An explicit `: dynarr` annotation
  would surface its contract but is not in the "round out the
  missing five" remit. Phase 3 candidate if demand appears.
- **Closure-param annotation enforcement.** The analyzer copies
  `ClosureParamData::annotated_type` already; enforcement at the
  closure boundary stays deferred (Phase 1 §2.2).
- **Body-side type checking.** Inherits Phase 1 R3-Q2.
- **Type inference.** Inherits Phase 1 R3-Q3.
- **Builtin `param_types` coverage sweep.** Owned by
  `prd-compiler-type-system.md` Phase 4. Orthogonal.
- **`Number → Signal` coerce on `: num`.** R5-Q2 precedent rules
  this out (lossy direction; not defensible). Use `: signal` /
  `: sig` if Signal-shaped is acceptable.
- **`DynArray → Array` coerce on `: arr`.** Same reasoning —
  semantically different (runtime-varying vs. compile-time
  unrolled).
- **Cedar VM bytecode changes.** Annotations stay compile-time only.
  No opcode work.
- **Long-form aliases for the Phase 2 keywords.** No `number`,
  `record`, `array`, `string`, `function` aliases. The abbreviated
  forms are canonical and exclusive (P2-Q13). If demand emerges,
  long-form aliases can be added in a follow-up PRD.

---

## 3. Target Syntax

### 3.1 Phase 2 examples

```akkado
// : num — strict compile-time constant
fn unison_spread(freq: sig, voices: num, detune: sig) =
    each(range(voices), (i) -> osc("saw", freq * (1 + (i - voices/2) * detune)))

unison_spread(220, 5, 0.01)         // OK — voices is a constant
unison_spread(220, lfo, 0.01)       // E184 — Number expected, got Signal

// : rec — Record or Pattern (matches builtin precedent)
fn arpinst(e: rec) =
    osc("saw", e.freq) * adsr(e.gate, 0.01, 0.1, 0.5, 0.2) * e.vel

n"c4 e4 g4" |> arpinst(@) |> out(@)       // Pattern argument — OK
arpinst({freq: 440, gate: 1, vel: 0.8})    // Record argument — OK

// : arr — compile-time Array only
fn mixer(channels: arr, gain: sig) = sum(channels) * gain

mixer([osc("sin", 220), osc("sin", 330)], 0.5)   // OK
mixer(notes(e), 0.5)                              // E184 — DynArray, not Array

// : str — compile-time String only
fn osctype(kind: str, freq: sig) = osc(kind, freq)

osctype("saw", 440)                  // OK
osctype(input_kind, 440)             // E184 if input_kind is a Signal

// : fn — Function reference
fn each_voice_call(voices: arr, cb: fn) = each(voices, cb)

each_voice_call(voices, (v) -> v * 0.5)   // OK

// : evs — Phase 1 keyword, renamed (was : stream)
fn transpose(events: evs, n) =
    event_map(events, (e) -> {note: e.note + n})

n"c4 e4 g4".transpose(7)             // Pattern argument — OK
midi("ctrl1").transpose(12)          // Pattern (runtime-event-source flag) — OK
```

Grammar (unchanged from Phase 1; only the token set widens):

```ebnf
fn_param   ::= identifier (':' type_name)? ('=' default_expr)?
            | '...' identifier                       // rest, no annotation
            | destructure_pattern                    // destructure, no annotation
type_name  ::= 'evs' | 'sig' | 'signal'              // Phase 1 (after retrofit)
            | 'num' | 'rec' | 'arr' | 'str' | 'fn'   // Phase 2
```

Note: `signal` keeps working alongside `sig` (alias). `stream` no
longer parses as a type name after the rename — Phase 1 is local
only, so no downstream callers need to migrate.

### 3.2 Error examples

```akkado
fn pick(opts: arr, idx: num) = opts[idx]

pick([220, 440, 880], lfo)
// E184: parameter 'idx' of fn 'pick' expects Number, got Signal —
//       no coercion path
//   --> at call site, arg 1
```

```akkado
fn echo_chain(stages: num, in: sig) = ...

echo_chain("3", osc("sin", 220))
// E184: parameter 'stages' of fn 'echo_chain' expects Number, got String —
//       no coercion path
```

```akkado
fn each_record(things: arr, cb: fn) = each(things, cb)

each_record(things, "saw")
// E184: parameter 'cb' of fn 'each_record' expects Function, got String —
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

**Note on the `Stream` variant name.** The keyword renames to `evs`
but the underlying `ParamValueType::Stream` / `ValueType::Stream`
variant names in C++ stay unchanged. The variant is internal-only;
only the source-level keyword surface changes. This keeps the rename
small (lexer + parser + concept doc + tests) rather than touching
every C++ site that references `ParamValueType::Stream`.

**Note on `EventSource` (post Phase 5 Commit I).** Phase 5 collapsed
`ValueType::EventSource` into `ValueType::Pattern` with a
`PatternPayload::is_runtime_event_source` flag. Runtime event
streams (`midi(...)`, etc.) are `ValueType::Pattern` `TypedValue`s.
The annotation surface is unaffected: `: evs` (== `ParamValueType::Stream`)
accepts `Pattern`, which covers both the mini-notation and
runtime-event-source cases. `type_compatible(Stream, Pattern) = true`
remains the load-bearing rule.

### 4.2 Compatibility table (Phase 2)

| Annotation | Actual ValueType                                        | Behavior                                                                                                |
|------------|---------------------------------------------------------|---------------------------------------------------------------------------------------------------------|
| `: num`    | `Number`                                                | Pass-through; bind buffer via existing default-case binding (`codegen_functions.cpp:811-813`).          |
| `: num`    | `Signal`, `Pattern`, `Record`, `Array`, `DynArray`, `String`, `Function`, `StateCell`, `Stream`, `Void` | **E184** — no defensible coercion path. (Phase 5 Commit I collapsed `EventSource` into `Pattern`; the `Pattern` row covers both cases.) |
| `: rec`    | `Record`                                                | Field extraction via existing path (`codegen_functions.cpp:779-791`).                                   |
| `: rec`    | `Pattern`                                               | Bind via existing default-case path. Matches `type_compatible(Record, Pattern) = true` precedent.       |
| `: rec`    | other                                                   | **E184**.                                                                                               |
| `: arr`    | `Array`                                                 | Multi-buffer bind via existing path (`codegen_functions.cpp:805-810`).                                  |
| `: arr`    | `DynArray`                                              | **E184**. Mirrors Phase 1 §4.2's `: stream`/DynArray reject.                                            |
| `: arr`    | other                                                   | **E184**.                                                                                               |
| `: str`    | `String`                                                | Existing `param_string_defaults_` map path (`codegen_functions.cpp:510-512`).                           |
| `: str`    | other                                                   | **E184**.                                                                                               |
| `: fn`     | `Function`                                              | Existing `param_function_refs_` map path (`codegen_functions.cpp:523-526`).                             |
| `: fn`     | other                                                   | **E184**.                                                                                               |

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

`akkado/src/codegen_functions.cpp:539-623` (`handle_user_function_call`
param-binding loop) gains five `else if` branches following the Phase
1 Stream/Signal pattern verbatim. Sketch (diagnostic body uses
ValueType names — `Number`/`Record`/`Array`/`String`/`Function` —
not the source-level keyword abbreviations, so messages stay
discoverable):

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
binding-selection block at `codegen_functions.cpp:719-818`, which
already routes Record/Array/Function/String/Stream/DynArray to their
type-specific binding paths. **No new binding logic is needed** —
Phase 2 only adds precondition checks at the call boundary; the
post-check binding machinery exists already from un-annotated and
Phase 1 paths.

An optional refactor (not required): the five Phase 2 branches plus
Phase 1's Stream branch all share the shape `if (actual != expected
ValueType) error("E184", ...)`. A small dispatch helper could
collapse them, but it adds indirection for marginal gain. The PRD
recommends leaving the five `else if` branches verbatim for clarity,
matching Phase 1's style.

### 4.4 Parser & AST changes

- **Lexer** (`akkado/src/lexer.cpp:12-22`): replace the existing
  `{"stream", TokenType::Stream}` entry with `{"evs", TokenType::Evs}`
  (Phase 1 retrofit rename), and add:
  ```cpp
  {"sig",    TokenType::Signal},   // alias of `signal`
  {"num",    TokenType::Number},   // reuses literal token; parser disambiguates by lexeme
  {"rec",    TokenType::Record},
  {"arr",    TokenType::Array},
  {"str",    TokenType::String},   // reuses literal token; parser disambiguates by lexeme
  ```
  `fn` is already registered (declaration keyword). The Phase 2 parser
  recognises `TokenType::Fn` in annotation position.
- **Token enum** (`akkado/include/akkado/token.hpp`):
  - Rename `TokenType::Stream` → `TokenType::Evs` (Phase 1 retrofit).
    Update the `token_type_name()` printer at `token.hpp:112-113`
    similarly.
  - Add new variants `TokenType::Record`, `TokenType::Array` for the
    two genuinely-new keyword tokens.
  - `TokenType::Number` and `TokenType::String` are reused for the
    `num` / `str` keywords (no new variants needed). The parser
    distinguishes literal vs keyword by inspecting `tok.lexeme`.
  - `TokenType::Fn` is reused for the `fn` annotation keyword (no
    new variant; it already exists for the declaration keyword).
  - `TokenType::Signal` is reused for both `signal` and `sig`
    (alias).
- **Parser** (`akkado/src/parser.cpp:1220-1231`): extend
  `parse_optional_annotation()` lambda. The lookup becomes:
  ```cpp
  if (tok.type == TokenType::Evs)    return ParamValueType::Stream;
  if (tok.type == TokenType::Signal) return ParamValueType::Signal;
  if (tok.type == TokenType::Record) return ParamValueType::Record;
  if (tok.type == TokenType::Array)  return ParamValueType::Array;
  if (tok.type == TokenType::Fn)     return ParamValueType::Function;
  if (tok.type == TokenType::Number && tok.lexeme == "num")
                                      return ParamValueType::Number;
  if (tok.type == TokenType::String && tok.lexeme == "str")
                                      return ParamValueType::String;
  // Otherwise E185 (unknown type name).
  ```
  The `tok.lexeme` check disambiguates the keyword form from a stray
  numeric or string literal in annotation position (which would have
  been an E185 candidate anyway). E185's "did you mean" hint is
  updated to list all seven valid type names.
- **Analyzer** (`akkado/src/analyzer.cpp:378-391`, `:525-531`,
  `:779-791`): **no changes**. The annotation propagation path is
  type-agnostic — it copies `ParamValueType` end to end.

### 4.5 Compile-time vs runtime

Annotations stay compile-time only. The Cedar VM remains untyped; no
bytecode format changes; no new opcodes. Inherits Phase 1 §4.5.

---

## 5. Impact Assessment

| Component                                          | Status        | Notes                                                                                  |
|----------------------------------------------------|---------------|----------------------------------------------------------------------------------------|
| Akkado lexer                                       | **Modified**  | Rename `stream`→`evs`; add `sig`, `num`, `rec`, `arr`, `str` entries (6 keyword changes total) |
| Akkado token enum                                  | **Modified**  | Rename `TokenType::Stream`→`Evs`; add `Record`, `Array`; reuse `Number`/`String`/`Fn`/`Signal` for keywords |
| Akkado parser                                      | **Modified**  | Updated cases in `parse_optional_annotation()`; lexeme disambiguation for Number/String |
| Akkado analyzer                                    | **Stays**     | Annotation propagation is type-agnostic                                                |
| `handle_user_function_call`                        | **Modified**  | Five new `else if` branches following the Phase 1 Stream/Signal template               |
| `ValueType` enum                                   | **Stays**     | `Number` already exists                                                                |
| `ParamValueType` enum                              | **Modified**  | Add `Number`                                                                           |
| `type_compatible()` lookup                         | **Modified**  | One new case for `Number`                                                              |
| `param_value_type_name()` printer                  | **Modified**  | One new case for `Number`                                                              |
| Symbol table                                       | **Stays**     | Existing binding paths cover all five Phase 2 types                                    |
| Cedar VM                                           | **Stays**     | No bytecode/opcode/instruction-format change                                           |
| `prd-compiler-type-system.md` Phase 4              | **Stays**     | Orthogonal                                                                             |
| Existing stdlib fns                                | **Modified** *(retrofit)* | `akkado/stdlib/event_transforms.ak` uses `: stream`; rename to `: evs` to match the Phase 1 retrofit |
| Phase 1 test file (`test_param_type_annotations.cpp`) | **Modified** *(retrofit)* | All Phase 1 `: stream` test cases renamed to `: evs`; add `: sig` alias parser test |
| Phase 1 concept doc                                | **Modified** *(retrofit)* | `parameter-type-annotations.md` updated to document the rename + abbreviations |
| Existing user patches                              | **Stays** *(except identifier collision)* | Reserved keywords may collide with user variable names. Phase 1 is local-only (not released), so the `: stream` → `: evs` rename has no downstream blast radius. |
| Hot-swap path                                      | **Stays**     | `annotated_type` is a static AST field                                                 |
| `prd-runtime-event-transforms.md` Phase 2b         | **Unblocked already by Phase 1** | Phase 2 is additive, not load-bearing for event-transforms                |

---

## 6. File-Level Changes

| File                                                       | Change                                                                                          |
|------------------------------------------------------------|-------------------------------------------------------------------------------------------------|
| `akkado/include/akkado/token.hpp`                          | Rename `TokenType::Stream` → `TokenType::Evs`; add `TokenType::Record`, `TokenType::Array`; update `token_type_name()` printer at `:112-113` |
| `akkado/src/lexer.cpp`                                     | Replace `{"stream", TokenType::Stream}` with `{"evs", TokenType::Evs}`; add `{"sig", TokenType::Signal}`, `{"num", TokenType::Number}`, `{"rec", TokenType::Record}`, `{"arr", TokenType::Array}`, `{"str", TokenType::String}` (lexer.cpp:12-22) |
| `akkado/include/akkado/typed_value.hpp`                    | Add `ParamValueType::Number` to enum (typed_value.hpp:41-50)                                    |
| `akkado/include/akkado/builtins.hpp`                       | Extend `param_value_type_name()` (builtins.hpp:32-44) and `type_compatible()` (builtins.hpp:56-68) for `Number` |
| `akkado/src/parser.cpp`                                    | Extend `parse_optional_annotation()` lambda (parser.cpp:1220-1231) with the new token cases; add lexeme disambiguation for `TokenType::Number`/`String` |
| `akkado/src/codegen_functions.cpp`                         | Five new `else if` branches in `handle_user_function_call` (codegen_functions.cpp:539-623)      |
| `akkado/stdlib/event_transforms.ak`                        | Rename `: stream` → `: evs` in `transpose` / `velocity` / etc. (retrofit; lines 20-21+)         |
| `akkado/tests/test_lexer.cpp`                              | Update existing `stream signal` keyword case to `evs sig`; add cases for the four new keywords (`num`, `rec`, `arr`, `str`) and verify `signal` still tokenises (alias kept) |
| `akkado/tests/test_parser.cpp`                             | Update existing `: stream` annotation grammar cases to `: evs`; add cases for the five new abbreviated keywords (see §10.1) |
| `akkado/tests/test_param_type_annotations.cpp`             | **All Phase 2 codegen cases land here** (existing Phase 1 home). Update Phase 1 `: stream` cases to `: evs`; add Phase 2 cases under `[type-annotation]` tag (see §10.2) |
| `web/static/docs/concepts/parameter-type-annotations.md`   | Update to cover all seven annotation keywords; document the `stream`→`evs` rename + `sig` alias; add examples for each new keyword |
| `web/src/lib/docs/lookup-index.ts`                         | Rebuilt via `bun run build:docs` after the concept-doc update                                   |

No changes required in:

- `cedar/` (no VM work).
- `experiments/` (no DSP-level testing).

---

## 7. Error Codes

| Code | Site                                              | Meaning                                                                              |
|------|---------------------------------------------------|--------------------------------------------------------------------------------------|
| `E104` (existing) | `parse_param_list()`                  | Annotation not allowed on destructure or rest param (Phase 1 restriction; covers Phase 2 keywords too) |
| `E160` (existing, **unchanged**) | `handle_user_function_call` un-annotated and `: signal`/`: sig` paths | User function parameter cannot accept a polyphonic non-sample pattern as scalar |
| `E184` (existing, **reused**) | `handle_user_function_call` annotated paths | Argument type incompatible with parameter annotation — no defensible coercion path. Phase 2 reuses the same code; the diagnostic body names the new ValueType. |
| `E185` (existing, **scope narrows**) | `parse_param_list()`             | Unknown type name in annotation. Phase 2 removes five cases from `E185`'s firing surface (the new keywords). The "did you mean" hint is updated to list all seven valid type names. |

No new error codes are introduced in Phase 2.

---

## 8. Edge Cases

### 8.1 `: num = 5` default

Numeric default + numeric annotation composes cleanly. The existing
numeric-default branch (`codegen_functions.cpp:639-664`) emits a
`PUSH_CONST` buffer and binds the param verbatim, without consulting
`annotated_type` (per Phase 1 §8.9). The resulting buffer is a
constant `5` — exactly what `: num` requires. No new diagnostic
needed.

### 8.2 `: rec = {a: 1}` default

The existing default-eval path rejects non-trivial Record literals as
defaults with `E105`. The Phase 2 `: rec` annotation doesn't change
this — `E105` fires first. No new diagnostic needed.

### 8.3 `: str = "x"` default

The existing string-default path (`codegen_functions.cpp:510-512`)
emits `BUFFER_UNUSED` and populates `param_string_defaults_`. The
binding works as today. The annotation type-check at the call site
doesn't run for omitted args (per Phase 1 §8.9), so the default is
implicitly trusted. No new diagnostic needed.

### 8.4 `: arr = [1, 2]` and `: fn = (x) -> x` defaults

Both rejected by `E105` at compile-time const-eval (arrays of
non-trivial elements and lambda literals are not const-evaluable).
The `: arr`/`: fn` annotation doesn't see them. No new diagnostic
needed.

### 8.5 Reserved-keyword collision surface

`num`, `rec`, `arr`, `str`, `sig`, `evs` become reserved keywords on
Phase 2 landing. `fn` is already reserved (declaration keyword).
`signal` keeps its reserved status from Phase 1.

**In-tree breakage check (run 2026-05-23):**

```bash
find . -name "*.ak" -not -path "./build/*" \
  | xargs grep -lE "\b(num|rec|arr|str|sig|evs)\b" 2>/dev/null
# (no output)
```

The only `.ak` file in tree (`akkado/stdlib/event_transforms.ak`)
does not use any of these as identifiers. **No `.ak` migration
needed beyond the deliberate `: stream` → `: evs` rename in §6.**
The C++ test/source tree has matches inside C++ strings (e.g.
`"str"` literals in test cases) — none of which are Akkado source.

External user patches using any of these six names as Akkado
identifiers (variables, function names, fn parameters) will get a
parse error after Phase 2 lands and need to rename. The concept-doc
update at `parameter-type-annotations.md` will warn users
explicitly. Phase 1 is local-only (committed but not released), so
the `: stream` → `: evs` rename does not affect any downstream user
code.

### 8.6 `: rec` accepting a Pattern argument

Existing `type_compatible(Record, Pattern) = true`. A Pattern passed
to a `: rec`-annotated param passes the precondition check. The
binding falls through to the default-case symbol path
(`codegen_functions.cpp:811-813`) — *not* the Record field-extraction
path (`:779-791`), because that path is gated on
`arg_tv.type == ValueType::Record`. Inside the body, the
param resolves to a Pattern `TypedValue`, so field-access paths like
`p.freq` resolve to the Pattern's per-field buffers (as they do
today for any Pattern symbol). The body author writes Record-shaped
code (`p.freq`, `p.vel`); Pattern provides those fields natively.

### 8.7 `: arr` rejecting DynArray with E184

DynArray is the `notes(e)` / `freqs(e)` accessor's return type
([`prd-pattern-event-arrays.md`](prd-pattern-event-arrays.md)). It
is runtime-varying-length; `: arr` expects compile-time-unrolled.
The diagnostic body explicitly names DynArray as the rejected case
(see §4.3's `: arr` branch sketch) and points users at the
un-annotated path. This mirrors Phase 1 §4.2's `: stream`/DynArray
reject behavior.

### 8.8 `: fn` and closure values

A Function value is either a top-level `fn` reference (passed via
the existing `param_function_refs_` map at
`codegen_functions.cpp:523-526`) or a closure literal that has been
promoted to a Function symbol. `: fn` checks
`arg_tv.type == ValueType::Function`; the existing call-time
dispatch path handles binding verbatim. The closure-param-annotation
deferral (Phase 1 §2.2) is unrelated — `: fn` is a *parameter type*,
not a closure's own param's type.

### 8.9 `: str` and pattern strings (`"c4 e4"`)

A bare double-quoted string parses to `ValueType::String`. The mini-
notation literals `n"c4 e4"` and `p"c4 e4"` parse to
`ValueType::Pattern`, not String — they have prefixed sigils. So
`: str` accepts plain strings only, never mini-notation literals.
This is the correct behavior — a `: str`-annotated param is for
dispatch keys and mode selectors, not patterns.

### 8.10 Hot-swap: annotation changes between revisions

Inherits Phase 1 §8.5 verbatim. `annotated_type` is a static field
on `ParsedParam` / `FunctionParamInfo`; hot-swap re-runs the
analyzer from the new source and the new annotation simply applies.
No special path-hash logic.

### 8.11 Polymorphic call: same fn called with multiple actual subtypes

Only `: rec` has multiple accepted ValueType actuals (Record and
Pattern). Each call inlines the body with the caller's `TypedValue`
propagated, and any downstream field-access uses the inlined-symbol's
type. Both paths are exercised by §10's tests.

### 8.12 Nested annotation

```akkado
fn outer(things: arr, cb: fn) = inner_user_fn(things, cb)
```

If `inner_user_fn`'s params are also `: arr` / `: fn`, both
boundary checks succeed (pass-through). No special case.

### 8.13 Lexeme disambiguation for `: num` and `: str`

Because `num` / `str` share `TokenType::Number` / `TokenType::String`
with literals, `parse_optional_annotation()` must inspect
`tok.lexeme` to distinguish:

- `: 42` — `tok.type == Number`, `tok.lexeme == "42"`. Falls through
  to the E185 path (unknown type name; numeric literals aren't valid
  in annotation position).
- `: num` — `tok.type == Number`, `tok.lexeme == "num"`. Resolves to
  `ParamValueType::Number`.
- `: "x"` — `tok.type == String`, `tok.lexeme == "x"` (note: the
  lexeme of a string literal includes the content, not the quotes).
  Falls through to E185.
- `: str` — `tok.type == String`, `tok.lexeme == "str"`. Resolves to
  `ParamValueType::String`.

This is the one place the parser dispatch is not purely
TokenType-driven. Implementers should keep the lexeme checks
adjacent to the TokenType checks for maintainability.

---

## 9. Phasing

Phase 2 is one phase — no sub-split. Steps mirror Phase 1 §9
lifecycle:

| Step | Deliverable                                                                                                              | Tests                                                          |
|------|--------------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------|
| 2.1  | **Phase 1 retrofit**: rename `stream`→`evs` (lexer + token + parser); add `sig` alias; update Phase 1 tests + stdlib + concept doc | Updated `test_lexer.cpp`, `test_parser.cpp`, `test_param_type_annotations.cpp` |
| 2.2  | Add new lexer keywords + TokenType variants: `rec`/`arr` (new), `num`/`str` (reuse literal tokens via lexeme disambiguation) | `test_lexer.cpp` keyword cases                                 |
| 2.3  | `ParamValueType::Number`; extend `type_compatible()` + `param_value_type_name()`                                          | unit test on `type_compatible(Number, X)`                      |
| 2.4  | Parser `parse_optional_annotation()` extension (with lexeme disambiguation for `num`/`str`)                               | `test_parser.cpp` annotation-grammar cases (see §10.1)         |
| 2.5  | `handle_user_function_call`: five new `else if` branches with `E184` precondition checks                                  | `test_param_type_annotations.cpp` (see §10.2)                  |
| 2.6  | End-to-end verification examples                                                                                          | see §10.3                                                      |
| 2.7  | Concept-doc update (document rename + abbreviations) + lookup index rebuild                                              | `bun run build:docs`                                           |

---

## 10. Testing / Verification

### 10.1 Parser tests (`akkado/tests/test_parser.cpp`)

| Case                                                                  | Expectation                                                   |
|-----------------------------------------------------------------------|---------------------------------------------------------------|
| `fn f(x: num) = x * 2`                                                | parses; `params[0].annotated_type == Number`                  |
| `fn f(r: rec) = r.freq`                                               | parses; `params[0].annotated_type == Record`                  |
| `fn f(a: arr) = a[0]`                                                 | parses; `params[0].annotated_type == Array`                   |
| `fn f(s: str) = s`                                                    | parses; `params[0].annotated_type == String`                  |
| `fn f(cb: fn) = cb(1)`                                                | parses; `params[0].annotated_type == Function`                |
| `fn f(e: evs, n) = e`                                                 | parses; `params[0].annotated_type == Stream` (rename of Phase 1 case) |
| `fn f(r: sig) = r`                                                    | parses; `params[0].annotated_type == Signal` (alias)          |
| `fn f(r: signal) = r`                                                 | parses; `params[0].annotated_type == Signal` (long form still works) |
| `fn f(e: stream) = e`                                                 | `E185` — `stream` is no longer a valid type name (renamed to `evs`) |
| `fn f(x: num = 5) = x * 2`                                            | parses; annotation + default compose                          |
| `fn f({x, y}: rec) = x + y`                                           | `E104` — annotation on destructure                            |
| `fn f(...args: arr) = sum(args)`                                      | `E104` — annotation on rest                                   |
| `fn f(x: bogustype) = x`                                              | `E185` — unknown type name; "did you mean evs/sig/num/rec/arr/str/fn?" |
| `arr = 5` (top-level assignment using reserved name)                  | parse error — `arr` is now a reserved keyword                 |
| `fn arr(...) = ...` (using reserved name as fn name)                  | parse error                                                   |
| `fn f(arr) = arr` (using reserved name as param name)                 | parse error                                                   |

### 10.2 Codegen tests (`akkado/tests/test_param_type_annotations.cpp`)

Phase 2 codegen cases land in the existing Phase 1 test file under
the existing `[type-annotation]` tag. For each annotation, the
canonical accepted-type case plus 2–3 rejected-type cases:

| Annotation | Case                                                              | Expectation                                                          |
|------------|-------------------------------------------------------------------|----------------------------------------------------------------------|
| `: num`    | `fn f(x: num) = x` called with `5`                                | no `E184`; body binds `x` to constant-5 buffer                       |
| `: num`    | `fn f(x: num) = x` called with `osc("sin", 1)` (Signal)           | `E184` "expects Number, got Signal"                                  |
| `: num`    | `fn f(x: num) = x` called with `n"c4 e4"` (Pattern)               | `E184` "expects Number, got Pattern"                                 |
| `: num`    | `fn f(x: num) = x` called with `"text"` (String)                  | `E184` "expects Number, got String"                                  |
| `: rec`    | `fn f(r: rec) = r.freq` called with `{freq: 440}`                 | no `E184`; field-extraction binds `freq`                             |
| `: rec`    | `fn f(r: rec) = r.freq` called with `n"c4 e4"` (Pattern)          | no `E184`; body's `r.freq` resolves via Pattern's per-field buffer   |
| `: rec`    | `fn f(r: rec) = r.freq` called with `220` (Number)                | `E184` "expects Record, got Number"                                  |
| `: rec`    | `fn f(r: rec) = r.freq` called with `[1, 2]` (Array)              | `E184` "expects Record, got Array"                                   |
| `: arr`    | `fn f(a: arr) = a[0]` called with `[220, 440]`                    | no `E184`; multi-buffer bind; `a[0]` emits `ARRAY_INDEX`             |
| `: arr`    | `fn f(a: arr) = a[0]` called with `notes(e)` (DynArray)           | `E184` with the DynArray-specific hint                               |
| `: arr`    | `fn f(a: arr) = a[0]` called with `{x: 1}` (Record)               | `E184` "expects Array, got Record"                                   |
| `: str`    | `fn f(s: str) = s` called with `"saw"`                            | no `E184`; binding via `param_string_defaults_`                      |
| `: str`    | `fn f(s: str) = s` called with `220` (Number)                     | `E184` "expects String, got Number"                                  |
| `: str`    | `fn f(s: str) = s` called with `n"c4"` (Pattern)                  | `E184` "expects String, got Pattern"                                 |
| `: fn`     | `fn f(cb: fn) = cb(1)` called with `(x) -> x * 2`                 | no `E184`; binding via `param_function_refs_`                        |
| `: fn`     | `fn f(cb: fn) = cb(1)` called with `220` (Number)                 | `E184` "expects Function, got Number"                                |
| `: fn`     | `fn f(cb: fn) = cb(1)` called with `"saw"` (String)               | `E184` "expects Function, got String"                                |

Plus:

- A smoke test verifying un-annotated params still behave bit-for-bit
  identically (no regression of Phase 1's R2-Q4 invariant).
- A Phase-1-retrofit test: all existing `: stream`-using cases
  renamed to `: evs` continue to pass with identical bytecode
  output. Adding a new alias case verifying `: sig` and `: signal`
  produce identical bytecode for the same fn.

### 10.3 End-to-end verification examples

**Multi-annotation fn (one of each):**

```akkado
fn unison_arp(p: evs, voices: num, kind: str, mix: sig) =
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
fn play_event(e: rec, mix: sig) =
    osc("saw", e.freq) * adsr(e.gate, 0.01, 0.1, 0.5, 0.2) * e.vel * mix

n"c4 e4 g4" |> play_event(@, 0.7) |> out(@)
```

Acceptance:

1. Compiles with no errors.
2. Pattern argument satisfies `: rec` (via Pattern ⊆ Record).
3. Renders three notes at c4/e4/g4 with full ADSR envelope.

**Negative case (E184 surfaces cleanly):**

```akkado
fn pick(opts: arr, idx: num) = opts[idx]

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

# Rebuild concept-doc lookup index after the doc update
cd web && bun run build:docs

# End-to-end manual check
bun run dev
# Paste §10.3 examples; listen.
```

---

## 11. Resolved Design Decisions

All decisions below were resolved by the clarification rounds on
2026-05-23 (this session). They are final and locked; implementation
follows directly.

**P2-Q1. Deliverable shape. → RESOLVED: standalone Phase 2 PRD file.**
This PRD lives at `docs/prd-parameter-type-annotations-phase-2.md`,
independent of the Phase 1 PRD. Phase 1 §9 explicitly named "a
follow-up PRD authored once Phase 1 ships" — this is it. The Phase
1 PRD is not re-edited (beyond its existing pointer at §12 to this
PRD).

**P2-Q2. Reserved keyword vs context-only recognition. → RESOLVED: reserved keywords.**
The seven keywords (`evs`, `sig`, `num`, `rec`, `arr`, `str`, `fn`)
become reserved on Phase 2 landing — `fn` was already reserved; the
other six are newly reserved. `signal` and `stream` (Phase 1's local
keywords) reach Phase 2 differently: `signal` stays reserved with
`sig` added as an alias; `stream` is removed entirely and replaced
by `evs`. In-tree `.ak` grep (§8.5) confirms no current Akkado source
uses any of these as identifiers. Context-only recognition was
considered and rejected: it adds parser complexity without
proportional benefit given the empty in-tree collision surface.

**P2-Q3. `: num` strictness. → RESOLVED: strict — Number only.**
`: num` accepts only `ValueType::Number` (compile-time constant).
Signal does not coerce. Matches Phase 1 R5-Q2 precedent ("coerce
when defensible ≠ force-coerce regardless"). Use `: sig` /
`: signal` if the param is OK with runtime audio. A future "strict
+ W-warning suggesting `: sig`" option was considered and rejected
for Phase 2 — silent hard-error keeps the surface predictable; the
warning can land in Phase 3 if demand emerges.

**P2-Q4. `: rec` and Pattern. → RESOLVED: accept (matches builtin precedent).**
`type_compatible(Record, Pattern) = true` is the existing precedent
for builtin param-type checks. `: rec` annotation respects the
same rule — Pattern passes the precondition check; the body sees a
Pattern `TypedValue` (not a synthesised Record), and field-access
paths resolve via the Pattern's per-field buffers as they do for
any Pattern symbol.

**P2-Q5. `: arr` and DynArray. → RESOLVED: reject with E184 + DynArray-specific hint.**
Mirrors Phase 1 §4.2's `: stream`/DynArray reject. DynArray is
runtime-varying-length; `: arr` is compile-time-unrolled. No
defensible coercion path. Diagnostic body explicitly names DynArray
to help users discover the un-annotated DynArray-passing path.

**P2-Q6. No new W-class warnings. → RESOLVED: silent passes for defensible coercions, hard E184 otherwise.**
Phase 2 inherits Phase 1's choice. Lossy-but-doable coercions (Phase
1's mono Pattern → Signal under `: signal`/`: sig`) stay silent.
Phase 2's five new annotations have no lossy-but-doable coercion
paths (the strict choices in §4.2 are all "exactly this type or
E184"), so the warning surface is empty.

**P2-Q7. No new error codes. → RESOLVED: reuse E184 and E185.**
`E184` covers type-mismatch; `E185` covers unknown type names. The
Phase 1 catalog at §7 already names both. Phase 2 adds no codes.

**P2-Q8. Closure-param annotation enforcement. → RESOLVED: stays deferred.**
The analyzer's site-1 propagation
(`analyzer.cpp:378-391`) copies `ClosureParamData::annotated_type`
end-to-end already; the AST tolerates closure-param annotations.
Enforcement at the closure boundary is Phase 3 work. Phase 2 keeps
the Phase 1 §2.2 deferral.

**P2-Q9. No body-side type checking, no inference. → RESOLVED: inherit Phase 1.**
Phase 1 R3-Q2 (no body-side check) and R3-Q3 (no inference) carry
forward unchanged.

**P2-Q10. `: dynarr` annotation. → RESOLVED: out of scope.**
DynArray flows through un-annotated today via the typed-value-
preserving branch at `codegen_functions.cpp:792-804`. An explicit
annotation would surface the contract but is not in Phase 2's
"round out the missing five" remit. Filed as Phase 3 candidate.

**P2-Q11. Refactor `handle_user_function_call` dispatch into a helper. → RESOLVED: no.**
The five Phase 2 branches plus the two Phase 1 branches share a
shape, but extracting a helper trades clarity for indirection.
Phase 2 keeps the verbatim `else if` style, matching Phase 1.
Refactor can happen later if a sixth/seventh case lands.

**P2-Q12. Token strategy for `num`/`str` keywords. → RESOLVED: reuse `TokenType::Number` / `String` with lexeme disambiguation.**
Both `TokenType::Number` and `TokenType::String` already exist for
numeric and string literals respectively (token.hpp:18-19). Adding
new variants (`NumberKw`, `StringKw`) was considered and rejected
because the lexeme-based disambiguation in
`parse_optional_annotation()` is a single tight if-statement and
keeps the token enum smaller. The implementer must keep the lexeme
checks adjacent to the TokenType checks (see §8.13). For `fn`, the
existing `TokenType::Fn` (declaration keyword) is reused in
annotation position — no new variant needed.

**P2-Q13. Abbreviated keywords + Phase 1 retrofit. → RESOLVED: ship abbreviated; rename `stream`→`evs`; alias `signal`+`sig`.**
Per the abbreviation policy ("use abbreviations for the types in
general"), Phase 2 ships five abbreviated keywords (`num`, `rec`,
`arr`, `str`, `fn`). Phase 1 is retrofitted to match the policy:
`stream` is renamed to `evs` (local rename — Phase 1 hasn't been
released externally, so no downstream migration cost); `signal`
gains a `sig` alias (both forms work). The asymmetry between
`stream`→rename and `signal`→alias is the user's choice; it
acknowledges that `evs` (event stream) is a clearer abbreviation
that better reflects the abstract supertype's semantics, while
`sig`/`signal` is a pure shortening. Long-form aliases for the
five Phase 2 keywords (`number`/`record`/etc.) are explicitly out
of scope (§2.2); only the abbreviated forms are reserved.

**P2-Q14. Phase 2 test file. → RESOLVED: `akkado/tests/test_param_type_annotations.cpp`.**
Phase 1 created `test_param_type_annotations.cpp` specifically for
this feature (header: "Later commits append parser-grammar and
codegen-binding cases here under the same tag"). Phase 2 cases go
in the same file, under the same `[type-annotation]` tag. The
unrelated `test_fn_annotations.cpp` is for `#inline` annotations
and recursion detection (PRD prd-runtime-functions-control-flow
L2) and is not used here.

---

## 12. Next Step

All design-blocking prerequisites are cleared:

1. ✅ Phase 1 of `prd-parameter-type-annotations.md` landed locally
   (`stream`/`signal` annotations, `ParamValueType::Stream`,
   dispatch, tests, doc) — not released externally.
2. ✅ All fourteen Phase 2 design decisions resolved (§11).
3. ✅ In-tree breakage check clean (§8.5).
4. ✅ Existing builtin `type_compatible()` already covers the four
   non-Number types — only the Number case is new.

**Implementation may begin at Phase 2 step 2.1** (Phase 1 retrofit:
rename `stream`→`evs`, add `sig` alias, update Phase 1 tests +
stdlib + concept doc), proceeding through the steps in §9. Each
step is independently testable; the §10.3 end-to-end examples are
the final acceptance check.
