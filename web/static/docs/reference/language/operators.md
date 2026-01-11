---
title: Operators
category: language
order: 3
keywords: [operators, precedence, arithmetic, plus, minus, multiply, divide, power, pipe]
---

# Operators

Akkado uses operators for arithmetic, signal flow, and more.

## Operator Precedence

From highest to lowest precedence:

| Operator | Name | Example |
|----------|------|---------|
| `.` | Method call | `sig.map(f)` |
| `^` | Power | `x ^ 2` |
| `*` `/` | Multiply, Divide | `a * b` |
| `+` `-` | Add, Subtract | `a + b` |
| `\|>` | Pipe | `a \|> b` |

## Arithmetic Operators

All arithmetic operators work on signals at audio rate:

```akk
// Addition - mix two oscillators
sin(220) + sin(330) |> out(%, %)

// Subtraction - phase cancellation
sin(220) - sin(220.5) |> out(%, %)

// Multiplication - amplitude modulation
sin(220) * sin(5) |> out(%, %)

// Division - scaling
saw(110) / 2 |> out(%, %)

// Power - exponential curves
lfo(0.5) ^ 2 |> out(%, %)
```

## Operator Desugaring

Operators are converted to function calls:

| Operator | Desugars To |
|----------|-------------|
| `a + b` | `add(a, b)` |
| `a - b` | `sub(a, b)` |
| `a * b` | `mul(a, b)` |
| `a / b` | `div(a, b)` |
| `a ^ b` | `pow(a, b)` |

## The Pipe Operator

The `|>` operator defines signal flow. It has the lowest precedence:

```akk
// Signal flows left to right
saw(110) |> lp(%, 800) |> out(%, %)
```

See [Pipes & Holes](pipes) for full details.

## The Hole Operator

The `%` symbol is an explicit input port:

```akk
// % refers to the left-hand side of |>
saw(110) |> lp(%, 500) |> out(%, %)
```

## Combining Operators

Operators follow precedence rules:

```akk
// Multiplication before addition
a + b * c    // equals a + (b * c)

// Use parentheses to override
(a + b) * c  // equals (a + b) * c

// Pipe has lowest precedence
a + b |> lp(%, 500)  // equals (a + b) |> lp(%, 500)
```

## Unary Minus

Negation works on numbers and signals:

```akk
// Negative number
saw(-110)   // Won't work - use neg()

// Correct way
saw(110) |> neg(%) |> out(%, %)

// Or multiply by -1
saw(110) * -1 |> out(%, %)
```

Related: [Pipes & Holes](pipes), [Math Functions](../builtins/math)
