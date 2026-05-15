> **Status: DONE** — All 6 features implemented (string defaults, named args, closures, variadic, partial application, compose) and Phase 6 user-facing docs updated. One non-blocking edge case (strict misuse error for string-default params) deferred to §7.6.

# Advanced Function System PRD

## Executive Summary

This document specifies extensions to Akkado's user-defined function system:

- **String default parameters**: `fn osc(type = "sin", freq = 440) -> ...`
- **Named arguments for user functions**: `my_filter(sig, q: 2.0, cut: 500)`
- **Closures as return values**: `fn make_filter(cut) -> (sig) -> lp(sig, cut)`
- **Variadic rest parameters**: `fn mix(...sigs) -> sum(sigs) / len(sigs)`
- **Partial application**: `add3 = add(3, _)` creates `(x) -> add(3, x)`
- **Function composition**: `compose(lp(_, 1000), delay(_, 0.3))` chains functions

All features maintain the **full inlining model** -- no runtime function objects, no new VM opcodes. Everything resolves at compile time. The Cedar VM is completely unaffected.

---

## 1. Current State

### 1.1 What Exists Today

Akkado functions support:

- `fn name(params) -> body` with expression or block bodies
- Numeric-only default parameters: `fn f(x, cut = 1000) -> ...`
- Named arguments for **builtins only**: `lp(sig, q: 2.0, cut: 500)`
- Closures as function arguments: `fn apply(sig, fx) -> fx(sig)`
- Read-only variable capture in closures
- Match dispatch with compile-time resolution for literal arguments
- Full inlining at every call site
- Polyphonic expansion through function parameters (chords/arrays)

### 1.2 Current Limitations

| Limitation | Impact |
|---|---|
| Numeric-only defaults | Cannot write `fn osc(type = "sin")` -- the main stdlib pattern |
| Named args builtins-only | `my_filter(sig, cut: 500)` fails for user functions |
| Functions cannot return closures | Cannot build function factories like `make_filter(1000)` |
| Fixed arity | Cannot write `mix(a, b, c, d)` with arbitrary signal count |
| No partial application | Must manually write `(x) -> add(3, x)` instead of `add(3, _)` |
| No composition | Must manually write `(x) -> b(a(x))` instead of `compose(a, b)` |

### 1.3 Key Architecture Constraints

1. **Full inlining**: Functions are expanded at every call site. No call stack, no function pointers. This means all call-site argument counts are known at compile time.
2. **Immutable variables**: All Akkado variables are immutable. Closure capture is inherently safe.
3. **`visit()` returns `uint16_t`**: The codegen visitor returns a buffer index. Returning a function value requires a side-channel mechanism.
4. **`param_literals_` for match dispatch**: String/number literals passed as arguments are tracked in a map keyed by parameter name hash, enabling compile-time match resolution. String defaults must integrate with this mechanism.
5. **`multi_buffers_` for arrays**: Array operations (`sum`, `map`, `fold`, `len`) work through the multi-buffer system, keyed by AST `NodeIndex`. Variadic params must register as multi-buffers.

---

## 2. Feature Specifications

### 2.1 String Default Parameters

#### Syntax

```akkado
fn osc(type = "sin", freq = 440) -> match(type) {
    "sin": sine(freq)
    "saw": saw(freq)
    _: sine(freq)
}

osc(440)              // type defaults to "sin"
osc("saw", 440)       // explicit type
```

#### Rules

- String defaults use the same `= value` syntax as numeric defaults
- Only string and number literals are allowed as defaults (no expressions)
- String defaults participate in compile-time match resolution
- When a string default is used (argument omitted), it does NOT produce an audio buffer -- it exists only for match dispatch via `param_literals_`
- Required parameters must still precede optional parameters
- Mixing string and numeric defaults is allowed: `fn f(type = "sin", gain = 0.5)`

#### Interaction with Match Dispatch

The primary use case for string defaults is the `match(type)` pattern. When `osc(440)` is called:

1. `type` defaults to `"sin"`
2. The default string literal is added to `param_literals_`
3. `match(type)` resolves at compile time -- only the `"sin"` branch is emitted

This is identical to how `osc("sin", 440)` works today, except the string comes from the default rather than the call site.

#### Data Structure Changes

**`ClosureParamData`** (`ast.hpp`):
```cpp
struct ClosureParamData {
    std::string name;
    std::optional<double> default_value;       // numeric default
    std::optional<std::string> default_string; // string default
};
```

**`FunctionParamInfo`** (`symbol_table.hpp`):
```cpp
struct FunctionParamInfo {
    std::string name;
    std::optional<double> default_value;
    std::optional<std::string> default_string;
    NodeIndex default_node = NULL_NODE;  // Points to literal AST node for param_literals_
};
```

