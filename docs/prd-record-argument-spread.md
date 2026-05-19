> **Status: IMPLEMENTED** — Record/array argument spread (`f(..record)`, `f(..array)`) and array-literal spread (`[..arr, x]`) are shipped, alongside the pre-existing record-to-record spread (`{..base, field: value}`). Key code: `ArgumentData.spread_source` (`akkado/include/akkado/ast.hpp:185`), parser `..` handling (`akkado/src/parser.cpp:1229`, `:719`), `expand_call_arguments()` with E140/E180 (`akkado/src/codegen.cpp:2291-2385`), name/positional binding + W160 (`akkado/src/codegen_functions.cpp`), array-literal flattening (`akkado/src/codegen.cpp:316-341`). Tests: `[parser][spread]`, `[analyzer][spread]`, `[codegen][spread]`.

# PRD: Spread Arguments and Array Spread in Akkado

## 1. Executive Summary

Akkado already supports record literals (`{freq: 440, vel: 0.8}`), array literals (`[1, 2, 3]`), field access (`r.freq`), pipe bindings (`expr as r`), and record-to-record merging (`{..base, extra: val}`). All are compile-time expanded — there is no runtime record or array spread type.

This PRD proposes adding **spread** in two forms:

1. **Record argument spread**: `..record` in a function call expands record fields into named arguments. Enables the Enhanced Sampler PRD (G8) preset pattern:
   ```akkado
   preset = {bits: 12, sr: 26040, loop_mode: "auto"}
   samples("my-bank", ..preset)
   ```

2. **Array positional spread**: `..array` in a function call or array literal expands array elements positionally:
   ```akkado
   // In function calls — positional argument expansion
   args = [1000, 0.7]
   filter(signal, ..args)  // equivalent to: filter(signal, 1000, 0.7)

   // In array literals — concatenation/flattening
   base = [1, 2, 3]
   extended = [..base, 4, 5]  // [1, 2, 3, 4, 5]
   ```

Record spread and array spread are **mutually exclusive** within a single call — you cannot mix them. This keeps the semantics clear: record spread fills by name, array spread fills by position.

### Key Design Decisions

- **`..` syntax** (double-dot), consistent with existing record-to-record spread — no new token needed
- **Compile-time only** — records and arrays expand at compile time, consistent with the existing semantic model
- **Record spread works with all callables** — special handler builtins AND user-defined functions
- **Array spread works with function calls AND array literals**
- **Named parameter matching for records** — record fields match function parameters by name
- **Positional expansion for arrays** — array elements fill positional parameters left-to-right
- **Mutually exclusive** — a single call cannot contain both record and array spread
- **Mixing allowed within same type** — positional args + array spread, or spread + named args (for records)

---

## 2. Problem Statement

### 2.1 Current Behavior vs Proposed

| Aspect | Current | Proposed |
|--------|---------|----------|
| **Record merging** | `{..base, x: 1}` — merges record fields into new record | Stays the same (already works) |
| **Record argument spread** | Not supported | `f(..record)` — expands record fields into named arguments |
| **Array literal spread** | Not supported | `[..arr, x, y]` — concatenates arrays at compile time |
| **Array argument spread** | Not supported | `f(..array)` — expands array elements as positional arguments |
| **Function call args** | Positional + named: `f(1, b: 2)` | + record spread: `f(1, ..r, c: 3)`; + array spread: `f(1, ..arr)` |
| **Preset patterns** | Must repeat keyword args every call | Define once as record, spread everywhere |
| **Extra record fields** | N/A (record merging copies all) | Warning W160 — field has no matching parameter |
| **Extra array elements** | N/A | Error — too many positional arguments |
| **Mixing spread types** | N/A | Record and array spread mutually exclusive in one call |

### 2.2 Root Cause

Function call argument collection (`parse_argument()`) only handles positional and named arguments. There is no mechanism to unpack a record's fields or an array's elements into the argument list. Array literals have no spread flattening. The codegen for user functions (`handle_user_function_call`) assigns arguments to parameters by position only.

### 2.3 Existing Infrastructure to Build On

| Component | Location | Reuse |
|-----------|----------|-------|
| `TokenType::DotDot` | `akkado/include/akkado/token.hpp:82` | Already exists — used for record spread `{..expr}` and match ranges |
| Record spread codegen | `akkado/src/codegen.cpp:1882-1949` | Already handles `..expr` in record literals — same concept for calls |
| `ArgumentData` struct | `akkado/include/akkado/ast.hpp:161` | Extend with `is_spread` flag |
| `TypedValue::Record` | `akkado/include/akkado/codegen.hpp` | Already carries `fields: unordered_map<string, TypedValue>` |
| `TypedValue::Array` | `akkado/include/akkado/codegen.hpp` | Already carries `elements: vector<uint16_t>` buffers |
| Array literal codegen | `akkado/src/codegen.cpp:224` | Extend to handle spread children |
| User function codegen | `akkado/src/codegen_functions.cpp:69` | Extend to handle spread args (record: name match, array: positional) |
| Builtin signature system | `akkado/include/akkado/builtins.hpp` | Has named params — record spread can match by name |
| Analyzer | `akkado/src/analyzer.cpp` | Validate spread source type (record or array) |

