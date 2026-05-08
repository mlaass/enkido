---
title: Arrays
category: language
order: 6
index_headings: true
keywords: [array, list, arrays, indexing, len, length, map, reduce, fold, zipWith, zip, take, drop, reverse, sum, mean, average, rotate, shuffle, sort, normalize, scale, range, repeat, linspace, random, harmonics, polyphony, voices, multi-buffer, comprehension]
group: language
subgroup: data
icon: Brackets
tagline: Literals, comprehensions, and the full higher-order toolkit.
---

# Arrays

Arrays are ordered, fixed-size collections of values. They are the primary way to express **parallel signal flows** in Akkado: every element of an array becomes its own buffer, processed in parallel by the audio graph.

```akk
freqs = [220, 330, 440]
map(freqs, (f) -> osc("sin", f)) |> sum(%) * 0.3 |> out(%, %)
```

Arrays exist at compile time only; there is no runtime growable list type. `len(arr)`, `take(n, arr)`, and similar operations are evaluated when the patch is compiled, not while audio is running.

## Array literals

Use square brackets to create an array:

```akk
// Numbers
[1, 2, 3, 4]

// Frequencies for a chord
[261.6, 329.6, 392.0]

// Mixed types are allowed
[1, "hello", true]

// Empty arrays
[]

// Nested arrays
[[1, 2], [3, 4]]
```

Trailing commas are not accepted: `[1, 2, 3,]` is a parse error.

## Indexing

Use `arr[i]` to access an element. Indices wrap modulo the array length, so out-of-range indices stay valid:

```akk
freqs = [220, 330, 440]
freqs[0]    // 220
freqs[-1]   // 440 (last element)
freqs[5]    // 440 (5 mod 3 = 2)
```

Compile-time indices (number literals) are resolved to a direct element reference with no runtime cost. Dynamic indices (e.g. an LFO selecting between voices) emit an `ARRAY_INDEX` opcode that reads the array per-sample.

```akk
// Compile-time index, zero-cost lookup
voices = [osc("sin", 220), osc("saw", 330), osc("tri", 440)]
voices[0] |> out(%, %)

// Runtime index, selects a voice from an LFO
sel = (lfo(0.5) + 1) * 1.5  // 0..3 ramp
voices[sel] |> out(%, %)
```

## Auto-expansion via map and sum

Akkado does not auto-expand arrays into scalar functions today. To run an array of values through a synth voice, use `map` to lift the function over each element, then a reduction like `sum` to fold the parallel voices back into a single output:

```akk
// Three sine voices summed into one output
[220, 330, 440]
  |> map(%, (f) -> osc("sin", f))
  |> sum(%) * 0.3
  |> out(%, %)
```

