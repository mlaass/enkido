> **Status: DONE** — Implemented with smart disambiguation (default-filling vs partial application).

# Underscore Placeholder PRD — Positional Default Skipping

## Executive Summary

This PRD specifies using `_` (underscore) as a positional placeholder in function call arguments to skip optional parameters and use their default values. This allows users to set a later optional parameter without manually specifying all preceding ones.

### Why?

Many DSP builtins have 3-5+ parameters where only the first few are required. Today, to set the 4th parameter you must explicitly pass the defaults for parameters 2 and 3:

```akkado
// Current: must know and repeat default values to set 'wet'
delay(@, 0.25, 0.0, 0.0, 0.8)

// Proposed: skip middle params, compiler fills defaults
delay(@, 0.25, _, _, 0.8)
```

This is especially valuable for DSP functions where default values are carefully tuned (filter Q, delay feedback, envelope times) and users shouldn't need to look them up just to reach a later parameter.

**Key design decisions:**
- `_` works for both **builtins** and **user-defined functions** with defaults
- `_` on a **required parameter** (no default) is a **compile error**
- Outside function call arguments, `_` remains a normal identifier (e.g., match wildcards)
- Trailing arguments can still be omitted as today — `_` is only needed to skip **middle** parameters
- Multiple consecutive `_` are allowed (each skips one position)

---

## 1. Current State

### 1.1 Token

`TokenType::Underscore` already exists (`token.hpp:78`). The lexer recognizes standalone `_` as a distinct token (`lexer.cpp:176-179`). The parser currently converts it to `Identifier("_")` in expression contexts and uses it as a wildcard in `match` arms.

### 1.2 Optional Parameters

Builtins define optional parameters via `BuiltinInfo.optional_count` and `BuiltinInfo.defaults[]` (`builtins.hpp:72-121`). When arguments are omitted at the end of a call, the compiler emits `PUSH_CONST` instructions for each default (`codegen.cpp:1045-1069`).

User functions define defaults via `FunctionParamInfo.default_value`, `.default_string`, or `.default_node` (`symbol_table.hpp:16-23`). Missing trailing arguments are filled from these defaults (`codegen_functions.cpp:165-279`).

### 1.3 Limitation

Today, defaults can only fill **trailing** missing arguments. There is no way to skip a middle parameter:

```akkado
// delay(in, time, fb, dry, wet) — dry defaults to 0.0, wet to 1.0
delay(@, 0.25, 0.8)          // OK: skips dry and wet (trailing)
delay(@, 0.25, _, _, 0.8)    // ERROR today: _ is not handled as skip
```

---

## 2. Target Syntax

### 2.1 Basic Usage

```akkado
// Skip optional middle parameters
delay(@, 0.25, _, _, 0.8)       // time=0.25, fb=default, dry=default, wet=0.8
lp(@, 5000, _)                  // cut=5000, q=default (0.707)
adsr(gate, _, _, 0.8)           // attack=default, decay=default, sustain=0.8
```

### 2.2 User-Defined Functions

```akkado
fn synth(freq, wave = "saw", cut = 2000, res = 0.5) =
    osc(wave, freq) |> lp(@, cut, res)

// Skip wave and cut, set res
synth(440, _, _, 0.9)

// Skip just wave
synth(440, _, 8000)
```

### 2.3 Trailing Omission Still Works

```akkado
// These remain equivalent — no trailing _ needed
delay(@, 0.25, 0.8)
delay(@, 0.25, 0.8, _, _)   // Valid but redundant
```

### 2.4 Error Cases

```akkado
// _ on required parameter — compile error (E106)
delay(_, 0.25, 0.8)
// Error E106: Cannot skip required parameter 'in' — no default value

// _ in named argument — compile error (E107)
delay(@, time: _)
// Error E107: '_' placeholder not valid in named arguments — use named arg omission instead
```

### 2.5 Non-Assignable: `_` as Reserved Identifier

`_` cannot be used as an assignment target. It is a reserved identifier.

```akkado
_ = 42           // Error: Cannot assign to reserved identifier '_'
const _ = 42     // Error: Cannot assign to reserved identifier '_'
fn _(x) -> x     // Error: Cannot use '_' as a function name
```

In expression position, `_` is a normal identifier reference (which will fail as "undefined variable" since `_` can never be defined). This preserves its existing use as a wildcard in `match` arms.

```akkado
match(v) { _: 0 }  // Valid: wildcard (existing behavior)
```

---

## 3. Architecture

### 3.1 AST Representation

