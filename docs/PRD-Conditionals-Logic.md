# PRD: Conditionals and Logic Operators for Akkado

## Overview

Add signal-rate conditionals and logic operators to Akkado with both C-style infix syntax (`>`, `<`, `&&`, `||`) and function-call syntax (`gt`, `lt`, `band`, `bor`). These operators work on audio signals, outputting 0.0 or 1.0 values sample-by-sample.

## Problem Statement

Currently, Akkado has no way to express conditional logic at signal rate. Users cannot:
- Select between two signals based on a condition
- Create comparisons (is signal A greater than signal B?)
- Combine boolean signals with AND/OR/NOT logic

This limits pattern-based audio programming where decisions need to happen at audio rate.

## Goals

1. **C-style syntax** - Familiar operators: `>`, `<`, `>=`, `<=`, `==`, `!=`, `&&`, `||`, `!`
2. **Function-call syntax** - For consistency with other builtins: `gt`, `lt`, `select`, etc.
3. **Signal-rate operation** - All operations process 128 samples per block
4. **Boolean convention** - Values > 0 are true, otherwise false; outputs are 0.0 or 1.0

## Non-Goals

- Compile-time conditionals (if/else statements) - future work
- Short-circuit evaluation (not applicable at signal rate)
- Pattern-matching/match expressions - separate PRD

## Features

### 1. Comparison Operators

| Operator | Function | Description | Example |
|----------|----------|-------------|---------|
| `>` | `gt(a, b)` | Greater than | `osc("sin", 1) > 0` |
| `<` | `lt(a, b)` | Less than | `freq < 1000` |
| `>=` | `gte(a, b)` | Greater or equal | `amp >= 0.5` |
| `<=` | `lte(a, b)` | Less or equal | `x <= y` |
| `==` | `eq(a, b)` | Equality (epsilon) | `note == 60` |
| `!=` | `neq(a, b)` | Not equal (epsilon) | `note != 60` |

**Epsilon comparison:** Equality uses `|a - b| < 1e-6` for floating-point safety.

### 2. Logic Operators

| Operator | Function | Description | Example |
|----------|----------|-------------|---------|
| `&&` | `band(a, b)` | Logical AND | `(x > 0) && (y > 0)` |
| `\|\|` | `bor(a, b)` | Logical OR | `gate \|\| trig` |
| `!` | `bnot(a)` | Logical NOT | `!gate` |

**Truth convention:** Values > 0.0 are truthy; values <= 0.0 are falsy.

### 3. Select (Ternary)

| Function | Description | Example |
|----------|-------------|---------|
| `select(cond, a, b)` | If cond > 0, output a; else output b | `select(gate, osc1, osc2)` |

**Note:** No infix ternary syntax (`cond ? a : b`) - use `select()` function.

## Use Cases

### 1. Gate-based signal selection
```akkado
// Switch between oscillators based on gate
gate = pat("1 0 1 0")
select(gate, osc("saw", 440), osc("sqr", 220)) |> out(%, %)
```

### 2. Threshold-based effects
```akkado
// Apply distortion only when signal is loud
sig = osc("saw", 110)
loud = sig > 0.5
select(loud, dist(sig, 4), sig) |> out(%, %)
```

### 3. Rhythmic logic
```akkado
// Combine two rhythmic patterns with OR
gate1 = pat("1 0 0 0")
gate2 = pat("0 0 1 0")
combined = gate1 || gate2  // "1 0 1 0"
```

### 4. Signal-rate square wave from sine
```akkado
// Convert sine to square via comparison
osc("sin", 440) > 0 |> out(%, %)
```

### 5. Range detection
```akkado
// Output 1.0 when frequency is in range [200, 800]
freq = lfo(0.5) * 1000
in_range = (freq >= 200) && (freq <= 800)
```

## Architecture

### Cedar VM Layer (COMPLETE)

Opcodes 140-149 in `cedar/include/cedar/vm/instruction.hpp`:
```cpp
SELECT = 140,     // out = (cond > 0) ? a : b
CMP_GT = 141,     // out = (a > b) ? 1.0 : 0.0
CMP_LT = 142,     // out = (a < b) ? 1.0 : 0.0
CMP_GTE = 143,    // out = (a >= b) ? 1.0 : 0.0
CMP_LTE = 144,    // out = (a <= b) ? 1.0 : 0.0
CMP_EQ = 145,     // out = (|a - b| < epsilon) ? 1.0 : 0.0
CMP_NEQ = 146,    // out = (|a - b| >= epsilon) ? 1.0 : 0.0
LOGIC_AND = 147,  // out = ((a > 0) && (b > 0)) ? 1.0 : 0.0
LOGIC_OR = 148,   // out = ((a > 0) || (b > 0)) ? 1.0 : 0.0
LOGIC_NOT = 149,  // out = (a > 0) ? 0.0 : 1.0
```