The `default_node` field stores the `NodeIndex` of the literal AST node (StringLit or NumberLit) allocated by the parser. This enables codegen to insert it directly into `param_literals_` without creating synthetic nodes.

**`ParsedParam`** (`parser.hpp`):
```cpp
struct ParsedParam {
    std::string name;
    std::optional<double> default_value;
    std::optional<std::string> default_string;
};
```

#### Parser Changes

**`parse_param_list()`** (`parser.cpp:662`): Extend default value parsing:

```cpp
if (match(TokenType::Equals)) {
    if (check(TokenType::Number)) {
        Token num_tok = advance();
        default_value = std::get<NumericValue>(num_tok.value).value;
        seen_default = true;
    } else if (check(TokenType::String)) {
        Token str_tok = advance();
        default_string = str_tok.as_string();
        seen_default = true;
    } else {
        error("Default parameter value must be a number or string literal");
        break;
    }
}
```

**`parse_grouping()` lookahead fix** (`parser.cpp:567-598`): The closure detection lookahead doesn't skip `= <default>` patterns after parameter names. This means `(x = 5) -> x` fails the lookahead and is parsed as a grouped expression. Fix: in the lookahead loop, after consuming an Identifier, also skip `= <literal>` if present:

```cpp
// Inside lookahead loop, after advancing past identifier:
if (check(TokenType::Equals)) {
    advance(); // consume '='
    if (check(TokenType::Number) || check(TokenType::String)) {
        advance(); // consume literal
    }
}
```

#### Codegen Changes

**`handle_user_function_call()`** (`codegen_functions.cpp:70-96`): Add string default branch:

```cpp
} else if (func.params[i].default_string.has_value()) {
    param_buf = BufferAllocator::BUFFER_UNUSED;
    std::uint32_t param_hash = fnv1a_hash(func.params[i].name);
    param_literals_[param_hash] = func.params[i].default_node;
}
```

Same change in `handle_function_value_call()`.

#### Analyzer Changes

- `collect_definitions()`: propagate `default_string` and `default_node` when building `UserFunctionInfo`
- Argument count validation: treat `default_string.has_value()` as "has default"

---

### 2.2 Named Arguments for User Functions

#### Syntax

```akkado
fn filtered(sig, cut = 1000, q = 0.7) -> lp(sig, cut, q)

filtered(noise())                    // defaults
filtered(noise(), 500)               // positional
filtered(noise(), cut: 500)          // named
filtered(noise(), q: 2.0, cut: 500)  // named, any order
```

#### Rules

- Same rules as builtin named args: positional args first, then named args
- Named args cannot conflict with positional args at the same position
- Duplicate named args are an error
- Unknown parameter names are an error
- After reordering, gaps are filled by defaults (existing codegen handles this)

#### Implementation

Named argument reordering already works for builtins via `reorder_named_arguments()` (`analyzer.cpp:1310`). It takes a `BuiltinInfo&` and uses `builtin.find_param(name)` to find parameter indices.

**Generalize** by extracting the core logic into an overload that accepts a parameter name list:

```cpp
bool reorder_named_arguments(NodeIndex call_node,
                             const std::vector<std::string>& param_names,
                             const std::string& func_name);
```

The existing `BuiltinInfo` overload becomes a wrapper that extracts param names from `builtin.param_names`.

In `resolve_and_validate()`, add the call for user functions:

```cpp
} else if (sym->kind == SymbolKind::UserFunction) {
    std::vector<std::string> pnames;
    for (const auto& p : sym->user_function.params) pnames.push_back(p.name);
    reorder_named_arguments(node, pnames, func_name);
    // existing arg count validation follows...
}
```

---

### 2.3 Closures as Return Values

#### Syntax

```akkado
fn make_filter(cut) -> (sig) -> lp(sig, cut)

filt = make_filter(1000)
noise() |> filt(@) |> out(@, @)
```

```akkado
fn make_voice(wave = "saw") -> (freq) -> {
    osc(wave, freq) |> lp(@, freq * 4)
}

my_voice = make_voice("tri")
my_voice(440) |> out(@, @)
```

#### Rules

- A function whose body is a Closure expression returns a `FunctionRef`
- Captured variables (function parameters) are bound at call time as read-only captures
- The returned closure can be assigned to a variable and called normally
- Nested closure returns work: `fn f(a) -> (b) -> (c) -> a + b + c`
- String parameters captured from the outer function participate in match dispatch when the inner closure is called

#### Core Challenge

`visit()` returns `uint16_t` (buffer index). A closure-returning function doesn't produce audio -- it produces a `FunctionRef`. A side-channel is needed.

#### Implementation

**New field** in `CodeGenerator` (`codegen.hpp`):

```cpp
std::optional<FunctionRef> pending_function_ref_;
```

**`handle_user_function_call()`** (`codegen_functions.cpp`): After pushing scope and binding params, before visiting body, check if the body is a Closure node:

```cpp
const Node& body_ref = ast_->arena[func.body_node];
if (body_ref.type == NodeType::Closure) {
    // Don't visit the closure -- construct a FunctionRef
    FunctionRef ref = build_closure_ref(func.body_node);

    // Capture bound function params
    for (size_t i = 0; i < func.params.size(); ++i) {
        if (param_bufs[i] != BUFFER_UNUSED) {
            ref.captures.push_back({func.params[i].name, param_bufs[i]});
        }
    }
    // Also propagate param_literals_ entries for string captures
    // (needed for match dispatch in the returned closure)

    pending_function_ref_ = ref;
    symbols_->pop_scope();
    // restore saved state...
    return BUFFER_UNUSED;
}
// Otherwise: normal body visit (existing code)
```

**Closure-ref construction**: The implementation reuses the existing `resolve_function_arg()` helper (`codegen.hpp:592`) directly on the function body node — no separate `build_closure_ref()` wrapper was needed. `resolve_function_arg()` extracts a `FunctionRef` from any closure or fn-reference AST node, which is exactly what's required here.

**Assignment handler** (`codegen.cpp`): After `visit(rhs)`, check for pending function ref:

```cpp
if (pending_function_ref_) {
    symbols_->define_function_value(var_name, *pending_function_ref_);
    pending_function_ref_ = std::nullopt;
    return BUFFER_UNUSED;
}
```

**Analyzer** (`analyzer.cpp`), `collect_definitions()`: When RHS is a Call to a user function whose body is a Closure node, register the LHS as `FunctionValue` instead of `Variable`. This ensures the symbol table has the correct kind before codegen runs:

```cpp
// In collect_definitions, Assignment case:
if (rhs is Call && called function body is Closure) {
    // Build a preliminary FunctionRef from the closure's params
    symbols_.define_function_value(name, preliminary_ref);
} else {
    // existing variable/array/record classification
}
```

#### Interaction with String Captures

When `make_filter("lowpass")` is called, and the function does `match(type)` inside the returned closure, the string literal `"lowpass"` must propagate through the capture mechanism into `param_literals_` when the closure is later called.

This requires storing literal values alongside captures in `FunctionRef`:

```cpp
struct FunctionRef {
    // ... existing fields ...
    std::unordered_map<std::uint32_t, NodeIndex> captured_literals;  // For match dispatch
};
```

When `handle_function_value_call()` processes a FunctionRef with `captured_literals`, it merges them into `param_literals_` before visiting the body.

---

### 2.4 Variadic Rest Parameters

#### Syntax

```akkado
fn mix(...sigs) -> sum(sigs) / len(sigs)

mix(a)              // 1 signal
mix(a, b)           // 2 signals
mix(a, b, c, d)     // 4 signals
```

```akkado
fn chain(input, ...fxs) -> fold(fxs, input, (acc, fx) -> fx(acc))

noise() |> chain(@, lp(_, 500), delay(_, 0.3), reverb(_))
```

```akkado
fn parallel(...fns) -> (sig) -> {
    sum(map(fns, (f) -> f(sig))) / len(fns)
}

fx = parallel(lp(_, 500), hp(_, 2000), bandpass(_, 800, 2))
noise() |> fx(@) |> out(@, @)
```

#### Rules

- Rest parameter uses `...name` syntax and must be the **last** parameter
- Rest parameter cannot have a default value
- Required/optional parameters can precede the rest parameter: `fn f(a, b = 5, ...rest)`
- Minimum argument count enforced (required params only), no maximum
- Rest parameter is an array in the function body -- all existing array operations work: `sum()`, `len()`, `map()`, `fold()`, `zipWith()`
- Empty rest (`fn f(...xs)` called as `f()`) produces an empty array: `len(xs) == 0`
- Rest args can be function values (closures/fn refs) -- see `chain` example above

#### Lexer Changes

**`token.hpp`**: Add `DotDotDot` token type.

**`lexer.cpp`**: When lexing `.`, check if the next two characters are also `.`. If so, consume all three and emit `DotDotDot`. This does not conflict with existing `.` usage (field access) because `...` never appears in field access position.

#### AST Changes

**`ClosureParamData`** (`ast.hpp`):
```cpp
struct ClosureParamData {
    std::string name;
    std::optional<double> default_value;
    std::optional<std::string> default_string;
    bool is_rest = false;
};
```

**`FunctionDefData`** (`ast.hpp`):
```cpp
struct FunctionDefData {
    std::string name;
    std::size_t param_count;
    bool has_rest_param = false;
};
```

#### Parser Changes

**`parse_param_list()`** (`parser.cpp:662`): Before parsing the identifier, check for `...`:

```cpp
bool is_rest = match(TokenType::DotDotDot);
// parse identifier name...
if (is_rest) {
    if (match(TokenType::Equals)) {
        error("Rest parameter cannot have a default value");
    }
    params.push_back(ParsedParam{name, std::nullopt, std::nullopt, true});
    if (!check(TokenType::RParen)) {
        error("Rest parameter must be the last parameter");
    }
    break;
}
```

**`parse_fn_def()`** (`parser.cpp:1137`): Store `has_rest_param` in `FunctionDefData`.

`ParsedParam`: Add `bool is_rest = false;`.

#### Symbol Table Changes

**`FunctionParamInfo`**: Add `bool is_rest = false;`.

**`UserFunctionInfo`**: Add `bool has_rest_param = false;`.

#### Analyzer Changes

Argument count validation: if `has_rest_param`, enforce minimum (required params before rest) but no maximum:

```cpp
if (fn.has_rest_param) {
    size_t min_args = count of params where !is_rest && !has_default;
    if (arg_count < min_args) error(...);
    // no max check
}
```

#### Codegen Changes

**`handle_user_function_call()`** (`codegen_functions.cpp`): When reaching the rest parameter in the binding loop:

1. Visit all remaining call arguments to get buffer indices
2. Define the rest param as `SymbolKind::Array` with `source_node = NULL_NODE`:

```cpp
if (func.params[i].is_rest) {
    std::vector<std::uint16_t> rest_buffers;
    for (size_t j = i; j < args.size(); ++j) {
        // Check if arg is a function ref (for variadic function args)
        auto func_ref = resolve_function_arg(args[j]);
        if (func_ref) {
            // Store function ref for later binding
            // ... handle variadic function args ...
        } else {
            rest_buffers.push_back(visit(args[j]));
        }
    }

    // Define as Array with pre-computed buffers
    ArrayInfo arr{};
    arr.buffer_indices = rest_buffers;
    arr.source_node = NULL_NODE;  // Synthetic array
    arr.element_count = rest_buffers.size();

    Symbol sym{};
    sym.kind = SymbolKind::Array;
    sym.name = func.params[i].name;
    sym.name_hash = fnv1a_hash(sym.name);
    sym.buffer_index = rest_buffers.empty() ? BUFFER_UNUSED : rest_buffers[0];
    sym.multi_buffers = rest_buffers;
    sym.array = arr;
    symbols_->define(sym);

    break;  // Rest consumes all remaining args
}
```

**Identifier handler for Array** (`codegen.cpp:435`): Add branch for synthetic arrays (`source_node == NULL_NODE`):

```cpp
if (sym->kind == SymbolKind::Array) {
    if (sym->array.source_node == NULL_NODE) {
        // Synthetic array (rest param) -- buffers already computed
        if (!sym->array.buffer_indices.empty()) {
            register_multi_buffer(node, sym->array.buffer_indices);
            node_buffers_[node] = sym->array.buffer_indices[0];
            return sym->array.buffer_indices[0];
        }
        std::uint16_t zero = emit_zero(buffers_, instructions_);
        node_buffers_[node] = zero;
        return zero;
    }
    // Original path: visit source node...
}
```

Existing `handle_len_call()` already checks `SymbolKind::Array` and uses `sym->array.element_count` -- works as-is.

Existing `handle_sum_call()`, `handle_map_call()`, `handle_fold_call()` use `is_multi_buffer()` / `get_multi_buffers()` -- works as-is since we register `multi_buffers` on the Symbol.

---

### 2.5 Partial Application

#### Syntax

```akkado
fn add(a, b) -> a + b
add3 = add(3, _)     // creates (x) -> add(3, x)
add3(4)              // returns 7

fn clamp(lo, x, hi) -> min(max(x, lo), hi)
clamp01 = clamp(0, _, 1)  // creates (x) -> clamp(0, x, 1)

// Works with builtins too
soft_lp = lp(_, 500, 0.7)   // creates (sig) -> lp(sig, 500, 0.7)
noise() |> soft_lp(@) |> out(@, @)

// Multiple placeholders
f = clamp(_, _, 1)    // creates (a, b) -> clamp(a, b, 1)
```

#### Rules

- `_` in argument position creates a partial application
- Each `_` becomes a parameter of the resulting closure, in left-to-right order
- The result is a `FunctionValue` (closure) that can be assigned and called
- Works with both builtins and user-defined functions
- Can be passed directly as a function argument: `map(arr, add(3, _))`
- Can be used inline in pipe chains: `noise() |> lp(_, 500)(@) |> out(@, @)`

#### Unambiguity with Match Wildcards

`_` is `TokenType::Underscore`. In match arms, it's parsed by match-specific code (`parser.cpp:1060`) and creates a `MatchArm` with `is_wildcard = true`. In argument position, it goes through `parse_prefix()` and becomes `Identifier("_")`. These are completely different AST paths -- no conflict.

#### Parser Changes

**`parse_prefix()`** (`parser.cpp`): Add case for `TokenType::Underscore`:

```cpp
case TokenType::Underscore: {
    Token tok = advance();
    NodeIndex node = make_node(NodeType::Identifier, tok);
    arena_[node].data = Node::IdentifierData{"_"};
    return node;
}
```

This allows `_` to appear anywhere an expression is expected.

#### Analyzer Changes

Partial application rewriting happens in **`rewrite_pipes()`** (pass 2), where AST transformations already occur.

**Detection**: Scan Call node arguments for `Identifier("_")`.

**`rewrite_partial_application(NodeIndex call_node)`**:

1. Scan arguments for `Identifier("_")` placeholders
2. Generate unique param names: `__p0`, `__p1`, etc.
3. Replace each `_` identifier with the generated param name
4. Transform the Call node in-place into a Closure node:
   - Create Identifier children for generated params
   - The original Call (with `_` replaced) becomes the body

```
Before:  Call[add, NumberLit(3), Identifier("_")]
After:   Closure[Identifier("__p0"), Call[add, NumberLit(3), Identifier("__p0")]]
```

**`collect_definitions()`** (pass 1): Detect calls with `_` args and register the LHS as `FunctionValue` (since after the pass 2 rewrite it will be a Closure). This ensures the symbol table is correct before codegen.

```cpp
// In collect_definitions, Assignment case:
if (rhs is Call && call_has_placeholder_args(rhs)) {
    FunctionRef ref{};
    ref.closure_node = rhs;  // Will be rewritten to Closure in pass 2
    ref.is_user_function = false;
    // Count placeholders to determine arity
    int ph_count = count_placeholders(rhs);
    for (int i = 0; i < ph_count; ++i) {
        ref.params.push_back({"__p" + std::to_string(i)});
    }
    symbols_.define_function_value(name, ref);
}
```

---

### 2.6 Function Composition via `compose()`

#### Syntax

```akkado
// Two functions
pipeline = compose(lp(_, 1000), delay(_, 0.3))
noise() |> pipeline(@) |> out(@, @)

// Three or more functions
fx_chain = compose(lp(_, 800), saturate(_, 0.3), reverb(_))
saw(220) |> fx_chain(@) |> out(@, @)

// With user-defined functions
fn double(x) -> x * 2
fn inc(x) -> x + 1
f = compose(double, inc)   // (x) -> inc(double(x))
f(5)  // returns 11

// Inline use
noise() |> compose(lp(_, 500), delay(_, 0.3))(@) |> out(@, @)
```

#### Rules

- `compose(a, b)` creates a function `(x) -> b(a(x))` -- left-to-right application order (like Unix pipes)
- All arguments must be functions (closures, fn references, or partial applications)
- Accepts 2 or more arguments: `compose(a, b, c)` = `(x) -> c(b(a(x)))`
- The result is a `FunctionValue` that can be assigned and called
- Each function in the chain receives the output of the previous as its single argument
- A composition operator (syntax TBD) may be added in a future iteration

#### Implementation

**Special handler**: Add `compose` to the `special_handlers` map in `codegen.cpp` (alongside `map`, `fold`, `sum`).

**`handle_compose_call()`** (`codegen_functions.cpp`):

```cpp
std::uint16_t handle_compose_call(NodeIndex node, const Node& n) {
    // Resolve all arguments as function refs
    std::vector<FunctionRef> chain;
    NodeIndex arg = n.first_child;
    while (arg != NULL_NODE) {
        NodeIndex arg_value = unwrap_argument(arg);
        auto ref = resolve_function_arg(arg_value);
        if (!ref) {
            error("compose() arguments must be functions", ...);
            return BUFFER_UNUSED;
        }
        chain.push_back(*ref);
        arg = ast_->arena[arg].next_sibling;
    }

    if (chain.size() < 2) {
        error("compose() requires at least 2 function arguments", ...);
        return BUFFER_UNUSED;
    }

    // Build a composed FunctionRef
    FunctionRef composed{};
    composed.closure_node = NULL_NODE;
    composed.is_user_function = false;
    composed.params = {{"__compose_input"}};
    composed.compose_chain = std::move(chain);

    pending_function_ref_ = composed;
    return BUFFER_UNUSED;
}
```

**Composed ref execution** in `handle_function_value_call()`: When a FunctionRef has a non-empty `compose_chain`, apply each ref in sequence:

```cpp
if (!func.compose_chain.empty()) {
    // Apply chain: result = chain[n](...(chain[1](chain[0](input))))
    std::uint16_t current_buf = /* visit the single argument */;
    for (const auto& ref : func.compose_chain) {
        current_buf = apply_function_ref(ref, current_buf, n.location);
    }
    node_buffers_[node] = current_buf;
    return current_buf;
}
```

**`FunctionRef` extension** (`symbol_table.hpp`):

