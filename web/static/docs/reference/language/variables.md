---
title: Variables & Assignment
category: language
order: 2
keywords: [variables, assignment, let, binding, scope, identifier]
---

# Variables & Assignment

Variables store values for reuse throughout your patch.

## Basic Assignment

Use `=` to bind a value to a name:

```akk
// Store a frequency
freq = 440

// Use it in oscillators
osc("sin", freq) |> out(%, %)
```

## Signal Variables

Variables can store entire signal chains:

```akk
// Store an oscillator
osc = osc("saw", 110) |> lp(%, 800)

// Use it multiple times
osc |> out(%, %)
```

## Modular Patching

Variables make complex patches readable:

```akk
// Define components
lfo_mod = osc("sin", 0.5) * 500
base_freq = 110
filter_cutoff = 400 + lfo_mod

// Build the patch
osc("saw", base_freq) |> lp(%, filter_cutoff) |> out(%, %)
```

## Naming Rules

- Names must start with a letter or underscore
- Can contain letters, numbers, and underscores
- Case-sensitive (`Freq` and `freq` are different)
- Cannot use reserved keywords

```akk
// Valid names
freq = 440
my_osc = osc("sin", 220)
bass1 = osc("saw", 55)
_internal = 0.5

// Invalid names
// 1freq = 440     // Can't start with number
// my-osc = sin()  // No hyphens
```

## Scope

Variables are visible from their definition point to the end of the program:

```akk
a = 100
b = a * 2      // a is visible here
c = a + b      // both a and b visible
```

Related: [Operators](operators), [Pipes & Holes](pipes)