---

## 3. Goals and Non-Goals

### 3.1 Goals

- **G1**: `..record` syntax in function calls expands record fields into named arguments
- **G2**: `..array` syntax in function calls expands array elements as positional arguments
- **G3**: `..array` syntax in array literals flattens array elements: `[..arr, x, y]`
- **G4**: Works with all callables: special handler builtins, user `fn`, and closures
- **G5**: Record fields match function parameters by NAME, not position
- **G6**: Array elements fill positional parameters left-to-right
- **G7**: Record spread and array spread are mutually exclusive within a single call
- **G8**: Mixing positional args, record spread, and named args is allowed
- **G9**: Mixing positional args and array spread is allowed
- **G10**: Extra fields in spread record produce a warning (W160)
- **G11**: Extra array elements produce an error (too many positional args)
- **G12**: Missing required parameters still produce error E105
- **G13**: Spread records work as dot-call receivers: `{..r, freq: 440}.method()`
- **G14**: Compile-time only — no runtime spread type, consistent with existing model

### 3.2 Non-Goals

- **Runtime record or array types** — records and arrays stay compile-time expanded. No reflection, no dynamic access
- **Runtime spread** — spread source must be a statically known record/array (literal or `let`-bound)
- **Rest parameter spread reversal** — `...rest` in function definitions collects remaining args; this PRD does NOT add the inverse operation of converting args back to a record/array
- **Computed field names** — `{[expr]: value}` conflicts with compile-time expansion
- **Destructuring assignment** — `{a, b} = record` is deferred (Q2 in records PRD)
- **Mixed record + array spread in one call** — explicitly excluded to keep semantics clear

---

## 4. Target Syntax and User Experience

### 4.1 Basic Argument Spread

```akkado
// Define a preset record
sp1200 = {bits: 12, sr: 26040}

// Spread into samples()
samples("github:tidalcycles/Dirt-Samples", ..sp1200)

// Equivalent to:
// samples("github:tidalcycles/Dirt-Samples", bits: 12, sr: 26040)
```

### 4.2 Inline Record Spread

```akkado
// Spread an inline record
samples("my-bank", ..{bits: 8, sr: 12000})

// Spread + override
preset = {bits: 12, sr: 26040}
samples("my-bank", ..preset, bits: 8)  // bits overridden to 8
```

### 4.3 Mixing Positional, Spread, and Named Args

```akkado
// Positional + spread + named
fn filter(in, cutoff, res, dry, wet) -> ...
config = {res: 0.7, dry: 0.5, wet: 0.5}
filter(signal, 1000, ..config)
// Equivalent to: filter(signal, 1000, res: 0.7, dry: 0.5, wet: 0.5)

// Spread + named override
filter(signal, 1000, ..config, wet: 0.8)  // wet overridden to 0.8

// Multiple spreads (later spread overrides earlier for same field)
base = {dry: 0.3, wet: 0.7}
extra = {wet: 0.5}
filter(signal, 1000, 0.7, ..base, ..extra)  // wet = 0.5 (from extra)
```

### 4.4 User Function Spread

```akkado
// User function with named parameters
fn synth(freq, wave, cutoff, env_attack, env_decay) ->
    osc(wave, freq) |> lp(@, cutoff) * ar(1, env_attack, env_decay)

// Preset as record
config = {wave: "saw", cutoff: 2000, env_attack: 0.01, env_decay: 0.3}

// Spread into function call — fields match params by NAME
synth(440, ..config)
// Equivalent to: synth(440, wave: "saw", cutoff: 2000, env_attack: 0.01, env_decay: 0.3)

// Fields can be in any order — name matching handles it
config = {env_decay: 0.3, wave: "saw", env_attack: 0.01, cutoff: 2000}
synth(440, ..config)  // Same result
```

### 4.5 Dot-Call with Spread Receiver

```akkado
// Spread result is the receiver for dot-call
config = {freq: 440}
{..config, wave: "saw"}.osc("sin") |> out(@, @)
// Equivalent to: osc("sin", 440) |> out(@, @)
// (osc is called with the spread record's first buffer as the implicit receiver)
```

### 4.6 Sampler Presets (from Enhanced Sampler PRD)

```akkado
// Custom sampler preset
my_gritty = {
    bits: 8,
    sr: 12000,
    pitch_algo: "granular",
    grain_size: 0.020,
    grain_overlap: 2,
    grain_window: "rect",
    grain_jitter: 0.05,
}

// Apply preset
samples("my-bank", ..my_gritty)

// Combine with named preset + overrides
samples("my-bank", preset: "sp1200", ..my_gritty, grain_jitter: 0.1)
```

### 4.7 Error and Warning Examples