For chord-based polyphony driven by patterns, see [poly](../builtins/sequencing#poly): chord patterns require an explicit `poly()` wrapper to expand into per-voice signals.

## len

**Length**: Compile-time array length.

| Param | Type  | Default | Description |
|-------|-------|---------|-------------|
| arr   | array | -       | Input array |

Returns the number of elements as a constant signal. Errors if the argument is not a known array.

```akk
n = len([1, 2, 3, 4])  // 4
osc("sin", 220) * (1 / n) |> out(%, %)
```

## map

**Map**: Apply a unary function to each element. Each call to `fn` receives a unique semantic path, so stateful functions (oscillators, filters) get independent state per voice.

| Param | Type     | Default | Description |
|-------|----------|---------|-------------|
| array | array    | -       | Input array |
| fn    | function | -       | Closure `(x) -> ...` applied to each element |

```akk
// Three independent saw voices, summed
[110, 220, 440]
  |> map(%, (f) -> osc("saw", f) |> lp(%, 1200))
  |> sum(%) * 0.3
  |> out(%, %)
```

## reduce

**Reduce**: Reduce an array to a scalar with a binary function and an initial value. (Named `reduce` rather than `fold` because `fold` is the [wavefolding distortion](../builtins/distortion#fold).)

| Param | Type     | Default | Description |
|-------|----------|---------|-------------|
| array | array    | -       | Input array |
| fn    | function | -       | Binary closure `(acc, x) -> ...` |
| init  | signal   | -       | Initial accumulator value |

Threads `init` through `fn(init, arr[0])`, `fn(result, arr[1])`, ... in left-to-right order. Empty arrays return `init` unchanged.

```akk
// Sum-of-squares as a custom reducer
reduce([1, 2, 3, 4], (acc, x) -> acc + x * x, 0)  // 1 + 4 + 9 + 16 = 30

// Product (multiplicative reduce). Use init=1 (the multiplicative identity);
// empty arrays return 1, matching the math convention.
reduce([2, 3, 4], (a, b) -> a * b, 1)  // 24
```

## zipWith

**Zip With**: Combine two arrays element-wise with a binary function. Truncates to the shorter input.

| Param | Type     | Default | Description |
|-------|----------|---------|-------------|
| a     | array    | -       | First array |
| b     | array    | -       | Second array |
| fn    | function | -       | Binary closure `(x, y) -> ...` |

```akk
// Apply per-voice gains to a chord
freqs = [220, 330, 440]
gains = [1.0, 0.7, 0.5]
zipWith(freqs, gains, (f, g) -> osc("sin", f) * g)
  |> sum(%) * 0.3
  |> out(%, %)
```

## zip

**Zip**: Interleave two arrays into a flat array `[a[0], b[0], a[1], b[1], ...]`. Truncates to the shorter input.

| Param | Type  | Default | Description |
|-------|-------|---------|-------------|
| a     | array | -       | First array |
| b     | array | -       | Second array |

```akk
zip([1, 2, 3], [10, 20, 30])  // [1, 10, 2, 20, 3, 30]
```

## take

**Take**: First `n` elements. `n` must be a number literal. If `n` exceeds the array length, the entire array is returned.

| Param | Type    | Default | Description |
|-------|---------|---------|-------------|
| n     | literal | -       | Element count |
| array | array   | -       | Input array |

```akk
take(2, [10, 20, 30, 40])  // [10, 20]
```

## drop

**Drop**: Skip the first `n` elements; return the rest. `n` must be a number literal. If `n` ≥ length, returns an empty array.

| Param | Type    | Default | Description |
|-------|---------|---------|-------------|
| n     | literal | -       | Skip count |
| array | array   | -       | Input array |

```akk
drop(1, [10, 20, 30, 40])  // [20, 30, 40]
```

## reverse

**Reverse**: Reverse element order.

| Param | Type  | Default | Description |
|-------|-------|---------|-------------|
| array | array | -       | Input array |

```akk
reverse([1, 2, 3])  // [3, 2, 1]
```

## sum

**Sum**: Add all elements. Empty array yields a single zero. Single-element array passes through untouched.

| Param | Type  | Default | Description |
|-------|-------|---------|-------------|
| array | array | -       | Input array |

```akk
// Mix three voices
[osc("sin", 220), osc("sin", 330), osc("sin", 440)]
  |> sum(%) * 0.33
  |> out(%, %)
```

## mean

**Mean**: Arithmetic average. Empty array returns `0`.

| Param | Type  | Default | Description |
|-------|-------|---------|-------------|
| array | array | -       | Input array |

```akk
// Average detuned voices
detuned = [220, 220.5, 219.5]
center = mean(detuned)  // ~220
```

## rotate

**Rotate**: Rotate elements by `n` positions. Positive `n` rotates right, negative rotates left, and the offset wraps modulo the array length. `n` must be a number literal.

| Param | Type    | Default | Description |
|-------|---------|---------|-------------|
| array | array   | -       | Input array |
| n     | literal | -       | Rotation offset |

```akk
rotate([1, 2, 3, 4], 1)   // [4, 1, 2, 3]
rotate([1, 2, 3, 4], -1)  // [2, 3, 4, 1]
rotate([1, 2, 3, 4], 5)   // same as rotate(..., 1)
```

## shuffle

**Shuffle**: Deterministic Fisher-Yates permutation seeded by the call's semantic path. Two calls in the same code position always produce the same permutation; calls in different positions produce different ones. Pass an explicit `seed` to vary the permutation while keeping the call in place.

| Param | Type    | Default | Description |
|-------|---------|---------|-------------|
| array | array   | -       | Input array |
| seed  | literal | path    | Compile-time integer mixed into the path-derived seed |

```akk
// Same shuffle every compile, useful for stable variations
shuffle([220, 330, 440, 550])
  |> map(%, (f) -> osc("saw", f))
  |> sum(%) * 0.25
  |> out(%, %)

// A different permutation at the same code position
shuffle([220, 330, 440, 550], 7)
```

## sort

**Sort**: Ascending numeric order by default; pass `reverse=true` for descending. Operates at compile time on array literals of numbers; non-literal inputs pass through unchanged.

| Param   | Type    | Default | Description |
|---------|---------|---------|-------------|
| array   | array   | -       | Input array |
| reverse | literal | false   | Sort descending when truthy |

```akk
sort([3, 1, 4, 1, 5, 9, 2, 6])        // [1, 1, 2, 3, 4, 5, 6, 9]
sort([3, 1, 4, 1, 5, 9, 2, 6], true)  // [9, 6, 5, 4, 3, 2, 1, 1]
```

## normalize

**Normalize**: Scale elements from their current min/max to `[lo, hi]`. With no extra arguments the output range is `[0, 1]`. Single-element arrays return `lo`.

| Param | Type    | Default | Description |
|-------|---------|---------|-------------|
| array | array   | -       | Input array |
| lo    | literal | 0       | Target minimum |
| hi    | literal | 1       | Target maximum (must be > lo) |

```akk
normalize([10, 20, 30])         // [0.0, 0.5, 1.0]
normalize([10, 20, 30], -1, 1)  // [-1.0, 0.0, 1.0]
```

If all elements are equal, the divisor is zero; avoid `normalize` on constant-valued arrays.

## scale

**Scale**: Map elements from their current min/max to `[lo, hi]`.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| array | array  | -       | Input array |
| lo    | signal | -       | Target minimum |
| hi    | signal | -       | Target maximum |

```akk
// Map a normalized envelope shape to a filter cutoff range
shape = [0, 0.5, 1, 0.3]
cutoffs = scale(shape, 200, 4000)
```

## range

**Range**: Integer sequence stepping from `start` toward `end` (exclusive). If `start > end`, the sequence counts down. `step` is the interval size and is always treated as positive; direction is determined by `start` vs `end`. All arguments must be compile-time constants.

| Param | Type    | Default | Description |
|-------|---------|---------|-------------|
| start | literal | -       | Start value (inclusive) |
| end   | literal | -       | End value (exclusive) |
| step  | literal | 1       | Interval size (must be non-zero) |

```akk
range(0, 4)       // [0, 1, 2, 3]
range(4, 0)       // [4, 3, 2, 1]
range(2, 2)       // []
range(0, 10, 2)   // [0, 2, 4, 6, 8]
range(10, 0, 3)   // [10, 7, 4, 1]
```

## repeat

**Repeat**: Array of `n` copies of `value`. `n` must be a number literal.

| Param | Type    | Default | Description |
|-------|---------|---------|-------------|
| value | signal  | -       | Value to repeat |
| n     | literal | -       | Repeat count |

```akk
repeat(0.5, 4)  // [0.5, 0.5, 0.5, 0.5]
```

## linspace

**Linspace**: `n` evenly spaced values from `start` to `end`, **inclusive on both ends**. The optional `mode` selects the spacing curve: `"linear"` (default), `"log"` (geometric, natural for frequency sweeps), or `"geom"` (alias for `"log"`). All non-mode arguments must be compile-time constants. `n=1` returns `[start]`; `n ≤ 0` returns an empty array.

| Param | Type    | Default    | Description |
|-------|---------|------------|-------------|
| start | literal | -          | First value |
| end   | literal | -          | Last value |
| n     | literal | -          | Number of points |
| mode  | string  | `"linear"` | `"linear"`, `"log"`, or `"geom"` |

`log` and `geom` require `start > 0` and `end > 0`.

```akk
linspace(0, 1, 5)                  // [0, 0.25, 0.5, 0.75, 1]
linspace(100, 800, 4)              // [100, 333.33, 566.67, 800]
linspace(20, 20000, 4, "log")      // [20, ~200, ~2000, 20000]
```

## random

**Random**: `n` deterministic random values, seeded by the call's semantic path. Same code position → same numbers. Pass optional `min`/`max` to rescale from the default `[0, 1)` range.

| Param | Type    | Default | Description |
|-------|---------|---------|-------------|
| n     | literal | -       | Number of values |
| min   | literal | 0       | Lower bound (inclusive) |
| max   | literal | 1       | Upper bound (exclusive; must be > min) |

```akk
// Reproducible random voice gains in [0, 1)
gains = random(4)

// Random MIDI notes in C4–C5
notes = random(8, 60, 72)

// Random detune in cents
detune = random(6, -50, 50)
```

## harmonics

**Harmonics**: Harmonic series `[fundamental, 2·fundamental, ..., n·fundamental]` by default. Pass an optional `ratio` to bend the series: `ratio>1` stretches partials sharper than integer multiples (piano/bell-style inharmonicity); `ratio<1` compresses them. All arguments must be compile-time constants.

| Param       | Type    | Default | Description |
|-------------|---------|---------|-------------|
| fundamental | literal | -       | Base frequency |
| n           | literal | -       | Number of harmonics |
| ratio       | literal | 1.0     | Stretch factor (must be > -1) |

```akk
// Natural integer series (default)
harmonics(110, 8)
  |> map(%, (f) -> osc("sin", f) / (f / 110))
  |> sum(%) * 0.2
  |> out(%, %)

// Slightly stretched, piano-like spectrum
harmonics(110, 8, 1.05)
  |> map(%, (f) -> osc("sin", f) / (f / 110))
  |> sum(%) * 0.2
  |> out(%, %)
```

## Polyphony builtins

When you need pattern-driven voice allocation rather than parallel processing of static arrays, use `poly`, `mono`, `legato`, and `spread`. They are documented in the [Sequencing reference](../builtins/sequencing).

## Common patterns

**Parallel synthesis from a frequency list:**

```akk
[110, 165, 220, 330]
  |> map(%, (f) -> osc("saw", f) |> lp(%, f * 4))
  |> sum(%) * 0.2
  |> out(%, %)
```

**Inharmonic spectrum with linspace:**

```akk
linspace(220, 1100, 5)
  |> map(%, (f) -> osc("sin", f))
  |> sum(%) * 0.2
  |> out(%, %)
```

**Random-detuned chorus:**

```akk
base = 220
detune = scale(random(6), -5, 5)  // ±5 Hz spread
detune
  |> map(%, (d) -> osc("saw", base + d))
  |> sum(%) * 0.15
  |> out(%, %)
```

Related: [Pipes & Holes](pipes), [Closures](closures), [Math Functions](../builtins/math), [Sequencing](../builtins/sequencing)
