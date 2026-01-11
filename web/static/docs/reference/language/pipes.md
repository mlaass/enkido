---
title: Pipes & Holes
category: language
order: 1
keywords: [pipe, hole, "|>", "%", signal flow, graph, connection, routing]
---

# Pipes & Holes

The pipe operator (`|>`) and hole (`%`) are the foundation of Akkado's signal flow model.

## The Pipe Operator |>

The pipe operator connects audio processing nodes, creating a signal flow graph.

```akk
// Signal flows left to right
saw(220) |> lp(%, 800) |> out(%, %)
```

This reads as: "Generate a sawtooth at 220 Hz, pipe it through a lowpass filter at 800 Hz, then pipe to the output."

### Why Pipes?

Pipes make signal flow explicit and readable:

```akk
// Without pipes (harder to read)
out(lp(saw(220), 800), lp(saw(220), 800))

// With pipes (clearer)
saw(220) |> lp(%, 800) |> out(%, %)
```

## The Hole %

The hole (`%`) is a placeholder that gets filled with the signal from the left side of the pipe.

### Single Hole

When you use `%` once, it receives the piped signal:

```akk
sin(440) |> out(%, %)
//           ^    ^
//           |    |
//           Both holes filled with sin(440)
```

### Multiple Holes

You can use `%` multiple times in the same expression:

```akk
// Send signal to both filter and output
sin(440) |> lp(%, 1000) + % |> out(%, %)
//                   ^
//                   Original unfiltered signal mixed back in
```

### Hole with Math

The hole can be used in calculations:

```akk
// Reduce volume by half
sin(440) |> % * 0.5 |> out(%, %)
```

```akk
// Add DC offset
sin(440) |> % + 0.5 |> out(%, %)
```

## Chaining Pipes

Build complex signal chains by connecting multiple pipes:

```akk
// Oscillator -> Filter -> Distortion -> Output
saw(110) |> lp(%, 800) |> tanh(% * 2) |> out(%, %)
```

## Branching

The hole lets you create parallel paths:

```akk
// Dry/wet mix
saw(110) |> lp(%, 500) * 0.7 + % * 0.3 |> out(%, %)
//          filtered (wet)    original (dry)
```

## Examples

### Basic Synth Voice

```akk
// Oscillator through filter with envelope
saw(220) * adsr(trigger(2), 0.01, 0.2) |> lp(%, 1000) |> out(%, %)
```

### Effects Chain

```akk
// Guitar-like processing
saw(110) |> tanh(% * 3) |> lp(%, 2000) |> delay(%, 0.3, 0.4) |> out(%, %)
```

### Stereo Processing

```akk
// Stereo spread
saw(220) |> out(lp(%, 500), lp(%, 2000))
```