```cpp
struct FunctionRef {
    NodeIndex closure_node;
    std::vector<FunctionParamInfo> params;
    std::vector<CaptureInfo> captures;
    bool is_user_function;
    std::string user_function_name;
    std::vector<FunctionRef> compose_chain;  // For compose()
    std::unordered_map<std::uint32_t, NodeIndex> captured_literals;  // For match dispatch
};
```

---

## 3. Implementation Order

Features are ordered by dependency:

```
1. String Default Parameters      (standalone -- foundation for stdlib osc)
2. Named Arguments for User Fns   (standalone -- small scope, builds on existing infra)
3. Closures as Return Values      (enables features 5 and 6)
4. Variadic Rest Parameters       (standalone -- builds on existing array infra)
5. Partial Application            (depends on 3 -- uses closure machinery)
6. Function Composition           (depends on 5 -- uses partial application)
```

Features 1 and 2 are independent and can be done in parallel.
Feature 4 is independent of 3/5/6 and can be done in parallel with 3.

---

## 4. Files to Modify

### Lexer & Tokens
| File | Changes | Features |
|------|---------|----------|
| `akkado/include/akkado/token.hpp` | Add `DotDotDot` token type | 4 |
| `akkado/src/lexer.cpp` | Lex `...` (three consecutive dots) | 4 |

### AST
| File | Changes | Features |
|------|---------|----------|
| `akkado/include/akkado/ast.hpp` | Extend `ClosureParamData` (string default, is_rest), extend `FunctionDefData` (has_rest_param) | 1, 4 |

### Parser
| File | Changes | Features |
|------|---------|----------|
| `akkado/include/akkado/parser.hpp` | Extend `ParsedParam` (string default, is_rest) | 1, 4 |
| `akkado/src/parser.cpp` | `parse_param_list()` (string/rest), `parse_grouping()` lookahead fix, `parse_prefix()` underscore case | 1, 4, 5 |

### Symbol Table
| File | Changes | Features |
|------|---------|----------|
| `akkado/include/akkado/symbol_table.hpp` | Extend `FunctionParamInfo` (string default, default_node, is_rest), extend `UserFunctionInfo` (has_rest_param), extend `FunctionRef` (compose_chain, captured_literals) | 1, 3, 4, 6 |

### Analyzer
| File | Changes | Features |
|------|---------|----------|
| `akkado/include/akkado/analyzer.hpp` | New `reorder_named_arguments` overload, `rewrite_partial_application` decl | 2, 5 |
| `akkado/src/analyzer.cpp` | String default propagation, named arg reordering for user fns, closure-return detection in `collect_definitions`, variadic arg count validation, partial application rewriting, placeholder detection | 1, 2, 3, 4, 5 |

### Code Generator
| File | Changes | Features |
|------|---------|----------|
| `akkado/include/akkado/codegen.hpp` | `pending_function_ref_` field, `build_closure_ref()` decl, `handle_compose_call()` decl | 3, 6 |
| `akkado/src/codegen_functions.cpp` | String default codegen, closure-as-return detection, `build_closure_ref()`, rest param array binding, `handle_compose_call()`, composed ref execution | 1, 3, 4, 6 |
| `akkado/src/codegen.cpp` | Assignment handler for `pending_function_ref_`, Array identifier with `NULL_NODE` source, `compose` in special handler map | 3, 4, 6 |

### Documentation
| File | Changes | Features |
|------|---------|----------|
| `docs/agent-guide-userspace-functions.md` | Update for all new features | All |
| `web/static/docs/reference/language/closures.md` | Update closure docs | 3, 5, 6 |

---

## 5. Testing Strategy

### 5.1 Parser Tests (`test_parser.cpp`)

```akkado
// String defaults
fn osc(type = "sin", freq = 440) -> freq
fn multi(a, type = "saw", gain = 0.5) -> a

// Rest params
fn mix(...sigs) -> sigs
fn chain(input, ...fxs) -> input

// Partial application (after rewrite, should be Closure)
add(3, _)
lp(_, 500, 0.7)
f(_, 2, _)
```

### 5.2 Analyzer Tests (`test_analyzer.cpp`)

```akkado
// Named args for user functions
fn f(a, b, c) -> a + b + c
f(1, c: 3, b: 2)  // reordered to f(1, 2, 3)

// Named args with defaults and gaps
fn g(a, b = 5, c = 10) -> a + b + c
g(1, c: 20)  // reordered to g(1, <default>, 20)

// Closure-returning function classification
fn make_gain(amt) -> (sig) -> sig * amt
g = make_gain(0.5)  // g should be FunctionValue, not Variable

// Variadic arg count
fn mix(...sigs) -> sigs
mix()        // OK (0 rest args)
mix(a, b, c) // OK (3 rest args)

// Partial application classification
add3 = add(3, _)  // add3 should be FunctionValue
```

### 5.3 End-to-End Tests (`test_akkado.cpp`)

