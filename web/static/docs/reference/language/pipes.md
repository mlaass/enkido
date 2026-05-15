---
title: Pipes & Holes
category: language
order: 1
keywords: [pipe, hole, "|>", "@", signal flow, graph, connection, routing]
group: language
subgroup: syntax
icon: Plug
tagline: Forward composition with -> and |>; the @ hole.
---

# Pipes & Holes

The pipe operator (`|>`) and hole (`@`) define Akkado's signal flow model.

## The pipe operator |>

The pipe operator connects audio processing nodes, creating a signal flow graph.

```akk
// Signal flows left to right
osc("saw", 220) |> lp(@, 800) |> out(@)
```

This reads as: "Generate a sawtooth at 220 Hz, pipe it through a lowpass filter at 800 Hz, then pipe to the output."

### Why pipes?

Pipes make signal flow explicit and readable:

```akk
// Without pipes (harder to read)
out(lp(osc("saw", 220), 800), lp(osc("saw", 220), 800))

// With pipes (clearer)
osc("saw", 220) |> lp(@, 800) |> out(@)
```

## The hole @

The hole (`@`) is a placeholder that gets filled with the signal from the left side of the pipe.

### Single hole

When you use `@` once, it receives the piped signal:

```akk
osc("sin", 440) |> out(@)
//           ^    ^
//           |    |
//           Both holes filled with osc("sin", 440)
```

### Multiple holes

You can use `@` multiple times in the same expression:

```akk
// Send signal to both filter and output
osc("sin", 440) |> lp(@, 1000) + @ |> out(@)
//                          ^
//                          Original unfiltered signal mixed back in
```

### Hole with math

The hole can be used in calculations:

```akk
// Reduce volume by half
osc("sin", 440) |> @ * 0.5 |> out(@)
```

```akk
// Add DC offset
osc("sin", 440) |> @ + 0.5 |> out(@)
```

## Chaining pipes

Build signal chains by connecting multiple pipes:

```akk
// Oscillator -> Filter -> Distortion -> Output
osc("saw", 110) |> lp(@, 800) |> saturate(@, 2) |> out(@)
```

## Branching

The hole lets you create parallel paths:

```akk
// Dry/wet mix
osc("saw", 110) |> lp(@, 500) * 0.7 + @ * 0.3 |> out(@)
//          filtered (wet)    original (dry)
```

## Examples

### Basic synth voice

```akk
// Oscillator through filter with envelope
osc("saw", 220) * adsr(trigger(2), 0.01, 0.2) |> lp(@, 1000) |> out(@)
```

### Effects chain

```akk
// Guitar-like processing
osc("saw", 110)
    |> saturate(@, 3)
    |> lp(@, 2000)
    |> delay(@, 0.3, 0.4)
    |> out(@)
```

### Stereo processing

```akk
// Stereo spread
osc("saw", 220) |> out(lp(@, 500), lp(@, 2000))
```
