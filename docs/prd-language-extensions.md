# PRD: Akkado Language Extensions (Phases 1, 3, 4)

> **Status: COMPLETE** — All 8 features implemented.

## Overview

This PRD specifies 8 language features across Phases 1, 3, and 4 of the [Language Evolution Vision](vision-language-evolution.md). All features are **compiler-only** — no Cedar VM changes required. They extend the Akkado language with better ergonomics, compile-time evaluation, type safety, and pattern matching improvements.

**Explicitly deferred** (separate PRDs):
- Module/import system — current string-prepend stdlib works fine
- `state` keyword — needs VM investigation (Phase 2)
- Raw delay line primitives — depends on `state`
- Macro system, custom operators — low priority

### Feature Summary

| # | Feature | Phase | Dependencies | Complexity |
|---|---------|-------|-------------|------------|
| 1 | [Dot-call syntax](#1-dot-call-syntax) | 1 | None | Trivial |
| 2 | [`const fn`](#2-const-fn) | 1 | None | Medium |
| 3 | [TypedValue refactor](#3-typedvalue-refactor) | 3 | None | High |
| 4 | [Match destructuring](#4-match-destructuring) | 3 | None (better errors with 3) | Medium |
| 5 | [Pattern methods](#5-pattern-methods) | 3 | 1 + 3C | Low |
| 6 | [Expression defaults](#6-expression-defaults) | 4 | 2 | Low |
| 7 | [Range patterns](#7-range-patterns) | 4 | None | Low |
| 8 | [Record spreading](#8-record-spreading) | 4 | None | Low-Medium |

---

## 1. Dot-Call Syntax

**`x.f(args)` desugars to `f(x, args)`**

### Current State

95% implemented. The full pipeline exists:
- Lexer: `Dot` token (`token.hpp:41`)
- Parser: `parse_method_call()` creates `NodeType::MethodCall` AST nodes (`parser.cpp:858-894`)
- Analyzer: `desugar_method_call()` rewrites to `Call` nodes (`analyzer.cpp:898-939`)
- Codegen: defensive error at `codegen.cpp:1097-1099` — should never fire if analyzer runs

### Specification

**Syntax:**
```akkado
// These are equivalent:
saw(440).lp(800).delay(0.3, 0.5)
delay(lp(saw(440), 800), 0.3, 0.5)
saw(440) |> lp(@, 800) |> delay(@, 0.3, 0.5)
```

**Semantics:**
- Purely syntactic — `x.f(a, b)` ALWAYS means `f(x, a, b)`, no exceptions
- No special method resolution, no "methods" concept
- Works with builtins and user-defined functions equally
- Left-associative chaining: `a.f().g()` → `g(f(a))`
- Interacts with pipe: `x.f(a) |> g(@, b)` works (dot-call resolves first)

**Non-goals:**
- No property access via dot (that's `FieldAccess`, a separate node type)
- No method overloading based on receiver type (purely syntactic rewrite)

### Implementation

**Already done:**
- Parser creates `MethodCall` nodes with receiver as first child, method name in `IdentifierData`
- Analyzer `desugar_method_call()` rewrites to `Call(method_name, Argument(receiver), Argument(arg1), ...)`

**Remaining work:**
1. Verify the desugar path works end-to-end with existing builtins
2. The codegen E113 error (`codegen.cpp:1097-1099`) should be kept as a safety net — if a `MethodCall` reaches codegen, the analyzer failed to desugar it
3. Add tests

### Files to Modify

| File | Change |
|------|--------|
| `akkado/tests/test_parser.cpp` | AST shape tests for `x.f(a)` |
| `akkado/tests/test_analyzer.cpp` | Verify desugaring produces correct Call nodes |
| `akkado/tests/test_akkado.cpp` | End-to-end: `saw(440).lp(800)` compiles and produces correct output |

### Test Cases

```akkado
// Basic chaining
osc("sin", 440).lp(800)                    // → lp(osc("sin", 440), 800)

// Multi-step chain
saw(440).lp(800).delay(0.3, 0.5, 1, 0.5)  // → delay(lp(saw(440), 800), 0.3, 0.5, 1, 0.5)

// With pipe
osc("sin", 440).lp(800) |> out(@, @)       // dot resolves before pipe

// User-defined functions
fn gain(sig, amt) -> sig * amt
saw(440).gain(0.5)                          // → gain(saw(440), 0.5)

// No-arg method (parentheses required)
// x.f() → f(x) — parser requires ()
```

---

## 2. `const fn` — Compile-Time Pure Evaluation

> **Status: Implemented** — `bda74f9`

**User-defined functions that execute at compile time, producing constant values.**

### Current State

Not implemented. The compiler has hardcoded compile-time evaluation for `linspace`, `harmonics`, `random` as codegen special-cases (`codegen_arrays.cpp:1300-1425`). These only work with literal arguments.

### Specification

**Syntax:**
```akkado
const fn linspace(start, end, n) -> {
    range(0, n) |> map(@, (i) -> start + (end - start) * i / (n - 1))
}

const fn edo_scale(divisions) -> {
    range(0, divisions) |> map(@, (i) -> pow(2, i / divisions))
}

const fn mtof(note) -> 440.0 * pow(2.0, (note - 69.0) / 12.0)

const fn just_intonation() -> [1, 9/8, 5/4, 4/3, 3/2, 5/3, 15/8]

const fn wavetable(n) -> range(0, n) |> map(@, (i) -> sin(2 * 3.14159265 * i / n))
```

**Semantics:**
- `const fn` marked functions MUST be pure — the compiler verifies this
- All arguments must resolve to compile-time constants at every call site
- The body is evaluated during compilation using a compile-time interpreter
- Return value is a compile-time constant: a `double` or `vector<double>` (constant array)
- Result is emitted as `PUSH_CONST` instruction(s) — zero runtime cost

**Allowed in body:**
- Arithmetic: `+`, `-`, `*`, `/`, `@`
- Comparisons: `<`, `>`, `<=`, `>=`, `==`, `!=`
- Math builtins: `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`, `pow`, `sqrt`, `abs`, `log`, `exp`, `floor`, `ceil`, `min`, `max`, `clamp`
- Array operations: `range`, `map`, `fold`, `sum`, `len`, `zip`, `zipWith`
- Match expressions (with const patterns)
- Calls to other `const fn` functions
- Array literals `[a, b, c]`

**Forbidden in body:**
- Stateful builtins (`osc`, `lp`, `delay`, etc.)
- `out()`, `param()`, `toggle()`, `button()`
- Pattern functions (`pat()`, `seq()`)
- Anything producing audio-rate signals
- References to non-const variables

**Error conditions:**
- Non-const argument at call site: `"const fn 'name' requires compile-time constant arguments"`
- Non-pure operation in body: `"const fn body cannot use 'osc' — only pure operations allowed"`
- If any argument is non-literal at call site, emit error (don't silently fall back to runtime)

### Implementation

#### New Token: `Const`

Add `Const` to `TokenType` enum (`token.hpp`). Add `"const"` to keyword map (`lexer.cpp`).

#### Parser Changes

In function definition parsing, detect `const fn` prefix:

```
const fn name(params) -> body
```

- If current token is `Const` and next is `Fn`, parse as const function def
- Set `FunctionDefData::is_const = true` (new field)
- Otherwise, parse normally

#### AST Changes

Add `is_const` flag to `FunctionDefData`:

```cpp
struct FunctionDefData {
    std::string name;
    std::size_t param_count;
    bool has_rest_param = false;
    bool is_const = false;  // NEW: const fn marker
};
```

#### Symbol Table Changes

Add `is_const` to `UserFunctionInfo`:

```cpp
struct UserFunctionInfo {
    // ... existing fields ...
    bool is_const = false;  // NEW: const fn marker
};
```

#### Analyzer Changes

When registering a `const fn`, verify purity of the body:
- Walk the body AST
- Whitelist: arithmetic ops, math builtin names, array operations, match, calls to other const fns
- Reject: any identifier resolving to a non-const builtin, any pattern/stateful construct
- Error with source location if non-pure operation found

#### Compile-Time Interpreter

New component (can be a new file `akkado/src/const_eval.cpp` or inline in codegen):

**Core type:**
```cpp
using ConstValue = std::variant<double, std::vector<double>>;
```

**Operations:**
- Arithmetic on `double` values (+, -, *, /, @)
- Arithmetic on arrays (element-wise or scalar broadcast)
- Math functions: `sin(double) -> double`, etc. (delegate to `<cmath>`)
- `range(start, end) -> vector<double>` (integer range)
- `map(array, lambda) -> vector<double>` (apply lambda to each element)
- `fold(array, lambda, init) -> double` (reduce)
- `sum(array) -> double`
- `len(array) -> double`
- Match: evaluate scrutinee, compare literal patterns, return winning arm's value
- Calls to other const fns: recursively invoke the interpreter (depth limit: 16)

**Error handling:**
- Division by zero: emit compile error
- Recursion depth exceeded: emit error `"const fn recursion depth exceeded (max 16)"`
- Non-const operation encountered: emit error with operation name

#### Codegen Integration

When visiting a `Call` node whose target is a const fn:
1. Visit all argument nodes — each must produce a compile-time constant
2. If any argument is not const, emit error
3. Invoke the compile-time interpreter with argument values
4. Emit the result as `PUSH_CONST` (scalar) or series of `PUSH_CONST` (array)

#### Migration Path

Once `const fn` works, the hardcoded generators in `codegen_arrays.cpp` can be migrated to stdlib const fns:

```akkado
const fn linspace(start, end, n) -> {
    range(0, n) |> map(@, (i) -> start + (end - start) * i / (n - 1))
}

const fn harmonics(fundamental, count) -> {
    range(1, count + 1) |> map(@, (i) -> fundamental * i)
}

const fn normalize(arr) -> {
    mx = fold(arr, (a, b) -> max(abs(a), abs(b)), 0)
    map(arr, (x) -> x / mx)
}
```

This migration is not required immediately but should happen once const fn is stable.

### Files to Modify

| File | Change |
|------|--------|
| `akkado/include/akkado/token.hpp` | Add `Const` token type |
| `akkado/src/lexer.cpp` | Add `"const"` to keyword map |
| `akkado/include/akkado/ast.hpp` | Add `is_const` to `FunctionDefData` |
| `akkado/src/parser.cpp` | Parse `const fn` prefix |
| `akkado/include/akkado/symbol_table.hpp` | Add `is_const` to `UserFunctionInfo` |
| `akkado/src/analyzer.cpp` | Verify const fn purity |
| `akkado/src/const_eval.cpp` (new) | Compile-time interpreter |
| `akkado/include/akkado/const_eval.hpp` (new) | Interpreter header |
| `akkado/src/codegen.cpp` or `codegen_functions.cpp` | Invoke interpreter for const fn calls |

### Test Cases

```akkado
// Scalar const fn
const fn double_it(x) -> x * 2
osc("sin", double_it(220))                // → osc("sin", 440)

// Array const fn
const fn harmonics(f, n) -> range(1, n + 1) |> map(@, (i) -> f * i)
harmonics(110, 4)                          // → [110, 220, 330, 440]

// Nested const fn calls
const fn mtof(n) -> 440.0 * pow(2.0, (n - 69.0) / 12.0)
const fn chord(a, b, c) -> [mtof(a), mtof(b), mtof(c)]
chord(60, 64, 67)                          // → [261.6, 329.6, 392.0]

// Error: non-const argument
const fn double_it(x) -> x * 2
freq = param("freq", 440)
double_it(freq)                            // ERROR: requires compile-time constant

// Error: non-pure body
const fn bad(x) -> osc("sin", x)          // ERROR: osc is not pure

// Match in const fn
const fn clamp01(x) -> match { x < 0: 0, x > 1: 1, _: x }
```

---

## 3. TypedValue Refactor

> **Status: Implemented** — Phase 3A (`c733f66`), Phases 3B+3C (`57bf870`)

**Change `visit()` to return `TypedValue` instead of `uint16_t`, enabling type checking and simplifying compound type handling.**

### Current State

Not implemented. The existing type system PRD (`docs/prd-compiler-type-system.md`) provides a thorough design. This section summarizes the three sub-phases and how they integrate with the other features in this PRD.

Currently, `visit()` returns `uint16_t` (a buffer index) with no type information. Structural metadata is tracked in six ad-hoc maps on the `CodeGenerator`:

| Map | Type | Purpose |
|-----|------|---------|
| `node_buffers_` | `NodeIndex → uint16_t` | Visit cache |
| `multi_buffers_` | `NodeIndex → vector<uint16_t>` | Arrays, chords |
| `stereo_outputs_` | `NodeIndex → {left, right}` | Stereo pairs |
| `record_fields_` | `NodeIndex → {name → buffer}` | Record/pattern fields |
| `polyphonic_fields_` | `NodeIndex → {freq[], ...}` | Per-voice pattern data |
| `array_lengths_` | `buffer → uint8` | Array element count |

### Sub-Phase 3A: Change `visit()` → `TypedValue`

**Pure refactoring — all existing tests must pass identically.**

Core change:
- `visit()` return type changes from `uint16_t` to `TypedValue`
- Add `buffer_of(TypedValue)` helper for callers that just need the buffer index
- `node_buffers_` becomes `node_types_: unordered_map<NodeIndex, TypedValue>`
- Ad-hoc maps gradually subsumed by TypedValue payloads
- Keep `stereo_outputs_` — stereo is orthogonal to type

The `TypedValue` struct and `ValueType` enum are specified in the existing [Type System PRD](prd-compiler-type-system.md). Key types: Signal, Number, Pattern, Record, Array, String, Function, Void.

**Mechanical transformation:** Every `visit()` call site either:
- Uses `.buffer` to get the raw index (backward compat)
- Or switches on `.type` for type-aware behavior

### Sub-Phase 3B: Add Type Annotations to Builtins

Add `param_types` array to `BuiltinInfo` (`builtins.hpp`):

```cpp
struct BuiltinInfo {
    // ... existing fields ...
    std::array<ValueType, MAX_BUILTIN_PARAMS> param_types;  // NEW
};
```

Type checking in `visit_call()`:
- After visiting each argument, check `type_compatible(arg.type, expected_type)`
- Emit clear error: `"argument 'freq' expects Signal, got Pattern"`

**Type compatibility rules:**

| Expected | Accepts |
|----------|---------|
| Signal | Signal, Number (promoted to constant buffer) |
| Number | Number only |
| Pattern | Pattern only |
| Record | Record, Pattern (Pattern is a Record subtype) |
| Array | Array only |
| String | String only |
| Function | Function only |

Pattern → Signal coercion is deliberately NOT supported. Users must access a specific field (`.freq`, `.vel`).

### Sub-Phase 3C: Leverage Types for New Features

With TypedValue available:
- `as` bindings carry full `TypedValue` → enables destructuring (feature 4)
- Pattern type enables pattern methods (feature 5) — type error if `.slow()` called on non-Pattern
- Match arms can check `TypedValue` type
- Builtin overload resolution based on argument types
- Better field access: `handle_field_access()` simplifies from 5 cases to a type switch

### Files to Modify

See [Type System PRD](prd-compiler-type-system.md) Section "Key Files" for the complete file list. Primary changes:

| File | Change |
|------|--------|
| `akkado/include/akkado/codegen.hpp` | `TypedValue` struct, `visit()` signature, remove ad-hoc maps |
| `akkado/src/codegen.cpp` | All `visit_*` return `TypedValue`, simplified field access |
| `akkado/src/codegen_patterns.cpp` | Pattern codegen returns Pattern `TypedValue` |
| `akkado/include/akkado/builtins.hpp` | `param_types` on `BuiltinInfo` |
| `akkado/src/codegen_functions.cpp` | Type checking in `visit_call()` |

---

## 4. Match Destructuring

> **Status: Implemented** — `cfbb699`

**Destructure records in match arms and `as` bindings.**

### Specification

**Match arm syntax:**
```akkado
match(event) {
    {freq, vel}: osc("sin", freq) * vel
    {freq, vel} if vel > 0.5: osc("saw", freq) * vel
    _: 0.0
}
```

**`as` binding syntax:**
```akkado
pat("c4 e4 g4") as {freq, vel, trig} |> osc("sin", freq) |> @ * vel
```

**Semantics:**
- `{freq, vel}` in pattern position means: bind `freq` to scrutinee's `.freq` field, bind `vel` to scrutinee's `.vel` field
- Acts as both pattern match AND variable binding
- In match arms: if scrutinee has the named fields, the arm matches (records don't have variant types, so destructuring always succeeds if the scrutinee is a record/pattern)
- In `as` bindings: desugars to a temp name plus individual field bindings

**Restrictions (v1):**
- Named fields only — no positional destructuring
- No nested destructuring: `{inner: {x, y}}` is NOT supported
- No rename syntax: `{freq: f}` is NOT supported — field name = binding name
- Field names must be valid identifiers

### Implementation

#### Match Arm Destructuring

**Parser changes (`parser.cpp`):**

In match arm pattern parsing (around `parse_match_expr()`, line 1099-1151), detect `{` at start of arm pattern:
- If arm pattern starts with `{`, parse as record destructuring pattern
- Parse `{ ident, ident, ... }`
- Store as new AST node or flag on `MatchArmData`

**New AST data:**
```cpp
struct MatchArmDestructureData {
    std::vector<std::string> field_names;  // Fields to extract
};
```

Add to `MatchArmData`:
```cpp
struct MatchArmData {
    bool is_wildcard;
    bool has_guard;
    NodeIndex guard_node;
    bool is_destructure = false;                           // NEW
    std::vector<std::string> destructure_fields;           // NEW
};
```

**Codegen changes (`codegen_functions.cpp`):**

In `handle_runtime_match()` and `handle_compile_time_match()`:
- For destructuring arms, emit field access for each named field
- Bind extracted fields as local variables in the arm's scope
- No match-failure semantics — records don't have variants, so destructuring always succeeds

Example compilation of `{freq, vel}: osc("sin", freq) * vel`:
1. Emit field access: `scrutinee.freq` → buffer A
2. Emit field access: `scrutinee.vel` → buffer B
3. Bind `freq` = buffer A, `vel` = buffer B in arm scope
4. Compile arm body with bindings active

#### `as` Binding Destructuring

**Analyzer changes (`analyzer.cpp`):**

In `rewrite_pipes()` (line 337-417), detect `as {field1, field2, ...}` pattern:
- Generate a temp name: `__destr_N`
- Rewrite: `expr as {freq, vel}` → `expr as __destr_N` + synthetic assignments `freq = __destr_N.freq; vel = __destr_N.vel`

This desugaring happens entirely in the analyzer, before codegen sees it. The codegen handles the resulting field access nodes normally.

**Parser changes:**

In pipe binding parsing, after `as`, check for `{`:
- If `{`, parse `{ ident, ident, ... }` and create a `PipeBinding` node with new destructuring data
- Store field names in `PipeBindingData`:

```cpp
struct PipeBindingData {
    std::string binding_name;
    std::vector<std::string> destructure_fields;  // NEW: empty for normal binding
};
```

When `destructure_fields` is non-empty, `binding_name` is auto-generated by the analyzer.

### With vs Without Type System

**Without type system (pre-3A):**
- Implementable as pure syntactic desugaring
- Field access will fail at codegen if field doesn't exist (existing field access error)
- Error messages are generic ("unknown field 'xyz'")

**With type system (post-3C):**
- Verify scrutinee is Record or Pattern type before destructuring
- Emit specific error: `"Cannot destructure Signal — expected Record or Pattern"`
- Verify each field exists: `"Record has no field 'xyz'"`

### Files to Modify

| File | Change |
|------|--------|
| `akkado/include/akkado/ast.hpp` | Destructure fields on `MatchArmData` and `PipeBindingData` |
| `akkado/src/parser.cpp` | Parse `{field, ...}` in match arms and `as` bindings |
| `akkado/src/analyzer.cpp` | Desugar `as {fields}` in `rewrite_pipes()` |
| `akkado/src/codegen_functions.cpp` | Handle destructuring in match arm codegen |

### Test Cases

```akkado
// Match arm destructuring
r = {freq: 440, vel: 0.8}
match(r) {
    {freq, vel}: osc("sin", freq) * vel       // → osc("sin", 440) * 0.8
    _: 0
}

// As binding destructuring
pat("c4 e4 g4") as {freq, vel, trig} |> osc("sin", freq) |> @ * vel |> out(@, @)

// With guard
match(event) {
    {freq, vel} if vel > 0.5: osc("saw", freq) * vel
    {freq}: osc("sin", freq) * 0.3
    _: 0
}

// Error: destructure non-record (with type system)
x = osc("sin", 440)
match(x) {
    {freq}: freq                               // ERROR: Cannot destructure Signal
}
```

---

## 5. Pattern Methods

> **Status: Implemented** — via dot-call (`0dc05a3`) + TypedValue type checking (`57bf870`)

**Pattern transforms callable as methods: `pat("c4 e4").slow(2).transpose(12)`**

### Specification

```akkado
// These are equivalent:
pat("c4 e4 g4").slow(2).transpose(12).velocity(0.8)
velocity(transpose(slow(pat("c4 e4 g4"), 2), 12), 0.8)
```

### Dependencies

- **Dot-call syntax (feature 1):** Provides the `x.f(args)` → `f(x, args)` rewrite
- **TypedValue (feature 3C):** Validates that `.slow()` is called on Pattern-typed values

### Implementation

This feature is essentially **free** once dot-call and the type system exist:

1. Dot-call already desugars `pat("c4").slow(2)` to `slow(pat("c4"), 2)`
2. Codegen already handles `slow(pat, factor)` via `handle_slow_call()` (`codegen_patterns.cpp:1586-1654`)
3. Type system (3C) validates first argument is Pattern type

**The only new work needed:**
- Add type annotations to pattern transform builtins in `builtins.hpp` (param 0 = Pattern type)
- With type system: emit error `"slow() expects Pattern as first argument, got Signal"` if user writes `osc("sin", 440).slow(2)`

**Pattern transform builtins affected:** `slow`, `fast`, `rev`, `transpose`, `velocity`, `bank`, `n`, `tune`

### Files to Modify

| File | Change |
|------|--------|
| `akkado/include/akkado/builtins.hpp` | Add Pattern type annotation to transform builtins |

### Test Cases

```akkado
// Method-style chaining
pat("c4 e4 g4").slow(2)                    // → slow(pat("c4 e4 g4"), 2)
pat("c4 e4").transpose(12).velocity(0.8)   // → velocity(transpose(pat("c4 e4"), 12), 0.8)

// Mixed with pipe
pat("c4 e4").slow(2) |> poly(4, (f, g, v) -> ...)

// Error (with type system): not a pattern
osc("sin", 440).slow(2)                    // ERROR: slow expects Pattern, got Signal
```

---

## 6. Expression Defaults

> **Status: Implemented** — `695269e`

**Function parameter defaults that are compile-time expressions, not just literals.**

### Current State

Defaults must be literals: `fn f(x, cut = 1000)`. The `FunctionParamInfo.default_value` field stores an `optional<double>` (`symbol_table.hpp:18`).

### Specification

**Syntax:**
```akkado
fn f(x, cut = 440 * 2) -> lp(x, cut)           // arithmetic
fn f(x, cut = mtof(60)) -> lp(x, cut)           // pure/const fn call
fn f(x, freqs = harmonics(440, 5)) -> sum(...)   // const fn call returning array
```

**Semantics:**
- Default expressions must be evaluable at compile time
- Allowed: arithmetic, math builtins, `const fn` calls, array literals
- Forbidden: stateful operations, non-const function calls, patterns
- The compiler evaluates the default expression during compilation using the same compile-time interpreter as `const fn`
- If default expression isn't const-evaluable, emit error: `"Default parameter must be a compile-time constant expression"`

### Dependencies

- **`const fn` (feature 2):** Provides the compile-time interpreter used to evaluate default expressions

### Implementation

**Parser changes (`parser.cpp`):**

In `parse_param_list()`, after detecting `=` for a default value:
- Currently: parse a number literal and store in `default_value`
- New: parse a full expression (call `parse_expression()`) and store the resulting AST node index in `FunctionParamInfo.default_node` (field already exists but only used for literal node refs)

**Symbol table changes (`symbol_table.hpp`):**

`FunctionParamInfo` already has `default_node` (AST node index). The `default_value` field remains for backward compat with simple literal defaults. Add a flag or rely on `default_node != NULL_NODE` to indicate expression default.

**Codegen changes (`codegen_functions.cpp`):**

When expanding a function call and an argument is omitted (using default):
1. Check if `param.default_node` points to an expression (not just a literal)
2. If expression: evaluate it using the compile-time interpreter (from `const fn` implementation)
3. Emit the result as `PUSH_CONST`
4. If the expression is not const-evaluable, emit error

### Files to Modify

| File | Change |
|------|--------|
| `akkado/src/parser.cpp` | Parse expression after `=` in parameter list |
| `akkado/include/akkado/symbol_table.hpp` | Clarify `default_node` usage for expressions |
| `akkado/src/codegen_functions.cpp` | Evaluate expression defaults via const eval |

### Test Cases

```akkado
// Arithmetic default
fn highpass(sig, cut = 440 * 4) -> hp(sig, cut)
highpass(noise())                           // cut = 1760

// Const fn call default
const fn mtof(n) -> 440.0 * pow(2.0, (n - 69.0) / 12.0)
fn pitched(sig, freq = mtof(60)) -> lp(sig, freq)
pitched(noise())                            // freq = 261.6

// Array default
const fn harmonics(f, n) -> range(1, n + 1) |> map(@, (i) -> f * i)
fn additive(freqs = harmonics(220, 4)) -> sum(map(freqs, (f) -> osc("sin", f)))
additive()                                  // freqs = [220, 440, 660, 880]

// Error: non-const default
fn bad(sig, cut = param("cut", 440)) -> lp(sig, cut)  // ERROR: not const
```

---

## 7. Range Patterns

**Half-open range matching in match arms: `0.0..0.3: expr`**

### Specification

**Syntax:**
```akkado
match(velocity) {
    0.0..0.3: "soft"
    0.3..0.7: "medium"
    0.7..1.0: "loud"
    _: "unknown"
}
```

**Semantics:**
- `a..b` in match arm position means `a <= x < b` (half-open interval)
- Both bounds must be number literals (compile-time constants)
- Negative numbers allowed: `-1.0..0.0`
- Bounds are not checked for overlap — user's responsibility (like existing match arms)

**Compilation:**
- In compile-time match: evaluate range check statically (`low <= scrutinee < high`)
- In runtime match: emit `CMP_GTE` + `CMP_LT` + `LOGIC_AND` → feed into `SELECT`

### Implementation

#### New Token: `DotDot`

Add `DotDot` (`..`) to `TokenType` enum. Must be distinguished from existing `DotDotDot` (`...`).

**Lexer changes (`lexer.cpp`):**

When seeing `.`:
1. If next char is `.`: consume it, check third char:
   - If third is `.`: consume → `DotDotDot` (existing variadic)
   - Else: → `DotDot` (new range)
2. Else: → `Dot` (existing field/method access)

#### Parser Changes

In `parse_match_expr()` (line 1099-1151), when parsing arm patterns:
- After parsing a number literal, check for `DotDot` token
- If found, parse another number literal as the upper bound
- Store as range pattern data on the match arm

#### AST Changes

Extend `MatchArmData`:

```cpp
struct MatchArmData {
    bool is_wildcard;
    bool has_guard;
    NodeIndex guard_node;
    bool is_range = false;           // NEW
    double range_low = 0.0;          // NEW
    double range_high = 0.0;         // NEW
    // ... destructure fields from feature 4 ...
};
```

#### Codegen Changes

**Compile-time match (`handle_compile_time_match()`):**
- Add range check: `if (is_range && scrutinee_val >= arm.range_low && scrutinee_val < arm.range_high)`

**Runtime match (`handle_runtime_match()`):**
- For range arms, emit:
  1. `PUSH_CONST range_low` → buffer A
  2. `CMP_GTE scrutinee, A` → buffer B (condition: x >= low)
  3. `PUSH_CONST range_high` → buffer C
  4. `CMP_LT scrutinee, C` → buffer D (condition: x < high)
  5. `LOGIC_AND B, D` → buffer E (combined condition)
  6. Use buffer E as the arm's condition in the SELECT chain

### Files to Modify

| File | Change |
|------|--------|
| `akkado/include/akkado/token.hpp` | Add `DotDot` token type |
| `akkado/src/lexer.cpp` | Lex `..` vs `...` |
| `akkado/include/akkado/ast.hpp` | Range fields on `MatchArmData` |
| `akkado/src/parser.cpp` | Parse range patterns in match arms |
| `akkado/src/codegen_functions.cpp` | Handle range patterns in both match paths |

### Test Cases

```akkado
// Compile-time range match
match(0.5) {
    0.0..0.3: 1
    0.3..0.7: 2        // ← selected
    0.7..1.0: 3
    _: 0
}                       // → emits PUSH_CONST 2.0

// Runtime range match
vel = param("vel", 0.5, 0, 1)
match(vel) {
    0.0..0.3: osc("sin", 440) * 0.3
    0.3..0.7: osc("sin", 440) * 0.6
    0.7..1.0: osc("saw", 440) * 1.0
    _: dc(0)
}

// Edge cases
match(0.3) {
    0.0..0.3: 1        // NOT selected (half-open: 0.3 is excluded)
    0.3..0.7: 2        // selected (0.3 is included)
    _: 0
}

// Negative ranges
match(x) {
    -1.0..0.0: "negative"
    0.0..1.0: "positive"
    _: "out of range"
}

// Range with guard
match(vel) {
    0.0..0.5 if mode == "soft": soft_synth(freq)
    0.5..1.0: loud_synth(freq)
    _: dc(0)
}
```

---

## 8. Record Spreading

> **Status: Implemented** — `695269e`

**Copy fields from an existing record with `..expr` in record literals.**

### Specification

**Syntax:**
```akkado
base = {freq: 440, vel: 0.8, dur: 0.5}
modified = {..base, freq: 880}           // override freq, keep vel and dur
extended = {..base, pan: 0.5}            // add new field
```

**Semantics:**
- `..expr` in record literal position copies all fields from `expr`
- Explicit fields override spread fields (last wins)
- Shallow copy only — no nested spreading
- Spread source must evaluate to a record or pattern value
- Only one spread per record literal in v1: `{..a, ..b, x: 1}` is NOT supported
- Spread must appear as first entry in the record literal (before explicit fields)

### Implementation

#### Parser Changes

In `parse_record_literal()` (`parser.cpp:1271-1330`):
- After `{`, check for `..` token (`DotDot` — added in feature 7)
- If found: parse expression as spread source, store reference
- Continue parsing remaining fields normally

**AST representation options:**

Option A: Add spread source to record node data:
```cpp
struct RecordLitData {
    NodeIndex spread_source = NULL_NODE;  // NEW: source record to spread from
};
```

Option B: Use a special first child node with a flag:
- Add `is_spread` flag to `RecordFieldData`
- The spread source is stored as a child Argument with `is_spread = true` and the expression as its child

Option A is simpler and recommended.

#### Codegen Changes

In record literal handling (`codegen.cpp`):

1. If `spread_source != NULL_NODE`:
   a. Visit spread source expression → get result
   b. Look up its record fields (from `record_fields_` map or TypedValue in phase 3A+)
   c. Copy all field→buffer mappings from source
2. Process explicit fields normally, overriding any that overlap with spread fields
3. Register final field set in `record_fields_`

**Without type system:** Codegen looks up the spread source in `record_fields_` map. If not found (e.g., spread source is a signal), emit error: `"spread source is not a record"`

**With type system (3A+):** Check `TypedValue.type == Record` or `Pattern`. Emit type error otherwise.

### Files to Modify

| File | Change |
|------|--------|
| `akkado/include/akkado/ast.hpp` | Add `RecordLitData` with spread source, or spread flag |
| `akkado/src/parser.cpp` | Parse `..expr` in record literals |
| `akkado/src/codegen.cpp` | Handle spread in record literal codegen |

### Test Cases

```akkado
// Basic spread + override
base = {freq: 440, vel: 0.8}
modified = {..base, freq: 880}
osc("sin", modified.freq) * modified.vel   // → osc("sin", 880) * 0.8

// Spread + extend
base = {freq: 440, vel: 0.8}
extended = {..base, pan: 0.5}
extended.pan                                // → 0.5
extended.freq                               // → 440

// Spread pattern into record
pat("c4 e4") as e
custom = {..e, vel: 0.3}                   // override velocity
osc("sin", custom.freq) * custom.vel

// Error: spread non-record
x = osc("sin", 440)
{..x, freq: 880}                            // ERROR: spread source is not a record

// Spread preserves all fields
r = {a: 1, b: 2, c: 3}
s = {..r}                                   // identical copy
s.a                                         // → 1
s.b                                         // → 2
s.c                                         // → 3
```

---

## Implementation Order

All features are now complete. Actual implementation order:

```
 ✅ 1. Dot-call (verify + tests)
 ✅ 2. Range patterns                     — 243d1e3
 ✅ 3. const fn                           — bda74f9
 ✅ 4. Expression defaults                — 695269e
 ✅ 5. Record spreading                   — 695269e
 ✅ 6. Match destructuring                — cfbb699
 ✅ 7. TypedValue Phase A                 — c733f66
 ✅ 8. TypedValue Phases B+C              — 57bf870
```

Pattern methods (feature 5) are a natural consequence of dot-call + TypedValue — no separate commit needed.

---

## Verification

For each feature:
1. **Parser tests** in `akkado/tests/test_parser.cpp` — verify AST shape
2. **Analyzer tests** in `akkado/tests/test_analyzer.cpp` — verify symbol resolution and desugaring
3. **End-to-end tests** in `akkado/tests/test_akkado.cpp` — compile and verify output values
4. **Build:** `cmake --build build && ./build/akkado/tests/akkado_tests`
5. **Manual testing** in web app: `cd web && bun run dev`

### Cross-Feature Integration Tests

```akkado
// Dot-call + pattern methods + destructuring
pat("c4 e4 g4")
    .slow(2)
    .transpose(12)
    as {freq, vel, trig}
    |> osc("sin", freq)
    |> @ * vel
    |> out(@, @)

// Const fn + expression defaults + record spreading
const fn mtof(n) -> 440.0 * pow(2.0, (n - 69.0) / 12.0)
base_note = {freq: mtof(60), vel: 0.8}
fn play(note = {..base_note, vel: 0.5}) -> osc("sin", note.freq) * note.vel

// Range patterns + match destructuring
pat("c4 e4 g4") as {freq, vel} |>
match(vel) {
    0.0..0.3: osc("sin", freq) * vel
    0.3..0.7: osc("tri", freq) * vel
    0.7..1.0: osc("saw", freq) * vel
    _: dc(0)
} |> out(@, @)
```

---

## Design Principles

All features in this PRD follow the principles from the [Language Evolution Vision](vision-language-evolution.md):

1. **The VM stays minimal** — all features are compiler-only. Same opcodes, same instruction format, same bytecode.
2. **Zero-allocation runtime** — every construct compiles to fixed-size instructions on pre-allocated buffers.
3. **Live-coding ergonomics** — dot-call, destructuring, and expression defaults reduce keystrokes. Range patterns improve readability.
4. **Backward compatibility** — every feature is additive. Existing programs compile identically.
5. **Faust-style composition** — functions compose, types are structural, dispatch is compile-time.