```akkado
// W160: Extra field ignored with warning
fn f(a, b) -> a + b
r = {a: 1, b: 2, extra: 3}
f(..r)  // W160: Spread field 'extra' has no matching parameter in f(a, b)

// E105: Missing required parameter (unchanged behavior)
fn g(a, b, c) -> a + b + c
r = {a: 1, b: 2}
g(..r)  // E105: Missing required argument for parameter 'c'

// E140: Spread source is not a record/array
x = 42
f(..x)  // E140: Spread source is not a record or array

// E107: Too many positional arguments from array spread
fn f(a, b) -> a + b
arr = [1, 2, 3]
f(..arr)  // E107: Too many arguments — function f(a, b) takes 2, got 3

// Error: Mixing record and array spread
r = {a: 1}
arr = [2, 3]
f(..r, ..arr)  // Error: Cannot mix record and array spread in one call
```

### 4.8 Array Spread in Function Calls

```akkado
// Define an array of positional arguments
filter_args = [1000, 0.7, 0.3, 0.7]
filter(signal, ..filter_args)
// Equivalent to: filter(signal, 1000, 0.7, 0.3, 0.7)

// Positional + array spread
filter(signal, 1000, ..[0.7, 0.3, 0.7])
// Equivalent to: filter(signal, 1000, 0.7, 0.3, 0.7)

// Multiple array spreads (elements concatenate left-to-right)
start = [1000]
end = [0.3, 0.7]
filter(signal, ..start, 0.7, ..end)
// Equivalent to: filter(signal, 1000, 0.7, 0.3, 0.7)

// Array spread + named args (NOT allowed — arrays are positional only)
fn filter(in, cutoff, res, dry, wet) -> ...
filter(signal, ..[1000, 0.7], wet: 0.8)  // Valid: positional from spread, then named
```

### 4.9 Array Spread in Array Literals

```akkado
// Concatenate arrays
base = [1, 2, 3]
extended = [..base, 4, 5]  // [1, 2, 3, 4, 5]

// Multiple spreads
a = [1, 2]
b = [3, 4]
c = [5, 6]
all = [..a, ..b, ..c]  // [1, 2, 3, 4, 5, 6]

// Inline spread
merged = [..[1, 2], 3, ..[4, 5]]  // [1, 2, 3, 4, 5]

// Empty spread
empty = []
result = [..empty, 1, 2]  // [1, 2]

// Spread at any position
start = [1, 2]
result = [0, ..start, 3]  // [0, 1, 2, 3]
```

---

## 5. Architecture / Technical Design

### 5.1 Semantic Model

Records are expanded at compile-time into named buffers. Arrays are expanded into fixed-length element lists. Spread in both cases is resolved at compile-time:

```
Source:                        Compile-time expansion:
f(1, ..r, c: 3)               f(1, a: 1, b: 2, c: 3)    // if r = {a: 1, b: 2}
f(1, ..arr)                   f(1, x, y)                 // if arr = [x, y]
[..arr, 4, 5]                 [x, y, 4, 5]               // if arr = [x, y]
```

The expansion happens in two phases:
1. **Analyzer**: Validates spread source type (record or array), checks mutual exclusivity
2. **Codegen**: Expands spread into the argument list (records → named, arrays → positional)

### 5.2 AST Changes

Extend `ArgumentData` to track spread arguments:

```cpp
// akkado/include/akkado/ast.hpp
struct ArgumentData { std::optional<std::string> name; };  // Named arg
```

No change needed to `ArgumentData` — spread arguments use the same struct.
The spread source is the child of the `Argument` node, and the spread token
(`..`) is tracked by the parser. During codegen, the spread source's type
(record vs array) determines the expansion strategy.

No new `NodeType` is needed — spread arguments are still `NodeType::Argument`
with a single child (the expression to spread). The parser tracks spread via
a dedicated `spread_source` node index on `ArgumentData`:

```cpp
// akkado/include/akkado/ast.hpp (modified)
struct ArgumentData {
    std::optional<std::string> name;   // Named arg
    NodeIndex spread_source = NULL_NODE; // ..expr spread source (NULL = not a spread)
};
```

For array literal spread, existing `ArrayLit` children are already individual
expression nodes. Spread array elements are just child nodes flagged by the
parser — no AST struct change needed beyond tracking spread.

### 5.3 Parser Changes

Extend `parse_argument()` to detect `..` prefix:

```cpp
NodeIndex Parser::parse_argument() {
    Token start = current();

    // NEW: Check for spread argument: ..expr
    if (check(TokenType::DotDot)) {
        advance();  // consume '..'
        NodeIndex node = make_node(NodeType::Argument, start);
        NodeIndex spread_expr = parse_expression();
        arena_[node].data = Node::ArgumentData{std::nullopt, spread_expr};
        return node;
    }

    // ... existing named/positional arg logic ...
}
```

For array literals, extend `parse_array_literal()` to detect `..` prefix
before each element:

