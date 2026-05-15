> **Status: NOT STARTED** — Draft PRD; reviewed and revised. Phase 1.5 (builtin signature migration) added as a hard prerequisite; closure desugaring inserts as a named argument keyed on the closure-typed slot.

# PRD: `->>` Closure-Pipe Operator

## Executive Summary

This PRD specifies a new binary operator `->>` that provides syntactic sugar for passing a closure as the last argument to a function call. It combines the ergonomics of pipe-style block syntax (familiar from Ruby, Swift, and other languages) with Akkado's existing field-access pattern on `%`/`@`.

### Why?

Many Akkado builtins take a function/closure as their last argument — `poly()`, `mono()`, `legato()`, `tap_delay()`, `map()`, `reduce()`, `zipWith()`, and future higher-order functions. Today, passing an inline closure is verbose:

```akkado
// Current: must write full closure signature and param names
pat("C4' Am7' G4'") |> poly(%, (freq, gate, vel) -> saw(freq) * ar(gate) * vel) |> out(%, %)

// Proposed: ->> provides the closure body, compiler fills params
pat("C4'") |> poly(%) ->> saw(%.freq) * ar(%.gate) * %.vel |> out(%, %)
```

The operator saves repetitions of boilerplate parameter names, makes the instrument body the visual focus, and works uniformly across all higher-order builtins.

### Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Token | `->>` (single 3-char token) | Combines `->` (closure arrow) and `>` (pipe direction); distinct from both `->` and `\|>` |
| Hole aliases | Both `%` and `@` | Both already parse as holes; users choose whichever reads better |
| Precedence | Higher than `\|>` | `f() ->> body \|> rest` = `(f() ->> body) \|> rest` — `->>` binds the closure body first |
| Closure param count | Inferred from function definition | Compiler knows the target function's closure signature; `%.field` maps to named params |
| Body grouping | Both `(...)` and `{...}` | Parens for inline pipe chains; braces for multi-statement blocks |

---

## 1. Current State

### 1.1 Higher-Order Builtins

Several builtins accept a closure today. The position of the closure parameter varies between builtins:

| Builtin | Current Signature | Closure Slot | Closure Arity | Closure Param Names |
|---------|------------------|---------------|---------------|---------------------|
| `poly(input, instrument, voices=64)` | `instrument` (slot 1) — `voices` follows | 3 params | `freq`, `gate`, `vel` |
| `mono(instrument)` / `mono(input, instrument)` | last (1-arg form) / last (2-arg form) | 3 params | `freq`, `gate`, `vel` |
| `legato(instrument)` / `legato(input, instrument)` | last (1-arg form) / last (2-arg form) | 3 params | `freq`, `gate`, `vel` |
| `tap_delay(in, time, fb, processor, dry?, wet?)` | `processor` (slot 3) — optional `dry`/`wet` follow | 1 param | `x` (or user-named) |
| `map(array, fn)` | last | 1 param | `x` |
| `reduce(array, fn, init)` | `fn` (slot 1) — `init` follows | 2 params | `acc`, `x` |
| `zipWith(a, b, fn)` | last | 2 params | `a`, `b` |

