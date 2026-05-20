> **Status: NOT STARTED** — Draft PRD; redesigned 2026-05-20 from a single-closure
> operator into a **universal** closure-pipe: chained labeled `->>` clauses fill any
> number of named closure slots on any callable (builtin, `when`, or user `fn`);
> nested `->>` is supported with scoped hole resolution. Phase 1.5 (builtin signature
> migration) from the prior draft is **removed** — closures now route by name.

# PRD: `->>` Closure-Pipe Operator (Universal)

## Executive Summary

This PRD specifies a binary operator `->>` that provides syntactic sugar for passing
**closures as named arguments** to a function call. It combines the ergonomics of
pipe-style block syntax (familiar from Ruby, Swift, Kotlin) with Akkado's existing
field-access pattern on `@` (the canonical hole token; `%` is a legacy alias).

`->>` is **universal**: one mechanism serves *any callable that takes a callable* —
single-closure builtins (`poly`, `map`, `tap_delay`, …), multi-callback constructs
(`when(cond, then, else)`), user-defined `fn`s with function-typed parameters, and
future parallel/router effects. There are no per-builtin special cases in the
operator itself.

### Why?

Many Akkado callables take a function/closure argument. Today, passing an inline
closure is verbose, and there is no concise way at all to supply *more than one*
callback:

```akkado
// Current: must write the full closure signature and param names
n"C4' Am7' G4'" |> poly(@, (freq, gate, vel) -> saw(freq) * ar(gate) * vel) |> out(@, @)

// Proposed: ->> provides the closure body, compiler fills the params
n"C4'" |> poly(@) ->> saw(@.freq) * ar(@.gate) * @.vel |> out(@, @)

// Multiple callbacks — one labeled ->> clause per slot:
when(beat(2) > 1)
  ->> then: saw(@.freq)
  ->> else: sine(@.freq)
```

### Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Token | `->>` (single 3-char token) | Combines `->` (closure arrow) and `>` (pipe direction); distinct from both `->` and `\|>` |
| Hole aliases | `@` is canonical; `%` is a legacy alias | Both parse identically as holes; new code prefers `@` |
| Multiple callbacks | **chained labeled `->>`** — one clause per closure slot | A function with N closure slots receives N `->>` clauses, each routed by `label:` |
| Label requirement | required when the callable has >1 closure slot; optional (omitted) when it has exactly 1 | Single-closure ergonomics stay terse; multi-closure stays unambiguous |
| Nesting | **allowed** — a `->>` body may contain further `->>` calls | `@` binds to the *innermost* closure; outer closures are reached via `as name` |
| Closure routing | inserted as a **named argument** keyed on the slot name | Closures need not be the last positional slot — no signature migration |
| Precedence | binds tighter than `\|>`, looser than every arithmetic/logical operator | `f() ->> body \|> rest` = `(f() ->> body) \|> rest` |
| Body grouping | both `(...)` and `{...}` | Parens for inline pipe chains; braces for multi-statement blocks |

---

## 1. Current State

### 1.1 Callables That Take Closures

| Callable | Current signature | Closure slot(s) | Closure arity | Closure param names |
|----------|-------------------|-----------------|---------------|---------------------|
| `poly(input, instrument, voices=64)` | `instrument` (slot 1) | 1 | 3 | `freq`, `gate`, `vel` |
| `mono` / `legato` | instrument slot | 1 | 3 | `freq`, `gate`, `vel` |
| `tap_delay(in, time, fb, processor, dry?, wet?)` | `processor` (slot 3) | 1 | 1 | `x` |
| `tap_delay_ms` / `tap_delay_smp` | mirror `tap_delay` | 1 | 1 | `x` |
| `map(array, fn)` | `fn` (slot 1) | 1 | 1–2 | `val`, `idx` |
| `reduce(array, fn, init)` | `fn` (slot 1) | 1 | 2 | `acc`, `elem` |
| `zipWith(a, b, fn)` | `fn` (slot 2) | 1 | 2 | `a`, `b` |
| `when(cond, then, else)` | *future* — closure-based `when` from `prd-runtime-functions-control-flow.md` | **2** | per construct | per construct |
| user `fn` | function-typed params (see §1.5) | 0..N | per declaration | per declaration |

No builtin today takes *more than one* closure (`compose(f, g, …)` is multi-function
but chains its arguments — not the multi-slot case). The closure-based
`when(cond, then, else)` is the first concrete multi-callback callable and is the
motivating case for this redesign.

### 1.2 How Closure Params Are Resolved Today

For builtins, the expected closure arity and param names are **not** in `BuiltinInfo`
— they are hardcoded in each codegen handler (`handle_poly_call`,
`handle_tap_delay_call`, array op handlers in `codegen_arrays.cpp`). `->>` requires
this metadata to be declared (see §4).