```cpp
// Inside array parsing loop:
if (check(TokenType::DotDot)) {
    advance();  // consume '..'
    NodeIndex spread_expr = parse_expression();
    // Mark this child as spread (store spread_source on a wrapper or
    // reuse Argument node with spread_source set)
    arena_.add_child(node, spread_expr);  // tracked as spread child
}
```

### 5.4 Analyzer Changes

The analyzer already validates record and array types. Add spread-specific validation:

1. **Validate spread source type**: When encountering a spread argument, verify
   the expression resolves to a record or array type. Error E140 if neither.
2. **Mutual exclusivity check**: If a call contains both record spread and array
   spread arguments, produce a compile error.
3. **Field-parameter compatibility** (optional, can be deferred to codegen):
   Cross-reference record spread fields with target function parameters to
   produce warnings for unmatched fields.

For array literal spread, validate each spread source is an array type.

### 5.5 Codegen Changes

#### 5.5.1 Spread Type Detection

Before processing spread arguments, the analyzer or codegen must determine
the spread type (record vs array) and check mutual exclusivity:

```
Algorithm: ValidateSpreadTypes(node)

1. has_record_spread = false, has_array_spread = false

2. For each argument child of the call node:
   a. If is_spread:
      - Visit spread source → get TypedValue
      - If Record type → has_record_spread = true
      - If Array type → has_array_spread = true
      - Else → error E140

3. If has_record_spread AND has_array_spread → error: "Cannot mix record and array spread in one call"
```

#### 5.5.2 Record Spread Argument Collection

In `handle_user_function_call()` and special handler dispatch, when the spread
source is a record, expand fields into named arguments:

```
Algorithm: CollectCallArgsWithRecordSpread(node, func_params)

1. Initialize: filled_params = set(), args_by_name = map(), positional_args = list()

2. For each argument child of the call node:
   a. If is_spread AND source is Record:
      - Visit spread source → get Record TypedValue
      - For each field in record.fields:
        - If field.name matches an unfilled param → args_by_name[field.name] = field.value
        - Else → mark as extra field (warning W160)
   b. If named arg (name: value):
      - args_by_name[name] = visit(value)
      - Mark param as filled
   c. If positional arg:
      - positional_args.push(visit(value))
      - Mark corresponding param as filled

3. Build final param list:
   - For each param in func_params:
     - If param is in positional_args (by index) → use it
     - Else if param is in args_by_name → use it
     - Else if param has default → use default
     - Else → error E105 (missing required arg)
```

#### 5.5.3 Array Spread Argument Collection

When the spread source is an array, expand elements as positional arguments:

```
Algorithm: CollectCallArgsWithArraySpread(node, func_params)

1. Initialize: all_positional = list()  // (value, source_node)

2. For each argument child of the call node:
   a. If is_spread AND source is Array:
      - Visit spread source → get Array TypedValue
      - For each element in array.elements:
        - all_positional.push(element)
   b. If is_spread AND source is Record:
      - → error (mutual exclusivity already checked)
   c. If named arg (name: value):
      - args_by_name[name] = visit(value)
      - Mark corresponding param as filled
   d. If positional arg:
      - all_positional.push(visit(value))

3. Check array length:
   - Count unfilled positional params (total params - named-filled params)
   - If all_positional.size() > unfilled_positional_params → error (too many args)
   - If all_positional.size() < required_unfilled_params → error E105

4. Assign positional args to unfilled params in order
```

#### 5.5.4 Array Literal Spread Flattening

In `handle_array_literal()`, flatten spread arrays into the result:

```
Algorithm: HandleArrayLiteralWithSpread(node)

1. Initialize: all_elements = list<uint16_t>

2. For each child of ArrayLit node:
   a. If child is a spread expression:
      - Visit child → get Array TypedValue
      - Append all element buffers to all_elements
   b. Else:
      - Visit child → get TypedValue
      - Append buffer to all_elements

3. Build Array TypedValue with all_elements
```

This means `[..[1, 2], 3, ..[4, 5]]` produces an array with 5 element buffers:
`[buf_1, buf_2, buf_3, buf_4, buf_5]`.

#### 5.5.5 Special Handler Builtins

For special handlers like `samples()`, `param()`, etc., the existing keyword
argument parsing in each handler already works with named args. Record spread
expansion happens BEFORE the handler is called — by the time
`handle_samples_call()` runs, the spread fields are already expanded into
named arguments in the AST.

This means special handlers require NO changes — they just see the expanded
named arguments. Array spread into special handlers works as positional args
as usual.

#### 5.5.6 Dot-Call with Spread Receiver

When the receiver of a dot-call (`expr.method()`) is a record literal with spread:

```akkado
{..config, freq: 440}.osc("sin")
```

The record literal codegen runs first, producing field buffers. The **first
buffer** of the resulting record becomes the implicit receiver for the dot-call.
This is consistent with how non-record dot-calls work today.

### 5.6 Error and Warning Codes

