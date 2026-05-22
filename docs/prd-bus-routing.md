> **Status: SHIPPED — all three phases complete (2026-05-22)** — Drafted
> 2026-05-18; scope expanded 2026-05-20 (numbered buses, per-bus FX, routing
> operators). Phase 1 (numbered buses + always-safe master), Phase 2 (per-bus
> FX: `mixer`/`master`), and Phase 3 (diamond operator `<>` + docs) are all
> implemented and tested.

# Bus Routing & Master Bus PRD

## Executive Summary

Today, every `out(...)` call in an Akkado program sums its signal directly
into the audio device's stereo output (`op_output` at
`cedar/include/cedar/opcodes/utility.hpp:42-58`). There is no shared
processing stage between the per-voice/per-track outputs and the device,
and no concept of a routing bus.

This PRD adds a **unified bus system**:

- **Numbered buses.** `bus(N, L, R?)` sums a signal into bus `N`. Bus
  indices are dynamic — the compiler allocates a stereo buffer pair for
  each index actually referenced in the program.
- **Bus 0 is the master.** Bus 0 is the device-output bus. `out(L, R?)`
  is an alias for `bus(0, L, R?)`. Every non-zero bus auto-sums into
  bus 0.
- **Per-bus FX via `mixer`.** `mixer(N, closure)` attaches a processing
  closure to bus `N`'s summed signal. `master(closure)` is an alias for
  `mixer(0, closure)`. One mechanism for every bus.
- **Always-safe master.** Bus 0 carries a default soft-clip at 0.9 and a
  non-disableable safety stage (NaN/Inf guard + hard rail at ±1.0).
- **Diamond operator.** `<>` is sugar for `|> out(@)` and `<>(N)` is
  sugar for `|> bus(N, @)` — a one-token statement terminator.
- **Compiler-rewritten.** No new VM opcode; `bus()` writes into per-bus
  scratch buffers and the compiler appends a per-bus epilogue that runs
  the `mixer` chains and the bus-0 safety stage.

Goals: make global compression / soft-clipping a one-liner, give
live-coders real group/track buses, prevent amplitude blow-ups by
default, and keep hot-swap and live-coding ergonomics.

---

## 0. Implementation Phases

The feature ships in three phases. Each phase is independently useful and
leaves the compiler in a working, tested state.

### Phase 1 — Bus core + always-safe master *(shipped 2026-05-22)*

- `bus(N, …)` builtin; `out(…)` lowered to `bus(0, …)` (both routed
  through one `handle_bus_call` so `out(@)` ≡ `bus(0, @)` byte-identical).
- Per-bus stereo scratch buffer pairs, allocated after the main DAG.
- The per-block epilogue: clear bus accumulators (prologue), sum non-zero
  buses into bus 0, run the default soft-clip @ 0.9, run the forced
  safety stage (NaN/Inf sanitize + hard rail ±1.0), store to the device.
- Diagnostics `E260` (non-literal bus index) and `W202` (mono `bus()`
  auto-broadcast).
- No `mixer`/`master`, no `<>`.

Implementation notes / deviations from the draft below:

- **No new VM opcode.** A new `InstructionFlag::BUS_WRITE` bit selects
  whether `op_output` accumulates into the device sinks (clear) or into
  the `(out_buffer, out_buffer+1)` scratch pair (set). This is the
  smallest possible change and keeps every hand-written `OUTPUT` test
  (which never sets the flag) writing to the device unchanged.
