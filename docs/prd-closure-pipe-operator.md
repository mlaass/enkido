> **Status: NOT STARTED** — Draft PRD for review

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

Several builtins accept a closure as their last argument today:

| Builtin | Closure Signature | Param Names |
|---------|------------------|-------------|
| `poly(input, instrument, voices=64)` | 3 params | `freq`, `gate`, `vel` |
| `mono(instrument)` / `mono(input, instrument)` | 3 params | `freq`, `gate`, `vel` |
| `legato(instrument)` / `legato(input, instrument)` | 3 params | `freq`, `gate`, `vel` |
| `tap_delay(in, time, fb, processor)` | 1 param | `x` (or user-named) |
| `map(array, fn)` | 1 param | user-named |
| `reduce(array, fn, init)` | 2 params | user-named |
| `zipWith(a, b, fn)` | 2 params | user-named |

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
- `%.gate` → direct reference to the `gate` parameter
- `%.vel` → direct reference to the `vel` parameter

### 3.2 Mono / Legato

```akkado
// mono with inline ->> (1-arg form: no separate input)
pat("c4 e4 g4")
  |> mono(->> saw(%.freq) * ar(%.gate))
  |> out(%, %)

// legato with explicit pattern
pat("c4 e4 g4")
  |> legato(%) ->> saw(%.freq) * ar(%.gate)
  |> out(%, %)
```

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
  |> poly(%) ->>
       saw(%.freq)
       |> lp(%, 2000 * adsr(%.gate))
       * %.vel
  |> out(%, %)
```

Semantically identical to the parens form; the choice is stylistic.

### 3.5 tap_delay

```akkado
// Current:
tap_delay(sig, 0.5, 0.3, (x) -> x * 0.5) * 0.7

// With ->>:
tap_delay(sig, 0.5, 0.3) ->> % * 0.5
```

For single-param closures, bare `%` (without field access) refers to the single parameter directly.

### 3.6 Array Operations

```akkado
// Current:
map([1, 2, 3], (x) -> x * 2)
reduce([1, 2, 3], (a, b) -> a + b, 0)

// With ->>:
map([1, 2, 3]) ->> % * 2
reduce([1, 2, 3], 0) ->> %a + %b
```

For multi-param closures, `%a`, `%b` (or `%.a`, `%.b` with record-style access) map to `a`, `b` respectively. The exact field-access syntax is determined by the function's closure parameter names.

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

// This parses as:
// let __result = poly(pat("C4'"), (freq, gate, vel) -> saw(freq) * ar(gate) * vel)
// out(__result, __result)
```

---

## 4. Desugaring Semantics

### 4.1 The Transformation

```
fn(<required_args>) ->> <body>
  ⇒
fn(<required_args>, (<params>) -> <body_with_holes_resolved>)

fn(<required_args>, <optional_args>) ->> <body>
  ⇒
fn(<required_args>, <optional_args>, (<params>) -> <body_with_holes_resolved>)
```

Where:
1. `<params>` are determined by looking up the function's closure parameter signature
2. `<body>` is scanned for `%`/`@` references
3. `%.field` → direct reference to parameter named `field`
4. `%` (bare, no field) → the single parameter (for 1-param closures); issues a warning with multi-param closures
5. `@` works identically to `%` throughout

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

**User-defined functions:** The closure parameter names are the function's parameter names themselves. When a user writes:
```akkado
fn my_hof(a, b, fn): fn(a, b)
```

The last parameter type is `Function`, and its signature is determined by the function's own parameter expectations (future work — MVP focuses on builtins).

### 4.4 Body Parsing

The RHS of `->>` is parsed at `Precedence::Addition` (same as the RHS of `|>`), which means it captures everything up to the next `|>` at the outer level. When the user wraps the body in `(...)` or `{...}`, the parser enters a delimited sub-expression that can contain its own `|>` operators.