| Code | Severity | Message | When |
|------|----------|---------|------|
| W160 | Warning | "Spread field '{field}' has no matching parameter in {fn}({params})" | Record spread has fields not matching any function parameter |
| E140 | Error | "Spread source is not a record or array" | `..expr` where expr is neither record nor array type (reused from existing) |
| E105 | Error | "Missing required argument for parameter '{param}'" | Required parameter not filled by positional, spread, or named arg (existing) |
| E107 | Error | "Too many arguments — function {fn} takes {n}, got {m}" | Array spread produces more elements than the function has positional slots |
| E180 | Error | "Cannot mix record and array spread in one call" | A single call contains both `..record` and `..array` spread arguments |

---

## 6. Impact Assessment

| Component | Status | Notes |
|-----------|--------|-------|
| `TokenType` | **Stays** | `DotDot` already exists |
| `ast.hpp` | **Modified** | Add `spread_source` to `ArgumentData` |
| `parser.cpp` | **Modified** | `parse_argument()` detects `..` prefix; array literal parser detects `..` before elements |
| `analyzer.cpp` | **Modified** | Validate spread source type (record or array); check mutual exclusivity |
| `codegen.cpp` | **Modified** | Array literal handler flattens spread children |
| `codegen_functions.cpp` | **Modified** | `handle_user_function_call()` handles spread args (record: name match, array: positional); same for `handle_closure()` |
| `codegen_patterns.cpp` | **Stays** | Special handlers receive expanded args; no changes needed |
| `builtins.hpp` | **Stays** | Named param signatures already exist |
| `codegen_arrays.cpp` | **Modified** | `handle_spread_call()` may need updates; array literal flattening |
| `test_parser.cpp` | **Modified** | Tests for spread argument parsing + array literal spread |
| `test_codegen.cpp` | **Modified** | Tests for spread argument codegen + array spread |
| `test_analyzer.cpp` | **Modified** | Tests for spread source validation + mutual exclusivity |

### 6.1 Components with NO Changes

| Component | Reason |
|-----------|--------|
| `token.hpp` | `DotDot` token already exists |
| `lexer.cpp` | `..` already tokenized correctly |
| `codegen.cpp` (record literals) | Record-to-record spread already implemented |
| Special handler functions | Receive pre-expanded named arguments |
| Cedar VM | No changes — compile-time only feature |

---

## 7. File-Level Changes

| File | Change |
|------|--------|
| `akkado/include/akkado/ast.hpp` | Add `NodeIndex spread_source = NULL_NODE` to `ArgumentData` struct |
| `akkado/src/parser.cpp` | Extend `parse_argument()` to detect `TokenType::DotDot` prefix and set `spread_source`; extend array literal parser to detect `..` before elements |
| `akkado/src/analyzer.cpp` | In `analyze_call()`: validate spread source type (record or array); check mutual exclusivity; error E140/E180 as needed |
| `akkado/src/codegen.cpp` | Extend `handle_array_literal()` (line ~224) to flatten spread children; `handle_method_call()` handles record receivers |
| `akkado/src/codegen_functions.cpp` | Extend `handle_user_function_call()`: collect spread fields/elements, match to params by name (record) or position (array), produce W160/E107 as needed; same for `handle_closure()` |
| `akkado/tests/test_parser.cpp` | Add tests: `f(..r)`, `f(..arr)`, `[..a, b]`, mutual exclusivity parse (parser accepts both, analyzer rejects) |
| `akkado/tests/test_codegen.cpp` | Add tests: record spread, array spread, array literal spread, spread + positional, spread + named, mutual exclusivity error |
| `akkado/tests/test_analyzer.cpp` | Add tests: record spread validation, array spread validation, non-record/array spread error, mutual exclusivity error |

---

## 8. Implementation Phases

### Phase 1 — AST Extension and Parser Support (2 days)

**Goal**: Parser accepts `..expr` as an argument in function calls and in array literals.

**Files to modify**:
- `akkado/include/akkado/ast.hpp` — add `spread_source` to `ArgumentData`
- `akkado/src/parser.cpp` — extend `parse_argument()` for `..` prefix; extend array literal parser

**Verification**:
- `f(..r)` and `f(..arr)` parse correctly
- `f(1, ..r, c: 3)` parses correctly
- `[..a, b, c]` parses correctly
- AST shows `spread_source` set on spread arguments
- Parser accepts both record and array spread in one call (analyzer will reject)

### Phase 2 — Analyzer Validation (1-2 days)

**Goal**: Analyzer validates spread source type and mutual exclusivity.

**Files to modify**:
- `akkado/src/analyzer.cpp` — check spread source type, check mutual exclusivity

**Verification**:
- `f(..r)` where `r = {a: 1}` — passes
- `f(..arr)` where `arr = [1, 2, 3]` — passes
- `f(..x)` where `x = 42` — error E140
- `f(..r, ..arr)` — error E180 (mutual exclusivity)
- `f(..unknown)` — existing undefined variable error

