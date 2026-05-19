---
title: Operators
category: language
order: 3
keywords: [operators, precedence, arithmetic, plus, minus, multiply, divide, power, pipe, comparison, logic, conditional, gt, lt, gte, lte, eq, neq, equal, equality, greater, less, and, or, not, boolean, ternary]
group: language
subgroup: syntax
icon: Calculator
tagline: Arithmetic, comparison, logic, and pipe operators with precedence.
---

# Operators

Akkado uses operators for arithmetic, comparison, logic, and signal flow.

## Operator precedence

From highest to lowest precedence:

| Precedence | Operator | Name | Associativity | Example |
|------------|----------|------|---------------|---------|
| 9 (highest) | `(...)`, `f(...)` | Call, group | Left | `lp(sig, 800)` |
| 8 | `!` | Prefix NOT | Right | `!gate` |
| 7 | `^` | Power | Right | `x ^ 2` |
| 6 | `*` `/` `@` | Multiply, divide, modulo | Left | `a * b` |
| 5 | `+` `-` | Add, subtract | Left | `a + b` |
| 4 | `>` `<` `>=` `<=` | Comparison | Left | `x > 0` |
| 3 | `==` `!=` | Equality | Left | `x == 0` |
| 2 | `&&` | Logical AND | Left | `a && b` |
| 1 | `\|\|` | Logical OR | Left | `a \|\| b` |
| 0 (lowest) | `\|>` | Pipe | Left | `a \|> b` |

## Arithmetic operators

All arithmetic operators work on signals at audio rate:

```akk
// Addition - mix two oscillators
osc("sin", 220) + osc("sin", 330) |> out(@)

// Subtraction - phase cancellation
osc("sin", 220) - osc("sin", 220.5) |> out(@)

// Multiplication - amplitude modulation
osc("sin", 220) * osc("sin", 5) |> out(@)

// Division - scaling
osc("saw", 110) / 2 |> out(@)

// Power - exponential curves
lfo(0.5) ^ 2 |> out(@)
```

## Comparison operators

Compare two signals sample-by-sample. Output is `1.0` (true) or `0.0` (false).

```akk
// Gate from a continuous signal
lfo(0.5) > 0 |> ar(@, 0.01, 0.1) |> out(@)

// Square wave from a sine via threshold
osc("sin", 440) > 0 |> out(@)

// Range detection
freq = lfo(0.5) * 1000
in_range = (freq >= 200) && (freq <= 800)
```

`==` and `!=` use an epsilon of `1e-6`, so floating-point drift does not produce false negatives.

## Logic operators

Combine boolean signals. Inputs are treated as truthy when `> 0`. Outputs are `0.0` or `1.0`.

```akk
// Layered gates
g1 = n"1 0 0 0"
g2 = n"0 0 1 0"
combined = g1 || g2  // "1 0 1 0"

// Accent only when both conditions fire
loud = lfo(0.5) > 0.5
hit = trigger(4)
band = loud && hit

// Inverse gate, fires when there's no trigger
sustain = !trigger(4)
```

## Conditional selection

There is no infix ternary; use the `select` function:

```akk
// Switch oscillators based on a gate
gate = n"1 0 1 0"
select(gate, osc("saw", 440), osc("sqr", 220)) |> out(@)
```

## Operator desugaring

All operators are converted to function calls during parsing:

| Operator | Desugars To |
|----------|-------------|
| `a + b`  | `add(a, b)` |
| `a - b`  | `sub(a, b)` |
| `a * b`  | `mul(a, b)` |
| `a / b`  | `div(a, b)` |
| `a ^ b`  | `pow(a, b)` |
| `a > b`  | `gt(a, b)` |
| `a < b`  | `lt(a, b)` |
| `a >= b` | `gte(a, b)` |
| `a <= b` | `lte(a, b)` |
| `a == b` | `eq(a, b)` |
| `a != b` | `neq(a, b)` |
| `a && b` | `band(a, b)` |
| `a \|\| b` | `bor(a, b)` |
| `!a`     | `bnot(a)` |

## The pipe operator

The `|>` operator defines signal flow. It has the lowest precedence so it always splits an expression at the natural boundary:

```akk
// Signal flows left to right
osc("saw", 110) |> lp(@, 800) |> out(@)
```

See [Pipes & Holes](pipes) for full details.

## The hole operator

The `@` symbol is an explicit input port:

```akk
// @ refers to the left-hand side of |>
osc("saw", 110) |> lp(@, 500) |> out(@)
```

## Combining operators

Operators follow precedence rules:

```akk
// Multiplication before addition
a + b * c    // equals a + (b * c)

// && binds tighter than ||
1 || 0 && 0  // equals 1 || (0 && 0) = 1

// Comparison binds tighter than logic
5 > 3 && 2 < 4  // equals (5 > 3) && (2 < 4) = 1

// Arithmetic binds tighter than comparison
2 + 3 > 4    // equals (2 + 3) > 4 = 1

// Use parentheses to override
(a + b) * c
```

## Unary minus

Negation works on numbers and signals:

```akk
// Negative number
saw(-110)   // Won't work - use neg()

// Correct way
osc("saw", 110) |> neg(@) |> out(@)

// Or multiply by -1
osc("saw", 110) * -1 |> out(@)
```

Related: [Pipes & Holes](pipes), [Math Functions](../builtins/math), [Conditionals & Logic](conditionals)
