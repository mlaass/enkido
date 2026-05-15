---
title: State Cells
category: builtins
order: 11
keywords: [state, get, set, cell, persistent, register, slot, memory, counter, accumulator, hold, store]
group: tools
subgroup: state-io
icon: Database
tagline: Persistent cells with get/set for counters and accumulators.
---

# State Cells

State cells let you persist a single floating-point value across audio blocks from inside a closure. They are the building block for writing your own stateful operators (counters, latches, custom envelopes, slot-based step sequencers, ...) directly in Akkado, without needing to add a C++ opcode.

A state cell has three operations:

- `state(init)`: allocate a cell, set its initial value
- `cell.get()`: read the current value
- `cell.set(v)`: write a new value

Each `state(...)` call site at a distinct AST position gets its own slot. Cells survive hot-swap as long as their `state(...)` AST position is structurally unchanged.

## state

**Allocate a state cell.** Returns a handle that `get` / `set` consume.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| init  | number | -       | Value written to the slot the very first time the cell is touched |

```akk
my_idx = state(0)            // a new cell starting at 0
my_idx.set(42)               // write
my_idx.get()                 // read → 42
```

The `init` value is consumed *only* on the first execution of the program; subsequent runs (including those after a hot-swap edit) preserve whatever value `set()` last wrote. To force re-initialization after editing the init expression, move the `state(...)` call to a different AST position.

`state` is a reserved identifier; you cannot define a closure named `state`.

## get

**Read the current value of a state cell.** Returns a Signal whose every sample equals the slot value.

| Param | Type      | Default | Description |
|-------|-----------|---------|-------------|
| cell  | StateCell | -       | The cell to read |

```akk
counter = state(0)
counter.get() |> mtof(@) |> sine(@) |> out(@)
```

`get` is a reserved identifier.

## set

**Write a new value to a state cell.** Stores the value at the *last sample* of the input buffer (state cells are scalar registers; per-sample writes would conflict with that semantic). Returns the new value as a Signal so `set` can be used in expression position.

| Param | Type      | Default | Description |
|-------|-----------|---------|-------------|
| cell  | StateCell | -       | The cell to write to |
| value | signal    | -       | The new value (last sample stored) |

```akk
// Increment a counter on every rising edge of trig
n = state(0)
n.set(n.get() + gateup(trig))

// In expression position: increment AND read in a single line
arr[n.set(n.get() + 1)]
```

`set` is a reserved identifier.

## Example: writing a stepper in userspace

```akk
notes = [60, 64, 67, 72]

// Walk the array forward on every rising edge of trig.
step = (arr, trig) -> arr[counter(trig)]

// Walk forward or backward by `dir` each tick, using a state cell directly.
step_dir = (arr, trig, dir) -> {
  idx = state(0)
  idx.set(select(gateup(trig), idx.get() + dir, idx.get()))
  arr[idx.get()]
}

notes.step(trigger(4)) |> mtof(@) |> sine(@) |> out(@)
```

## Record-valued state cells

A cell can hold a record instead of a single scalar. Each field becomes its own
internal slot, but you read and write the whole record as a unit:

```akk
voice = state({freq: 440, vel: 0.5, gate: 0})

// get(cell) returns the whole record; field access works as usual.
osc("sin", get(voice).freq) * get(voice).vel * get(voice).gate

// set(cell, new_record) replaces the whole record. The new value's field
// names must exactly match the cell's declared shape.
set(voice, {freq: 880, vel: 0.7, gate: 1})
```

### Per-field sugar

When a state cell holds a record, you can read and write fields directly on
the cell handle. Both forms desugar to the explicit `get` / `set` calls and
are observably equivalent, but the surface is much shorter:

```akk
voice = state({freq: 440, vel: 0.5, gate: 0})

// Read sugar: cell.field is shorthand for get(cell).field.
tone = osc("sin", voice.freq) * voice.vel * voice.gate
out(tone, tone)

// Write sugar: cell.field = expr is shorthand for
//   set(cell, {..get(cell), field: expr}).
voice.gate = 1
voice.freq = 880

// Self-referential updates are fine — RHS reads land before LHS writes.
counter = state({n: 0})
counter.n = counter.n + 1
```

Field assignment is statement-only — it cannot appear as the right-hand side
of a pipe (E205). Hoist the write to a top-level statement:

```akk
// Doesn't parse:
//   button("hit") |> voice.gate = 1     // E205
// Write it as two statements instead:
trig = button("hit")
voice.gate = trig
```

Plain value records stay immutable; the sugar only applies when the receiver
is a state cell. `r = {x: 1}; r.x = 5` errors with E150 and a hint pointing
at `state({...})`. Nested-field writes (`voice.outer.inner = 5`) are deferred
and rejected with E204 — rewrite as `set(voice, {..get(voice), outer: {..get(voice).outer, inner: v}})`.

A future `update(cell, patch)` stdlib helper will package whole-record updates
as `update(voice, {gate: 1})`; until argument spread lands inside builtin call
sites it must be written inline as `set(voice, {..get(voice), gate: 1})`.

Constraints on record cells:

- Each field's value must be a number or signal; nested record fields are not
  yet supported.
- `set()` must pass a record with exactly the same field-name set as the
  initial record — extra or missing fields produce error E189.
- A scalar cell rejects record values, and vice versa.
- Hot-swap preserves cell contents per field. Adding a new field to the cell's
  shape across a reload freshly initializes that field; removing a field
  retires its slot.

```akk
// Whole-record updates compose with ordinary expressions.
counter = state({n: 0, last: 0})
button("inc") |> set(counter, {n: get(counter).n + 1, last: get(counter).n})
```

Related: [Records](../language/records.md), [counter](edge.md#counter), [gateup](edge.md#gateup), [Method calls](../language/methods.md)