### Phase 3 — Record Spread Codegen (2-3 days)

**Goal**: `handle_user_function_call()` handles record spread with name-based matching.

**Files to modify**:
- `akkado/src/codegen_functions.cpp` — extend argument collection and matching logic

**Verification**:
- `r = {b: 2, a: 1}; fn f(a, b) -> a + b; f(..r)` — params matched by name
- `f(1, ..r, c: 3)` — positional + record spread + named combined
- `r = {a: 1, b: 2, extra: 3}; f(..r)` — W160 for extra field
- `r = {a: 1}; f(a: 10, ..r)` — spread overrides named arg (later wins)

### Phase 4 — Array Spread Codegen (2-3 days)

**Goal**: Array spread expands elements as positional arguments; array literal spread flattens.

**Files to modify**:
- `akkado/src/codegen_functions.cpp` — array spread in function calls
- `akkado/src/codegen.cpp` — array literal spread flattening

**Verification**:
- `arr = [1, 2, 3]; fn f(a, b, c) -> a+b+c; f(..arr)` — positional expansion
- `fn f(a, b); arr = [1, 2, 3]; f(..arr)` — error: too many args
- `[..[1,2], 3]` — flattens to `[1, 2, 3]`
- `f(1, ..arr)` where `arr = [2, 3]` — `f(1, 2, 3)`
- `f(..a, ..b)` where `a = [1], b = [2, 3]` — `f(1, 2, 3)`

### Phase 5 — Dot-Call Spread Receiver (1 day)

**Goal**: `{..r, freq: 440}.method()` uses spread record's first buffer as receiver.

**Files to modify**:
- `akkado/src/codegen.cpp` — `handle_method_call()` handles record receivers

**Verification**:
- `{freq: 440}.osc("sin")` — uses freq buffer as receiver
- `{..base, freq: 440}.osc("sin")` — uses merged record's first buffer

### Phase 6 — Testing and Integration (1-2 days)

**Files to modify**:
- All test files — comprehensive tests
- Integration with existing record spread tests

**Verification**:
- All existing tests pass (no regressions)
- New tests cover all syntax combinations
- Integration test: `preset = {bits: 12, sr: 26040}; samples("uri", ..preset)` compiles correctly

---

## 9. Edge Cases

### 9.1 Record Spread Edge Cases

#### 9.1.1 Empty Spread Source

| Input | Expected Behavior |
|-------|-------------------|
| `r = {}; f(..r)` | Valid. No fields injected. Missing params get defaults or error E105. |
| `f(..{})` | Valid. Inline empty record. Same as `f()`. |

#### 9.1.2 Multiple Record Spreads

| Input | Expected Behavior |
|-------|-------------------|
| `r1 = {a: 1}; r2 = {a: 2, b: 3}; f(..r1, ..r2)` | `a = 2` (r2 wins), `b = 3`. Order matters. |
| `f(..r2, ..r1)` | `a = 1` (r1 wins), no `b` field. |

#### 9.1.3 Record Spread + Named Override Order

| Input | Expected Behavior |
|-------|-------------------|
| `r = {a: 1}; f(..r, a: 10)` | `a = 10`. Named arg after spread overrides. |
| `f(a: 10, ..r)` where `r = {a: 1}` | `a = 1`. Spread after named arg overrides named. |

**Decision**: Later arguments always win. This matches the semantics of record-to-record spread where later fields override earlier ones.

#### 9.1.4 Record Spread + Positional Interaction

| Input | Expected Behavior |
|-------|-------------------|
| `fn f(a, b, c); r = {c: 3}; f(1, 2, ..r)` | `a = 1` (positional), `b = 2` (positional), `c = 3` (spread). |
| `fn f(a, b, c); r = {b: 2}; f(1, ..r)` | `a = 1` (positional), `b = 2` (spread), `c` missing → E105. |
| `fn f(a, b, c); r = {a: 10, b: 2}; f(1, ..r)` | `a = 1` (positional wins), `b = 2` (spread). `a: 10` from spread is unused → W160. |

**Decision**: Positional args fill params by index first. Record spread fills remaining unfilled params by name. If a spread field matches an already-filled param, it's an extra field (W160).

### 9.2 Array Spread Edge Cases

#### 9.2.1 Empty Array Spread

| Input | Expected Behavior |
|-------|-------------------|
| `arr = []; f(..arr)` | Valid. No positional args injected. Missing params get defaults or error E105. |
| `f(..[])` | Valid. Inline empty array. Same as `f()`. |

#### 9.2.2 Multiple Array Spreads

| Input | Expected Behavior |
|-------|-------------------|
| `a = [1]; b = [2, 3]; f(..a, ..b)` | `f(1, 2, 3)`. Elements concatenate left-to-right. |
| `f(..b, ..a)` | `f(2, 3, 1)`. |

#### 9.2.3 Array Spread + Positional

