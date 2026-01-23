---
title: Closures
category: language
order: 4
keywords: [closures, functions, lambda, anonymous, arrow, callback]
---

# Closures

Closures are anonymous functions that capture their environment. They're the bridge between patterns and synthesis.

## Basic Syntax

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

## Using Closures

Closures are commonly used with patterns and higher-order functions:

```akk
// Pattern triggers closure for each note
pat("c4 e4 g4") |> ((freq) -> osc("sin", freq) * ar(trigger(4))) |> out(%, %)
```

## Closures as Voices

In the Akkado philosophy, closures are "voices" - they define how control data becomes sound:

```akk
// Define a synth voice
voice = (freq) ->
    osc("saw", freq) |> lp(%, 1000) * ar(trigger(4))

// Use with a pattern
pat("c3 e3 g3 c4") |> voice |> out(%, %)
```

## Multiple Parameters

Closures can receive multiple values:

```akk
// Frequency and velocity
(freq, vel) -> osc("sin", freq) * vel * ar(trigger(4))
```

## Closure with Pipes

The pipe operator works naturally inside closures:

```akk
synth = (f) ->
    osc("saw", f)
    |> lp(%, 800 + osc("sin", 2) * 400)
    |> saturate(%, 2)

synth(110) |> out(%, %)
```

## Capturing Variables

Closures capture variables from their surrounding scope:

```akk
cutoff = 1200
filter_voice = (freq) -> osc("saw", freq) |> lp(%, cutoff)

filter_voice(220) |> out(%, %)
```

Related: [Variables](variables), [Pipes & Holes](pipes)