Implementation in `cedar/include/cedar/opcodes/logic.hpp`.

### Akkado Builtin Layer (COMPLETE)

Builtins registered in `akkado/include/akkado/builtins.hpp`:
- `select`, `gt`, `lt`, `gte`, `lte`, `eq`, `neq`, `band`, `bor`, `bnot`

### Parser Layer (TODO)

Add infix operators to lexer and parser:

**Lexer tokens:**
```cpp
TOKEN_GT,        // >
TOKEN_LT,        // <
TOKEN_GTE,       // >=
TOKEN_LTE,       // <=
TOKEN_EQ_EQ,     // == (distinct from assignment =)
TOKEN_NEQ,       // !=
TOKEN_AND_AND,   // &&
TOKEN_OR_OR,     // ||
TOKEN_BANG,      // !
```

**Pratt parser precedence (low to high):**
```
||           (precedence 2)
&&           (precedence 3)
==, !=       (precedence 4)
>, <, >=, <= (precedence 5)
+, -         (precedence 6)
*, /         (precedence 7)
!            (prefix, precedence 8)
```

**AST lowering:** Parser desugars infix operators to function calls:
- `a > b` → `gt(a, b)`
- `a && b` → `band(a, b)`
- `!a` → `bnot(a)`

## Technical Design

### Lexer Changes

**File:** `akkado/src/lexer.cpp`

Add multi-character token recognition:
```cpp
case '>':
    if (peek() == '=') { advance(); return make_token(TOKEN_GTE); }
    return make_token(TOKEN_GT);
case '<':
    if (peek() == '=') { advance(); return make_token(TOKEN_LTE); }
    return make_token(TOKEN_LT);
case '=':
    if (peek() == '=') { advance(); return make_token(TOKEN_EQ_EQ); }
    return make_token(TOKEN_EQ);  // assignment
case '!':
    if (peek() == '=') { advance(); return make_token(TOKEN_NEQ); }
    return make_token(TOKEN_BANG);
case '&':
    if (peek() == '&') { advance(); return make_token(TOKEN_AND_AND); }
    // single & could be bitwise AND (future) or error
case '|':
    if (peek() == '|') { advance(); return make_token(TOKEN_OR_OR); }
    if (peek() == '>') { advance(); return make_token(TOKEN_PIPE); }
    // single | could be bitwise OR (future) or error
```

### Parser Changes

**File:** `akkado/src/parser.cpp`

Add infix parsing rules:
```cpp
// In get_precedence():
case TOKEN_OR_OR:    return 2;
case TOKEN_AND_AND:  return 3;
case TOKEN_EQ_EQ:
case TOKEN_NEQ:      return 4;
case TOKEN_GT:
case TOKEN_LT:
case TOKEN_GTE:
case TOKEN_LTE:      return 5;

// In parse_infix():
// Desugar to function call AST node
if (is_comparison_op(op)) {
    return make_call(op_to_builtin(op), {left, right});
}
```

**Operator to builtin mapping:**
```cpp
std::string op_to_builtin(TokenType op) {
    switch (op) {
        case TOKEN_GT:      return "gt";
        case TOKEN_LT:      return "lt";
        case TOKEN_GTE:     return "gte";
        case TOKEN_LTE:     return "lte";
        case TOKEN_EQ_EQ:   return "eq";
        case TOKEN_NEQ:     return "neq";
        case TOKEN_AND_AND: return "band";
        case TOKEN_OR_OR:   return "bor";
    }
}
```

### Prefix NOT Operator

**Parsing:**
```cpp
// In parse_prefix():
case TOKEN_BANG:
    return make_call("bnot", {parse_expression(PREC_UNARY)});
```

## Operator Precedence Table

| Precedence | Operators | Associativity |
|------------|-----------|---------------|
| 1 (lowest) | `\|>` | Left |
| 2 | `\|\|` | Left |
| 3 | `&&` | Left |
| 4 | `==`, `!=` | Left |
| 5 | `>`, `<`, `>=`, `<=` | Left |
| 6 | `+`, `-` | Left |
| 7 | `*`, `/`, `%` | Left |
| 8 | `!` (prefix) | Right |
| 9 (highest) | Function call, indexing | Left |

## Test Plan

### Unit Tests (akkado/tests/test_codegen.cpp)

**Tag:** `[conditionals]`