| Input | Expected Behavior |
|-------|-------------------|
| `fn f(a, b, c); arr = [2]; f(1, ..arr, 3)` | `a = 1`, `b = 2`, `c = 3`. |
| `fn f(a, b); arr = [2]; f(1, ..arr, 3)` | Error — 3 positional args for 2-param function. |

### 9.3 Mutual Exclusivity

| Input | Expected Behavior |
|-------|-------------------|
| `r = {a: 1}; arr = [2]; f(..r, ..arr)` | Error E180: Cannot mix record and array spread. |
| `f(..arr, ..r)` | Error E180 (same). |
| `f(..r, named: 3)` | Valid — record spread + named args. |
| `f(..arr)` | Valid — array spread only. |

### 9.4 Spread in Closures

| Input | Expected Behavior |
|-------|-------------------|
| `r = {a: 1}; g = (x) -> x; g(..r)` | Record spread applies to closure call. |
| `arr = [1]; g = (x) -> x; g(..arr)` | Array spread applies to closure call. |
| `n"c4" \|> {..r, freq: 440} as cfg \|> osc("sin", cfg.freq)` | Record spread in pipe binding works (existing feature). |

### 9.5 Spread with Pattern as Source

| Input | Expected Behavior |
|-------|-------------------|
| `p = n"c4"; f(..p)` | Pattern as record spread source — has `freq`, `vel`, `trig` fields. Fields match function params by name. |

### 9.6 Nested Spread in Record + Argument Spread

| Input | Expected Behavior |
|-------|-------------------|
| `base = {a: 1}; r = {..base, b: 2}; f(..r)` | Record spread resolves first → `r = {a: 1, b: 2}`. Then argument spread → `f(a: 1, b: 2)`. |

### 9.7 Array Literal Spread

| Input | Expected Behavior |
|-------|-------------------|
| `[..[], 1, 2]` | `[1, 2]` |
| `a = [1, 2]; b = [3, 4]; [..a, ..b]` | `[1, 2, 3, 4]` |
| `[..1]` | Error — spread source is not an array |
| `[..a, ..a]` | `[1, 2, 1, 2]` — same array spread twice is fine |

### 9.8 Dot-Call Receiver Ambiguity

| Input | Expected Behavior |
|-------|-------------------|
| `{freq: 440, vel: 0.8}.osc("sin")` | First buffer (`freq`) is the receiver. |
| `{vel: 0.8, freq: 440}.osc("sin")` | First buffer (`vel`) is the receiver. Order matters. |
| `{..empty, freq: 440}.osc("sin")` | `freq` is the first (and only) buffer → receiver. |

---

## 10. Testing / Verification Strategy

### 10.1 Parser Tests

| Test | Input | Expected |
|------|-------|----------|
| Record spread arg | `f(..r)` | Parses, `spread_source` set |
| Array spread arg | `f(..arr)` | Parses, `spread_source` set |
| Spread with named | `f(..r, c: 3)` | Two arguments: spread + named |
| Multiple record spreads | `f(..r1, ..r2)` | Two spread arguments |
| Positional + spread + named | `f(1, ..r, c: 3)` | Three arguments |
| Inline record spread | `f(..{a: 1, b: 2})` | Spread with inline record literal |
| Array literal spread | `[..a, b, c]` | Array with spread child |
| Multiple array spreads | `[..a, ..b]` | Array with two spread children |
| Both spreads in call | `f(..r, ..arr)` | Parses (analyzer rejects) |
| Dot-dot not confused with number | `f(.5)` | Parses as number literal, not spread |

### 10.2 Analyzer Tests

| Test | Input | Expected |
|------|-------|----------|
| Record spread source | `r = {a: 1}; f(..r)` | No error |
| Array spread source | `arr = [1, 2]; f(..arr)` | No error |
| Non-record/array spread | `x = 42; f(..x)` | Error E140 |
| Mutual exclusivity | `r = {a: 1}; arr = [2]; f(..r, ..arr)` | Error E180 |
| Undefined spread source | `f(..unknown)` | Undefined variable error (existing) |
| Pattern as spread source | `p = n"c4"; f(..p)` | No error (pattern has record fields) |
| Array literal spread | `a = [1, 2]; [..a, 3]` | No error |
| Array literal non-array spread | `x = 42; [..x]` | Error E140 |

### 10.3 Codegen Tests