```
// Without grouping: RHS stops at next |>
poly(%) ->> saw(%.freq) * %.vel |> out(%)      // RHS = "saw(%.freq) * %.vel"
// (poly(%) ->> saw(%.freq) * %.vel) |> out(%)  // ->> binds tighter

// With parens: RHS captures everything including pipes
poly(%) ->> (saw(%.freq) |> lp(%, 2000)) |> out(%)
// RHS = "(saw(%.freq) |> lp(%, 2000))"
// poly(%) ->> (saw(%.freq) |> lp(%, 2000))  →  poly(%, (freq, gate, vel) -> saw(freq) |> lp(freq, 2000))
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

Add `->>` as a binary infix operator with precedence between `Precedence::Call` (member access / calls) and `Precedence::Pipe`:

| Operator | Precedence Level |
|----------|-----------------|
| `.` `()` `[]` | `Call` (highest) |
| `->>` | `PipeArg` (new — above Pipe) |
| `\|>` | `Pipe` (lowest) |

Parsing rule for `->>`:

1. The LHS must be a function call (detected as a `Call` expression node). Non-call LHS is an error.
2. Parse the RHS at `Precedence::Addition` (same as pipe RHS).
3. Create a new AST node `ClosurePipe` with children: the call node and the body node.

During semantic analysis (or a dedicated desugaring pass), `ClosurePipe` is rewritten:

```
ClosurePipe
  ├── Call(fn_name, [arg_0, arg_1, ...])  // LHS: partial call
  └── body_expr                            // RHS: closure body
  ⇒
Call(fn_name, [arg_0, arg_1, ..., Closure([param_0, param_1, ...], body_expr)])
```

Where:
- The `Closure` node is constructed synthetically with the correct parameter list
- All `%`/`@` references in `body_expr` are rewritten to the corresponding parameter identifiers
- `%.freq` → `Identifier("freq")` (if `freq` is one of the closure params)
- Bare `%` → `Identifier(first_param_name)` (for single-param closures)

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

Example definitions:

```cpp
// poly
{"poly", {cedar::Opcode::NOP, 2, 1, true,
          {"input", "instrument", "voices", "", "", ""},
          {NAN, NAN, 64.0f, NAN, NAN},
          "Polyphonic voice manager...",
          .closure_param_names = {"freq", "gate", "vel"},
          .closure_param_count = 3}},

// tap_delay
{"tap_delay", {cedar::Opcode::DELAY_TAP, 4, 2, true,
               {"in", "time", "fb", "processor", "dry", "wet"},
               {0.0f, 1.0f, NAN, NAN, NAN},
               "Tap delay with feedback chain...",
               .closure_param_names = {"x"},
               .closure_param_count = 1}},