#### Comparison tests
```cpp
TEST_CASE("Comparison operators", "[conditionals]") {
    // Function syntax
    check_output("gt(10, 5)", 1.0f);
    check_output("gt(5, 10)", 0.0f);
    check_output("lt(5, 10)", 1.0f);
    check_output("gte(5, 5)", 1.0f);
    check_output("lte(5, 5)", 1.0f);
    check_output("eq(5, 5)", 1.0f);
    check_output("neq(5, 10)", 1.0f);

    // Infix syntax
    check_output("10 > 5", 1.0f);
    check_output("5 < 10", 1.0f);
    check_output("5 >= 5", 1.0f);
    check_output("5 == 5", 1.0f);
    check_output("5 != 10", 1.0f);
}
```

#### Logic tests
```cpp
TEST_CASE("Logic operators", "[conditionals]") {
    // Function syntax
    check_output("band(1, 1)", 1.0f);
    check_output("band(1, 0)", 0.0f);
    check_output("bor(1, 0)", 1.0f);
    check_output("bor(0, 0)", 0.0f);
    check_output("bnot(1)", 0.0f);
    check_output("bnot(0)", 1.0f);

    // Infix syntax
    check_output("1 && 1", 1.0f);
    check_output("1 || 0", 1.0f);
    check_output("!1", 0.0f);
}
```

#### Select tests
```cpp
TEST_CASE("Select function", "[conditionals]") {
    check_output("select(1, 100, 50)", 100.0f);
    check_output("select(0, 100, 50)", 50.0f);
    check_output("select(-1, 100, 50)", 50.0f);  // negative is falsy
}
```

#### Precedence tests
```cpp
TEST_CASE("Operator precedence", "[conditionals]") {
    // && binds tighter than ||
    check_output("1 || 0 && 0", 1.0f);  // 1 || (0 && 0) = 1

    // Comparison binds tighter than logic
    check_output("5 > 3 && 2 < 4", 1.0f);  // (5 > 3) && (2 < 4)

    // Arithmetic binds tighter than comparison
    check_output("2 + 3 > 4", 1.0f);  // (2 + 3) > 4 = 5 > 4 = 1
}
```

#### Signal-rate tests
```cpp
TEST_CASE("Signal-rate conditionals", "[conditionals]") {
    // Sine to square conversion
    check_signal("osc(\"sin\", 1) > 0", /* verify square wave */);

    // Select between oscillators
    check_compiles("select(pat(\"1 0\"), osc(\"saw\", 440), osc(\"sqr\", 220))");
}
```

## Implementation Checklist

### Phase 1: Function Syntax Tests (verify existing implementation)
- [ ] Add test cases for `gt`, `lt`, `gte`, `lte`, `eq`, `neq`
- [ ] Add test cases for `band`, `bor`, `bnot`
- [ ] Add test cases for `select`
- [ ] Verify all tests pass

### Phase 2: Lexer Changes
- [ ] Add tokens: `TOKEN_GT`, `TOKEN_LT`, `TOKEN_GTE`, `TOKEN_LTE`
- [ ] Add tokens: `TOKEN_EQ_EQ`, `TOKEN_NEQ`
- [ ] Add tokens: `TOKEN_AND_AND`, `TOKEN_OR_OR`, `TOKEN_BANG`
- [ ] Handle `>` vs `>=`, `<` vs `<=`, `=` vs `==`, `!` vs `!=`
- [ ] Handle `|` vs `||` vs `|>`, `&` vs `&&`

### Phase 3: Parser Changes
- [ ] Add precedence for comparison operators
- [ ] Add precedence for logic operators
- [ ] Add infix parsing rules
- [ ] Add prefix parsing for `!`
- [ ] Desugar to function call AST nodes

### Phase 4: Integration Tests
- [ ] Add infix syntax tests
- [ ] Add precedence tests
- [ ] Add signal-rate tests
- [ ] Run full test suite

### Phase 5: Documentation
- [ ] Update language reference
- [ ] Add examples to web docs
- [ ] Update builtin help (F1)

## Risks & Mitigations

| Risk | Mitigation |
|------|------------|
| `\|` conflicts with `\|>` pipe | Check for `>` after `\|` before deciding token type |
| `=` conflicts with `==` | Already handled; just add `==` case |
| Precedence errors | Comprehensive precedence tests |
| Breaking existing code | All changes are additive; existing syntax unchanged |

## Future Work

- **Ternary operator** `cond ? a : b` - sugar for `select(cond, a, b)`
- **Compile-time if/else** - for constant folding and array operations
- **Pattern matching** - `match` expressions for complex conditionals
- **Bitwise operators** - `&`, `|`, `^`, `~`, `<<`, `>>` for integer/bit manipulation
