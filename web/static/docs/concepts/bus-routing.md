---
title: Bus Routing & the Master Bus
category: concepts
order: 10
keywords: [bus, mixer, master, out, routing, diamond, signal-flow, multi-bus, master-bus, soft-clip, safety, glue-compressor]
---

# Bus Routing & the Master Bus

Every Akkado patch ends by sending audio somewhere. The **bus system** is
the layer between your per-voice / per-track outputs and the speakers: a set
of numbered stereo summing points, a master with built-in safety, and a
one-token operator for routing.

## Buses are numbered; bus 0 is the master

A **bus** is a stereo summing point identified by a non-negative integer.

- **Bus 0** is the **master** — the bus the audio device receives.
- **Buses 1, 2, 3, …** are ordinary buses. Each one auto-sums into bus 0
  after its own processing runs.

`out(L, R?)` is exactly `bus(0, L, R?)` — `out` is a pure alias for the
master bus. Every existing patch keeps working unchanged.

```akkado
osc("saw", 220) |> out(@)        // route to the master
kick            |> bus(1, @)     // route to bus 1
```

Multiple writers for the same bus **sum** — that is the whole point of a
bus. Send three drum voices to bus 1 and they mix together there.

The topology is flat: a non-zero bus always feeds bus 0 directly. Buses
cannot feed other non-zero buses.

```text
   out() / <> / bus(0,…) writers ───────────────┐
                                                 ▼
  bus(1,…) writers ─▶ [bus 1] ─▶ mixer(1) ──▶ [bus 0] ─▶ master/mixer(0)
  bus(2,…) writers ─▶ [bus 2] ─▶ mixer(2) ──▶   │              │
        ⋮                                       │              ▼
                                                │        [forced safety]
                                                │              │
                                                └────────────▶ device
```

Bus indices must be **compile-time integer literals**. `bus(x, @)` where
`x` is a `param` or a computed value is a compile error (`E260`) — the set
of buses is fixed when the program loads.

## Per-bus FX with `mixer` and `master`

`mixer(N, closure)` attaches a processing closure to bus `N`. The closure
runs **once per output block** on the bus's summed signal — after every
writer has contributed — and its result replaces the bus signal downstream.

```akkado
// A drum bus: process the bus once, not each hit
kick  |> bus(1, @)
snare |> bus(1, @)
hat   |> bus(1, @)
mixer(1, (s) -> s |> comp(@, -8, 6) |> softclip(@, 0.9))
```

`master(closure)` is an alias for `mixer(0, closure)`.

```akkado
// Glue compressor + soft clip on the whole mix
master((s) -> s |> comp(@, -12, 4) |> softclip(@, 0.85))
```

Closures come in two arities, inferred from the parameter list:

```akkado
mixer(1, (s) -> s |> softclip(@, 0.9))               // one stereo value
mixer(2, (l, r) -> stereo(softclip(l, 0.9),
                          softclip(r, 0.9)))         // left + right
```

A `mixer`/`master` closure is signal-processing code. It **may not** contain
a sink — `out`, `bus`, `mixer`, `master`, or `<>` inside a closure body is
`E261`. A bus with no `mixer` call is pure identity.

Closures capture top-level bindings, which is the recommended way to expose
bus controls to the web UI:

```akkado
drive = param("master_drive", 0.85, 0.5, 1.0)
master((s) -> s |> softclip(@, drive))
```

## The master is always safe

Bus 0 has two guarantees that cannot be removed:

1. **Default tone chain** — with no `master`/`mixer(0)` call, bus 0 runs a
   polynomial soft-clip at 0.9. Supplying a `master(…)` replaces it.
2. **Forced safety stage** — after the bus-0 chain, a fixed stage runs: a
   NaN/Inf sanitizer plus a hard ±1.0 rail. The user cannot disable it.

So even an aggressive master closure cannot reach the device above ±1.0:

```akkado
master((s) -> s |> @ * 100)   // still clamped to ±1.0 by the safety stage
```

To run the master with no tone processing — only the forced safety — pass
the identity closure:

```akkado
master((s) -> s)
```

## The diamond operator `<>`

The **diamond operator** replaces the trailing `|> out(@)` / `|> bus(N, @)`
of a pipe statement with a single token.

| Sugar    | Expands to       | Meaning                 |
|----------|------------------|-------------------------|
| `<>`     | `\|> out(@)`     | route to the master bus |
| `<>(N)`  | `\|> bus(N, @)`  | route to bus `N`        |

```akkado
osc("saw", 220) <>                              // ≡ osc("saw", 220) |> out(@)
kick            <>(1)                           // ≡ kick |> bus(1, @)
n"c4 e4 g4" as e |> osc("saw", e.freq) <>(2)
```

`<>` is a **statement terminator**. It binds looser than `|>`, so it always
captures the whole pipe chain: `a |> b |> c <>(1)` routes `(a |> b |> c)` to
bus 1. It produces void and may only trail a complete statement — anywhere
else (a sub-expression, an assignment right-hand side) is `E263`.

The optional `(N)` is a non-negative integer literal. `<>(0)` is identical
to bare `<>`; bare `<>` is the idiomatic spelling for the master.

## Putting it together

```akkado
// Master glue, live-tweakable
drive = param("master_drive", 0.85, 0.5, 1.0)
master((s) -> s |> comp(@, -12, 4) |> softclip(@, drive))

// A drum bus with its own glue compressor
kick  <>(1)
snare <>(1)
hat   <>(1)
mixer(1, (s) -> s |> comp(@, -8, 6))

// A lead voice straight to the master
n"c4 e4 g4 b4" as e |> osc("saw", e.freq) |> @ * e.vel <>
```

## Reference

- [`out`](../reference/builtins/utility.md#out),
  [`bus`](../reference/builtins/utility.md#bus),
  [`mixer`](../reference/builtins/utility.md#mixer),
  [`master`](../reference/builtins/utility.md#master) — builtin reference.
- [Effect Parameters](./effect-parameters.md) — the dry/wet convention used
  inside mixer closures.