### 1.3 Hole Operator `@` (canonical) / `%` (legacy alias)

Both `%` (`TokenType::Hole`) and `@` (`TokenType::At`) parse identically as holes via
`parse_hole()` (`parser.cpp:705-772`). `@` is canonical; `%` parses for backward
compatibility. `@.field` / `@field` (dotless) access a field of the hole.

### 1.4 Pipe Operator `|>`

`a |> f(@)` rewrites to `f(a)` (hole substitution) during semantic analysis via
`rewrite_pipes()` in `analyzer.cpp`. `|>` has the lowest operator precedence
(`Precedence::Pipe`, `parser.hpp:14`).

### 1.5 User Functions with Function-Typed Parameters

User functions can already *receive* a function and call it:

```akkado
fn apply(sig, fx) -> fx(sig)
apply(saw(440), my_gain)
```

But there is **no syntax today to declare** that a `fn` parameter is function-typed
or what its callback's parameter names are. §4.4 adds that.

---

## 2. Goals and Non-Goals

### Goals

1. **Concise closure-as-argument syntax**: `fn(args) ->> body` desugars to
   `fn(args, slot: (params) -> body)`.
2. **Multiple callbacks**: `fn(args) ->> a: bodyA ->> b: bodyB` fills several named
   closure slots in one call.
3. **Universal**: one implementation works for builtins, `when`, and user `fn`s — the
   operator does no callable-specific branching.
4. **Nested callbacks**: a `->>` body may itself contain `->>` calls; `@` resolves to
   the innermost closure, outer closures are reachable via `as name`.
5. **Field-access destructuring**: `@.freq` inside a body maps to the named closure
   param `freq`.
6. **Body grouping**: both `(parens)` and `{braces}` for bodies containing pipe chains.
7. **Backwards compatible**: all existing explicit-closure syntax works unchanged.

### Non-Goals

- **Not a general "pipe to any function"**: `->>` supplies *closure* arguments only.
  For ordinary function application, `|>` already works.
- **Not a standalone expression**: `->> body` without a preceding call is an error.
- **No implicit function inference**: the LHS must be an explicit call.
- **No runtime closure type**: desugaring is purely syntactic — the same AST nodes
  are produced as if the user wrote explicit closures. Closures stay fully inlined at
  compile time.
- **MVP LHS is a `Call`**: `MethodCall` LHS (`n"…".each_voice(...)`) is deferred to
  Future Work (§11).

---

## 3. Target Syntax

### 3.1 Single Closure (unlabeled)

```akkado
// Current:
n"C4' Am7' G4' F4'"
  |> poly(@, (freq, gate, vel) -> saw(freq) * ar(gate) * vel)
  |> out(@, @)

// With ->>:
n"C4' Am7' G4' F4'"
  |> poly(@) ->> saw(@.freq) * ar(@.gate) * @.vel
  |> out(@, @)
```

`poly` has exactly one closure slot, so the clause needs no label. Inside the body:
`@.freq` / `@.gate` / `@.vel` are direct references to the `instrument` closure's
parameters.

> `.gate` is held high with a 1-sample drop at every event onset (pair with ADSR);
> `.trig` is a 1-sample pulse (pair with AR / sample retrigger). See
> mini-notation-reference §Event Fields.

### 3.2 Multiple Callbacks (chained labeled `->>`)

When a callable has more than one closure slot, each `->>` clause **must** carry a
`label:` naming the slot it fills:

```akkado
// when(cond, then, else) — two closure slots `then` and `else`
when(beat(2) > 1)
  ->> then: saw(@.freq)
  ->> else: sine(@.freq)

// → when(beat(2) > 1,
//        then: (freq, gate, vel) -> saw(freq),
//        else: (freq, gate, vel) -> sine(freq))
```

Clause order is free — labels route each closure to its slot. Omitting a label when
the callable has >1 slot is error **E510**.

### 3.3 Mono / Legato

```akkado
n"c4 e4 g4" |> mono(@)   ->> saw(@.freq) * ar(@.gate) |> out(@, @)
n"c4 e4 g4" |> legato(@) ->> saw(@.freq) * ar(@.gate) |> out(@, @)
```

### 3.4 Pipes Inside the Closure Body (Parens)

When the body contains its own pipe chain, wrap it in `(...)`. Inside a grouped body,
`@` follows context-dependent rules relative to `|>`:

- `@.freq` **before** `|>` → closure param `freq`
- `@` **after** `|>` → standard pipe hole (filled by the pipe input)

```akkado
n"C4'"
  |> poly(@) ->> (saw(@.freq) |> lp(@, 2000 * adsr(@.gate)) * @.vel)
  |> out(@, @)
```

`@.freq` and `@.gate` (before `|>`) resolve to closure params; `@` in `lp(@, …)`
(after `|>`) is filled by `saw(freq)`'s output. The outer `|> out(@, @)` attaches to
`poly()`'s result.

