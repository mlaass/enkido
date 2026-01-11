---
title: Math Functions
category: builtins
order: 10
keywords: [math, add, sub, mul, div, pow, neg, abs, sqrt, log, exp, floor, ceil, min, max, clamp, wrap]
---

# Math Functions

Mathematical operations for signal processing and control logic.

## Arithmetic

### add

**Add** - Adds two signals.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| a     | signal | -       | First operand |
| b     | signal | -       | Second operand |

Equivalent to the `+` operator.

```akk
// Mixing two oscillators
add(sin(220), sin(330)) * 0.5 |> out(%, %)
```

---

### sub

**Subtract** - Subtracts second signal from first.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| a     | signal | -       | First operand |
| b     | signal | -       | Second operand |

Equivalent to the `-` operator.

```akk
// Difference of oscillators
sub(sin(220), sin(221)) |> out(%, %)
```

---

### mul

**Multiply** - Multiplies two signals.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| a     | signal | -       | First operand |
| b     | signal | -       | Second operand |

Equivalent to the `*` operator. Commonly used for amplitude modulation.

```akk
// Ring modulation
mul(sin(220), sin(30)) |> out(%, %)
```

---

### div

**Divide** - Divides first signal by second.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| a     | signal | -       | Dividend |
| b     | signal | -       | Divisor |

Equivalent to the `/` operator.

---

### pow

**Power** - Raises base to exponent.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| base  | signal | -       | Base value |
| exp   | signal | -       | Exponent |

Equivalent to the `^` operator.

```akk
// Exponential curve
pow(lfo(0.5), 2) |> out(%, %)
```

---

## Unary Math

### neg

**Negate** - Inverts the sign of a signal.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| x     | signal | -       | Input |

```akk
// Invert phase
neg(sin(220)) |> out(%, %)
```

---

### abs

**Absolute Value** - Returns the absolute value.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| x     | signal | -       | Input |

Useful for full-wave rectification or envelope following.

```akk
// Full-wave rectification
abs(sin(110)) |> lp(%, 50) |> out(%, %)
```

---

### sqrt

**Square Root** - Returns the square root.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| x     | signal | -       | Input (should be >= 0) |

---

### log

**Natural Logarithm** - Returns ln(x).

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| x     | signal | -       | Input (should be > 0) |

---

### exp

**Exponential** - Returns e^x.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| x     | signal | -       | Input |

Useful for exponential envelopes and frequency scaling.

---

### floor

**Floor** - Rounds down to nearest integer.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| x     | signal | -       | Input |

```akk
// Quantize an LFO
floor(lfo(0.5) * 8) / 8 |> out(%, %)
```

---

### ceil

**Ceiling** - Rounds up to nearest integer.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| x     | signal | -       | Input |

---

## Binary Math

### min

**Minimum** - Returns the smaller of two values.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| a     | signal | -       | First value |
| b     | signal | -       | Second value |

```akk
// Limit signal to 0.5
min(sin(220), 0.5) |> out(%, %)
```

---

### max

**Maximum** - Returns the larger of two values.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| a     | signal | -       | First value |
| b     | signal | -       | Second value |

```akk
// Ensure signal doesn't go below 0
max(sin(220), 0) |> out(%, %)
```

---

## Ternary Math

### clamp

**Clamp** - Constrains value between lo and hi.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| x     | signal | -       | Input value |
| lo    | signal | -       | Lower bound |
| hi    | signal | -       | Upper bound |

```akk
// Keep signal in -0.5 to 0.5 range
clamp(saw(110), -0.5, 0.5) |> out(%, %)
```

---

### wrap

**Wrap** - Wraps value around range boundaries.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| x     | signal | -       | Input value |
| lo    | signal | -       | Lower bound |
| hi    | signal | -       | Upper bound |

When the value exceeds hi, it wraps to lo (and vice versa). Useful for creating sawtooth-like modulation.

```akk
// Wrapped phasor
wrap(phasor(1) * 3, 0, 1) |> out(%, %)
```
