---
title: Conditionals & Logic
category: language
order: 4
index_headings: true
keywords: [conditional, conditionals, logic, comparison, select, gt, lt, gte, lte, eq, neq, band, bor, bnot, ternary, if, else, boolean, and, or, not, equal, equality, greater, less, threshold, gate]
group: language
subgroup: syntax
icon: GitBranch
tagline: select, comparison, and boolean primitives.
---

# Conditionals & Logic

Signal-rate decision-making: compare signals, combine boolean signals with AND/OR/NOT, and pick between two signals based on a condition. Every operation runs sample-by-sample at audio rate.

## Truth convention

- A signal value is **truthy** when it is greater than `0.0`. Anything `<= 0.0` is **falsy** (including `0`, `-0`, and negative numbers).
- Comparison and logic operators output exactly `1.0` for true and `0.0` for false.
- Equality (`eq`, `neq`, `==`, `!=`) uses an epsilon of `1e-6` so floating-point round-off does not produce false negatives.

All operators below are also available as infix syntax; see [Operators](operators.md) for the precedence table.

---

## select

**Select** - Choose between two signals based on a condition.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| cond  | signal | -       | Condition signal: picks `a` when `cond > 0`, otherwise `b` |
| a     | signal | -       | Output when `cond` is truthy |
| b     | signal | -       | Output when `cond` is falsy |

Sample-by-sample mux. There is no infix ternary syntax; use `select(cond, a, b)`.

```akk
// Switch oscillators on a gate pattern
gate = pat("1 0 1 0")
select(gate, osc("saw", 440), osc("sqr", 220)) |> out(@)
```

```akk
// Apply distortion only when the input is loud
sig = osc("saw", 110)
select(sig > 0.5, dist(sig, 4), sig) |> out(@)
```

---

## gt

**Greater Than** - Outputs `1.0` when `a > b`, otherwise `0.0`.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| a     | signal | -       | First operand |
| b     | signal | -       | Second operand |

Equivalent to the `>` operator.

```akk
// Square wave from a sine via threshold
osc("sin", 440) > 0 |> out(@)
```

---

## lt

**Less Than** - Outputs `1.0` when `a < b`, otherwise `0.0`.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| a     | signal | -       | First operand |
| b     | signal | -       | Second operand |

Equivalent to the `<` operator.

```akk
// Open the filter only on quiet sections
freq = lfo(0.25) * 2000 + 200
osc("saw", 110) |> lp(@, freq * (freq < 1000)) |> out(@)
```

---

## gte

**Greater or Equal** - Outputs `1.0` when `a >= b`, otherwise `0.0`.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| a     | signal | -       | First operand |
| b     | signal | -       | Second operand |

Equivalent to the `>=` operator.

```akk
// Hold a gate above a threshold
amp = lfo(0.5)
gate = gte(amp, 0.5)
osc("sin", 440) * gate |> out(@)
```

---

## lte

**Less or Equal** - Outputs `1.0` when `a <= b`, otherwise `0.0`.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| a     | signal | -       | First operand |
| b     | signal | -       | Second operand |

Equivalent to the `<=` operator.

```akk
osc("sin", 440) * lte(lfo(0.5), 0) |> out(@)
```

---

## eq

**Approximate Equality** - Outputs `1.0` when `|a - b| < 1e-6`, otherwise `0.0`.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| a     | signal | -       | First operand |
| b     | signal | -       | Second operand |

Equivalent to the `==` operator. The epsilon protects against floating-point drift: values like `0.1 + 0.2` and `0.3` compare equal.

```akk
// Trigger only on exact step matches
freq = pat("c4 e4 g4 c5") |> @.freq
hit = eq(freq, 261.6)  // 1.0 only on c4 (~261.6 Hz)
```

---

## neq

**Approximate Inequality** - Outputs `1.0` when `|a - b| >= 1e-6`, otherwise `0.0`.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| a     | signal | -       | First operand |
| b     | signal | -       | Second operand |

Equivalent to the `!=` operator. The exact inverse of `eq` (same epsilon).

```akk
// Drop a voice on the rest steps only
freq = pat("c4 ~ g4 c5") |> @.freq
voice = osc("saw", freq) * neq(freq, 0)
voice |> out(@)
```

---

## band

**Logical AND** - Outputs `1.0` when both inputs are truthy (`> 0`), otherwise `0.0`.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| a     | signal | -       | First operand |
| b     | signal | -       | Second operand |

Equivalent to the `&&` operator.

```akk
// Accent only when both gates fire
loud = lfo(0.5) > 0.5
hit  = trigger(4)
band(loud, hit) |> ar(@, 0.005, 0.1) |> out(@)
```

---

## bor

**Logical OR** - Outputs `1.0` when at least one input is truthy, otherwise `0.0`.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| a     | signal | -       | First operand |
| b     | signal | -       | Second operand |

Equivalent to the `||` operator.

```akk
// Layer two rhythmic patterns into one gate
g1 = pat("1 0 0 0")
g2 = pat("0 0 1 0")
combined = bor(g1, g2)  // "1 0 1 0"
```

---

## bnot

**Logical NOT** - Outputs `1.0` when the input is falsy (`<= 0`), otherwise `0.0`.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| a     | signal | -       | Input |

Equivalent to the prefix `!` operator.

```akk
// Inverse gate, sustain when no trigger
gate = trigger(4)
sustain = bnot(gate)
osc("sin", 220) * sustain |> out(@)
```

---

## See also

- [Operators](operators.md): infix syntax (`>`, `<`, `&&`, `||`, `!`) and precedence
- [Math Functions](../builtins/math.md): arithmetic on signals
- [Pattern Functions](../builtins/sequencing.md): generating gates and triggers