### 3.5 Pipes Inside the Closure Body (Braces)

```akkado
n"C4'"
  |> poly(@) ->> {
       saw(@.freq)
       |> lp(@, 2000 * adsr(@.gate))
       * @.vel
     }
  |> out(@, @)
```

Semantically identical to the parens form; the choice is stylistic.

### 3.6 tap_delay

```akkado
// tap_delay(in, time, fb, processor, dry?, wet?)
tap_delay(sig, 0.5, 0.3) ->> @ * 0.5
```

For a single-param closure, bare `@` (no field) refers to that one parameter.
Optional `dry`/`wet` keep their defaults; supply them positionally or by name *before*
`->>`:

```akkado
tap_delay(sig, 0.5, 0.3, dry: 0.2, wet: 0.8) ->> @ * 0.5
```

### 3.7 Array Operations

```akkado
map([1, 2, 3])      ->> @.val * 2
reduce([1, 2, 3], 0) ->> @.acc + @.elem
zipWith([1, 2], [3, 4]) ->> @.a + @.b
```

Closure params are accessed via `@.field` (field names come from the slot's
`callback_params` metadata, §4.3). For single-param closures, bare `@` is shorthand.
Note `reduce` keeps its existing `(array, fn, init)` signature — the closure routes
to the `fn` slot by name, so `init` may follow it (see §5).

### 3.8 Nested `->>` and `as` Binding

A `->>` body may itself contain `->>` calls. `@` always refers to the **innermost**
closure. To reach an **outer** closure's record, name it with `as name` on the call:

```akkado
n"C4'"
  |> poly(@) as v ->> ( map(harmonics) ->> osc("sin", @.val * v.freq) )
  |> out(@, @)
```

- `@.val` → `map`'s closure param (innermost scope)
- `v.freq` → `poly`'s `instrument` closure param, reached via the `as v` binding
- `as` here names the synthesized closure's event record; it is scoped to the `->>`
  construct and is distinct from the `expr as name` pipe-binding (which still works
  for ordinary pipelines).

The plain `as` pipe-binding also works *inside* a body, exactly as in any expression:

```akkado
n"C4'"
  |> poly(@) ->>
       saw(@.freq) as snd
       |> lp(snd, 2000 * adsr(@.gate))
       * @.vel
  |> out(@, @)
```

### 3.9 Chaining with `|>`

`->>` binds tighter than `|>`, so the outer pipeline operates on the completed call:

```akkado
// parses as: (poly(n"C4'") ->> body) |> out(@, @)
n"C4'" |> poly(@) ->> saw(@.freq) * ar(@.gate) * @.vel |> out(@, @)
```

---

## 4. Desugaring Semantics

### 4.1 The Transformation

A `->>` chain is a postfix construct on a call:

```
<call> [as <name>] ->> [label₁:] body₁  ->> [label₂:] body₂  ...
  ⇒
<call>( <existing args...>,
        <slot₁>: (<params₁>) -> body₁′,
        <slot₂>: (<params₂>) -> body₂′,
        ... )
```

Where:
1. Each clause produces one synthetic `Closure`, appended to the call as a **named
   argument** keyed on the closure slot's name. Named-arg routing places it at the
   correct `param_names` index regardless of position — closures need **not** be the
   last positional slot.
2. `<slotᵢ>` is the slot named by `labelᵢ`. If the callable has exactly one closure
   slot, the (single) clause may omit its label and fills that slot.
3. `<paramsᵢ>` are the slot's `callback_params` (§4.3).
4. `bodyᵢ′` is `bodyᵢ` with all `@`/`%` holes resolved against the closure scope
   stack (§4.5).

### 4.2 `@` Semantics in the Body with Pipes

Inside a `->>` body, `@` follows a **context-dependent** rule relative to `|>`:

- **Before any `|>`**: `@` is the closure parameter record. `@.freq` accesses `freq`.
- **After a `|>`**: `@` is a standard pipe hole, replaced by the piped value.

```akkado
poly(pat) ->> (saw(@.freq) |> lp(@, 2000))
// @.freq (before |>)        → closure param `freq`
// @ in lp(@, 2000) (after |>) → pipe hole, filled by saw(freq)
// result: (freq,gate,vel) -> saw(freq) |> lp(@, 2000)
```

To use the closure param deeper in a pipe chain, bind it first with `as`:

```akkado
poly(pat) ->> (@.freq as f |> saw(f) |> lp(@, 2000))
```

### 4.3 Closure Slot Metadata

A single metadata shape describes every closure slot, for builtins and user `fn`s
alike:

```cpp
struct ClosureSlotInfo {
    std::string_view slot_name;                                   // "instrument", "then", "fn"
    std::array<std::string_view, MAX_CLOSURE_PARAMS> callback_params; // {"freq","gate","vel"}
    std::uint8_t callback_param_count = 0;
    bool required = true;
};
```

