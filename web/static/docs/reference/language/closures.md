---
title: Closures
category: language
order: 4
keywords: [closures, functions, lambda, anonymous, arrow, callback, partial, compose, factory, underscore, placeholder]
group: language
subgroup: syntax
icon: Parentheses
tagline: Anonymous functions and partial application.
---

# Closures

Closures are anonymous functions that capture their environment. They connect patterns to synthesis.

## Basic syntax

```akk
(parameter) -> expression
```

A closure takes parameters and returns the result of an expression:

```akk
// Closure that doubles its input
(x) -> x * 2

// Closure with multiple parameters
(freq, amp) -> osc("sin", freq) * amp
```

## Using closures

Closures are commonly used with patterns and higher-order functions:

```akk
// Pattern triggers closure for each note
pat("c4 e4 g4") |> ((freq) -> osc("sin", freq) * ar(trigger(4))) |> out(@)
```

## Closures as voices

Closures are "voices" in Akkado: they define how control data becomes sound.

```akk
// Define a synth voice
voice = (freq) ->
    osc("saw", freq) |> lp(@, 1000) * ar(trigger(4))

// Use with a pattern
pat("c3 e3 g3 c4") |> voice |> out(@)
```

## Multiple parameters

Closures can receive multiple values:

```akk
// Frequency and velocity
(freq, vel) -> osc("sin", freq) * vel * ar(trigger(4))
```

## Closure with pipes

The pipe operator works inside closures:

```akk
synth = (f) ->
    osc("saw", f)
    |> lp(@, 800 + osc("sin", 2) * 400)
    |> saturate(@, 2)

synth(110) |> out(@)
```

## Capturing variables

Closures capture variables from their surrounding scope:

```akk
cutoff = 1200
filter_voice = (freq) -> osc("saw", freq) |> lp(@, cutoff)

filter_voice(220) |> out(@)
```

## Returning closures from `fn`

A `fn` whose body is a closure expression returns a function value that captures the outer params:

```akk
fn make_filter(cut) -> (sig) -> lp(sig, cut, 0.7, 0.5, 0.5)

filt = make_filter(1000)
noise() |> filt(@) |> out(@)
```

The captured params are bound when the factory is called and remain read-only inside the returned closure. Nested factories work too: `fn f(a) -> (b) -> (c) -> a + b + c`.

## Partial application

Use `_` in any argument position to create a closure with that slot left open:

```akk
fn add(a, b) -> a + b
add3 = add(3, _)         // (x) -> add(3, x)
add3(4)                  // 7

soft_lp = lp(_, 500, 0.7, 0.5, 0.5)
noise() |> soft_lp(@) |> out(@)
```

Each `_` becomes a parameter of the resulting closure in left-to-right order. Works for both builtins and user-defined functions. Useful with higher-order helpers: `map(arr, add(3, _))`.

## Function composition

`compose(f, g, ...)` builds a left-to-right chain. `compose(a, b)` is `(x) -> b(a(x))`:

```akk
fn double(x) -> x * 2
fn inc(x) -> x + 1
f = compose(double, inc)   // (x) -> inc(double(x))
f(5)                       // 11

pipeline = compose(lp(_, 1000, 0.7, 0.5, 0.5), hp(_, 200, 0.7, 0.5, 0.5))
saw(440) |> pipeline(@) |> out(@)
```

`compose()` accepts 2 or more function-valued arguments: closures, fn refs, or partial applications. The result is itself a function value.

Related: [Variables](variables), [Pipes & Holes](pipes)