```akkado
// Feature 1: String defaults
fn osc(type = "sin", freq = 440) -> match(type) {
    "sin": freq * 1.0
    "saw": freq * 2.0
    _: freq * 0.5
}
osc(100)           // type="sin" -> 100.0
osc("saw", 100)    // type="saw" -> 200.0

// Feature 2: Named args
fn f(a, b = 5, c = 10) -> a + b + c
f(1, c: 20)        // 1 + 5 + 20 = 26

// Feature 3: Closure return
fn make_gain(amt) -> (sig) -> sig * amt
g = make_gain(0.5)
g(100)              // 50.0

fn make_filter(cut) -> (sig) -> lp(sig, cut)
filt = make_filter(1000)
noise() |> filt(@) |> out(@, @)  // should compile and produce filtered noise

// Feature 4: Variadic
fn mix(...sigs) -> sum(sigs) / len(sigs)
mix(100)                    // 100.0
mix(100, 200)               // 150.0
mix(100, 200, 300)          // 200.0

fn chain(input, ...fxs) -> fold(fxs, input, (acc, fx) -> fx(acc))
// chain(noise(), lp(_, 500), delay(_, 0.3))

// Feature 5: Partial application
fn add(a, b) -> a + b
add3 = add(3, _)
add3(4)             // 7.0

soft_lp = lp(_, 500, 0.7)
noise() |> soft_lp(@) |> out(@, @)

// Feature 6: Compose
fn double(x) -> x * 2
fn inc(x) -> x + 1
f = compose(double, inc)
f(5)                // 11.0 (double first, then inc)

pipeline = compose(lp(_, 1000), delay(_, 0.3))
noise() |> pipeline(@) |> out(@, @)
```

### 5.4 Build & Run

```bash
cmake --build build && ./build/akkado/tests/akkado_tests
```

---

## 6. Edge Cases & Pitfalls

### 6.1 String Defaults

- String defaults must NOT emit `PUSH_CONST` -- they have no buffer representation. `param_buf` must be `BUFFER_UNUSED`.
- Ensure the closure lookahead fix doesn't break `(x + y)` (grouped expression, not closure).
- **Known gap (deferred):** Using a string-defaulted param outside a `match()` scrutinee (e.g. `type * 2`) currently compiles silently rather than emitting a typed error. Properly enforcing this requires propagating string-default tags through nested user-fn calls so a chain like `outer() -> inner(type)` still routes the literal correctly. Tracked under §7.6.

### 6.2 Named Arguments

- When a named arg gap is left (omitted optional param), the reordered child list has a missing position. Codegen fills it with the default. Verify this interaction works correctly.
- Named args for variadic functions: named args should only apply to non-rest params. `mix(a, b, c)` where `mix` has `...sigs` -- all args go to rest, no names.

### 6.3 Closures as Return Values

- Captures reference buffer indices. Since buffers are linearly allocated and never freed, captured indices remain valid.
- Nested closure returns: `fn f(a) -> (b) -> (c) -> a + b + c` -- each level captures the outer params. Handle recursively.
- String literals captured from outer function must propagate to `param_literals_` when the inner closure is called.

### 6.4 Variadic Rest Parameters

- Empty rest args: `mix()` should produce `len(sigs) == 0`. Division by zero in `sum(sigs) / len(sigs)` is a user responsibility (document it).
- Rest params with function values: `chain(input, lp(_, 500))` -- the rest args should be detected as function refs via `resolve_function_arg()`. Need to handle mixed audio and function rest args (or restrict to one kind).
- `len()` depends on `SymbolKind::Array` with `element_count` -- ensure this is populated.

### 6.5 Partial Application

- `_` must not be treated as a variable reference. It's a syntax marker only.
- Partial application of a partially applied function: `f = add(_, _); g = f(3, _)` -- this is equivalent to `g = add(3, _)` but requires `f` to be a FunctionValue first. The rewrite in pass 2 handles this: `f(3, _)` sees `f` is a FunctionValue call with a `_`, so it rewrites to a closure `(x) -> f(3, x)`.
- Partial application in pipe chains: `noise() |> lp(_, 500)(@)` -- the `lp(_, 500)` produces a closure, then `(@)` calls it with pipe input. This requires the parser to handle `expr(args)` where `expr` is a partial application.

### 6.6 Compose

- `compose()` with a single argument should be an error (or identity? -- recommend error for clarity).
- All arguments must resolve as function refs. Non-function arguments should produce a clear error: "compose() argument 2 is not a function."
- Composed functions should participate in semantic path tracking for unique state IDs.

---

## 7. Future Extensions

### 7.1 Composition Operator

A dedicated operator (syntax TBD, potentially `>>`, `|>>`, or `~>`) for inline composition without `compose()`:

```akkado
pipeline = lp(_, 1000) >> delay(_, 0.3) >> reverb(_)
```