> **Scope:** this metadata only governs `->>` desugaring. An explicit closure keeps
> full naming freedom — `tap_delay(s, t, fb, (sample) -> sample * 0.5)` still works
> with `sample`. Metadata is consulted only when `->>` synthesizes the param list.

### 4.4 Declaring Closure Slots

**Builtins** — `BuiltinInfo` gains `closure_slots[]` + `closure_slot_count`. Each
`slot_name` must match a `param_names[i]` whose `param_types[i]` is
`ParamValueType::Function` (the enum value already exists in `builtins.hpp:34` but is
currently unused). Example:

```cpp
{"poly", { /* ... existing fields ... */
           .param_types = { /* slot 1 = */ ParamValueType::Function, ... },
           .closure_slots = {{ .slot_name = "instrument",
                               .callback_params = {"freq","gate","vel"},
                               .callback_param_count = 3, .required = true }},
           .closure_slot_count = 1 }},
```

**User functions** — a `fn` parameter written `name(p1, p2, …)` is function-typed,
with `p1..pn` as its callback param names:

```akkado
fn router(sig, wet(x), dry(x)) -> ...
fn voiced(notes, instrument(freq, gate, vel)) -> instrument(...)
```

`ParsedParam` and `FunctionParamInfo` gain `is_function` + `callback_params`;
`UserFunctionInfo` gains `closure_slots`, populated during FunctionDef registration.

**Unified lookup** — one helper `resolve_closure_slots(call_name)` checks the builtin
table first, then the user-function table, returning a span of `ClosureSlotInfo`. The
desugaring pass calls *only* this — no builtin-vs-user branching anywhere.

### 4.5 Scoped Hole Resolution

Desugaring maintains a **stack of closure scopes**, each
`{ param_names, optional as_name }`:

- Entering a synthesized closure for a clause **pushes** a scope with the slot's
  `callback_params`; if the originating call had `as name`, the scope records it.
- A nested `->>` inside a body pushes a further (innermost) scope.

Resolution of a hole in a body:

| Hole form | Resolves to |
|-----------|-------------|
| `@.field` / `@field` | the **innermost** scope's param `field`; **E502** if `field` is not an innermost param |
| bare `@` | the innermost scope's sole param; **E503** if the innermost closure has ≠1 param (ambiguous) |
| `name.field` where `name` is an `as`-bound scope | that outer scope's param `field` (**E502** variant if `field` absent) |

Outer-scope params are **not** reachable via bare `@` — only via an `as` binding.
Holes appearing in a `Pipe` RHS are **left untouched** (the closure-pipe pass runs
*before* `rewrite_pipes`, which then fills them as ordinary pipe holes — see §5.2).

### 4.6 Body Parsing

Each clause body is parsed at `Precedence::Or`: it captures unary, arithmetic, power,
comparison, equality, and logical operators, but **stops at the outer `|>`** and at
the next `->>`.

| Operator class | Captured by a clause body? |
|---|---|
| Method/Call/Primary (`. ()`) | Yes |
| Unary (`!`, `-`) | Yes |
| Power, Multiplication, Addition (`^ * / + -`) | Yes |
| Comparison, Equality (`< > == !=`) | Yes |
| Logical (`&& \|\|`) | Yes |
| `->>` | **No** — continues the chain (a nested `->>` attaches to a `Call` *inside* the body) |
| Pipe (`\|>`) | **No** — terminates the body |

`(...)` / `{...}` groupings admit their own `|>` chains.

---

## 5. Architecture / Technical Design

### 5.1 Lexer

Add `TokenType::ArrowPipe` (`token.hpp`) and a `"ArrowPipe"` entry in
`token_type_name()`. In `lexer.cpp`, in the `-` branch, after matching `>`, peek for a
second `>`; the 3-char match must precede the 2-char `Arrow`:

```cpp
case '-':
    if (peek() == '>') {
        advance();
        if (peek() == '>') { advance(); return make_token(TokenType::ArrowPipe); }
        return make_token(TokenType::Arrow);
    }
    // ... rest of minus handling
```

### 5.2 Parser

`->>` is **not** a new entry in the `Precedence` enum — adding one shifts every value
and forces a numeric-threshold audit. Instead, `->>` is parsed as a **postfix chain on
a `Call` node**, hooked into the existing postfix loop alongside `.method()` and
`[index]` (`parser.cpp:513-524`, `578-587`), guarded by "left is a `Call`". That loop
runs whenever `prec <= Precedence::Method`, which covers the `|>` RHS (parsed at
`Precedence::Addition`) and the top-level expression (`Precedence::Pipe`) — so the
`->>` chain binds tighter than `|>` *and* still fires correctly inside a `|>` RHS
(`a |> poly(@) ->> body`).