> **Note:** `->>` requires the closure parameter to occupy the **last positional slot** of the call. Builtins where this is not currently the case (`poly`, `reduce`, plus `tap_delay`'s optional trailing `dry`/`wet`) are reordered as part of Phase 1.5 (see §8). The post-migration shape is:
>
> - `poly(input, voices, instrument)` (closure last; `voices` defaults to 64)
> - `reduce(array, init, fn)` (closure last)
> - `tap_delay(in, time, fb, dry, wet, processor)` (closure last; `dry`/`wet` keep their defaults)
> - `tap_delay_ms`, `tap_delay_smp` mirror `tap_delay`

In every case, the caller must supply an explicit closure:

```akkado
// poly — 3-param closure
pat("C4' Am7'") |> poly(%, (freq, gate, vel) -> saw(freq) * ar(gate) * vel) |> out(%, %)

// tap_delay — 1-param closure
tap_delay(in, 0.5, 0.3, (x) -> x * 0.5)

// map — 1-param closure
map([1, 2, 3], (x) -> x * 2)
```

### 1.2 How Closure Params Are Resolved

For builtins, the closure's expected parameter count and names are **not** stored in `BuiltinInfo` metadata — they are hardcoded in each codegen handler:

- `handle_poly_call()` (`codegen_functions.cpp:1967-1976`): validates **exactly 3 params** named `freq`, `gate`, `vel`
- `handle_tap_delay_call()` (`codegen_functions.cpp:1738-1771`): validates **exactly 1 param**
- Array op handlers (`codegen_arrays.cpp`): accept 1 or 2 params depending on op

### 1.3 Hole Operator `%` / `@`

Both `%` (`TokenType::Hole`) and `@` (`TokenType::At`) parse identically as holes via `parse_hole()` (`parser.cpp:554-556`). `@` originated as a mini-notation weight modifier but has been extended to the general hole context.

### 1.4 Pipe Operator `|>`

The pipe operator rewrites `a |> f(%)` to `f(a)` (hole substitution) during semantic analysis (`analyzer.cpp:694-750`). It has the lowest operator precedence.

---

## 2. Goals and Non-Goals

### Goals

1. **Concise closure-as-last-arg syntax**: `fn(args) ->> body` desugars to `fn(args, (params) -> body)` where `%.field` maps to named closure params
2. **Works with all existing higher-order builtins**: `poly`, `mono`, `legato`, `tap_delay` family, `map`, `reduce`, `zipWith`
3. **Field-access destructuring**: `%.freq` inside the `->>` body is replaced with a direct reference to the `freq` closure param
4. **Body grouping**: support both `(parens)` and `{braces}` for closure bodies containing pipe chains
5. **Backwards compatible**: all existing closure syntax continues to work unchanged

### Non-Goals

- **Not a general "pipe to any function"**: `->>` is specifically for providing a closure argument to a partial function call. For ordinary function application, `|>` already works.
- **Not a standalone expression**: `->> body` without a preceding partial call is a compile error
- **No implicit function inference**: `->>` does not guess which function to call — the LHS must be an explicit (possibly partial) function invocation
- **No runtime closure type**: the desugaring is purely syntactic — the same AST nodes are produced as if the user wrote an explicit closure

---

## 3. Target Syntax

### 3.1 Basic Polyphony

```akkado
// Current:
pat("C4' Am7' G4' F4'")
  |> poly(%, (freq, gate, vel) -> saw(freq) * ar(gate) * vel)
  |> out(%, %)

// With ->>:
pat("C4' Am7' G4' F4'")
  |> poly(%) ->> saw(%.freq) * ar(%.gate) * %.vel
  |> out(%, %)
```

The compiler knows `poly()` expects `(freq, gate, vel)` from its builtin definition. Inside the `->>` body:
- `%.freq` → direct reference to the `freq` parameter
- `%.gate` → direct reference to the `gate` parameter (held high with a 1-sample drop at every event onset; use `.trig` if you want a 1-sample pulse instead — see mini-notation-reference §Event Fields for the trigger-vs-gate distinction)
- `%.vel` → direct reference to the `vel` parameter

### 3.2 Mono / Legato

```akkado
// mono with piped pattern (2-arg form)
pat("c4 e4 g4")
  |> mono(%) ->> saw(%.freq) * ar(%.gate)
  |> out(%, %)

// legato with piped pattern
pat("c4 e4 g4")
  |> legato(%) ->> saw(%.freq) * ar(%.gate)
  |> out(%, %)
```

> The 1-arg closure-only form (`mono((freq,gate,vel) -> …)`) requires writing the closure explicitly — `->>` always needs a function call to the **left** (per the Non-Goal in §2). Writing `mono(->> body)` is a compile error.

### 3.3 Pipes Inside the Closure Body (Parens)

When the closure body itself contains pipe chains, wrap it in `(...)` to disambiguate from the outer pipeline. Inside this grouped body, `%` follows context-dependent rules relative to `|>`:

- `%.freq` **before** `|>` → closure param `freq`
- `%` **after** `|>` → standard pipe hole (filled by the pipe input)

```akkado
// Current:
pat("C4'")
  |> poly(%, (freq, gate, vel) ->
       saw(freq) |> lp(freq, 2000 * adsr(gate)) * vel
     )
  |> out(%, %)

// With ->> (parens):
pat("C4'")
  |> poly(%) ->> (saw(%.freq) |> lp(%, 2000 * adsr(%.gate)) * %.vel)
  |> out(%, %)
```

Here `%.freq` (before `|>`) resolves to the closure param `freq`, while `%` in `lp(%, 2000)` (after `|>`) is filled by `saw(freq)` output — following standard pipe semantics. The parens explicitly scope the pipe chain to the closure body. The outer `|> out(%)` attaches to the `poly()` result, not the inner pipe chain.

### 3.4 Pipes Inside the Closure Body (Braces)

Braces `{...}` are an alternative grouping for multi-line or visually distinct closure bodies:

```akkado
pat("C4'")
  |> poly(%) ->> {
       saw(%.freq)
       |> lp(%, 2000 * adsr(%.gate))
       * %.vel
     }
  |> out(%, %)
```

Semantically identical to the parens form; the choice is stylistic.

### 3.5 tap_delay

```akkado
// Post-migration signature: tap_delay(in, time, fb, dry, wet, processor)
// Pre-migration call (today): tap_delay(sig, 0.5, 0.3, (x) -> x * 0.5) * 0.7

// With ->>:
tap_delay(sig, 0.5, 0.3) ->> % * 0.5
```

For single-param closures, bare `%` (without field access) refers to the single parameter directly. Optional `dry`/`wet` keep their defaults; users can supply them positionally before `->>`:

```akkado
tap_delay(sig, 0.5, 0.3, 0.2, 0.8) ->> % * 0.5  // dry=0.2, wet=0.8
```

### 3.6 Array Operations

```akkado
// Post-migration signatures: map(array, fn), reduce(array, init, fn), zipWith(a, b, fn)
// Pre-migration reduce signature (today): reduce(array, fn, init) — closure NOT last
// Pre-migration explicit calls:
map([1, 2, 3], (x) -> x * 2)
reduce([1, 2, 3], (a, b) -> a + b, 0)

// Post-migration explicit calls:
map([1, 2, 3], (x) -> x * 2)
reduce([1, 2, 3], 0, (acc, x) -> acc + x)

// With ->>:
map([1, 2, 3]) ->> %.x * 2
reduce([1, 2, 3], 0) ->> %.acc + %.x
zipWith([1, 2], [3, 4]) ->> %.a + %.b
```

Closure params are accessed exclusively via record-style `%.field` syntax. The available field names come from the builtin's `closure_param_names` metadata (see §4.3). For single-param closures, bare `%` is also accepted as shorthand.

### 3.7 `as` Binding Inside the Closure Body

The `as` binding works naturally inside the closure body, just as it does in any expression:

```akkado
pat("C4'")
  |> poly(%) ->>
       saw(%.freq) as snd
       |> lp(snd, 2000 * adsr(%.gate))
       * %.vel
  |> out(%, %)
```

### 3.8 Chaining with `|>`

Because `->>` binds tighter than `|>`, the outer pipeline operates on the result of the completed function call:

```akkado
// Precedence:
// (poly(pat("C4'")) ->> body) |> out(%, %)
pat("C4'") |> poly(%) ->> saw(%.freq) * ar(%.gate) * %.vel |> out(%, %)

// This parses (post-Phase-1.5 signature: poly(input, voices=64, instrument)) as:
// let __result = poly(pat("C4'"), instrument: (freq, gate, vel) -> saw(freq) * ar(gate) * vel)
// out(__result, __result)
// Note: `instrument` is inserted as a NAMED arg so prior named/positional args route correctly.
```

---

## 4. Desugaring Semantics

### 4.1 The Transformation

```
fn(<positional_args>) ->> <body>
  ⇒
fn(<positional_args>, <closure_param_name>: (<params>) -> <body_with_holes_resolved>)

fn(<positional_args>, name1: v1, name2: v2) ->> <body>
  ⇒
fn(<positional_args>, name1: v1, name2: v2, <closure_param_name>: (<params>) -> <body_with_holes_resolved>)
```

Where:
1. `<closure_param_name>` is the builtin's named slot whose type is `Function` (e.g., `instrument` for `poly`, `fn` for `reduce`/`map`/`zipWith`, `processor` for `tap_delay`). After Phase 1.5 this is always the **last** positional slot, but the desugaring inserts as a **named argument** so the closure routes correctly even when prior args were named (see §9.2).
2. `<params>` are determined from `BuiltinInfo.closure_param_names` (see §4.3)
3. `<body>` is scanned for `%`/`@` references
4. `%.field` → direct reference to parameter named `field`
5. `%` (bare, no field) → the single parameter for 1-param closures; for multi-param closures, resolves to the **first** param and emits **W150** (see §5.5 / §9.4)
6. `@` works identically to `%` throughout

### 4.2 `%` Semantics in the Closure Body with Pipes

Inside the `->>` body, `%` follows a **context-dependent** rule relative to `|>`:

- **Before any `|>` in the closure body**: `%` refers to the closure parameter (the event record). `%.freq` accesses the `freq` field.
- **After a `|>` in the closure body**: `%` follows standard pipe-hole semantics — it is replaced by the value flowing through the pipe.

```akkado
poly(pat) ->> (saw(%.freq) |> lp(%, 2000))
```

Desugaring:
1. `%.freq` (before `|>`) → `freq` (closure param reference)
2. `%` in `lp(%, 2000)` (after `|>`) → pipe hole, filled by the output of `saw(freq)`
3. Result: `(freq, gate, vel) -> saw(freq) |> lp(%, 2000)`

To use `%` as the closure param deeper in a pipe chain, use `as` to bind it before the pipe:

```akkado
poly(pat) ->> (%.freq as f |> saw(f) |> lp(%, 2000))
```

### 4.3 Parameter Name Resolution

The compiler determines the closure's parameter names from the function definition:

**Builtins:** Add a new field to `BuiltinInfo` (e.g., `closure_param_names`) that lists the expected closure parameter names. For `poly`: `{"freq", "gate", "vel"}`. For `tap_delay`: `{"x"}`.

> **Scope of `closure_param_names`:** This metadata only governs `->>` desugaring. Users writing an explicit closure keep full naming freedom — `tap_delay(s, t, fb, (sample) -> sample * 0.5)` continues to work with `sample`, not `x`. The metadata is consulted only when `->>` is the operator and the compiler must synthesize the param list itself.

**User-defined functions:** The closure parameter names are the function's parameter names themselves. When a user writes:
```akkado
fn my_hof(a, b, fn): fn(a, b)
```

The last parameter type is `Function`, and its signature is determined by the function's own parameter expectations (future work — MVP focuses on builtins).

### 4.4 Body Parsing

The RHS of `->>` is parsed at `Precedence::Or` — that is, the parser captures everything down to (but not including) the outer `|>`. This is broader than the existing `|>` RHS (which parses at `Precedence::Addition`) so that comparisons (`==`, `<`), logicals (`&&`, `||`), and arithmetic all flow into the closure body without parens.

| Operator class | Captured by RHS? |
|---|---|
| Method/Call/Primary (`. ()`) | Yes |
| Unary (`!`, `-`) | Yes |
| Power, Multiplication, Addition (`^ * / + -`) | Yes |
| Comparison, Equality (`< > == !=`) | Yes |
| Logical (`&& \|\|`) | Yes |
| Pipe (`\|>`) | **No** — terminates the body |

When the user wraps the body in `(...)` or `{...}`, the parser enters a delimited sub-expression that can contain its own `|>` operators.

```
// Without grouping: RHS stops at the next outer |>
poly(%) ->> saw(%.freq) * %.vel |> out(%)
// parses as: (poly(%) ->> saw(%.freq) * %.vel) |> out(%)

// Comparisons captured by the body (no parens needed):
poly(%) ->> saw(%.freq) * (%.vel > 0.5 ? 1.0 : 0.5) |> out(%)

// With parens: RHS captures everything including pipes
poly(%) ->> (saw(%.freq) |> lp(%, 2000)) |> out(%)
// RHS = "(saw(%.freq) |> lp(%, 2000))"
// poly(%) ->> (saw(%.freq) |> lp(%, 2000))  →  poly(%, instrument: (freq, gate, vel) -> saw(freq) |> lp(freq, 2000))
```

---

## 5. Architecture / Technical Design

### 5.1 Lexer

Add a new token type:

```cpp
// token.hpp
ArrowPipe,      // ->>
```

Lexing rule: When the lexer sees `-` followed by `>`, it checks if a third `>` follows. If so, emit `TokenType::ArrowPipe` (three chars). If not, emit `TokenType::Arrow` (two chars).

```
// lexer.cpp
case '-':
    if (peek() == '>') {
        advance();
        if (peek() == '>') {
            advance();
            return make_token(TokenType::ArrowPipe);
        }
        return make_token(TokenType::Arrow);
    }
    // ... rest of minus handling
```

### 5.2 Parser

Add `->>` as a binary infix operator at a new `PipeArg` precedence level inserted between `Pipe` (the existing lowest level) and `Or`:

| Operator | Precedence Level (existing values) |
|----------|-----------------|
| `\|>` | `Pipe` (1, lowest) |
| `->>` | `PipeArg` (new, slotted at value 2 — `Pipe < PipeArg < Or`) |
| `\|\|` | `Or` (now value 3) |
| `&&` | `And` |
| `==` `!=` | `Equality` |
| `< > <= >=` | `Comparison` |
| `+` `-` | `Addition` |
| `*` `/` | `Multiplication` |
| `^` | `Power` |
| `!` (prefix) | `Unary` |
| `.method()` | `Method` |
| `f()`, `[]`, `.` | `Call` |
| literals | `Primary` (highest) |

> Inserting `PipeArg` shifts every existing precedence value up by one. Parser code that compares precedence by value (e.g., the loop at `parser.cpp:459`) must be audited for hard-coded numeric thresholds during Phase 1.

Parsing rule for `->>`:

1. The LHS must be a function call (detected as a `Call` expression node). Non-call LHS is an error (E500).
2. Parse the RHS at `Precedence::Or` (broader than `|>`'s RHS — see §4.4).
3. Create a new AST node `ClosurePipe` with children: the call node and the body node.

During semantic analysis (or a dedicated desugaring pass), `ClosurePipe` is rewritten:

```
ClosurePipe
  ├── Call(fn_name, [arg_0, arg_1, ...])  // LHS: partial call
  └── body_expr                            // RHS: closure body
  ⇒
Call(fn_name, [arg_0, arg_1, ..., NamedArg(<closure_slot_name>, Closure([param_0, param_1, ...], body_expr))])
```

Where:
- The `Closure` node is constructed synthetically with the correct parameter list
- The synthesized argument is a **named** argument keyed on the builtin's closure-typed slot, so that `->>` works correctly even when prior args were named (see §9.2)
- All `%`/`@` references in `body_expr` are rewritten to the corresponding parameter identifiers
- `%.freq` → `Identifier("freq")` (if `freq` is one of the closure params)
- Bare `%` → `Identifier(first_param_name)` (for single-param closures, or first param + W150 for multi-param)

### 5.3 BuiltinInfo Extension

Add closure parameter metadata to `BuiltinInfo`:

```cpp
struct BuiltinInfo {
    // ... existing fields ...

    // Closure parameter names for the function parameter marked as Function type.
    // Empty = no closure signature metadata (params are user-defined at call site).
    std::array<std::string_view, MAX_CLOSURE_PARAMS> closure_param_names = {};
    std::uint8_t closure_param_count = 0;
};
```

Example definitions (post-migration ordering — closure is in the **last** positional slot):

```cpp
// poly  (signature: input, voices, instrument)
{"poly", {cedar::Opcode::NOP, 3, 0, true,
          {"input", "voices", "instrument", "", "", ""},
          {NAN, 64.0f, NAN, NAN, NAN},
          "Polyphonic voice manager...",
          .closure_param_names = {"freq", "gate", "vel"},
          .closure_param_count = 3}},

// tap_delay  (signature: in, time, fb, dry, wet, processor)
{"tap_delay", {cedar::Opcode::DELAY_TAP, 6, 0, true,
               {"in", "time", "fb", "dry", "wet", "processor"},
               {NAN, NAN, NAN, 0.0f, 1.0f, NAN},
               "Tap delay with feedback chain...",
               .closure_param_names = {"x"},
               .closure_param_count = 1}},
```

> The exact `required_count` / `optional_count` numbers above illustrate the slot layout — Phase 1.5 owns the actual `BuiltinInfo` rewrite and may keep optional defaults using a different shape (e.g., remaining `(4, 2, …)` with reordered name list). The invariant is: **closure slot is last positional**.

### 5.4 Analyzer / Desugaring Pass

The desugaring can happen either:

**Option A — During semantic analysis** (recommended for MVP):
When the analyzer encounters a `ClosurePipe` node:
1. Look up the function being called (by name) in the builtin table
2. Read `closure_param_count`, `closure_param_names`, and the **closure slot name** (the param marked as `Function` type) from the definition
3. Create a synthetic `Closure` node with those params
4. Walk the body expression, replacing `%`/`@` hole references with the corresponding `Identifier` nodes
5. Replace the `ClosurePipe` node with a `Call` node whose child list is the original positional/named args **plus** a new `NamedArg(<closure_slot_name>, synthetic_closure)`

**Option B — During codegen**:
Handle `ClosurePipe` directly in the codegen dispatcher, creating the closure structure at codegen time without an intermediate AST rewrite.

### 5.5 Validation

| Condition | Code | Type | Message |
|-----------|------|------|---------|
| LHS is not a call expression | E500 | Error | `->>` left side must be a function call, got <type> |
| LHS call has no closure slot | E501 | Error | `<fn>()` does not accept a closure argument |
| `%.field` not in closure params | E502 | Error | `%` has no field `<field>` in closure signature for `<fn>()`. Available params: <list> |
| `%` used bare but closure has >1 param | W150 | Warning | Ambiguous bare `%` reference — resolves to first param `<name>`. Use `%.<param_name>` to be explicit |
| `->>` used outside function call context | E504 | Error | `->>` operator requires a function call on the left |

---

## 6. Impact Assessment

| Component | Status | Notes |
|-----------|--------|-------|
| Lexer | **Modified** | New `TokenType::ArrowPipe`, 3-char lexing rule |
| Token enum | **Modified** | New `ArrowPipe` token type |
| Parser | **Modified** | New precedence level `PipeArg`; new `parse_closure_pipe()` handler; new `NodeType::ClosurePipe` |
| AST | **Modified** | New `NodeType::ClosurePipe` (or reuses existing `Pipe` with flag) |
| Semantic Analyzer | **Modified** | ClosurePipe desugaring pass (walk body, replace holes, build synthetic Closure) |
| `BuiltinInfo` | **Modified** | New `closure_param_names` and `closure_param_count` fields |
| Codegen handlers (`handle_poly_call`, `handle_reduce_call`, `handle_tap_delay_call`) | **Modified** | Phase 1.5 reorders builtin signatures so the closure occupies the last positional slot. Each handler's `extract_call_args` arity, arg-index reads, and error messages must update accordingly. |
| Codegen (post-desugaring path) | **Stays** | After Phase 3 desugaring, codegen sees standard `Call` + `Closure` nodes — no `ClosurePipe`-specific changes needed |
| Builtin definitions (`builtins.hpp`) | **Modified** | Phase 1.5: reorder `poly`, `reduce`, `tap_delay` family parameter lists. Phase 2: add `closure_param_names` / `closure_param_count` to every closure-taking builtin |
| Existing call sites (tests, examples, web docs, experiments) | **Modified** | Phase 1.5: every existing call to a reordered builtin must update positional args. Named-arg calls (`reduce(arr, fn: f, init: 0)`) keep working. |
| User functions with closure params | **Stays** | Future work — MVP covers builtins only |
| Existing closure syntax | **Stays** | `(params) -> body` continues to work unchanged |
| Pipe operator `|>` | **Stays** | No changes to `|>` semantics |

---

## 7. File-Level Changes

### Files to Modify

| File | Phase | Change |
|------|-------|--------|
| `akkado/include/akkado/builtins.hpp` | **1.5** | Reorder `poly` to `(input, voices, instrument)`, `reduce` to `(array, init, fn)`, `tap_delay`/`tap_delay_ms`/`tap_delay_smp` to `(in, time, fb, dry, wet, processor)`. Update `param_names`, defaults, `required_count`/`optional_count`. |
| `akkado/src/codegen_functions.cpp` | **1.5** | `handle_poly_call`: read `instrument` from slot 2 (after voices) instead of slot 1. `handle_tap_delay_call`: read `processor` (now slot 5) instead of slot 3. Update E400/E301 error messages. |
| `akkado/src/codegen_arrays.cpp` | **1.5** | `handle_reduce_call`: read `init` from slot 1 and `fn` from slot 2 (currently `fn` slot 1, `init` slot 2). Update E141/E142 messages. |
| Test fixtures and Akkado example files | **1.5** | Update every call to `poly`, `reduce`, `tap_delay*` that uses positional ordering. Named-arg calls are unaffected. |
| `web/static/docs/**/*.md` | **1.5** | Update reference docs for `poly`, `reduce`, `tap_delay*` to reflect new positional order. |
| `experiments/test_op_*.py` (poly, reduce, tap_delay drivers) | **1.5** | If any Python harness drives these via `make_*nary` with positional inputs, update buffer-index ordering. |
| `akkado/include/akkado/token.hpp` | 1 | Add `ArrowPipe` to `TokenType` enum; add `"ArrowPipe"` to `token_type_name()` |
| `akkado/src/lexer.cpp` | 1 | Add 3-char `->>` lexing rule before `->` Arrow rule |
| `akkado/include/akkado/parser.hpp` | 1 | Insert `PipeArg` into the `Precedence` enum between `Pipe` and `Or` (shifts subsequent values) |
| `akkado/include/akkado/ast.hpp` | 1 | Add `NodeType::ClosurePipe`; possibly add `ClosurePipeData` struct if needed for metadata |
| `akkado/src/parser.cpp` | 1 | Add `parse_closure_pipe()`; register `->>` infix at `PipeArg` precedence; audit hard-coded numeric precedence comparisons (e.g., `parser.cpp:459`) for the enum shift |
| `akkado/include/akkado/parser.hpp` | 1 | Add `parse_closure_pipe()` declaration |
| `akkado/src/analyzer.cpp` | 3 | Add closure-pipe desugaring pass: `ClosurePipe` → `Call` + `NamedArg(closure_slot, Closure(...))` with hole substitution |
| `akkado/include/akkado/builtins.hpp` | 2 | Add `closure_param_names`, `closure_param_count`, and a way to identify the closure-typed slot (e.g., a `ParamValueType::Function` annotation in `param_types`); populate for `poly`, `mono`, `legato`, `tap_delay` family, `map`, `reduce`, `zipWith` |
| `akkado/src/codegen.cpp` | 3 (optional) | (Only if desugaring is at codegen time instead of analysis) add `ClosurePipe` handler |

### Files That Stay

| File | Reason |
|------|--------|
| `cedar/**/*` | No Cedar VM changes — desugaring and signature reorder are purely in the Akkado compiler |
| `tools/**/*` | No CLI tool changes |

---

## 8. Implementation Phases

### Phase 1.5: Builtin Signature Migration (prerequisite — closure must be the last positional slot)

`->>` desugaring relies on a uniform invariant: the closure parameter is always the **last positional slot** of the call. Today, three builtins violate this (closure followed by trailing non-closure params). This phase reorders them and migrates every call site so Phases 1–4 can proceed against a clean invariant.

**Builtin signature changes:**

| Builtin | Before | After |
|---------|--------|-------|
| `poly` | `(input, instrument, voices=64)` | `(input, voices=64, instrument)` |
| `reduce` | `(array, fn, init)` | `(array, init, fn)` |
| `tap_delay` | `(in, time, fb, processor, dry=0, wet=1)` | `(in, time, fb, dry=0, wet=1, processor)` |
| `tap_delay_ms` | same shape as `tap_delay` | same shape as `tap_delay` |
| `tap_delay_smp` | same shape as `tap_delay` | same shape as `tap_delay` |

`mono`, `legato`, `map`, `zipWith` already place the closure last and are not affected.

**Per-handler updates:**

- `handle_poly_call` (`codegen_functions.cpp:1885+`): swap `args.nodes[1]` (instrument) and `args.nodes[2]` (voices). Keep voice-count literal extraction; only the slot index moves.
- `handle_tap_delay_call` (`codegen_functions.cpp:1704+`): closure is now `args.nodes[5]`, with `dry`=`args.nodes[3]` and `wet`=`args.nodes[4]`. Update validation and message text.
- `handle_reduce_call` (`codegen_arrays.cpp` reduce path): `init`=`args.nodes[1]`, `fn`=`args.nodes[2]`.
- Update existing E301/E303/E400/E141 error messages to reflect the new ordering.

**Migration scope (every positional call site):**

- `akkado/tests/**` — fixture files using the affected builtins
- Akkado example/tutorial files (in repo + `web/static/docs`)
- `experiments/test_op_*.py` — only if any harness drives `poly`/`reduce`/`tap_delay*` via positional input arrays; named buffer wiring (e.g. `set_param`) is unaffected
- Any internal codegen helpers that emit synthetic positional calls to the renamed slots

**Tests:**

- Existing tests that explicitly used the old ordering must be migrated and re-run. Tests passing only via named arguments (`reduce(arr, fn: f, init: 0)`) need no change.
- New regression test: an explicit pre-`->>` call in the new ordering compiles and produces the same bytecode shape as the equivalent named-arg call.

**Exit criteria:** All builtin call sites pass against the new signatures; full test suite green; no remaining references to the old positional ordering anywhere in the repo.

### Phase 1: Lexer + Parser

- Add `TokenType::ArrowPipe`
- Add 3-char `->>` lexing rule (3-char must be matched before 2-char `->`)
- Insert `Precedence::PipeArg` between `Pipe` and `Or` in the precedence enum (audit any code that compared precedence values numerically — see §5.2)
- Add `NodeType::ClosurePipe` to AST
- Add `parse_closure_pipe()` in parser; register at `PipeArg` precedence; parse RHS at `Precedence::Or`
- Validate LHS is a call expression (error E500)
- **Test**: `"poly(x) ->> y"` parses to `ClosurePipe(Call("poly", ["x"]), Identifier("y"))`
- **Test**: `"x ->> y"` (LHS is identifier, not a call) → E500
- **Test**: `"poly(x) ->> a == 1"` parses with `a == 1` inside the closure body (RHS captures the comparison)

### Phase 2: BuiltinInfo Metadata

- Add `closure_param_names`, `closure_param_count`, and closure-slot identification (e.g. `ParamValueType::Function` annotation) to `BuiltinInfo`
- Populate for every closure-taking builtin (closure-slot name in **bold**):

  | Builtin | Closure slot | Closure params |
  |---------|--------------|----------------|
  | `poly` | **`instrument`** (last after Phase 1.5) | `freq`, `gate`, `vel` |
  | `mono` | **`instrument`** (last) | `freq`, `gate`, `vel` |
  | `legato` | **`instrument`** (last) | `freq`, `gate`, `vel` |
  | `tap_delay` | **`processor`** (last after Phase 1.5) | `x` |
  | `tap_delay_ms` | **`processor`** (last after Phase 1.5) | `x` |
  | `tap_delay_smp` | **`processor`** (last after Phase 1.5) | `x` |
  | `map` | **`fn`** (last) | `x` |
  | `reduce` | **`fn`** (last after Phase 1.5) | `acc`, `x` |
  | `zipWith` | **`fn`** (last) | `a`, `b` |

- **Test**: Verify each builtin's metadata is correct (closure-slot name resolves to a `Function`-typed parameter, `closure_param_count` matches the number of names)

### Phase 3: Desugaring (Analysis Pass)

- Implement the `ClosurePipe` → `Call` + `NamedArg(closure_slot, Closure)` rewrite in the analyzer
- Walk the body tree, find all `%`/`@` hole references
- For `%.field`: resolve to parameter name; error if field not in closure params (E502)
- For bare `%` with single-param closure: resolve to the one param
- For bare `%` with multi-param closure: emit **warning W150** and resolve to the first param
- Append the synthetic `Closure` as a **named argument** keyed on the builtin's closure-slot name (so `->>` works correctly with prior named args — see §9.2)
- **Test**: `poly(pat) ->> saw(%.freq)` → `poly(pat, instrument: (freq, gate, vel) -> saw(freq))`
- **Test**: `tap_delay(sig, 0.5, 0.3) ->> % * 0.5` → `tap_delay(sig, 0.5, 0.3, processor: (x) -> x * 0.5)` (with `dry`/`wet` defaulted)
- **Test**: `poly(input: pat) ->> saw(%.freq)` → `poly(input: pat, instrument: …)` — works with mixed named + positional

### Phase 4: Integration Tests

- Full compilation tests with real codegen output
- Test poly with `->>`, mono, legato, tap_delay, map, reduce
- Test pipe chains inside parens/braces
- Test `as` binding inside closure body
- Test error cases

### Phase 5 (Future): User-Defined Functions

- Extend the `->>` desugaring to work with user-defined functions that accept closures
- The function definition must declare its closure parameter signature
- This requires encoding closure param info in the function symbol table, not just `BuiltinInfo`

---

## 9. Edge Cases

### 9.1 Nested `->>` Calls

```akkado
outer(%.a) ->> inner(%.b) ->> body  // Invalid: closure body cannot contain another ->>
```

**Expected:** Compile error — `->>` bodies cannot contain another `->>` operator. Users should use explicit closures for nested cases.

### 9.2 `->>` with Named Arguments

```akkado
poly(input: pat("C4'")) ->> saw(%.freq) * ar(%.gate) * %.vel
```

**Expected:** Works correctly. The desugaring inserts the closure as a **named argument** keyed on the builtin's closure-slot name (`instrument` for `poly`). The result is `poly(input: pat("C4'"), instrument: (freq, gate, vel) -> saw(freq) * ar(gate) * vel)`. The remaining unfilled positional slot (`voices`) keeps its default. This works regardless of which prior args were named or positional.

### 9.3 `->>` with Optional Arguments

After Phase 1.5, all closure-taking builtins put the closure in the **last** positional slot. Optional arguments (e.g., `tap_delay`'s `dry`/`wet`, `poly`'s `voices`) sit *before* the closure.

```akkado
// tap_delay post-migration: (in, time, fb, dry=0, wet=1, processor)
tap_delay(sig, 0.5, 0.3) ->> % * 0.5
// → tap_delay(sig, 0.5, 0.3, processor: (x) -> x * 0.5)  with dry/wet defaulted

// Override dry/wet positionally before ->>:
tap_delay(sig, 0.5, 0.3, 0.2, 0.8) ->> % * 0.5

// Or by name:
tap_delay(sig, 0.5, 0.3, dry: 0.2, wet: 0.8) ->> % * 0.5
```

**Expected:** Optional args before the closure slot fill via positional or named syntax as usual; `->>` only synthesizes the closure-slot argument.

### 9.4 Bare `%` with Multi-Param Closure

```akkado
poly(pat("C4'")) ->> %  // Bare % with multi-param closure
```

**Expected:** Warning W150. Bare `%` resolves to the first closure param (`freq` for poly). The compiled output is equivalent to writing `%.freq`. Users who intended a different param or who want to be explicit should use `%.<name>` syntax. This is a warning, not an error — the code compiles and runs correctly.

### 9.5 `->>` on a Non-Closure-Taking Function

```akkado
out(%) ->> saw(%.freq)  // out() doesn't take a closure
```

**Expected:** Error E501 — `out()` does not accept a closure argument.

### 9.6 `->>` with Zero-Argument Function

```akkado
myFn() ->> body
```

**Expected:** Works. The closure is the function's first and only argument. `myFn` must accept a function/closure as its sole parameter.

### 9.7 `->>` as Standalone Expression (No LHS Call)

```akkado
x ->> body  // x is not a call
```

**Expected:** Error E500 — LHS must be a function call.

### 9.8 Mismatched Hole Usage Count

```akkado
poly(pat) ->> saw(%.freq)                        // Uses only 1 of 3 params
poly(pat) ->> saw(%.freq) * ar(%.gate) * %.vel  // Uses all 3
```

**Expected:** Both valid. The closure signature always includes all 3 params regardless of how many are referenced in the body. Unused params are bound but not referenced — codegen handles them the same way.

---

## 10. Testing Strategy

### 10.1 Parser Tests

```cpp
// Token recognition
CHECK(lex("->>") == TokenType::ArrowPipe);
CHECK(lex("->")  != TokenType::ArrowPipe);  // plain arrow is separate

// AST construction
auto r = parse("poly(x) ->> y");
CHECK(r.node_type == NodeType::ClosurePipe);
CHECK(r.children[0].type == NodeType::Call);  // LHS call

// Error: LHS not a call
auto r2 = parse("x ->> y");
CHECK_FALSE(r2.success);  // E500

// Parens grouping
auto r3 = parse("poly(x) ->> (y |> z)");
CHECK(r3.success);
```

### 10.2 Desugaring Tests

```cpp
// Basic poly desugaring
auto r = compile_and_desugar("poly(pat) ->> saw(%.freq)");
auto closure = r.find_node(NodeType::Closure);
CHECK(closure.params.size() == 3);
CHECK(closure.params[0].name == "freq");
CHECK(closure.params[1].name == "gate");
CHECK(closure.params[2].name == "vel");
CHECK(closure.body.contains(Identifier("freq")));

// Single-param tap_delay
auto r2 = compile_and_desugar("tap_delay(sig, 0.5, 0.3) ->> % * 0.5");
auto closure2 = r2.find_node(NodeType::Closure);
CHECK(closure2.params.size() == 1);
CHECK(closure2.params[0].name == "x");

// Pipe chain inside parens — % before |> is closure param, after |> is pipe hole
auto r3 = compile_and_desugar("poly(pat) ->> (saw(%.freq) |> lp(%, 2000))");
// body: (freq, gate, vel) -> saw(freq) |> lp(%, 2000)
// %.freq before |> → Identifier("freq")
// % after |> → pipe hole (standard semantics)

// % after pipe still works as pipe hole
auto r3b = compile_and_desugar("poly(pat) ->> (saw(%.freq) * %.vel |> lp(%, 2000))");
// %.freq and %.vel before |> → closure params
// % after |> → pipe hole

// as binding inside body
auto r4 = compile_and_desugar("poly(pat) ->> saw(%.freq) as snd |> lp(snd, 2000)");
CHECK(r4.success);
```

### 10.3 Error Tests

```cpp
// E500: LHS not a call
CHECK(has_error(compile("x ->> y"), "E500"));

// E501: Function doesn't take closure
CHECK(has_error(compile("out(%) ->> saw(%.freq)"), "E501"));

// E502: Field not in closure params
CHECK(has_error(compile("poly(pat) ->> saw(%.nonexistent)"), "E502"));

// W150: Bare % with multi-param closure (warning only)
auto r = compile("poly(pat) ->> saw(%)");
CHECK(r.success);
CHECK(has_warning(r, "W150"));
```

### 10.4 Integration Tests

```cpp
// Full poly compilation with ->>
auto r = compile("pat(\"C4'\") |> poly(%) ->> saw(%.freq) * ar(%.gate) * %.vel |> out(%, %)");
CHECK(r.success);

// Compare bytecode with explicit closure version (post-Phase-1.5 ordering: closure last)
auto r_explicit = compile("pat(\"C4'\") |> poly(%, (freq, gate, vel) -> saw(freq) * ar(gate) * vel) |> out(%, %)");
CHECK(r.bytecode == r_explicit.bytecode);

// tap_delay with ->> (closure now last after dry/wet)
auto r2 = compile("tap_delay(sig, 0.5, 0.3) ->> % * 0.5");
CHECK(r2.success);

// reduce with ->> (closure last after Phase 1.5: reduce(array, init, fn))
auto r2b = compile("reduce([1, 2, 3], 0) ->> %.acc + %.x");
CHECK(r2b.success);

// map with ->>
auto r3 = compile("map([1, 2, 3]) ->> %.x * 2");
CHECK(r3.success);

// Named-arg interaction: closure inserts as named arg
auto r4 = compile("poly(input: pat(\"C4'\")) ->> saw(%.freq) * ar(%.gate) * %.vel");
CHECK(r4.success);
```

### 10.5 Build & Run

```bash
cmake --build build --target akkado_tests
./build/akkado/tests/akkado_tests "[closure-pipe]"
./build/akkado/tests/akkado_tests "[parser]"
./build/akkado/tests/akkado_tests "[codegen]"
```

---

## 11. Future Work

- **User-defined functions with closure params**: Support `->>` with non-builtin functions that accept closures. Requires encoding closure param signatures in the user function symbol table.
- **`@` as primary hole in `->>` context**: Some users may prefer `@.freq` over `%.freq` for visual distinction from pipe holes.
- **Multiple `->>` in argument list**: Currently only the last argument can be provided via `->>`. Future work could allow `->>` for other argument positions, though this is complex and unlikely to be needed.