No new AST node type needed. The parser already creates `Identifier("_")` for underscore in expression position. The codegen will detect this sentinel name when processing function arguments.

### 3.2 Compilation Flow

For a call like `delay(@, 0.25, _, _, 0.8)`:

```
1. Parser produces 5 argument nodes: [Hole, Number(0.25), Ident("_"), Ident("_"), Number(0.8)]
2. Codegen iterates arguments positionally against parameter list
3. For each argument:
   - If argument is Ident("_"):
     a. Look up parameter at this position
     b. If parameter has a default → emit PUSH_CONST with default value (E106 if no default)
     c. If argument is named (name: _) → emit error E107
   - Otherwise: visit argument normally
```

### 3.3 Disambiguation: Default-Filling vs Partial Application

`_` in function call arguments has **two** meanings, disambiguated by parameter metadata:

- **All `_` at positions with defaults** → **default-filling**: `_` is replaced with the parameter's default value at codegen time. The call proceeds normally.
- **Any `_` at a required position** → **partial application**: the analyzer rewrites the call into a closure (existing behavior).

This is implemented in the analyzer (`analyzer.cpp`) via `all_placeholders_have_defaults()`, checked at both pass 1 (definition collection) and pass 2 (pipe rewriting). When default-filling is determined, the `_` Identifier nodes survive into codegen unchanged.

### 3.4 Detection

In codegen, when processing each argument node, check:

```cpp
bool is_placeholder = (arena_[arg_node].type == NodeType::Identifier &&
                        arena_[arg_node].as_identifier() == "_");
```

This check happens **before** normal identifier resolution, so `_` never reaches the symbol table lookup.

---

## 4. Impact Assessment

| Component | Status | Notes |
|-----------|--------|-------|
| `token.hpp` | **No change** | `TokenType::Underscore` already exists |
| `lexer.cpp` | **No change** | Already lexes `_` correctly |
| `parser.cpp` | **No change** | Already parses `_` as `Identifier("_")` |
| `codegen.cpp` | **Modified** | Builtin call: detect `_` args, fill defaults |
| `codegen_functions.cpp` | **Modified** | User fn call: detect `_` args, fill defaults |
| `match` wildcards | **No change** | Match arm parsing is a distinct code path; `_` remains wildcard |
| Mini-notation `~` | **No change** | Rest in patterns is a separate system |
| Other expression contexts | **No change** | `_` remains a normal identifier outside call arguments |

---

## 5. File-Level Changes

| File | Change |
|------|--------|
| `akkado/src/codegen.cpp` | In builtin call compilation (~line 820-1089): when iterating argument nodes, detect `Identifier("_")` and substitute the builtin's default value. Emit E106 if parameter has no default, E107 if used in named argument position. |
| `akkado/src/codegen_functions.cpp` | In user function call compilation (~line 165-279): same detection — check for `_` placeholder, use `FunctionParamInfo` default. Emit E106 if no default exists, E107 for named args. |
| `akkado/tests/test_codegen.cpp` | Tests for builtin calls with `_`, user fn calls with `_`, error cases (E106, E107) |
| `akkado/tests/test_parser.cpp` | Verify `_` in argument position parses correctly (may already pass) |

---

## 6. Edge Cases

### 6.1 All Arguments Are Placeholders

```akkado
lp(@, _, _)   // Both cut and q use defaults — equivalent to lp(@)
```
Valid. Each `_` fills its position's default.

### 6.2 Underscore as Variable Name

```akkado
_foo = 42        // Valid identifier starting with underscore
f(_foo)          // Passes variable _foo, NOT a placeholder
f(_)             // Placeholder — standalone _ only
```
No conflict. Only standalone `_` (lexed as `TokenType::Underscore`, parsed as `Identifier("_")`) triggers placeholder behavior. Identifiers starting with underscore like `_foo` are normal variables.

### 6.3 Variadic / Rest Parameters

```akkado
fn f(x, ...rest) = ...
f(1, _, 3)    // _ here is in rest position — no default exists
```
Compile error: rest parameters have no defaults. `_` is not valid in rest parameter position.

### 6.4 User Function with Expression Default

```akkado
fn f(x, y = sin(0.5) * 100) = x + y
f(10, _)    // Uses compile-time evaluated default for y
```
Valid for **constant** expression defaults (literals, math builtins, const variables). `ConstEvaluator` evaluates the default node at compile time — same path as trailing omission today.

