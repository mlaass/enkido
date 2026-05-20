---
title: Functions, #inline, and Recursion
category: concepts
order: 8
keywords: [fn, function, inline, "#inline", annotation, recursion, recursive, E240, E244, callable, block]
---

# Functions, `#inline`, and Recursion

A user `fn` groups a reusable chunk of signal graph behind a name:

```akkado
fn lp_chain(input, cutoff) {
    input |> lp(@, cutoff) |> hp(@, 30)
}

saw(220) |> lp_chain(@, 800)  |> out(@)
saw(330) |> lp_chain(@, 1200) |> out(@)
```

Every call site is **expanded inline** at compile time: each call gets its own
copy of the body with its own DSP state, so the two `lp_chain` calls above run
as two independent filter chains. This expansion happens entirely in the
compiler — the audio engine only ever sees a flat instruction stream.

## `#inline`

`#inline` is a statement-level annotation that precedes a `fn` declaration:

```akkado
#inline fn fast_mix(a, b) { a * 0.5 + b * 0.5 }
```

It marks the function as a hot-path micro-body that should always be expanded
per call site. Because functions are already expanded at compile time, an
`#inline fn` and a plain `fn` currently emit **identical** bytecode — the
annotation is reserved as an explicit opt-in for a future release that adds a
shared, dispatched calling convention. Using it today is harmless and
forward-compatible.

Rules:

- `#inline` must immediately precede a `fn` (or `const fn`) declaration.
  Anything else is rejected with **E246**.
- `#` introduces an annotation; the only annotation defined today is
  `#inline`. An unknown name (e.g. `#cold`) is rejected with **E249**.

## Recursion is not supported

A `fn` may not call itself, directly or through a cycle of other functions:

```akkado
fn f(x) -> f(x)              // E240 — recursive fn 'f' not supported
fn a(x) -> b(x)
fn b(x) -> a(x)              // E240 — recursive fn 'a' (cycle: a -> b -> a)
```

Because call sites are expanded inline, a recursive `fn` would expand forever.
Recursion is rejected at compile time:

- **E240** — a recursive (or mutually recursive) `fn`.
- **E244** — same, when one of the functions in the cycle is `#inline`.

Compile-time-bounded recursion (an explicit depth cap) is planned for a future
release. For repeating a body a fixed number of times today, use
[`loop(N) { … }`](/docs/builtins/loop), and for one instance of a stateful
chain per element of a list, use an array with `each` once higher-order
operators land.