```

### 5.4 Analyzer / Desugaring Pass

The desugaring can happen either:

**Option A — During semantic analysis** (recommended for MVP):
When the analyzer encounters a `ClosurePipe` node:
1. Look up the function being called (by name) in the builtin table
2. Read `closure_param_count` and `closure_param_names` from the definition
3. Create a synthetic `Closure` node with those params
4. Walk the body expression, replacing `%`/`@` hole references with the corresponding `Identifier` nodes
5. Replace the `ClosurePipe` node with a `Call` node that includes the new closure as its last argument

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
| Codegen | **Stays** | After desugaring, codegen sees standard `Call` + `Closure` nodes — no changes needed |
| Builtin definitions | **Modified** | Each closure-taking builtin gets `.closure_param_names` / `.closure_param_count` |
| User functions with closure params | **Stays** | Future work — MVP covers builtins only |
| Existing closure syntax | **Stays** | `(params) -> body` continues to work unchanged |
| Pipe operator `|>` | **Stays** | No changes to `|>` semantics |

---

## 7. File-Level Changes

### Files to Modify

| File | Change |
|------|--------|
| `akkado/include/akkado/token.hpp` | Add `ArrowPipe` to `TokenType` enum; add `"ArrowPipe"` to `token_type_name()` |
| `akkado/src/lexer.cpp` | Add 3-char `->>` lexing rule before `->` Arrow rule |
| `akkado/include/akkado/ast.hpp` | Add `NodeType::ClosurePipe`; possibly add `ClosurePipeData` struct if needed for metadata |
| `akkado/src/parser.cpp` | Add `parse_closure_pipe()`; register `->>` infix at `PipeArg` precedence; add `PipeArg` to precedence table |
| `akkado/src/parser.hpp` | Add `parse_closure_pipe()` declaration |
| `akkado/src/analyzer.cpp` | Add closure-pipe desugaring pass: `ClosurePipe` → `Call` + `Closure` with param substitution |
| `akkado/include/akkado/builtins.hpp` | Add `closure_param_names` / `closure_param_count` to `BuiltinInfo`; populate for `poly`, `mono`, `legato`, `tap_delay` family, `map`, `reduce`, `zipWith` |
| `akkado/src/codegen.cpp` | (If desugaring is at codegen time instead of analysis) add `ClosurePipe` handler |

### Files That Stay

| File | Reason |
|------|--------|
| `cedar/**/*` | No Cedar VM changes — desugaring is purely in the Akkado compiler |
| `web/**/*` | No web app changes |
| `tools/**/*` | No CLI tool changes |
| `experiments/**/*` | No experiment changes |

---

## 8. Implementation Phases

### Phase 1: Lexer + Parser (Shipped: basic token recognition and AST construction)

- Add `TokenType::ArrowPipe`
- Add 3-char `->>` lexing rule
- Add `NodeType::ClosurePipe` to AST
- Add `parse_closure_pipe()` in parser with `PipeArg` precedence
- Validate LHS is a call expression (error E500)
- **Test**: `"poly(x) ->> y"` parses to `ClosurePipe(Call("poly", ["x"]), Identifier("y"))`
- **Test**: `"fn(a, b) ->> c"` where LHS is not a call → E500

### Phase 2: BuiltinInfo Metadata

- Add `closure_param_names` and `closure_param_count` to `BuiltinInfo`
- Populate for every closure-taking builtin:

  | Builtin | Params |
  |---------|--------|
  | `poly` | `freq`, `gate`, `vel` |
  | `mono` | `freq`, `gate`, `vel` |
  | `legato` | `freq`, `gate`, `vel` |
  | `tap_delay` | `x` |
  | `tap_delay_ms` | `x` |
  | `tap_delay_smp` | `x` |
  | `map` | `x` |
  | `reduce` | `acc`, `x` |
  | `zipWith` | `a`, `b` |

- **Test**: Verify each builtin's metadata is correct

### Phase 3: Desugaring (Analysis Pass)

- Implement the `ClosurePipe` → `Call` + `Closure` rewrite in the analyzer
- Walk the body tree, find all `%`/`@` hole references
- For `%.field`: resolve to parameter name; error if field not in closure params (E502)
- For bare `%` with single-param closure: resolve to the one param
- For bare `%` with multi-param closure: error E503
- Append the synthetic `Closure` node as the last argument to the call
- **Test**: `poly(pat) ->> saw(%.freq)` → `poly(pat, (freq, gate, vel) -> saw(freq))`
- **Test**: `tap_delay(sig, 0.5, 0.3) ->> % * 0.5` → `tap_delay(sig, 0.5, 0.3, (x) -> x * 0.5)`

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
fn(->> outer(%.a)) ->> inner(%.b)  // Invalid: ->> RHS can't contain another ->>
```

**Expected:** Compile error — `->>` bodies cannot contain `->>>` operators. Users should use explicit closures for nesting.

### 9.2 `->>` with Named Arguments

```akkado
poly(input: pat("C4'")) ->> saw(%.freq) * ar(%.gate) * %.vel
```

**Expected:** Works normally. The `->>` appends the closure as the last positional argument regardless of whether prior args were named.

### 9.3 `->>` with Optional Arguments After the Closure

```akkado
tap_delay(sig, 0.5, 0.3) ->> % * 0.5  // Already includes dry/wet before closing paren? No
```

**Expected:** `->>` fills the closure position only. If optional args exist AFTER the closure (e.g., `tap_delay` has `dry`/`wet` after `processor`), they are filled by their defaults — same behavior as if omitted today. Users can provide them before `->>`:

```akkado
tap_delay(sig, 0.5, 0.3, 0.2, 0.8) ->> % * 0.5
```

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

// Compare bytecode with explicit closure version
auto r_explicit = compile("pat(\"C4'\") |> poly(%, (freq, gate, vel) -> saw(freq) * ar(gate) * vel) |> out(%, %)");
CHECK(r.bytecode == r_explicit.bytecode);

// tap_delay with ->>
auto r2 = compile("tap_delay(sig, 0.5, 0.3) ->> % * 0.5");
CHECK(r2.success);

// map with ->>
auto r3 = compile("map([1, 2, 3]) ->> % * 2");
CHECK(r3.success);
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