- **The default soft-clip uses `Opcode::DISTORT_SOFT`** (the `softclip`
  builtin's opcode); the draft's "`distort_softclip`" is informal.
- **The epilogue is emitted only when the program has ≥1 sink** — see
  §5.3.
- A test-only `bypass_master` flag on `akkado::compile()` suppresses the
  whole master bus so pre-existing tests can probe raw `out()` values.
  It is **not reachable from user source**, so the PRD's
  non-disableable-safety guarantee (§4.3) still holds for every program
  a user can write.

### Phase 2 — Per-bus FX *(shipped 2026-05-22)*

- `mixer(N, closure)` + `master(closure)` alias; closure bodies inlined
  into the epilogue; both closure arities (`(s) -> …` and `(l, r) -> …`).
- Diagnostics `E_BUS_SINK_IN_CLOSURE` (`E261`), `E_BUS_MIXER_ARITY`
  (`E262`), `W_BUS_MIXER_OVERRIDDEN` (`W203`), `W_BUS_MIXER_MONO_RETURN`
  (`W204`), `W_BUS_NO_WRITERS` (`W205`).

Implementation notes / deviations from the draft below:

- **`handle_mixer_call` is deferred-codegen.** It validates the call and
  records a `MixerCall {bus_index, closure_node, arity, params, loc}`,
  emitting nothing at the call site. `emit_bus_epilogue` consumes the
  recorded calls — for each non-zero bus it inlines the closure body
  before summing into bus 0, and for bus 0 it inlines the closure body
  *instead of* the default soft-clip. The forced safety stage always
  follows. Closures are inlined with `inline_mixer_closure`, which binds
  the param(s) to the bus stereo pair and `visit()`s the body.
- **`E262 E_BUS_MIXER_ARITY`** was added (not in the draft's §7 table):
  a `mixer`/`master` closure must take exactly 1 (stereo) or 2 (left,
  right) plain parameters — 0, 3+, destructure, or rest params are
  rejected.
- **`W204` fires for both arities.** The draft scoped the mono-return
  warning to the `(s) -> …` form; in practice a `(l, r) -> …` closure
  that returns mono is also auto-broadcast `L = R` with `W204` (lenient,
  uniform) rather than a hard error.
- **No `top_level_only` enforcement.** No such mechanism exists in the
  compiler and Phase 1 shipped `bus`/`out` without it. `E261`
  (sink-in-closure) is the structural guard; a general top-level-only
  check is left as a follow-up.
- **Analyzer fix (general).** Phase 2's idiomatic closure form
  `(s) -> s |> f(@)` exposed a latent bug: the closure-from-pipe sugar
  in `rewrite_pipes` misfired on a pipe whose innermost LHS is a bound
  closure/function parameter (it saw the param as unbound during the
  pre-pass), wrongly producing a spurious nested closure. Fixed in
  `clone_subtree` by registering closure/function parameters in the
  symbol table before cloning the body. This also fixes the latent case
  for `poly`/`each`/`fn` bodies written as `(p) -> p |> …`.

### Phase 3 — Diamond operator + docs *(shipped 2026-05-22)*

- `<>` / `<>(N)` lexer + parser sugar.
- `web/static/docs/concepts/bus-routing.md`; builtin reference entries.
- Diagnostic `E_BUS_DIAMOND_POSITION` (`E263`).

Implementation notes / deviations from the draft below:

- **`<>` is a single `Diamond` token** added to `TokenType`. The lexer's
  `case '<'` matches `>` before `=` — maximal munch makes `<>` one token
  while a spaced `< 3` stays a `Less` comparison and `<=`/`>=` are
  untouched.
- **Lowered in the parser, not codegen.** `parse_statement` consumes a
  trailing `Diamond` *after* the full expression (so it binds looser than
  `|>`) and builds the AST `expr |> out(@)` (bare `<>`) or
  `expr |> bus(N, @)` (`<>(N)`) directly. Because both lower to the same
  `Call` nodes Phases 1/2 already handle, the bytecode is byte-identical
  to the hand-written pipe — no codegen change.
- **Index validation is not duplicated.** The `(N)` expression is wired in
  verbatim; `handle_bus_call` already rejects a non-literal index with
  `E260`, so `<>(x)` and `bus(x, @)` produce the identical diagnostic.
- **`E263` is enforced at a single chokepoint.** The draft anticipated two;
  in practice one suffices. A `Diamond` reaching `parse_prefix` (an
  expression was expected) is the misplaced case — and a `<>` left over
  after a non-expression statement *also* lands there, since it becomes the
  leading token of the next statement parse. A `<>` buried deeper inside a
  parenthesized sub-expression falls through to the existing grouping
  syntax error (acceptable backstop).

§12 tags each acceptance criterion with its phase.

---

## 1. Motivation

### 1.1 Current behavior

```cpp
// cedar/include/cedar/opcodes/utility.hpp (op_output)
ctx.output_left[i]  += sanitize(l);
ctx.output_right[i] += sanitize(r);
```

Each `out()` is an isolated sink. Three consequences:

1. **No global FX point.** Adding a master limiter today means manually
   wiring every track through a shared expression by hand, which the
   language has no first-class shortcut for.
2. **No grouping.** There is no way to route a subset of voices through
   a shared processing chain (a "drum bus", a "reverb send target").
3. **No global headroom guard.** The per-sample NaN/Inf check inside
   `op_output` rejects bad samples but does nothing to bound the
   summed amplitude — a four-voice synth easily clips the device.

### 1.2 What live-coders want

Two common reaches:

```akkado
// "Wrap everything in a glue compressor / soft clipper and forget it."
master((s) -> s |> comp(@, -12, 4) |> softclip(@, 0.8))

// "Send the drums to their own bus, process the bus, not each hit."
kick |> bus(1, @)
snare |> bus(1, @)
mixer(1, (s) -> s |> comp(@, -8, 6))   // glue the drum bus
```

…and the default behavior (no `master`/`mixer` call) should still be
safe.

---

## 2. The Bus Model

### 2.1 Buses are numbered, bus 0 is the master

A **bus** is a named stereo summing point identified by a non-negative
integer index:

- **Bus 0** is the **master / device-output bus**. Its post-processing
  output is what the audio device receives.
- **Buses 1, 2, 3, …** are ordinary buses. Each one **auto-sums into
  bus 0** after its own `mixer` chain runs.

The bus topology in v1 is **flat**: a non-zero bus always feeds bus 0
directly. A bus cannot feed another non-zero bus (see Non-Goals §11).

```text
   out() / <> / bus(0,…) writers ─────────────┐
                                              ▼
  bus(1,…) writers ─▶ [bus 1] ─▶ mixer(1) ──▶ [bus 0] ─▶ mixer(0)/master
  bus(2,…) writers ─▶ [bus 2] ─▶ mixer(2) ──▶   │           │
        ⋮                                       │           ▼
                                                │      [forced safety]
                                                │           │
                                                └──────────▶ device
```

### 2.2 Bus indices are dynamic but compile-time constant

There is **no fixed pool size**. The compiler scans the whole program
for every `bus(N, …)`, `mixer(N, …)`, and `<>` / `<>(N)` reference and
allocates one stereo scratch buffer pair per distinct index. Bus 0 is
always allocated.

The index **must be a compile-time non-negative integer literal**.
`bus(x, @)` where `x` is a runtime value (a `param`, a closure
argument, a computed expression) is a compile error
(`E_BUS_INDEX_NOT_LITERAL`) — the buffer set must be known at
program-load time. This is a deliberate v1 restriction; dynamic
routing is a non-goal.

---

## 3. Surface API

### 3.1 `bus(N, …)` — writing to a bus

```akkado
bus(N, signal)        // stereo (or mono, auto-broadcast L=R) signal
bus(N, L, R)          // explicit left + right
```

`bus` is a **sink** (returns void). It sums its signal into bus `N`'s
buffer, mirroring today's `out()` mono/stereo handling exactly:

- A mono argument is auto-broadcast `L = R` with a **W-warning**
  (mirrors current `out(mono)` behavior).
- The two-arg `bus(N, L, R)` form takes explicit channels.

Multiple `bus(N, …)` writers for the same `N` **sum** — that is the
whole point of a bus.

### 3.2 `out(…)` — alias for `bus(0, …)`

```akkado
out(signal)     ≡  bus(0, signal)
out(L, R)       ≡  bus(0, L, R)
```

`out` is retained verbatim for source compatibility; every existing
program keeps working unchanged. It is defined as a pure alias — the
compiler lowers `out(...)` to `bus(0, ...)` codegen.

### 3.3 `mixer(N, closure)` — per-bus FX

```akkado
mixer(N, closure)
```

The closure runs **once per output block** on bus `N`'s **summed
signal** — after every `bus(N, …)` writer has contributed, and (for
`N = 0`) after every non-zero bus has summed in. Its result replaces
the bus's signal for everything downstream.

Two closure arities are accepted; arity is inferred from the parameter
list:

```akkado
// Stereo form: closure receives one stereo signal value
mixer(1, (s) -> s |> comp(@, -12, 4) |> softclip(@, 0.85))

// Mono-pair form: closure receives left + right separately
mixer(1, (l, r) -> stereo(softclip(l, 0.9), softclip(r, 0.9)))
```

The mono-pair form must return a stereo value (typically via
`stereo(L, R)`). If a stereo-form closure returns a mono value it is
auto-broadcast `L = R` with a **W-warning** (`W_BUS_MIXER_MONO_RETURN`).

A bus with no `mixer(N, …)` call applies **no processing** (identity) —
it is a pure summing point. The one exception is bus 0, which carries a
default tone chain (§4.2).

### 3.4 `master(closure)` — alias for `mixer(0, closure)`

```akkado
master(closure)   ≡  mixer(0, closure)
```

`master` is retained verbatim. Supplying a `master(...)` (or an
explicit `mixer(0, ...)`) **replaces the default bus-0 tone chain**;
the forced safety stage (§4.3) still runs afterward.

### 3.5 The diamond operator — `<>` and `<>(N)`

The **diamond operator** `<>` replaces the trailing `|> bus(…)` of a
pipe statement:

| Sugar    | Expands to       | Meaning                  |
|----------|------------------|--------------------------|
| `<>`     | `\|> bus(0, @)`  | route to the master bus  |
| `<>(N)`  | `\|> bus(N, @)`  | route to bus `N`         |

```akkado
osc("saw", 220) <>            // ≡  osc("saw", 220) |> out(@)
kick            <>(1)         // ≡  kick |> bus(1, @)
n"c4 e4 g4" as e |> osc("saw", e.freq) <>(2)
```

Rules:

- `<>` is a **statement terminator**. It binds looser than `|>` —
  `a |> b |> c <>(1)` parses as `bus(1, (a |> b |> c))`. It produces
  void and may not appear in sub-expression position.
- The optional `(N)` argument is a non-negative integer literal — a
  plain parenthesized literal, parsed exactly like any call argument
  (interior whitespace is fine). With no argument, `<>` targets bus 0.
- `<>(0)` is exactly equivalent to `<>`. The redundancy is harmless;
  bare `<>` is the idiomatic spelling for master.

### 3.6 Disabling a bus's tone chain

There is **no special `none` token**. To run a bus (including bus 0)
with no tone processing, supply the identity closure:

```akkado
master((s) -> s)        // bus 0: no tone chain — only the forced safety stage runs
mixer(1, (s) -> s)      // bus 1: explicit identity (same as omitting it)
```

This keeps the surface uniform: `mixer`/`master` always take a closure.

### 3.7 Capturing top-level bindings

A `mixer`/`master` closure captures top-level scope under normal
closure rules. This is the recommended way to expose bus tweakables to
the web UI:

```akkado
drive = param("master_drive", 0.85, 0.1, 1.0)
master((s) -> s |> softclip(@, drive))
```

---

## 4. Signal Flow & Default Chain

### 4.1 Per-block evaluation order

Once per output block, after the main DAG has run:

1. For each non-zero bus `N` (order among them is irrelevant — they are
   independent): run `mixer(N)`'s closure on bus `N`'s summed buffer
   (identity if no `mixer(N)` call), then **sum** the result into
   bus 0's buffer.
2. Bus 0's buffer now holds: every `out()`/`<>`/`bus(0,…)` writer plus
   every non-zero bus contribution.
3. Run `mixer(0)`/`master`'s closure on bus 0 — or the **default tone
   chain** if neither is present.
4. Run the **forced safety stage** on bus 0.
5. Store bus 0 to the device sinks (`ctx.output_left/right`).

### 4.2 Default chain on bus 0

When no `master(...)` / `mixer(0, ...)` call appears, the compiler
emits a default tone chain on bus 0:

- **Algorithm**: `distort_softclip` (polynomial soft-clipper, existing
  opcode in `cedar/include/cedar/opcodes/distortion.hpp`).
- **Threshold**: fixed at **0.9**, not exposed as a parameter. Users who
  want a tunable threshold supply their own `master(...)` closure.
- **Stereo**: applied independently per channel.

Non-zero buses have **no default tone chain** — an un-`mixer`'d bus is
pure identity. They feed bus 0, which carries the default; double
soft-clipping every bus would be wrong.

### 4.3 Forced safety stage (bus 0 only)

Regardless of whether the user overrode bus 0's tone chain, a fixed
safety stage runs **after** the bus-0 chain and **only on bus 0**:

1. **NaN/Inf sanitize** — non-finite samples replaced with 0 (matches
   today's `op_output` guard).
2. **Hard rail at ±1.0** — `clamp(x, -1.0, 1.0)`. Not a soft clipper;
   this is the "do not damage speakers" final guarantee.

The user cannot disable this stage. An aggressive master closure
(`(s) -> s * 100`) cannot reach the device above ±1.0. Non-zero buses
do not get their own safety stage — they all flow through bus 0, which
is guarded.

---

## 5. Compilation Model

The bus system is implemented as a **compiler rewrite**. No new VM
opcode is required for the core flow, and no runtime dispatch happens.

### 5.1 Per-bus buffers

The compiler scans the program for every distinct bus index referenced
by `bus`, `mixer`, `<>`/`<>(N)`, `out`, `master`. For each index it
allocates a stereo scratch buffer pair (`bus_<N>_L`, `bus_<N>_R`) from
the existing arena at program-load time. Bus 0 is always allocated.

### 5.2 `bus()` codegen

Every `bus(N, …)` call (including `out` lowered to `bus(0, …)`, and the
`<>` / `<>(N)` sugar) emits the existing `OUTPUT`-style summing
instructions, retargeted to write into `bus_<N>_L/R` instead of
`ctx.output_left/right`. The mono/stereo branches of today's `out()`
codegen (`akkado/src/codegen.cpp` ~1380-1500) are reused unchanged;
only the destination buffer differs.

### 5.3 Epilogue

After the main DAG is topologically scheduled, the compiler appends a
fixed **epilogue segment** implementing §4.1:

```text
for each non-zero bus N:
    [load]  bus_<N>_L/R
    [run]   mixer(N) closure body   (or identity if none)
    [sum]   into bus_0_L/R
[load]   bus_0_L/R
[run]    mixer(0)/master closure body   (or default softclip 0.9 if none)
[guard]  NaN/Inf sanitize → clamp ±1.0
[store]  ctx.output_left, ctx.output_right
```

Each closure body is inlined into the epilogue at compile time, exactly
as if typed in source at the end of the program. State-ids inside a
closure follow the normal hot-swap rules — no reserved `/__bus/` path.

The epilogue is emitted **whenever the program has at least one
`out()`/`bus()` sink** — i.e. for every program that produces audio.
A program with no sink produces no audio and gets no epilogue, so
pure-computation snippets stay free of a silent master chain. (The
draft specified an unconditionally-emitted epilogue; emitting it only
when a sink exists is functionally identical for every audible program
and was adopted in Phase 1.)

### 5.4 Multiple `mixer`/`master` calls for the same bus

The compiler collects all `mixer(N, …)` invocations (with `master(c)`
counted as `mixer(0, c)`). For each bus `N`, the **last call in
program order** (standard "last bound wins" across modules) supplies
the inlined closure; earlier calls are dropped, each producing a
**W-warning** (`W_BUS_MIXER_OVERRIDDEN`) listing the dropped site and
the winning site.

### 5.5 Restrictions inside a `mixer`/`master` closure

A `mixer`/`master` closure is signal-processing code. It **may not**
contain a sink — `out`, `bus`, `mixer`, `master`, or `<>` inside a
closure body is a compile error (`E_BUS_SINK_IN_CLOSURE`). This rules
out routing cycles (a bus feeding itself) in v1.

---

## 6. Parsing the Diamond Operator (`<>`)

The diamond operator `<>` is lexed as a single token (`DIAMOND`). It
carries no ambiguity:

- **No comparison clash.** `<` and `>` are comparison operators, but
  `<>` is not a valid comparison — Akkado has no `<>` operator and a
  comparison always has an operand between the brackets. The lexer
  emits `<>` as one token via maximal munch.
- **No mini-notation clash.** Per-cycle alternation `<a b c>` only ever
  appears **inside typed-prefix string literals** (`n"<a b c>"`,
  `s"…"`), lexed within the string body — never as bare program tokens.

The parser accepts the diamond only in **statement-trailing position**
(after a complete pipe expression at statement level); elsewhere it is
a syntax error. An optional `(N)` immediately following is parsed as an
ordinary parenthesized argument — a non-negative integer literal
(`E_BUS_INDEX_NOT_LITERAL` otherwise), with interior whitespace
allowed. `<>` with no argument lowers to `bus(0, @)`; `<>(N)` lowers to
`bus(N, @)`.

---

## 7. Diagnostics

New diagnostic codes (numbers allocated from the next free slots during
implementation; placeholder names below):

| Code | Number | Kind | Phase | Trigger |
|------|--------|------|-------|---------|
| `E_BUS_INDEX_NOT_LITERAL` | `E260` | error | P1 | `bus`/`mixer`/`<>(N)` index is not a compile-time non-negative integer literal (also covers index ≥ 200). |
| `W_BUS_MONO_INPUT` | `W202` | warning | P1 | `bus(N, mono)` single-arg form auto-broadcasts `L = R`. (Not emitted for legacy `out(mono)` — that path stays silent to avoid warning every existing mono program.) |
| `E_BUS_SINK_IN_CLOSURE` | `E261` | error | P2 | `out`/`bus`/`mixer`/`master` used inside a `mixer`/`master` closure body. |
| `E_BUS_MIXER_ARITY` | `E262` | error | P2 | A `mixer`/`master` closure does not take exactly 1 or 2 plain parameters. |
| `W_BUS_MIXER_OVERRIDDEN` | `W203` | warning | P2 | More than one `mixer(N, …)` (or `master`) targets the same bus `N`. Lists dropped + winning sites. |
| `W_BUS_MIXER_MONO_RETURN` | `W204` | warning | P2 | A `mixer`/`master` closure returns a mono value. Auto-broadcasts `L = R`. |
| `W_BUS_NO_WRITERS` | `W205` | warning | P2 | `mixer(N, …)` with `N > 0` targets a bus that has no `bus(N, …)`/`<>(N)` writers — the closure processes silence. |
| `E_BUS_DIAMOND_POSITION` | `E263` | error | P3 | `<>` used outside statement-trailing position (sub-expression, assignment RHS, closure/fn body, leading position). |

> Phase 1 allocated `E260` and `W202` from the next free codegen slots.
> Phase 2 allocated `E261`, `E262`, `W203`, `W204`, `W205` from the next
> consecutive slots. Phase 3 allocated `E263`.

---

## 8. Examples

### 8.1 Default — nothing to do

```akkado
osc("saw", 220) <>
// implicit: master((s) -> softclip(s, 0.9)) + forced NaN/clamp safety
```

### 8.2 Glue compressor + soft clip on the master

```akkado
master((s) -> s
    |> comp(@, -12, 4)
    |> softclip(@, 0.85)
)

n"c4 e4 g4" as e
    |> osc("saw", e.freq)
    |> @ * e.vel
    <>
```

### 8.3 A drum bus

```akkado
kick  <>(1)
snare <>(1)
hat   <>(1)

mixer(1, (s) -> s |> comp(@, -8, 6) |> softclip(@, 0.9))
```

### 8.4 Live-tweakable master drive

```akkado
drive   = param("master_drive", 0.85, 0.5, 1.0)
ceiling = param("master_ceiling", -1.0, -6.0, 0.0)  // dB

master((s) -> s
    |> softclip(@, drive)
    |> limiter(@, db_to_amp(ceiling))
)
```

### 8.5 Per-channel processing on a bus

```akkado
mixer(2, (l, r) -> stereo(
    l |> softclip(@, 0.9),
    r |> softclip(@, 0.9)
))
```

### 8.6 Disable the default master tone chain

```akkado
master((s) -> s)   // forced safety stage still runs
```

---

## 9. Hot-Swap Behavior

A `mixer`/`master` closure is **just code** from the hot-swap system's
point of view. Opcode state-ids inside a closure follow the standard
semantic ID path rule. On code update:

- Edits inside a closure body rebind state under the normal hot-swap
  rules (matching state-ids preserved, structural changes
  micro-crossfaded).
- Swapping a `mixer(N, …)` call for a different one is a structural
  change — short crossfade per the global hot-swap policy.
- Removing `mixer(N, …)` reverts bus `N` to identity (bus 0: to the
  default tone chain). Adding it replaces that. Either is a structural
  epilogue change and gets the standard rebind treatment.
- Adding/removing a whole bus (a new `bus(N, …)` index) reallocates the
  buffer set on recompile; existing buses keep their state-ids.

No reserved `/__bus/` state-id path. No special handling.

---

## 10. File-Level Changes

| File | Change |
|------|--------|
| `akkado/include/akkado/builtins.hpp` | Add `bus` (index + mono/stereo signal, returns void) and `mixer` (index + closure, returns void). Define `out` as alias for `bus(0,…)` and `master` as alias for `mixer(0,…)`. Mark all four `top_level_only` for the sink/closure forms. |
| Lexer (`akkado/src/lexer*`) | Recognize `<>` as a single `DIAMOND` token. |
| Parser (`akkado/src/parser*`) | Accept `DIAMOND` only in statement-trailing position, with an optional parenthesized integer-literal argument; lower `<>`→`bus(0,@)`, `<>(N)`→`bus(N,@)`. |
| `akkado/src/codegen.cpp` (~1380-1500, `out()` branch) | Generalize the `out()` sink path to `bus(N,…)`: retarget `OUTPUT` summing instructions to `bus_<N>_L/R`. `out` lowers to `bus(0,…)`. |
| `akkado/src/codegen.cpp` (new section) | Scan all bus indices; allocate per-bus buffer pairs. Collect `mixer`/`master` calls; emit `W_BUS_MIXER_OVERRIDDEN`. Emit the multi-bus epilogue (§5.3). Validate index-is-literal and no-sink-in-closure. |
| `cedar/include/cedar/opcodes/utility.hpp` | No change for the core flow. `op_output`'s sanitize is preserved as the bus-0 safety stage's NaN guard, applied as inlined IR in the epilogue. |
| `cedar/include/cedar/opcodes/distortion.hpp` | No change. Default bus-0 chain reuses existing `distort_softclip`. |
| `cedar/include/cedar/vm/context.hpp` | No change. `output_left/right` remain the device sinks; buses use ordinary arena scratch buffers. |
| `web/static/docs/reference/builtins/` | Add `bus` / `mixer` entries; note `out` / `master` as aliases; document the diamond operator `<>` / `<>(N)`. |
| `web/static/docs/concepts/bus-routing.md` | **NEW** when the feature ships (out of scope for the PRD itself). |
| `docs/prd-bus-routing.md` | This PRD. |

---

## 11. Non-Goals (v1)

- **No bus-to-bus routing.** A non-zero bus always feeds bus 0
  directly. Buses cannot feed other non-zero buses; no routing graph.
- **No runtime/dynamic bus indices.** The index must be a compile-time
  integer literal. No `bus(param(...), @)`.
- **No reading a bus as a source.** There is no `bus(N)` (no signal
  arg) read form. Sends, sidechains, and feedback taps are a separate
  feature.
- **No sinks inside `mixer` closures.** Rules out routing cycles
  (`E_BUS_SINK_IN_CLOSURE`).
- **No `master()` zero-arg sugar.** Disable the tone chain with the
  identity closure `master((s) -> s)`.
- **No bypass UI / runtime on-off toggle.** Wire it in the closure:
  `mixer(N, (s) -> if(on, processed, s))`.
- **No pre/post-bus metering primitive.** Insert existing visualization
  builtins into a closure if needed.
- **No threshold parameter on the default bus-0 chain.** Fixed at 0.9;
  write a `master(...)` line to change it.
- **No safety stage on non-zero buses.** They all flow through the
  guarded bus 0.

---

## 12. Acceptance Criteria

Each criterion is tagged with its phase (§0).

- [x] **(P1)** Programs with no `master`/`mixer(0)` call run the default
      polynomial softclip at 0.9 + safety stage on bus 0; existing test
      programs produce audible output unchanged in character below 0.9
      amplitude.
- [x] **(P1)** `out(@)` and `bus(0, @)` produce byte-identical bytecode.
- [x] **(P3)** `<>` compiles identically to `|> out(@)`; `<>(N)` compiles
      identically to `|> bus(N, @)`.
- [x] **(P1)** A non-zero bus with multiple `bus(N, …)` writers sums all
      contributions and feeds bus 0.
- [x] **(P2)** `mixer(N, …)` on a non-zero bus processes that bus's summed
      signal before it reaches bus 0; a bus with no `mixer` is identity.
- [x] **(P2)** `master((s) -> s)` produces output identical to a program
      with no master tone processing (only the forced ±1.0 rail and NaN
      guard).
- [x] **(P2)** `mixer`/`master` closures of both arities (`(s) -> …` and
      `(l, r) -> …`) compile and produce expected stereo output.
- [x] **(P2)** Multiple `mixer(N, …)` for the same bus: only the last
      takes effect; `W_BUS_MIXER_OVERRIDDEN` reports all sites. `master`
      and `mixer(0, …)` together count as targeting the same bus.
- [x] **(P2)** Stereo-form closure returning mono: `W_BUS_MIXER_MONO_RETURN`
      reported; output is `L = R`.
- [x] **(P1)** `bus(N, expr)` with a non-literal `N` is rejected with
      `E_BUS_INDEX_NOT_LITERAL` (allocated as `E260`).
- [x] **(P2)** A sink (`out`/`bus`/`mixer`) inside a `mixer` closure
      is rejected with `E_BUS_SINK_IN_CLOSURE` (`E261`).
- [x] **(P1/P2)** An aggressive signal into the master is clamped to ±1.0
      by the safety stage; the device receives no |sample| > 1.0. Phase 1
      verifies this with `osc("saw",110) |> @ * 100 |> out(@)`; Phase 2
      adds the `master((s) -> s |> @ * 100)` *closure* form.
- [ ] **(P2)** Hot-swap: editing inside a `mixer`/`master` closure
      preserves stateful opcode state under the standard hot-swap rules.
      *(Mechanism in place — `inline_mixer_closure` uses a stable
      `push_path("mixer#" + N)` semantic-ID path keyed on the bus index;
      no dedicated hot-swap test added yet.)*
- [ ] **(P2)** A `param(...)` captured by a closure is live-tweakable from
      the web UI and updates the bus chain without recompile.
      *(Captures resolve through the normal closure path; no dedicated
      web-UI test added yet.)*
- [x] **(P3)** The diamond `<>` lexes as a single token and `<>(3)` routes
      to bus 3; `gain < 3` still lexes as a comparison.
- [x] **(P3)** A misplaced `<>` (sub-expression / assignment-RHS / leading
      position) is rejected with `E_BUS_DIAMOND_POSITION` (`E263`).

---

## 13. Resolution of Open Questions

| # | Question | Decision |
|---|----------|----------|
| 1 | `master()` zero-arg sugar | Do NOT add. Only `master(closure)` / `mixer(N, closure)`. |
| 2 | Safety-stage shape | Inline IR in the epilogue (no new opcode). |
| 3 | Diagnostic code numbers | Next consecutive free slots, assigned at implementation. |
| 4 | `<N>` vs `<>(N)` spelling | Use the diamond operator `<>` / `<>(N)`. `<>` is a single token — no comparison or mini-notation ambiguity, no whitespace rule. |
| 5 | Bus-to-bus routing | Out of scope v1; flat `N → 0` topology only. |

If any of these turn out to matter to a stakeholder, revise before
implementation lands.