New method `parse_closure_pipe_chain(NodeIndex call_lhs)`:

1. Require `call_lhs` is a `NodeType::Call` (else **E500**).
2. Optionally consume `as <Identifier>` — only when followed by `->>` (3-token
   lookahead `as` `Identifier` `->>`), so a plain `poly(@) as v` with no `->>` still
   parses as ordinary pipe-binding.
3. Loop while `check(TokenType::ArrowPipe)`:
   - Consume `->>`.
   - **Label**: if the next two tokens are `Identifier` then `Colon`, consume both —
     this clause is labeled. (2-token lookahead; unambiguous — record literals start
     with `{`, and a ternary `:` is never clause-leading.)
   - Parse the body with `parse_precedence(Precedence::Or)` (or a `(...)`/`{...}`
     group).
   - Emit one `ClosureClause` node (`ClosureClauseData{ optional<string> label }`)
     with the body as its child.
4. Build one `ClosurePipe` node (`ClosurePipeData{ optional<string> as_name }`):
   first child = the `Call`, then N `ClosureClause` children.

New AST: `NodeType::ClosurePipe`, `NodeType::ClosureClause`, the two data structs
added to the `Node::data` variant, plus `node_type_name()` entries.

### 5.3 `BuiltinInfo` / Symbol Table Extension

- `builtins.hpp`: `ClosureSlotInfo` struct, `MAX_CLOSURE_SLOTS` (4),
  `MAX_CLOSURE_PARAMS` (e.g. 4); `BuiltinInfo` gains `closure_slots[]` +
  `closure_slot_count`. Populate `poly`/`mono`/`legato`/`tap_delay`/`tap_delay_ms`/
  `tap_delay_smp`/`map`/`reduce`/`zipWith`, and the closure-based `when` once
  `prd-runtime-functions-control-flow.md` ships it. Set `param_types[slot] =
  ParamValueType::Function` for each closure slot.
- `symbol_table.hpp`: `FunctionParamInfo` gains `is_function` + `callback_params`;
  `UserFunctionInfo` gains `closure_slots`.
- `parser.hpp`/`parser.cpp`: `ParsedParam` gains `is_function` + `callback_params`;
  `parse_param_list` parses `name(p1, p2, …)` function-typed params.

### 5.4 Analyzer / Desugaring Pass

New pre-pass `desugar_closure_pipes(root)`, invoked **before** `rewrite_pipes` in the
`analyze` entry point (`analyzer.cpp:~65`). Running first means the synthetic closures
and any pipes inside their bodies are processed by the unchanged `rewrite_pipes` +
`substitute_nodes` afterward.

For each `ClosurePipe`:

1. `slots = resolve_closure_slots(fn_name)`. Empty → **E501**.
2. **Label resolution.** >1 slot → every clause must be labeled (**E510**), each label
   must match a `slot_name` (**E511**). Exactly 1 slot → a single unlabeled (or
   matching-labeled) clause fills it. More clauses than slots → **E512**.
3. **Required-slot check.** Every `required` slot must be filled (**E513**).
4. For each clause, synthesize a `Closure` node whose param children are `Identifier`
   nodes from the slot's `callback_params`, and whose body is the scope-resolved
   clause body (§4.5).
5. Append each synthetic closure as a **named `Argument`** (`ArgumentData.name =
   slot_name`) to the `Call`. Replace the `ClosurePipe` with that `Call`.
6. Recurse into the produced `Call` (so nested `->>` inside bodies desugar too).

The codegen path is unchanged: after desugaring, codegen sees standard `Call` +
`Closure` nodes, which the existing `handle_poly_call` / `handle_tap_delay_call` /
array-op handlers already accept.

### 5.5 Validation

| Condition | Code | Type | Message |
|-----------|------|------|---------|
| LHS is not a call expression | E500 | Error | `->>` left side must be a function call, got `<type>` |
| Callable has no closure slot | E501 | Error | `<fn>()` does not accept a closure argument |
| `@.field` not in innermost signature, not reachable via an `as` scope | E502 | Error | `@` has no field `<field>` in closure signature for `<fn>()`. Available: `<list>` |
| bare `@` used where innermost closure has ≠1 param | E503 | Error | Ambiguous bare `@` — closure `<slot>` has params `<list>`; use `@.<name>` |
| clause label omitted but callable has >1 closure slot | E510 | Error | `<fn>()` has multiple closure slots — label each `->>` clause (`->> <slot>: …`) |
| clause label matches no closure slot | E511 | Error | `<fn>()` has no closure slot `<label>`. Slots: `<list>` |
| more `->>` clauses than closure slots | E512 | Error | `<fn>()` has `<n>` closure slot(s) but `<m>` `->>` clauses given |
| a required closure slot left unfilled | E513 | Error | `<fn>()` requires a closure for slot `<slot>` |
| `as` name collides with an enclosing `->>` `as` name | E514 | Error | `as <name>` shadows an enclosing closure binding |
| `as` name shadows an in-scope variable/param | W151 | Warning | `as <name>` shadows an in-scope name |

