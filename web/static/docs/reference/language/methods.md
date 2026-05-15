---
title: Method Calls (UFCS)
category: language
order: 7
keywords: [method, dot, ufcs, call, syntax, chain]
group: language
subgroup: data
icon: Workflow
tagline: Universal function call syntax via dot.
---

# Method Calls (UFCS)

Akkado supports method-call syntax for any callable in scope, built-in or user-defined. The form

```akk
x.foo(a, b)
```

is equivalent to

```akk
foo(x, a, b)
```

This is universal-function-call syntax (UFCS): there is no per-type method table. The dispatch is purely lexical, the same name resolution as a regular call.

## With built-ins

Any built-in that takes a signal as its first argument can be called as a method:

```akk
sig.lp(1200)              // ≡ lp(sig, 1200)
sig.delay(0.3, 0.5, 0.5, 0.5)
freq.mtof()               // ≡ mtof(freq)
```

## Chaining

Method calls chain naturally:

```akk
sig.lp(1200).hp(200).delay(0.3, 0.5, 0.5, 0.5)
// ≡ delay(hp(lp(sig, 1200), 200), 0.3, 0.5, 0.5, 0.5)
```

## With user closures

A closure assigned to a name participates in UFCS:

```akk
double = (x) -> x * 2
my_signal.double()        // ≡ double(my_signal)
```

This is what makes [userspace operators](../builtins/state.md) like `step` read like first-class language features:

```akk
step = (arr, trig) -> arr[counter(trig)]

[60, 64, 67, 72].step(trigger(4)) |> mtof(@) |> sine(@) |> out(@)
```

## Mixing pipes and methods

Pipes and methods cooperate freely:

```akk
sig.lp(1200) |> hp(@, 200)     // method, then pipe
sig |> lp(@, 1200).hp(200)     // pipe, then method-chain
```

Pick whichever reads better at the call site.

## Errors

If the method name doesn't resolve to a callable in scope, the compiler emits the same "Unknown function" error as for a typo'd direct call. The receiver is included in the desugared call, so the error message points to the right name.

```
arr.no_such_op(1)
// error: Unknown function: 'no_such_op'
```

Related: [Pipes & Holes](pipes.md), [Closures](closures.md)