### 7.2 Recursive Functions

Currently blocked by the inlining model. Could be enabled with a fixed recursion depth limit and stack-based inlining.

### 7.3 Forward Declarations

Allow calling functions defined later in the source. Requires a two-pass approach in `collect_definitions`.

### 7.4 Type-Checked Parameters

Integration with the planned type system (`prd-compiler-type-system.md`) to validate parameter types at compile time.

### 7.5 Pattern/Expression Defaults

Allow expressions as defaults: `fn f(x, gain = param("gain", 0.5))`. Currently blocked by the requirement for literal-only defaults.

### 7.6 Strict Misuse Error for String-Default Params

Reject programs that reference a string-defaulted parameter outside a `match()` scrutinee with a clear diagnostic ("Cannot use string parameter '<name>' as audio signal — only valid as a `match()` scrutinee"). Requires propagating string-default tags through nested user-fn calls so legitimate forwarding (`outer() -> inner(type)`) still resolves at compile time.

---

## 8. Implementation Checklist

### Phase 1: String Defaults & Named Args (independent, can parallelize)
- [ ] Extend `ClosureParamData` with `default_string` field
- [ ] Extend `FunctionParamInfo` with `default_string` and `default_node` fields
- [ ] Extend `ParsedParam` with `default_string` and `is_rest` fields
- [ ] Update `parse_param_list()` to accept string literals as defaults
- [ ] Fix `parse_grouping()` lookahead to skip `= <literal>` patterns
- [ ] Propagate string defaults through analyzer `collect_definitions()`
- [ ] Update argument count validation for string defaults
- [ ] Add string default codegen in `handle_user_function_call()` and `handle_function_value_call()`
- [ ] Generalize `reorder_named_arguments()` to accept param name list
- [ ] Call reorder for user function calls in `resolve_and_validate()`
- [ ] Parser tests for string defaults
- [ ] Analyzer tests for named args with user functions
- [ ] End-to-end tests for both features

### Phase 2: Closures as Return Values
- [ ] Add `pending_function_ref_` field to `CodeGenerator`
- [ ] Add `captured_literals` field to `FunctionRef`
- [ ] Implement `build_closure_ref()` helper
- [ ] Detect Closure body in `handle_user_function_call()` and construct FunctionRef
- [ ] Handle `pending_function_ref_` in Assignment codegen
- [ ] Detect closure-returning calls in analyzer `collect_definitions()`
- [ ] Propagate `captured_literals` through `handle_function_value_call()`
- [ ] Tests for single-level and nested closure returns
- [ ] Tests for string capture propagation through returned closures

### Phase 3: Variadic Rest Parameters
- [ ] Add `DotDotDot` token type
- [ ] Implement `...` lexing
- [ ] Extend `ClosureParamData` with `is_rest` flag
- [ ] Extend `FunctionDefData` with `has_rest_param` flag
- [ ] Extend `FunctionParamInfo` with `is_rest` flag
- [ ] Extend `UserFunctionInfo` with `has_rest_param` flag
- [ ] Update `parse_param_list()` for rest parameter syntax
- [ ] Update `parse_fn_def()` to store `has_rest_param`
- [ ] Update analyzer arg count validation for variadic
- [ ] Implement rest param array binding in codegen `handle_user_function_call()`
- [ ] Handle `source_node == NULL_NODE` in Array identifier codegen
- [ ] Tests for variadic with 0, 1, N args
- [ ] Tests for `sum()`, `len()`, `map()`, `fold()` on rest params
- [ ] Tests for variadic with preceding required/optional params

### Phase 4: Partial Application
- [ ] Add `Underscore` case in `parse_prefix()` producing `Identifier("_")`
- [ ] Implement `rewrite_partial_application()` in analyzer (pass 2)
- [ ] Detect placeholder calls in `collect_definitions()` (pass 1)
- [ ] Tests for single placeholder: `add(3, _)`
- [ ] Tests for multiple placeholders: `f(_, 2, _)`
- [ ] Tests for partial application of builtins: `lp(_, 500)`
- [ ] Tests for partial application passed as function argument: `map(arr, add(3, _))`

### Phase 5: Function Composition
- [ ] Add `compose_chain` field to `FunctionRef`
- [ ] Add `compose` to `special_handlers` map
- [ ] Implement `handle_compose_call()`
- [ ] Handle composed refs in `handle_function_value_call()`
- [ ] Tests for 2-arg compose
- [ ] Tests for 3+ arg compose
- [ ] Tests for compose with partial application
- [ ] Tests for composed function in pipe chains

### Phase 6: Documentation
- [x] Update `docs/agent-guide-userspace-functions.md` (2026-04-26)
- [x] Update `web/static/docs/reference/language/closures.md` (2026-04-26)
- [x] Run `bun run build:docs` to rebuild docs index (2026-04-26)