---

## 6. Impact Assessment

| Component | Status | Notes |
|-----------|--------|-------|
| Lexer / Token enum | **Modified** | New `TokenType::ArrowPipe`, 3-char lexing rule |
| Parser | **Modified** | `parse_closure_pipe_chain()` as a `Call`-postfix; no `Precedence` enum change |
| AST | **Modified** | New `NodeType::ClosurePipe` / `ClosureClause` + data structs |
| `BuiltinInfo` | **Modified** | *Additive* — `closure_slots[]`, `closure_slot_count`; set `param_types[slot]=Function` |
| Symbol table | **Modified** | `FunctionParamInfo` / `UserFunctionInfo` gain closure-slot fields |
| Parser param list | **Modified** | `name(p1,…)` function-typed param syntax |
| Semantic Analyzer | **Modified** | New `desugar_closure_pipes` pre-pass (scoped hole resolution) |
| Codegen handlers | **Stays** | Desugaring produces standard `Call` + `Closure`; existing handlers accept them |
| Builtin signatures (`poly`/`reduce`/`tap_delay*`) | **Stays** | No reordering — closures route by name (see §5) |
| Existing call sites / tests / docs | **Stays** | No positional-signature migration |
| Existing closure syntax `(params) -> body` | **Stays** | Unchanged |
| Pipe operator `\|>` | **Stays** | No changes to `\|>` semantics |
| Cedar VM | **Stays** | Purely an Akkado-compiler syntactic rewrite |

> **Why no signature migration?** The prior draft of this PRD had a "Phase 1.5" that
> reordered `poly`/`reduce`/`tap_delay*` so the closure was the last positional slot,
> because flat single-closure `->>` appended the closure positionally. The redesigned
> `->>` inserts each closure as a **named** argument keyed on its slot name, which the
> existing named-arg routing places correctly at any position. Phase 1.5 is therefore
> **removed entirely** — no `BuiltinInfo` reorder, no `handle_*_call` index changes,
> no call-site/test/doc churn.

---

## 7. File-Level Changes

| File | Phase | Change |
|------|-------|--------|
| `akkado/include/akkado/token.hpp` | 1 | Add `ArrowPipe` to `TokenType`; `"ArrowPipe"` in `token_type_name()` |
| `akkado/src/lexer.cpp` | 1 | 3-char `->>` lexing rule (before 2-char `->`) |
| `akkado/include/akkado/ast.hpp` | 1 | `NodeType::ClosurePipe` / `ClosureClause`; `ClosurePipeData` / `ClosureClauseData`; `Node::data` variant + `node_type_name()` |
| `akkado/include/akkado/parser.hpp` | 1 | Declare `parse_closure_pipe_chain()`; add `is_function`/`callback_params` to `ParsedParam` |
| `akkado/src/parser.cpp` | 1, 2 | Hook `parse_closure_pipe_chain` into the `Call`-postfix loop; parse `as name`, label lookahead, clause bodies at `Precedence::Or`; extend `parse_param_list` for `name(p1,…)` |
| `akkado/include/akkado/builtins.hpp` | 2 | `ClosureSlotInfo`, `MAX_CLOSURE_SLOTS`/`MAX_CLOSURE_PARAMS`; `BuiltinInfo.closure_slots` + `closure_slot_count`; populate closure-taking builtins; set `param_types[slot]=Function` |
| `akkado/include/akkado/symbol_table.hpp` | 2 | `FunctionParamInfo` / `UserFunctionInfo` closure-slot fields |
| `akkado/src/analyzer.cpp` | 2, 3 | Build `UserFunctionInfo::closure_slots` at FunctionDef registration; `resolve_closure_slots()`; `desugar_closure_pipes()` pre-pass before `rewrite_pipes` |
| `akkado/tests/**` | 1–4 | Parser, metadata, desugaring, integration, error tests |
| `web/static/docs/**` | 5 | Document `->>`, chained labels, `as name`, nesting |

### Files That Stay

| File | Reason |
|------|--------|
| `cedar/**` | No VM changes — `->>` is an Akkado-compiler syntactic rewrite |
| `tools/**` | No CLI changes |
| `akkado/src/codegen*.cpp` handlers | Desugaring produces standard `Call` + `Closure` — existing handlers consume them; only a regression test is added |

---

## 8. Implementation Phases

### Phase 1 — Lexer / Parser / AST
- `TokenType::ArrowPipe` + 3-char lexing rule.
- `NodeType::ClosurePipe` / `ClosureClause` + data structs.
- `parse_closure_pipe_chain()` hooked into the `Call`-postfix loop; optional `as
  name`; label lookahead; per-clause body at `Precedence::Or`; `(...)`/`{...}` groups.
