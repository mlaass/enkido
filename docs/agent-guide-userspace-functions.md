> **Status: REFERENCE** — Developer guide for function features. Current.

# Agent Guide: Writing Akkado Userspace Functions

Quick reference for writing `fn` definitions — stdlib helpers, DSP chains, or utility functions.

## Syntax

Two body forms:

```akkado
// Expression form
fn double(x) -> x * 2

// Block form (last expression is the return value)
fn voice(freq) -> {
    osc = saw(freq)
    lp(osc, 800)
}
```

## Parameters

Required parameters first, then optional parameters with literal defaults:

```akkado
fn filtered(sig, cut = 1000, q = 0.7) -> lp(sig, cut, q)
```

Call with positional or named arguments:

```akkado
filtered(noise())                    // defaults: cut=1000, q=0.7
filtered(noise(), 500)               // positional override
filtered(noise(), cut: 500)          // named argument
filtered(noise(), q: 2.0, cut: 500)  // named args, any order
```

### String defaults

Defaults can be either **number literals** or **string literals**:

```akkado
fn osc(type = "sin", freq = 440) -> match(type) {
    "sin": sine(freq)
    "saw": saw(freq)
    _: sine(freq)
}

osc(440)              // type defaults to "sin"
saw(440)       // explicit type
```

String defaults participate in compile-time `match()` resolution — when the default is used, only the matching arm is emitted into bytecode. String-defaulted params are **only meaningful inside a `match()` scrutinee**; using them as audio signals (e.g. `type * 2`) is undefined.

### Variadic rest parameters

A trailing `...name` collects remaining positional args into an array:

```akkado
fn mix(...sigs) -> sum(sigs) / len(sigs)

mix(a)              // 1 signal
mix(a, b)           // 2 signals
mix(a, b, c, d)     // 4 signals
```

```akkado
fn chain(input, ...fxs) -> fold(fxs, input, (acc, fx) -> fx(acc))
noise() |> chain(%, lp(_, 500), delay(_, 0.3, 0.5, 0.5, 0.5))
```

Rules:
- The rest parameter must be **last**
- Rest cannot have a default value
- Required and optional params can precede the rest: `fn f(a, b = 5, ...rest)`
- Empty rest is valid: `mix()` produces `len(sigs) == 0`
- Rest args can be function values (closures, partials, fn refs) — useful for fx chains

Rules (general):
- Default values must be **literal numbers or strings** (no expressions)
- Positional arguments must come before named arguments
- Argument count is validated — missing required args or extra args are compile errors

## Body Patterns

### Single expression

```akkado
fn gain(sig, amt = 0.5) -> sig * amt
```

### Block with locals

```akkado
fn thick_saw(freq) -> {
    a = saw(freq)
    b = saw(freq * 1.005)
    (a + b) * 0.5
}
```

### Match dispatch

```akkado
fn my_osc(type, freq) -> match(type) {
    "sin": sine(freq)
    "saw": saw(freq)
    "tri": tri(freq)
    _: sine(freq)
}
```

When called with a string literal (`my_saw440)`), the match resolves at **compile time** — only the winning branch is emitted. Commas between arms are optional.

### Match with guards

```akkado
fn shaped(sig, drive) -> match {
    drive > 2.0: fold(sig, drive)
    drive > 1.0: tanh(sig * drive)
    _: sig
}
```

### Pipe chains in body

```akkado
fn subtractive(freq, cut = 800) -> {
    saw(freq) |> lp(%, cut) |> % * 0.8
}
```

## Constraints

| Constraint | Detail |
|---|---|
| No recursion | Functions cannot call themselves |
| No forward declarations | Define before use |
| No closures over mutable state | All variables are immutable; outer reads are fine |
| Literal-only defaults | `fn f(x = 0.5)`, `fn f(x = "sin")` work; expressions do not |
| Builtin shadowing warns | Naming a function `lp`, `delay`, etc. compiles but warns; prefer distinct names |

Functions are **fully inlined** at every call site. Writing `fn f(x) -> x * 2` and calling `f(100)` compiles to the same bytecode as `100 * 2`.

## Stdlib Convention: Type Dispatch with `match`

The stdlib `osc()` function demonstrates the canonical pattern for wrapping multiple builtins behind a single name:

```akkado
fn osc(type, freq, pwm = 0.5, phase = 0.0, trig = 0.0) -> match(type) {
    "sin": sine(freq, phase, trig)
    "sine": sine(freq, phase, trig)
    "tri": tri(freq, phase, trig)
    "triangle": tri(freq, phase, trig)
    "saw": saw(freq, phase, trig)
    "sawtooth": saw(freq, phase, trig)
    "sqr": sqr(freq, phase, trig)
    "square": sqr(freq, phase, trig)
    "ramp": ramp(freq, phase, trig)
    "phasor": phasor(freq, phase, trig)
    "noise": noise(freq, trig)
    "white": noise(freq, trig)
    "sqr_pwm": sqr_pwm(freq, pwm, phase, trig)
    "pulse": sqr_pwm(freq, pwm, phase, trig)
    "saw_pwm": saw_pwm(freq, pwm, phase, trig)
    "var_saw": saw_pwm(freq, pwm, phase, trig)
    "sqr_minblep": sqr_minblep(freq, phase, trig)
    "sqr_pwm_minblep": sqr_pwm_minblep(freq, pwm, phase, trig)
    _: sine(freq, phase, trig)
}
```

Key points:
- String aliases map multiple names to the same builtin
- Not every branch uses every parameter (noise skips `phase`)
- The wildcard `_` provides a safe fallback
- Literal arguments at call sites enable compile-time branch elimination

## Interaction with Builtins

User functions can freely call any builtin:

```akkado
fn wobble(freq, rate = 4, depth = 200) -> {
    mod = sine(rate) * depth
    saw(freq + mod) |> lp(%, freq * 2)
}
```

User functions can shadow builtins (the stdlib `osc()` itself shadows the builtin `osc`). The compiler emits a warning but allows it. Prefer distinct names to avoid confusion.

## Pipe Integration

Functions work naturally in `|>` chains with `%` and `as`:

```akkado
// Function as pipe source
voice(440) |> reverb(%) |> out(%, %)

// Function receiving pipe input via %
saw(220) |> my_filter(%, 1000)

// Pipe binding with `as` for multi-field access
n"c4 e4 g4" as e |> sine(e.freq) |> % * e.vel |> out(%, %)
```

Functions can also appear inside closures:

```akkado
map(freqs, (f) -> voice(f) |> lp(%, 1000))
```

## Polyphonic Patterns

Chord and array values propagate through function parameters automatically. When a multi-buffer value (e.g., a chord) is passed to a function parameter, the function body runs per-voice:

```akkado
fn voice(freq) -> saw(freq) |> lp(@, 800)

// Frequency array auto-expands: voice runs once per element (C major)
voice([261.6, 329.6, 392.0]) |> out(@, @)
```

For explicit per-voice processing, use `map()` with a closure:

```akkado
fn poly(freqs, voice_fn) -> {
    sum(map(freqs, voice_fn)) / len(freqs)
}

poly([220, 330, 440], (f) -> saw(f) |> lp(%, 1000)) |> out(%, %)
```

## Nesting Functions

Functions can call other user-defined functions (each call inlines recursively):

```akkado
fn double(x) -> x * 2
fn quadruple(x) -> double(double(x))

quadruple(100)  // compiles to: 100 * 2 * 2
```

Arguments are evaluated in the **caller's scope** before the function body runs, so nested calls work correctly.

## Capturing Outer Variables

Functions can read (but not reassign) outer-scope variables:

```akkado
base_freq = 440
fn detune(ratio) -> base_freq * ratio

detune(1.5)  // 660
```

This works because all Akkado variables are immutable.

## Returning Closures

A function whose body is a closure expression returns a **function value** that captures the outer parameters:

```akkado
fn make_filter(cut) -> (sig) -> lp(sig, cut, 0.7, 0.5, 0.5)

filt = make_filter(1000)
noise() |> filt(%) |> out(%, %)
```

```akkado
fn make_voice(wave = "saw") -> (freq) -> match(wave) {
    "saw": saw(freq) |> lp(%, freq * 4, 0.7, 0.5, 0.5)
    "tri": tri(freq) |> lp(%, freq * 4, 0.7, 0.5, 0.5)
    _:     sine(freq)
}

my_voice = make_voice("tri")
my_voice(440) |> out(%, %)
```

Notes:
- Captured outer params are bound at the moment the factory is called — they're read-only inside the returned closure.
- Nested closure returns work: `fn f(a) -> (b) -> (c) -> a + b + c`.
- String captures propagate into compile-time `match()` resolution inside the returned closure (same machinery as string defaults).

## Partial Application with `_`

Use `_` in any argument position to create a closure with that slot left open:

```akkado
fn add(a, b) -> a + b
add3 = add(3, _)        // (x) -> add(3, x)
add3(4)                 // 7

clamp01 = clamp(0, _, 1)  // (x) -> clamp(0, x, 1)

soft_lp = lp(_, 500, 0.7, 0.5, 0.5)
noise() |> soft_lp(%) |> out(%, %)

f = clamp(_, _, 1)        // (a, b) -> clamp(a, b, 1)
```

Rules:
- Each `_` becomes a parameter of the resulting closure, in left-to-right order.
- Works for both builtins and user-defined functions.
- The result is a function value — assignable, callable, and passable as an argument: `map(arr, add(3, _))`.
- `_` in an argument position is unrelated to `_` as a match wildcard — different parser paths.

## Function Composition with `compose()`

`compose(f, g, ...)` builds a left-to-right chain — `compose(a, b)` is `(x) -> b(a(x))`:

```akkado
fn double(x) -> x * 2
fn inc(x) -> x + 1
f = compose(double, inc)   // (x) -> inc(double(x))
f(5)                       // 11

pipeline = compose(lp(_, 1000, 0.7, 0.5, 0.5), hp(_, 200, 0.7, 0.5, 0.5))
saw(440) |> pipeline(%) |> out(%, %)

fx = compose(lp(_, 800, 0.7, 0.5, 0.5),
             saturate(_, 0.3),
             reverb(_))
saw(220) |> fx(%) |> out(%, %)
```

Rules:
- `compose()` accepts 2 or more function-valued arguments (closures, fn refs, partial applications).
- Each function in the chain receives the output of the previous as its single argument.
- The result is a function value, usable inline: `noise() |> compose(lp(_, 500, 0.7, 0.5, 0.5), reverb(_))(%) |> out(%, %)`.
- Non-function arguments to `compose()` produce a compile error.

## Complete Example

```akkado
fn voice(freq, gate, wave = "saw", cut = 2000) -> {
    sig = match(wave) {
        "saw": saw(freq)
        "sqr": sqr(freq)
        _: sine(freq)
    }
    sig |> lp(%, cut) |> % * adsr(gate, 0.01, 0.1, 0.7, 0.3)
}

cutoff = param("cutoff", 2000, 100, 8000)

n"c4 e4 g4 c5" as e
  |> voice(e.freq, e.trig, "saw", cutoff)
  |> % * e.vel
  |> delay(%, 0.375, 0.4, 1.0, 0.3)
  |> out(%, %)
```
