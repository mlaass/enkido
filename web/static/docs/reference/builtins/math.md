---
title: Math Functions
category: builtins
order: 10
keywords: [math, add, sub, mul, div, pow, neg, abs, sqrt, log, exp, floor, ceil, min, max, clamp, wrap, sin, cos, tan, asin, acos, atan, atan2, sinh, cosh, tanh, trigonometry, hyperbolic]
---

# Math Functions

Mathematical operations for signal processing and control logic.

**Note:** `sin(x)`, `cos(x)`, `tan(x)`, and `tanh(x)` are pure math functions operating on values in radians. For audio oscillators, use `osc("sin", freq)` or `osc("saw", freq)` etc.

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
add(osc("sin", 220), osc("sin", 330)) * 0.5 |> out(%, %)
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
sub(osc("sin", 220), osc("sin", 221)) |> out(%, %)
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
mul(osc("sin", 220), osc("sin", 30)) |> out(%, %)
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
neg(osc("sin", 220)) |> out(%, %)
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
abs(osc("sin", 110)) |> lp(%, 50) |> out(%, %)
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
min(osc("sin", 220), 0.5) |> out(%, %)
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
max(osc("sin", 220), 0) |> out(%, %)
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
clamp(osc("saw", 110), -0.5, 0.5) |> out(%, %)
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
wrap(osc("phasor", 1) * 3, 0, 1) |> out(%, %)
```

---

## Trigonometric Functions

These functions operate on values in **radians**. They are pure math functions, not audio oscillators.

### sin

**Sine** - Returns the sine of an angle in radians.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| x     | signal | -       | Angle in radians |

**Important:** This is a pure math function, not an oscillator. For a sine oscillator, use `osc("sin", freq)`.

```akk
// Create a sine wave manually from a phasor
sin(osc("phasor", 440) * 2 * 3.14159) |> out(%, %)
```

```akk
// Waveshaping with sine
osc("saw", 110) |> sin(% * 3.14159) |> out(%, %)
```

---

### cos

**Cosine** - Returns the cosine of an angle in radians.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| x     | signal | -       | Angle in radians |

```akk
// Cosine is sine shifted by 90 degrees
cos(osc("phasor", 440) * 2 * 3.14159) |> out(%, %)
```

---

### tan

**Tangent** - Returns the tangent of an angle in radians.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| x     | signal | -       | Angle in radians |

```akk
// Tangent waveshaping (careful - goes to infinity!)
osc("sin", 110) * 0.3 |> tan(%) |> clamp(%, -1, 1) |> out(%, %)
```

---

### asin

**Arcsine** - Returns the inverse sine (result in radians).

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| x     | signal | -       | Value between -1 and 1 |

---

### acos

**Arccosine** - Returns the inverse cosine (result in radians).

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| x     | signal | -       | Value between -1 and 1 |

---

### atan

**Arctangent** - Returns the inverse tangent (result in radians).

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| x     | signal | -       | Input value |

Useful for soft saturation effects.

```akk
// Soft saturation using atan
osc("saw", 110) * 3 |> atan(%) / 1.57 |> out(%, %)
```

---

### atan2

**Two-argument Arctangent** - Returns the angle between the positive x-axis and the point (x, y).

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| y     | signal | -       | Y coordinate |
| x     | signal | -       | X coordinate |

Useful for phase calculations and coordinate conversions.

---

## Hyperbolic Functions

### sinh

**Hyperbolic Sine** - Returns the hyperbolic sine.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| x     | signal | -       | Input value |

---

### cosh

**Hyperbolic Cosine** - Returns the hyperbolic cosine.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| x     | signal | -       | Input value |

---

### tanh

**Hyperbolic Tangent** - Returns the hyperbolic tangent.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| x     | signal | -       | Input value |

**Important:** This is a pure math function. For tanh-based distortion with drive control, use `saturate(in, drive)`.

The `tanh` function outputs values between -1 and 1, making it useful for custom waveshaping when combined with multiplication.

```akk
// Manual saturation using tanh
osc("saw", 110) * 3 |> tanh(%) |> out(%, %)
```

```akk
// Adjustable saturation (multiply input for more drive)
osc("saw", 110) |> tanh(% * 5) |> out(%, %)
```

Related: [saturate](distortion#saturate)