- **Tests:** `lex("->>") == ArrowPipe`; `lex("->") != ArrowPipe`;
  `"poly(x) ->> y"` → `ClosurePipe(Call, ClosureClause(nolabel, Identifier))`;
  `"x ->> y"` → E500; `"when(c) ->> a: p ->> b: q"` → `ClosurePipe` with two labeled
  clauses; `"poly(@) as v ->> body"` records `as_name`; nested `->>` inside a body
  parses to a nested `ClosurePipe`.

### Phase 2 — Universal Closure-Slot Metadata
- `ClosureSlotInfo`; `BuiltinInfo.closure_slots`; populate every closure-taking
  builtin; `param_types[slot]=Function`.
- Callback-signature param syntax `name(p1,…)`; `UserFunctionInfo.closure_slots`.
- `resolve_closure_slots()` (builtins then user fns).
- **Tests:** each builtin's metadata correct; `fn router(sig, wet(x), dry(x))`
  registers two closure slots `wet`/`dry`.

### Phase 3 — Desugaring (Analysis Pre-Pass)
- `desugar_closure_pipes()` before `rewrite_pipes`; per-clause `Closure` synthesis;
  named-arg insertion; scope stack; `@`/`@.field` resolution; `as`-name outer reach;
  recurse for nesting; emit E50x/E51x/W151.
- **Tests:**
  - `poly(pat) ->> saw(@.freq)` → `poly(pat, instrument: (freq,gate,vel) -> saw(freq))`
  - `tap_delay(sig, 0.5, 0.3) ->> @ * 0.5` → `tap_delay(sig, 0.5, 0.3, processor: (x) -> x * 0.5)`
  - `when(c) ->> then: a ->> else: b` → `when(c, then: …, else: …)`
  - `poly(input: pat) ->> saw(@.freq)` — mixed named + positional
  - nested: `poly(@) as v ->> (map(h) ->> osc("sin", @.val * v.freq))`
  - `@` after `|>` in a body is left for `rewrite_pipes`

### Phase 4 — Integration & Error Tests
- Full compile tests; `->>` output bytecode-identical to the equivalent explicit
  named-closure-arg call, for `poly`, `mono`, `legato`, `tap_delay`, `map`, `reduce`,
  `zipWith`, and a user `fn` with a function-typed param.
- All error codes E500–E514 + W151 exercised.
- `when(...) ->> then: … ->> else: …` integration once the closure-based `when` from
  `prd-runtime-functions-control-flow.md` exists.

### Phase 5 — Docs
- Document `->>`, chained labels, `as name`, nesting/scoping in `web/static/docs/**`
  and the language reference.

### Phase 6 (Future) — see §11.

---

## 9. Edge Cases

### 9.1 Nested `->>` (now supported)

```akkado
poly(@) as v ->> ( map(harmonics) ->> osc("sin", @.val * v.freq) )
```