**Limitation:** Expression defaults that reference other parameters (e.g., `y = x * 2`) are **not** supported — `ConstEvaluator` does not receive parameter bindings, so it cannot resolve `x`. This fails with E105 ("Cannot evaluate default expression at compile time"). This is an existing limitation of trailing omission, not specific to `_`.

### 6.5 Named Arguments Mixed with Positional

```akkado
delay(@, 0.25, _, wet: 0.8)   // Positional _ then named arg
```
The `_` applies to positional parameter 2 (`fb`). Named args bypass positional ordering. `_` in a named arg position (`wet: _`) is error E107, since named args already skip by omission.

### 6.6 Redundant Trailing Underscores

```akkado
delay(@, 0.25, 0.8, _, _)   // Explicit _ for trailing params
```
Valid but redundant. Same result as `delay(@, 0.25, 0.8)`. No warning emitted.

---

## 7. Testing Strategy

### 7.1 Builtin Calls

```cpp
// _ skips middle optional param
auto r = compile("delay(osc(\"sin\", 440), 0.25, _, _, 0.8)");
CHECK(r.success);

// _ on required param → E106
auto r2 = compile("delay(_, 0.25, 0.8)");
CHECK_FALSE(r2.success);
CHECK(has_error_code(r2, "E106"));

// Same bytecode: explicit defaults vs _
auto r3 = compile("lp(osc(\"sin\", 440), 5000, _)");
auto r4 = compile("lp(osc(\"sin\", 440), 5000)");
CHECK(r3.bytecode.size() == r4.bytecode.size());
```

### 7.2 User-Defined Functions

```cpp
// _ with numeric default
auto r = compile("fn f(x, y = 10, z = 20) = x + y + z\nf(1, _, 30)");
CHECK(r.success);

// _ with constant expression default
auto r2 = compile("fn g(x, y = 100) = x * y\ng(5, _)");
CHECK(r2.success);

// _ on required user fn param → E106
auto r3 = compile("fn h(x, y) = x + y\nh(1, _)");
CHECK_FALSE(r3.success);
CHECK(has_error_code(r3, "E106"));
```

### 7.3 Error Cases

```cpp
// _ in named argument → E107
auto r = compile("delay(osc(\"sin\", 440), time: _)");
CHECK_FALSE(r.success);
CHECK(has_error_code(r, "E107"));
```

### 7.4 Build & Run

```bash
cmake --build build --target akkado_tests
./build/akkado/tests/akkado_tests "[codegen]"
./build/akkado/tests/akkado_tests "[parser]"
```

---

## 8. Implementation Checklist

- [ ] `codegen.cpp`: Detect `Identifier("_")` in builtin call argument iteration, emit `PUSH_CONST` with default
- [ ] `codegen.cpp`: Emit E106 when `_` is used on a required builtin parameter
- [ ] `codegen.cpp`: Emit E107 when `_` is used in a named argument position
- [ ] `codegen_functions.cpp`: Same `_` detection for user function calls, fill from `FunctionParamInfo` defaults
- [ ] `codegen_functions.cpp`: Emit E106 for required user fn params, E107 for named args
- [ ] `test_codegen.cpp`: Builtin `_` tests (skip middle, all placeholders, redundant trailing)
- [ ] `test_codegen.cpp`: User fn `_` tests (numeric default, expression default, required param error)
- [ ] `test_codegen.cpp`: Error case tests (E106, E107)
- [ ] `test_codegen.cpp`: Bytecode equivalence test (explicit defaults vs `_`)
- [ ] `test_parser.cpp`: Verify `_` in argument position parses as `Identifier("_")`

---

## 9. Done Criteria

- All checklist items complete and tests passing
- `delay(@, 0.25, _, _, 0.8)` compiles to identical bytecode as `delay(@, 0.25, 0.0, 0.0, 0.8)`
- `_` on required parameter produces E106 with clear error message
- `_` in named argument produces E107
- `_` in non-call contexts (assignment, match, array) is unaffected — remains a normal identifier
- No regressions in existing `match` wildcard behavior

---

## 10. Addendum: Specialized Handler Retrofit (2026-05-28)

### Problem

The codegen has two paths that already honored `_` correctly:

- the generic builtin path at `akkado/src/codegen.cpp:1352-1398` via the
  `CallSlot::Underscore` branch, and
- the user-function path at `akkado/src/codegen_functions.cpp:460-509`
  reading `FunctionParamInfo` defaults.

But several builtins compile through **specialized C++ handlers** that
extract args via `codegen::extract_call_args` and then read AST nodes
positionally. None of them knew about `_`:

- `handle_poly_call` (poly / mono / legato) — required `NodeType::NumberLit`
  for `voices` (E402) and `release` (E406).
- `handle_tap_delay_call` (tap_delay / _ms / _smp) — called `visit()` on
  `dry` / `wet` directly, so `_` was looked up as an unbound identifier.
- `handle_bus_call`, `handle_mixer_call` — no optional params today, but
  any future addition would silently regress.

`poly(@, instr, _, 2.5)` therefore failed with E402 even though the
BuiltinInfo declared `voices=64`, `release=0`.

### Fix

A single shared helper in `akkado/include/akkado/codegen/arrays.hpp`:

```cpp
void resolve_underscore_defaults(AstArena& arena,
                                 const StringInterner& interner,
                                 std::vector<NodeIndex>& args,
                                 const BuiltinInfo& builtin);
```

Walks `args` in place. For each entry that unwraps to `Identifier("_")`
and whose slot has a numeric default in `builtin`, allocates a synthetic
`NumberLit` node in `arena` and overwrites the entry. Numeric defaults
only — no specialized handler today reads a string-defaulted optional via
`extract_call_args`.

The predicate (`Identifier` + `IdentifierData` + view-equals "_") is
identical to the one used by `analyzer.cpp::is_placeholder_node` and the
generic `CallSlot::Underscore` branch; any future change to `_`'s
lexical form must update all three sites.

The arena passed in is taken via `const_cast<AstArena&>(ast_->arena)`
inside each handler. The arena was never strictly read-only by contract,
only by convention; the alternative was to relax const across every
internal codegen helper, which would have been much noisier.

### Handlers updated

- `handle_poly_call` — calls the helper between `extract_call_args` and
  arg parsing, looking up the BuiltinInfo for the **active mode**
  (`poly` / `mono` / `legato`) because each has a distinct
  `optional_count`. The E402 / E406 checks downstream stay as defensive
  guards for non-`_` non-literal expressions.
- `handle_tap_delay_call` — also migrated from a hand-rolled arg-walk to
  `extract_call_args(arena, child, 4, 6)`. Helper runs before the
  `visit(args[4])` / `visit(args[5])` calls so dry/wet `_` resolve cleanly.
- `handle_bus_call`, `handle_mixer_call` — invoke the helper as a no-op
  today (no defaults), locking in the architectural rule for future
  BuiltinInfo edits.

`handle_compose_call` and `handle_function_value_call` were already
correct: the former has no optional params; the latter routes back
through the user-function path that already handles `_`.

### Bug fix in BuiltinInfo metadata

The `defaults[]` array for `poly`, `mono`, and `legato` was indexed by
**absolute parameter position** (NaN padding for required slots up
front) rather than the project's convention of **optional-slot
indexing**. `BuiltinInfo::has_default(idx)` therefore returned `false`
for `voices` / `release`, which (a) made the analyzer pre-pass route any
`_` on those slots into partial application and (b) reported wrong
defaults through `builtins_json` (the editor autocomplete source).

Corrected (numeric defaults moved to the start of the `defaults` array,
matching `delay`, `lp`, every other builtin):

- `poly`:    `{NAN, NAN, 64.0f, 0.0f, NAN}` → `{64.0f, 0.0f, NAN, NAN, NAN}`
- `mono`:    `{NAN, NAN, 0.0f,  NAN,  NAN}` → `{NAN,   0.0f, NAN, NAN, NAN}`
- `legato`:  `{NAN, NAN, 0.0f,  NAN,  NAN}` → `{NAN,   0.0f, NAN, NAN, NAN}`

A `metadata: poly/mono/legato declare defaults at the right index`
section in `test_akkado.cpp` pins the new layout against future
regressions.

### Architectural rule going forward

Any **new specialized call handler** that does its own arg extraction
(does not route through the generic `CallSlot` dispatcher) MUST call
`codegen::resolve_underscore_defaults()` immediately after
`extract_call_args()`, passing the BuiltinInfo for the specific call
mode. Defensive "must be a number literal" checks downstream stay — `_`
simply no longer reaches them.

### Tests

Added to `akkado/tests/test_akkado.cpp` under the existing
`TEST_CASE("Underscore placeholder default-filling")`:

- `specialized: poly with _ for voices uses default 64`
- `specialized: poly with _ for release uses default 0`
- `specialized: poly with both optionals as _`
- `specialized: tap_delay with _ for dry`
- `metadata: poly/mono/legato declare defaults at the right index`