| Test | Input | Expected |
|------|-------|----------|
| Basic record spread | `r = {b: 2, a: 1}; fn f(a, b) -> a + b; f(..r)` | Compiles, `a = 1`, `b = 2` |
| Record spread with override | `r = {a: 1, b: 2}; f(..r, b: 99)` | `a = 1`, `b = 99` |
| Named before record spread | `r = {a: 1, b: 2}; f(b: 99, ..r)` | `a = 1`, `b = 2` (spread overrides named) |
| Positional + record spread | `r = {c: 3}; fn f(a, b, c) -> ...; f(1, 2, ..r)` | `a = 1`, `b = 2`, `c = 3` |
| Extra field warning | `r = {a: 1, extra: 99}; fn f(a) -> a; f(..r)` | Warning W160, compiles |
| Missing param error | `r = {a: 1}; fn f(a, b, c) -> ...; f(..r)` | Error E105 for `b` and `c` |
| Empty record spread | `r = {}; fn f(a = 1) -> a; f(..r)` | Compiles, `a = 1` (default) |
| Spread into closure | `r = {a: 1}; g = (a) -> a; g(..r)` | Compiles, `a = 1` |
| Dot-call spread receiver | `{..{freq: 440}}.osc("sin")` | Compiles, freq buffer as receiver |
| Basic array spread | `arr = [1, 2, 3]; fn f(a, b, c) -> ...; f(..arr)` | Compiles, `a=1, b=2, c=3` |
| Array spread + positional | `arr = [2, 3]; f(1, ..arr)` | `a = 1`, `b = 2`, `c = 3` |
| Array spread too many args | `fn f(a, b); arr = [1, 2, 3]; f(..arr)` | Error E107 |
| Multiple array spreads | `a = [1]; b = [2, 3]; f(..a, ..b)` | `f(1, 2, 3)` |
| Array literal spread | `[..[1, 2], 3]` | Array with 3 elements: 1, 2, 3 |
| Array literal multiple spreads | `a = [1, 2]; b = [3, 4]; [..a, ..b]` | Array with 4 elements |
| Mixed spread in call | `r = {a: 1}; arr = [2]; f(..r, ..arr)` | Error E180 |

### 10.4 Integration Tests

```cpp
// test_codegen.cpp
TEST_CASE("samples() with spread preset") {
    auto result = akkado::compile(R"(
        preset = {bits: 12, sr: 26040}
        samples("my-bank", ..preset)
    )");
    CHECK(result.success);
    // Verify UriRequest has SampleLoadParams with bits=12, sr=26040
}

TEST_CASE("param() with spread config") {
    auto result = akkado::compile(R"(
        cfg = {min: 20, max: 2000}
        param("cutoff", 440, ..cfg)
    )");
    CHECK(result.success);
}

TEST_CASE("user fn spread with named matching") {
    auto result = akkado::compile(R"(
        config = {cutoff: 1000, res: 0.7}
        fn my_filter(in, cutoff, res) -> lp(in, cutoff, res)
        my_filter(osc("sin", 440), ..config)
    )");
    CHECK(result.success);
}

TEST_CASE("array spread in function call") {
    auto result = akkado::compile(R"(
        args = [1000, 0.7]
        fn filter(in, cutoff, res) -> lp(in, cutoff, res)
        filter(osc("sin", 440), ..args)
    )");
    CHECK(result.success);
}

TEST_CASE("array literal spread flattening") {
    auto result = akkado::compile(R"(
        a = [1, 2]
        b = [3, 4]
        c = [..a, ..b, 5]
        len(c)
    )");
    CHECK(result.success);
    // Verify c has 5 elements
}

TEST_CASE("mutual exclusivity error") {
    auto result = akkado::compile(R"(
        r = {a: 1}
        arr = [2, 3]
        fn f(a, b, c) -> a + b + c
        f(..r, ..arr)
    )");
    CHECK_FALSE(result.success);
    // Verify E180 error
}
```

### 10.5 Build/Run Commands

```bash
# Build
cmake --build build

# Run akkado tests
./build/akkado/tests/akkado_tests "[parser][spread]"
./build/akkado/tests/akkado_tests "[codegen][spread]"
./build/akkado/tests/akkado_tests "[analyzer][spread]"

# Run all tests
./build/akkado/tests/akkado_tests
./build/cedar/tests/cedar_tests
```

---

## 11. Open Questions

### 11.1 Spread Order: Named Before vs After

Currently proposed: later arguments always win. So `f(a: 10, ..r)` where `r = {a: 1}` gives `a = 1` (spread wins). Is this the desired behavior, or should explicit named args always take priority regardless of position?

**Recommendation**: Later wins — consistent with record-to-record spread semantics and predictable from left-to-right reading.

### 11.2 Warning vs Error for Extra Fields

Proposed: extra fields produce W160 warning. Alternative: strict mode could make this an error.

**Recommendation**: Start with warning. This allows records to carry more fields than a specific function needs (e.g., a general `config` record used across multiple functions with different parameter sets).

### 11.3 Spread in Builtin Special Handlers

Special handlers like `samples()` receive expanded named arguments after spread resolution. But some handlers may have positional params that spread could accidentally match by name. E.g., if `samples("uri", ..r)` and `r` has a field named `uri`, would that conflict?

**Recommendation**: Spread fields are treated as NAMED arguments, not positional. So `samples("uri", ..r)` where `r = {uri: "other"}` would try to set the `uri` keyword arg, which may or may not be valid depending on the handler's signature. This is consistent behavior and the user can always use `..{..r, uri: "correct"}` to override.