**Expected:** Works. Inner `->>` desugars recursively; `@.val` is `map`'s innermost
param; `v.freq` reaches `poly`'s closure via the `as v` binding. Without `as v`,
`v.freq` would be an undefined-name error and `@.freq` (inside `map`'s body) would be
**E502** — `freq` is not a `map` callback param.

### 9.2 `->>` with Named Arguments

```akkado
poly(input: n"C4'") ->> saw(@.freq) * ar(@.gate) * @.vel
```

**Expected:** Works. The closure is inserted as a named argument
(`instrument: (freq,gate,vel) -> …`); `voices` keeps its default. Routing is correct
regardless of which prior args were named or positional.

### 9.3 `->>` with Optional Arguments

```akkado
tap_delay(sig, 0.5, 0.3) ->> @ * 0.5                 // dry/wet defaulted
tap_delay(sig, 0.5, 0.3, dry: 0.2, wet: 0.8) ->> @ * 0.5
```

**Expected:** Optional args fill positionally or by name as usual; `->>` only
synthesizes the closure-slot argument.

### 9.4 Bare `@` with a Multi-Param Closure

```akkado
poly(n"C4'") ->> @          // E503
```

**Expected:** **Error E503** — `poly`'s `instrument` closure has 3 params; bare `@` is
ambiguous. Use `@.freq` / `@.gate` / `@.vel`. (This is stricter than the prior draft,
which made it a warning with silent first-param fallback.)

### 9.5 `->>` on a Non-Closure-Taking Function

```akkado
out(@) ->> saw(@.freq)      // E501
```

### 9.6 `->>` with No Positional Arguments

```akkado
myFn() ->> body
```

**Expected:** Works when `myFn` declares a function-typed parameter (a closure slot)
that the `->>` clause fills. `myFn` is not parameter-less — it is *called* with no
positional arguments, and `->>` supplies the lone closure-slot argument by name.

### 9.7 `->>` as a Standalone Expression (no LHS call)

```akkado
x ->> body                  // E500
```

### 9.8 Missing / Extra Clauses

```akkado
when(c) ->> then: a                        // E513 — `else` slot unfilled (if required)
when(c) ->> then: a ->> else: b ->> x: c    // E511 — no slot `x`
poly(@) ->> a ->> b                          // E512 — poly has 1 slot, 2 clauses
when(c) ->> a ->> b                          // E510 — labels required (>1 slot)
```

### 9.9 Partial Hole Usage

```akkado
poly(pat) ->> saw(@.freq)                          // uses 1 of 3 params — OK
poly(pat) ->> saw(@.freq) * ar(@.gate) * @.vel     // uses all 3 — OK
```

**Expected:** Both valid. The synthesized closure always declares all of the slot's
params; unused ones are bound but unreferenced.

---

## 10. Testing Strategy

### 10.1 Parser Tests

```cpp
CHECK(lex("->>") == TokenType::ArrowPipe);
CHECK(lex("->")  != TokenType::ArrowPipe);

auto r = parse("poly(x) ->> y");
CHECK(r.node_type == NodeType::ClosurePipe);
CHECK(r.children[0].type == NodeType::Call);

CHECK_FALSE(parse("x ->> y").success);                       // E500

auto w = parse("when(c) ->> then: a ->> else: b");
CHECK(w.children.size() == 3);                               // call + 2 clauses

auto n = parse("poly(@) as v ->> (map(h) ->> @.val)");
CHECK(n.success);                                            // nested ->>
```

### 10.2 Desugaring Tests

```cpp
auto r = compile_and_desugar("poly(pat) ->> saw(@.freq)");
auto c = r.find_node(NodeType::Closure);
CHECK(c.params.size() == 3);
CHECK(c.params[0].name == "freq");

auto t = compile_and_desugar("tap_delay(sig, 0.5, 0.3) ->> @ * 0.5");
CHECK(t.find_node(NodeType::Closure).params.size() == 1);

auto when = compile_and_desugar("when(c) ->> then: a ->> else: b");
// → when(c, then: () -> a, else: () -> b)  with labels routed by name

auto nest = compile_and_desugar(
    "poly(@) as v ->> (map(h) ->> osc(\"sin\", @.val * v.freq))");
CHECK(nest.success);   // @.val → map param, v.freq → poly param via `as v`
```

### 10.3 Error Tests

```cpp
CHECK(has_error(compile("x ->> y"), "E500"));
CHECK(has_error(compile("out(@) ->> saw(@.freq)"), "E501"));
CHECK(has_error(compile("poly(pat) ->> saw(@.nope)"), "E502"));
CHECK(has_error(compile("poly(pat) ->> @"), "E503"));
CHECK(has_error(compile("when(c) ->> a ->> b"), "E510"));
CHECK(has_error(compile("when(c) ->> then: a ->> x: b"), "E511"));
CHECK(has_error(compile("poly(@) ->> a ->> b"), "E512"));
```

### 10.4 Integration Tests

```cpp
// ->> bytecode-identical to the explicit named-closure-arg form
auto a = compile("c\"C4'\" |> poly(@) ->> saw(@.freq)*ar(@.gate)*@.vel |> out(@,@)");
auto b = compile("c\"C4'\" |> poly(@, instrument: (freq,gate,vel) -> "
                 "saw(freq)*ar(gate)*vel) |> out(@,@)");
CHECK(a.bytecode == b.bytecode);

CHECK(compile("tap_delay(sig, 0.5, 0.3) ->> @ * 0.5").success);
CHECK(compile("reduce([1,2,3], 0) ->> @.acc + @.elem").success);
CHECK(compile("map([1,2,3]) ->> @.val * 2").success);

// user fn with a declared function-typed param
CHECK(compile("fn apply(sig, fx(x)) -> fx(sig)\n"
              "apply(saw(440)) ->> @ * 0.5").success);
```

### 10.5 Build & Run

```bash
cmake --build build --target akkado_tests
./build/akkado/tests/akkado_tests "[closure-pipe]"
./build/akkado/tests/akkado_tests "[parser]"
./build/akkado/tests/akkado_tests "[codegen]"
```

---

## 11. Future Work

- **`MethodCall` LHS**: `->>` on method calls (`n"…".each_voice(…) ->> …`). Requires
  the E500 check and `resolve_closure_slots` to accept method names.
- **N-way / variadic closure slots**: routers or switches with an unbounded set of
  labeled closure slots (`switch(sel) ->> a: … ->> b: … ->> c: …`).
- **Closure-slot type checking**: validating that a `->>` body's result type matches
  what the slot expects, via the type system (`prd-compiler-type-system.md`).
- **Block bodies with local bindings**: richer multi-statement `{...}` bodies inside
  a clause (`let`-style locals).
