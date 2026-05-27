# PRD: Userspace State Primitives and Edge-Triggered Operators

> **Status: COMPLETE** — Adds `state` cells, edge-triggered SAH modes, and method-call codegen so users can implement state-bearing operators (e.g. `step`) without C++ changes. All four phases shipped 2026-04-26 (Phases 1–4 in commits 4587e05, d6b3abc, 77eb3ba, c2ceb69; STATE_OP store semantics revised in 5941140 — see §5.1). Implementation audit: `docs/audits/prd-userspace-state-and-edge-primitives_audit_2026-05-01.md`.

## 1. Overview

This PRD introduces three small, coordinated additions to Akkado and Cedar that, together, make it possible to write **stateful userspace closures** — closures that maintain state across audio blocks. The motivating example is a `step` operator that walks an array on each rising edge of a trigger, but the same primitives unlock custom envelopes, counters, latches, and other live-coding building blocks that today require new C++ opcodes.

The three additions are:

1. **State cells (`state(init)`)** — a first-class persistent storage primitive for closures. Reads via `s.get()`, writes via `s.set(v)`. Each `state(...)` call site allocates its own state-pool slot via the existing semantic-ID-path mechanism. The builtin is named `state` to align with Cedar's existing "state pool" / `state_id` terminology.
2. **EDGE_OP opcode** — the existing `SAH` opcode renamed to reflect its broader role, with three new modes selected by `inst.rate`:
   - mode 0: classic sample-and-hold (unchanged)
   - mode 1: `gateup` — output 1.0 on rising edge of input
   - mode 2: `gatedown` — output 1.0 on falling edge of input
   - mode 3: `counter` — output increments on rising edge, with optional reset/start
3. **UFCS method-call codegen** — fill in the `MethodCall` codegen stub at `akkado/src/codegen.cpp:1513` so `x.foo(a, b)` lowers to `foo(x, a, b)` for any callable `foo` in scope (builtins and closures alike). The parser already accepts this syntax.

### Why?

Today, every stateful operator must be implemented as a C++ opcode in `cedar/include/cedar/opcodes/` because Akkado closures regenerate their bytecode each block — local bindings do not survive block boundaries. This PRD ends that limitation: users write state-bearing operators directly in Akkado and ship them in `.akk` patches. The three additions are the minimum surface area needed: state cells provide cross-block persistence, edge primitives provide trigger detection, and UFCS makes the call sites read naturally (`arr.step(trig)`).

### Major design decisions

- **State cells are the persistence primitive**, not language-level `static` declarations or mutable records. State cells are explicit, composable, and add a single new value type without altering the immutability of records or introducing a new keyword.
- **One new mode-dispatched opcode (`STATE_OP`)** is added for cell I/O (init / load / store via `inst.rate`), alongside the `SAH` → `EDGE_OP` rename. Both opcodes use the established `inst.rate` field convention from CLAUDE.md §Extended Parameter Patterns. No DSP-level opcodes are added.
- **`state` / `get` / `set` are regular builtins**, not a separate "intrinsic" category. Their codegen is special-cased by name in the same way `len` is special-cased today (it's a builtin that lowers to `PUSH_CONST` with the array length resolved at compile time). The names are reserved — user closures cannot shadow them.
- **Method calls desugar universally** — no type-dispatch table, no method tables on records. `x.foo(a)` ≡ `foo(x, a)` if `foo` is callable. Same model as Rust UFCS.

---

## 2. Problem Statement

### What exists today

| Capability | Status | Reference |
|---|---|---|
| Array literals `[1,2,3,4]` | ✅ Works | `akkado/src/parser.cpp:609`, `codegen.cpp:222` |
| Dynamic indexing `arr[i]` | ✅ Works (auto-wraps when `inst.rate == 0`) | `ARRAY_INDEX = 161`, `cedar/include/cedar/opcodes/arrays.hpp:37` |
| Multi-statement closure bodies | ✅ Works | `parser.cpp:882`, `codegen.cpp:893` |
| Reassignment within a block | ✅ Works (silent upsert) | `codegen.cpp:568` |
| Method-call **parsing** | ✅ Works | `parser.cpp:990` |
| Method-call **codegen** | ❌ Throws `E113 Method calls not supported in MVP` | `codegen.cpp:1513` |
| Sample-and-hold opcode | ✅ Works | `cedar/include/cedar/opcodes/utility.hpp:185` (handler), `instruction.hpp:63` (`SAH = 55`) |
| `clamp`, `wrap`, `select` opcodes | ✅ Work | `CLAMP = 46` (line 52), `WRAP = 47` (line 53), `SELECT = 140`; builtin table at `builtins.hpp:530, 534, 540` |
| `len` builtin | ✅ Works (compile-time, lowers to `PUSH_CONST`) | `builtins.hpp:694` |
| `%` modulo binary operator | ❌ Doesn't exist (the `%` token is `Hole`, used for pipe placeholders) | `lexer.cpp:205`, `BinOp` enum has no `Mod` |
| Cross-block persistence in closures | ❌ No mechanism | — |
| Standalone edge-detection builtins | ❌ Don't exist | — |

### What's missing

To write `step` as a userspace closure, a user must be able to (1) detect rising edges of a trigger signal, (2) hold a piece of state across audio blocks, and (3) call the operator with method-style syntax. None of those three are currently available. The compiler accepts `arr.step(trig)` as a parse but the codegen explicitly errors with `E113 Method calls not supported in MVP` at `akkado/src/codegen.cpp:1513`. The state-pool / `state_id` mechanism that backs every stateful built-in is not exposed to user code in any form.

### Current vs proposed

| Today | After this PRD |
|---|---|
| Stateful operators must be implemented in C++ | Stateful operators can be implemented in Akkado |
| `arr.step(trig)` → compile error | `arr.step(trig)` → `step(arr, trig)` |
| No way to "remember" a value between blocks in user code | `state(init)` + `.get()` / `.set()` |
| Edge detection is implicit inside specific opcodes (ADSR, AR, SAH, SAMPLE_PLAY) | Standalone `gateup(sig)`, `gatedown(sig)` builtins |
| Counter / accumulator only available via `SEQPAT_TRANSPORT` (internal) | `counter(trig, reset, start)` builtin |

---

## 3. Goals and Non-Goals

### Goals

1. **Ship `step` as userspace Akkado**. All four variants — `step(arr, trig)`, `step(arr, trig, reset)`, `step(arr, trig, reset, start)`, `step_dir(arr, trig, dir)` — implementable in `.akk` files using only the primitives this PRD introduces.
2. **Add `state(init)` cells** as a first-class value type with `.get()` and `.set(v)` operations. State cells use the existing state-pool / semantic-ID-path mechanism for cross-block persistence. Each `state(...)` call site gets its own slot.
3. **Rename `SAH` opcode to `EDGE_OP`** and add three modes via `inst.rate`: classic hold (rate 0, current behavior), `gateup` (rate 1), `gatedown` (rate 2), `counter` (rate 3). The existing `sah` builtin keeps working unchanged (lowers to `EDGE_OP rate=0`).
4. **Add new builtins**: `gateup(sig)`, `gatedown(sig)`, `counter(trig, reset, start)` (with reset/start optional via the default-constants pattern).
5. **Implement method-call codegen** as universal UFCS desugaring at the stub in `akkado/src/codegen.cpp:1513`. Works for any callable in scope — builtins and closures.
6. **Reserve `state`, `get`, `set` as built-in identifiers.** Users cannot define closures with these names — they always resolve to the cell-I/O builtins. This avoids the surprising case where `s.get()` could lex-resolve to a user closure that doesn't understand StateCell args.
7. **Zero regressions** in existing patches and tests. Existing `sah(in, trig)` calls continue to compile and produce identical bytecode (modulo opcode renaming).

### Non-Goals

- **No `static` keyword.** State cells subsume that use case and avoid reserving a new identifier.
- **No record field mutation.** Records remain immutable values. (Could be revisited in a future PRD if needed.)
- **No type dispatch on method calls.** UFCS only — `x.foo(a)` always means `foo(x, a)`. Records do not have method tables.
- **No type checking on `set()` value.** The state cell holds a single float; passing a non-float is undefined for now (matches existing builtin laxness).
- **No multi-element state cells.** Cells hold one float. Cells of arrays / records are out of scope.
- **No reactive / change-tracking semantics.** `set()` is immediate; there are no observers, no diff propagation. A state cell is a slot, not a stream.
- **No new DSP opcodes.** `EDGE_OP` is a rename of an existing opcode. `STATE_OP` is one new mode-dispatched opcode for cell I/O (init / load / store) — not a DSP operation, just slot access. The "no new opcodes" goal was about not multiplying DSP-level opcodes, not banning all instruction additions.
- **No PHP-style late binding for method calls.** UFCS resolves `foo` lexically at the call site.

---

## 4. Target Syntax

### 4.1 Edge-detection builtins

```akkado
// trigger() emits beat-division pulses; gateup detects rising edges of any signal
trig = trigger(4)        // 16th-note pulse train
hits = gateup(trig)      // 1.0 on the sample where trig goes 0 → 1, else 0.0

// gatedown for falling edges, useful for note-off events
release = gatedown(gate_signal)
```

### 4.2 Counter

```akkado
// Bare counter: increments by 1 on each rising edge of trig, never resets
n = counter(trigger(1))

// With reset: counter returns to 0 on rising edge of reset signal
n = counter(trigger(4), beat(4))      // resets every 4 beats

// With reset and explicit start value
n = counter(trigger(4), beat(4), 7)   // resets to 7 instead of 0
```

### 4.3 State cells

```akkado
// Top-level state: one stable slot
my_idx = state(0)

my_idx.get()              // read current value
my_idx.set(42)            // write new value (returns the new value, can be ignored)

// State inside a closure: each call site gets its own slot via semantic-ID hashing
make_counter = () -> {
  s = state(0)
  () -> { s.set(s.get() + 1); s.get() }   // returns a fresh counter closure each call
}
```

### 4.4 The `step` family in userspace

The simplest form (no reset, no start) using the new `counter` builtin and the existing `wrap(value, min, max)` builtin (`builtins.hpp:534`) to keep the index in range:

```akkado
step = (arr, trig) -> arr[wrap(counter(trig), 0, len(arr))]

// Usage
[60, 64, 67, 72].step(trigger(4)) |> note(%) |> osc("sin", %) |> out(%, %)
```

> **Note:** `ARRAY_INDEX` already wraps by default (`cedar/include/cedar/opcodes/arrays.hpp:55`, `((j % length) + length) % length` when `inst.rate == 0`), so `arr[counter(trig)]` would also work without `wrap()`. The explicit `wrap(..., 0, len(arr))` is used here for clarity and to mirror the bounds semantics of `step_dir` below.

With reset:

```akkado
step = (arr, trig, reset) -> arr[wrap(counter(trig, reset), 0, len(arr))]
```

With reset and start (the full form):

```akkado
step = (arr, trig, reset, start) -> arr[wrap(counter(trig, reset, start), 0, len(arr))]
```

A directional variant using a state cell directly:

```akkado
step_dir = (arr, trig, dir) -> {
  idx = state(0)
  idx.set(select(
    gateup(trig),
    wrap(idx.get() + dir, 0, len(arr)),
    idx.get()
  ))
  arr[idx.get()]
}

// Forward/backward arpeggiator
[60, 64, 67, 72].step_dir(trigger(4), 1)    // up
[60, 64, 67, 72].step_dir(trigger(4), -1)   // down
```

### 4.5 Method-call universality

UFCS works for any callable in scope:

```akkado
// Existing builtins
sig.lp(1200)          // ≡ lp(sig, 1200)
sig.delay(0.3, 0.5, 0.5, 0.5)

// User closures
my_func = (x, a) -> x * a + 1
val.my_func(2)        // ≡ my_func(val, 2)

// Pipe-and-method are interchangeable
sig |> lp(%, 1200)    // existing
sig.lp(1200)          // new equivalent
```

---

## 5. Architecture

### 5.1 State cells

A state cell is a new `ValueType` enum entry that carries a 32-bit `state_id` (computed from the AST position of the `state(...)` call site, same hashing scheme as builtins). The cell value is a compile-time handle; at runtime it corresponds to a single `float` slot in the state pool.

**Compile-time representation:**

```cpp
// in akkado/include/akkado/typed_value.hpp (extended)
enum class ValueType : std::uint8_t {
    Signal, Number, Pattern, Record, Array, String, Function, Void,
    StateCell,   // new: handle to a state-pool cell slot
};

struct TypedValue {
    ValueType type = ValueType::Void;
    std::uint16_t buffer = 0xFFFF;
    bool error = false;
    // ...existing fields...
    std::uint32_t cell_state_id = 0;  // populated when type == StateCell
};
```

**Runtime state:**

```cpp
// in cedar/include/cedar/opcodes/dsp_state.hpp (alongside OscState, SAHState, etc.)
struct CellState {
    float value = 0.0f;
    bool initialized = false;   // STATE_OP rate=0 (init) checks this; first touch sets to true
};
```

`CellState` is added to the state-pool variant alongside `OscState`, `EdgeState` (renamed `SAHState`), etc. The internal name keeps the `FooState` convention used throughout the pool.

**One new opcode: `STATE_OP`** (mirrors the EDGE_OP mode-dispatch pattern):

| `inst.rate` | Mode | Behavior |
|---|---|---|
| 0 | `init` | If `!state.initialized`, write `inst.inputs[0]` (init buffer, sample 0) into `state.value` and set `initialized = true`. Otherwise no-op. Output is a buffer broadcast of `state.value`. |
| 1 | `load` | Output is a buffer broadcast of `state.value`. No reads from inputs. |
| 2 | `store` | Write the **latest sample whose value differs from `state.value` at the start of the block** into `state.value`. Output is a buffer broadcast of the new `state.value` (so `set()` can be used in expression position). For inputs that vary sample-by-sample (the typical case), this collapses to "last sample" — but for gated writes such as `idx.set(select(gateup(t), idx+dir, idx))`, the rising-edge sample is captured even when later samples reassert the start-of-block value. See `docs/reports/2026-04-26-stepper-demo-array-and-state-bugs.md` for the rationale; the original "write last sample" contract dropped gated edges in ~127/128 blocks at 128-sample block size and broke the §4.4 `step_dir` example. |

**`state(init)` codegen** (named-builtin special case in `codegen.cpp`, in the same family as `len`):

1. Compute `state_id` from the AST node of the `state` call (semantic-ID path hash, same as existing builtins).
2. Emit a `STATE_OP rate=0` instruction with `inst.state_id = <hash>` and `inst.inputs[0]` = the init buffer.
3. Return a `TypedValue { type = ValueType::StateCell, cell_state_id = <hash>, buffer = <unused, 0xFFFF> }`.

**`s.get()` codegen:**

1. UFCS desugar: `s.get()` → `get(s)`.
2. `get` is a builtin whose codegen requires its argument to be a `StateCell` TypedValue. It extracts `cell_state_id` and emits `STATE_OP rate=1` with `inst.state_id = cell_state_id`.
3. Type error if the argument isn't a state cell.

**`s.set(v)` codegen:**

1. UFCS desugar: `s.set(v)` → `set(s, v)`.
2. `set` is a builtin whose codegen requires its first argument to be a `StateCell`. It extracts `cell_state_id` and emits `STATE_OP rate=2` with `inst.state_id = cell_state_id` and `inst.inputs[0]` = the value's buffer.
3. Returns a `Signal` TypedValue whose buffer is the STATE_OP output, so `set()` can be used in expression position.

> **Why named builtins instead of a new "intrinsic" category?** This follows an existing pattern: `len(arr)` is a builtin (`builtins.hpp:694`) that lowers to `PUSH_CONST` because the codegen special-cases the name to read the array length at compile time. `state`/`get`/`set` use the same mechanism — they're entries in `builtins.hpp` whose codegen is dispatched by name in `codegen.cpp`'s call handler. No new conceptual category, no `is_intrinsic` flag.

### 5.2 EDGE_OP (renamed from SAH)

The existing `SAH` opcode handler `op_sah` at `cedar/include/cedar/opcodes/utility.hpp:185` is renamed to `op_edge` and moved to a new file `cedar/include/cedar/opcodes/edge_op.hpp`. The `Opcode::SAH = 55` enum entry at `cedar/include/cedar/vm/instruction.hpp:63` is renamed to `Opcode::EDGE_OP` (value unchanged). Opcode metadata is auto-generated by `web/scripts/generate-opcode-metadata.ts` (run via `bun run build:opcodes`), so the rename propagates everywhere with one regeneration. The dispatch switch in `cedar/src/vm/vm.cpp:484` gets its `case Opcode::SAH:` updated to `case Opcode::EDGE_OP:`.

The handler dispatches on `inst.rate`:

```cpp
inline void op_edge(ExecutionContext& ctx, const Instruction& inst) {
  auto& state = ctx.states->get_or_create<EdgeState>(inst.state_id);
  float* out = ctx.buffers->get(inst.out_buffer);

  switch (inst.rate) {
    case 0: { // classic SAH (in, trig) — unchanged behavior
      const float* input = ctx.buffers->get(inst.inputs[0]);
      const float* trig = ctx.buffers->get(inst.inputs[1]);
      for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        if (state.prev_trigger <= 0.0f && trig[i] > 0.0f) state.held_value = input[i];
        state.prev_trigger = trig[i];
        out[i] = state.held_value;
      }
      break;
    }
    case 1: { // gateup(sig) — output 1.0 on rising edge of inputs[0]
      const float* sig = ctx.buffers->get(inst.inputs[0]);
      for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = (state.prev_trigger <= 0.0f && sig[i] > 0.0f) ? 1.0f : 0.0f;
        state.prev_trigger = sig[i];
      }
      break;
    }
    case 2: { // gatedown(sig) — output 1.0 on falling edge
      const float* sig = ctx.buffers->get(inst.inputs[0]);
      for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = (state.prev_trigger > 0.0f && sig[i] <= 0.0f) ? 1.0f : 0.0f;
        state.prev_trigger = sig[i];
      }
      break;
    }
    case 3: { // counter(trig, reset, start) — increment on trig edge, reset on reset edge
      const float* trig  = ctx.buffers->get(inst.inputs[0]);
      const float* reset = (inst.inputs[1] != 0xFFFF) ? ctx.buffers->get(inst.inputs[1]) : nullptr;
      const float* start = (inst.inputs[2] != 0xFFFF) ? ctx.buffers->get(inst.inputs[2]) : nullptr;
      for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // reset wins if both fire on the same sample
        if (reset && state.prev_reset_trigger <= 0.0f && reset[i] > 0.0f) {
          state.held_value = start ? start[i] : 0.0f;
        } else if (state.prev_trigger <= 0.0f && trig[i] > 0.0f) {
          state.held_value = state.held_value + 1.0f;
        }
        state.prev_trigger = trig[i];
        if (reset) state.prev_reset_trigger = reset[i];
        out[i] = state.held_value;
      }
      break;
    }
  }
}
```

`SAHState` (in `cedar/include/cedar/opcodes/dsp_state.hpp:102`) is renamed to `EdgeState` and gains a `prev_reset_trigger` field (used only in counter mode). Field names match the existing `held_value` / `prev_trigger` convention to minimize the rename sweep:

```cpp
struct EdgeState {
  float held_value = 0.0f;
  float prev_trigger = 0.0f;
  float prev_reset_trigger = 0.0f;  // new — counter mode only
};
```

### 5.3 UFCS method-call codegen

Replace the `E113` stub at `akkado/src/codegen.cpp:1513-1515` with a desugaring pass:

```cpp
case NodeType::MethodCall: {
  // Children: [receiver, arg1, arg2, ...] (per parser.cpp:1004-1022)
  // Method name is stored in node data as IdentifierData (parser.cpp:1005)
  // Desugar to: name(receiver, arg1, arg2, ...)
  //
  // Implementation: rewrite as if the user had written a Call node
  // with the method name as the callee and receiver prepended to args.
  return visit_call(ast_, node, /*callee_name=*/n.identifier_data(), /*prepend_receiver=*/true);
}
```

Existing call codegen (`visit_call`) already handles both builtin and closure callees uniformly — the only thing missing is the receiver-prepending entry point, which is straightforward.

If the named callable doesn't exist in scope, raise a clear error: `"E113: Method '{name}' not found — '{name}' must be a builtin or closure in scope"`.

### 5.4 How the userspace `step` works end-to-end

User code:

```akkado
step = (arr, trig) -> arr[wrap(counter(trig), 0, len(arr))]
[60, 64, 67, 72].step(trigger(4))
```

Compile flow:

1. `step = (arr, trig) -> arr[wrap(counter(trig), 0, len(arr))]` defines a closure in the symbol table.
2. `[60, 64, 67, 72].step(trigger(4))` parses as a MethodCall on an ArrayLit.
3. UFCS desugar (§5.3): `step([60,64,67,72], trigger(4))`.
4. Closure expansion: codegen visits the closure body with `arr` bound to the array buffer and `trig` bound to the trigger buffer.
5. `counter(trig)` resolves to `EDGE_OP rate=3 inputs=[trig, ∅, ∅]`. Its `state_id` is hashed from the AST path of the `counter` call inside this specific call site of `step` — so a second `step(...)` invocation with a different array gets a different counter slot.
6. `len(arr)` lowers to `PUSH_CONST` with the compile-time array length (existing builtin behavior).
7. `wrap(counter, 0, len)` lowers to `WRAP` (`Opcode::WRAP = 47`).
8. `arr[...]` lowers to `ARRAY_INDEX` (`inst.rate = 0`, the default wrap mode — but at this point the index is already in `[0, len)`, so the wrap is a no-op).

No language-level state-tracking is needed for the closure body — the `counter` builtin (via EDGE_OP) carries the persistent state. For the `step_dir` variant, the state cell created via `state(0)` is what carries the index across blocks via `STATE_OP`.

---

## 6. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| `cedar` opcode list (`cedar/include/cedar/vm/instruction.hpp`) | **Modified** | `SAH` → `EDGE_OP` rename (value 55, line 63). New `STATE_OP` entry. |
| `SAH` opcode handler (`cedar/include/cedar/opcodes/utility.hpp:185`) | **Moved + extended** | Moved to new `edge_op.hpp` as `op_edge` with multi-mode dispatch on `inst.rate` |
| `STATE_OP` handler | **New** | New file `cedar/include/cedar/opcodes/state_op.hpp` with mode dispatch on `inst.rate` |
| Opcode dispatch switch (`cedar/src/vm/vm.cpp:484`) | **Modified** | Update `case Opcode::SAH` → `case Opcode::EDGE_OP`; add `case Opcode::STATE_OP` |
| `SAHState` (`cedar/include/cedar/opcodes/dsp_state.hpp:102`) | **Renamed** | → `EdgeState`, gains `prev_reset_trigger` field. Existing fields keep names: `held_value`, `prev_trigger` |
| `CellState` (in same `dsp_state.hpp`) | **New** | Added to state-pool variant |
| Existing `sah` builtin (`builtins.hpp:603`) | **Stays** | Lowers to `EDGE_OP rate=0` — bytecode-compatible |
| `gateup`, `gatedown`, `counter` builtins | **New** | New entries in `akkado/include/akkado/builtins.hpp` |
| `state`, `get`, `set` builtins | **New** | New entries in `builtins.hpp`, codegen special-cased by name in `codegen.cpp` (same pattern as `len`). Names reserved — cannot be shadowed. |
| `TypedValue` / `ValueType` (`akkado/include/akkado/typed_value.hpp:13`) | **Modified** | Add `ValueType::StateCell` and `cell_state_id` field |
| MethodCall codegen (`akkado/src/codegen.cpp:1513`) | **Modified** | E113 stub → UFCS desugar |
| Auto-generated opcode metadata | **Regenerated** | Run `cd web && bun run build:opcodes` after rename + STATE_OP addition. Script: `web/scripts/generate-opcode-metadata.ts` |
| `web/wasm/nkido_wasm.cpp` | **Stays** | Uses generated opcode metadata; rebuild only |
| `tools/nkido/bytecode_dump.cpp` | **Stays** | Same |
| Existing tests | **Modified (mechanical)** | Any test referencing `Opcode::SAH` or `SAHState` by name needs the rename. Behavior tests pass unchanged. |
| Existing patches (`web/static/patches/*.akk`) | **Stays** | `sah(in, trig)` still works |
| Documentation (`web/static/docs/`) | **Modified** | New entries for state cells, gateup, gatedown, counter, methods |

---

## 7. File-Level Changes

### Modify

| File | Change |
|---|---|
| `cedar/include/cedar/vm/instruction.hpp` | Rename `SAH = 55` → `EDGE_OP = 55` (line 63). Add new `STATE_OP` enum entry (next free slot). |
| `cedar/include/cedar/opcodes/utility.hpp` | Remove `op_sah` (line 185), move into new `edge_op.hpp` |
| `cedar/include/cedar/opcodes/dsp_state.hpp` | Rename `SAHState` → `EdgeState` (line 102); add `prev_reset_trigger`. Add `CellState` struct. Register both in state-pool variant. |
| `cedar/src/vm/vm.cpp` | Main switch at line 484: `case Opcode::SAH` → `case Opcode::EDGE_OP`; add `case Opcode::STATE_OP` |
| `akkado/include/akkado/builtins.hpp` | Update `sah` entry (line 603) to use `EDGE_OP rate=0`; add `gateup` (rate=1), `gatedown` (rate=2), `counter` (rate=3), `state`, `get`, `set` entries |
| `akkado/include/akkado/typed_value.hpp` | Add `ValueType::StateCell` to enum (line 13-22); add `cell_state_id` field to `TypedValue` struct |
| `akkado/src/codegen.cpp` | Replace MethodCall E113 stub (lines 1513-1515) with UFCS desugar; add StateCell-aware codegen branches for `state`/`get`/`set` (same dispatch pattern as the existing `len` special case) |
| `akkado/src/parser.cpp` | Reserve `state`/`get`/`set` as identifiers — error if user tries to bind them as closure names |
| `web/static/docs/reference/builtins/` | New docs pages for state cells, edge primitives, counter |

### Create

| File | Purpose |
|---|---|
| `cedar/include/cedar/opcodes/edge_op.hpp` | New home for `op_edge` (the renamed/extended SAH handler) |
| `cedar/include/cedar/opcodes/state_op.hpp` | New home for `op_state` (init/load/store via `inst.rate`) |
| `web/static/docs/reference/builtins/state.md` | Reference for `state`, `get`, `set` |
| `web/static/docs/reference/builtins/edge.md` | Reference for `gateup`, `gatedown`, `counter` |
| `web/static/docs/reference/language/methods.md` | Reference for UFCS method calls |
| `web/static/patches/stepper-demo.akk` | Demo patch using userspace `step` |
| `akkado/tests/test_methods.cpp` | UFCS regression tests |
| `akkado/tests/test_state.cpp` | State cell semantics tests |
| `cedar/tests/test_edge_op.cpp` | Multi-mode EDGE_OP tests (replacing or augmenting `test_op_sah`) |
| `cedar/tests/test_state_op.cpp` | STATE_OP init/load/store tests |
| `experiments/test_op_edge.py` | Python opcode test for all four EDGE_OP modes |
| `experiments/test_op_state.py` | Python opcode test for STATE_OP modes |

### Stays — explicitly no change

| File | Reason |
|---|---|
| `cedar/include/cedar/opcodes/sequencing.hpp` | TRIGGER, EUCLID, etc. unaffected |
| `web/static/patches/chord-stab.akk` | No SAH usage |
| Any patch using `sah(in, trig)` | Continues to compile and run identically |

---

## 8. Implementation Phases

### Phase 1 — EDGE_OP rename and modes

**Status**: TODO
**Goal**: Land the opcode rename and three new modes with no other language changes. Existing `sah` continues to work.

Files to modify:
- `cedar/include/cedar/vm/instruction.hpp` — rename enum (line 63: `SAH = 55` → `EDGE_OP = 55`)
- `cedar/include/cedar/opcodes/utility.hpp` — remove `op_sah` (line 185)
- `cedar/include/cedar/opcodes/edge_op.hpp` — new, contains `op_edge` with mode dispatch
- `cedar/include/cedar/opcodes/dsp_state.hpp` — `SAHState` → `EdgeState` (line 102), add `prev_reset_trigger`, register in state-pool variant
- `cedar/src/vm/vm.cpp` — main switch (line 484): `case Opcode::SAH` → `case Opcode::EDGE_OP`
- `akkado/include/akkado/builtins.hpp` — keep `sah` (rate=0, line 603); add `gateup` (rate=1, 1 input), `gatedown` (rate=2, 1 input), `counter` (rate=3, 1+2 optional inputs)
- Any cedar test referencing `Opcode::SAH` or `SAHState` directly — mechanical rename

- [ ] Rename opcode enum and state struct
- [ ] Implement multi-mode handler with the four cases
- [ ] Update builtin table entries
- [ ] Run `cd web && bun run build:opcodes` to regenerate metadata
- [ ] All existing tests pass with no test code changes
- [ ] New cedar tests for each EDGE_OP mode
- [ ] New `experiments/test_op_edge.py` covering all four modes

### Phase 2 — UFCS method-call codegen

**Status**: TODO
**Goal**: Replace the E113 stub at `codegen.cpp:1513` with a working UFCS desugar.

Files to modify:
- `akkado/src/codegen.cpp` — MethodCall case

- [ ] Implement desugar: rewrite MethodCall as Call with receiver prepended
- [ ] Resolve callee in current scope (builtins + closures)
- [ ] Helpful error when callee is missing
- [ ] Tests: `arr.lp(1200) ≡ lp(arr, 1200)`, `val.my_closure(arg) ≡ my_closure(val, arg)`
- [ ] Mixed pipe + method tests: `sig.lp(1200) |> hp(%, 200)` — verify both lower correctly
- [ ] Update `web/static/docs/reference/language/methods.md`

### Phase 3 — State cells (`state`, `get`, `set`) + STATE_OP

**Status**: TODO
**Goal**: Add the `StateCell` value type, the new `STATE_OP` opcode (init/load/store via mode dispatch), and the three name-reserved builtins.

Files to modify:
- `cedar/include/cedar/vm/instruction.hpp` — add `STATE_OP` enum entry
- `cedar/include/cedar/opcodes/state_op.hpp` — new file with `op_state` mode dispatch (init/load/store)
- `cedar/include/cedar/opcodes/dsp_state.hpp` — add `CellState { float value; bool initialized; }`, register in state-pool variant
- `cedar/src/vm/vm.cpp` — add `case Opcode::STATE_OP` to main switch
- `akkado/include/akkado/typed_value.hpp` — `ValueType::StateCell` + `cell_state_id` field
- `akkado/include/akkado/builtins.hpp` — `state`, `get`, `set` entries (codegen dispatched by name)
- `akkado/src/codegen.cpp` — name-dispatched branches for `state`/`get`/`set` in the call handler (same pattern as existing `len` special case)
- `akkado/src/parser.cpp` — reserve `state`/`get`/`set` as identifiers (cannot be bound by user closures)

- [ ] `STATE_OP` opcode added; auto-generated metadata regenerated (`bun run build:opcodes`)
- [ ] `CellState` registered in state-pool
- [ ] `state(init)` emits `STATE_OP rate=0` and returns a `StateCell` TypedValue with hashed `cell_state_id`
- [ ] `s.get()` desugars (via Phase 2 UFCS) to `get(s)` and emits `STATE_OP rate=1`
- [ ] `s.set(v)` desugars to `set(s, v)` and emits `STATE_OP rate=2`
- [ ] Multiple `state(...)` call sites get distinct slots
- [ ] Closure invocations at different AST positions get distinct slots
- [ ] Type errors on `get`/`set` of non-StateCell args
- [ ] Reserved-name error: defining a closure named `state` / `get` / `set` produces a clear parse error
- [ ] Hot-swap: cell value preserved when AST position unchanged, even if the `init` literal changed (see §9)
- [ ] Tests in `akkado/tests/test_state.cpp` and `cedar/tests/test_state_op.cpp`
- [ ] Update docs

### Phase 4 — Userspace `step` family + demo patch

**Status**: TODO
**Goal**: Validate the primitives by writing all four `step` variants in `.akk` and shipping a demo patch.

Files to create:
- `web/static/patches/stepper-demo.akk` — uses `step` and `step_dir`
- `web/static/docs/tutorials/` — tutorial entry on writing stateful operators

- [ ] All four `step` variants compile and produce correct output (verified by ear and by `experiments/`)
- [ ] Demo patch loads and runs in the web UI
- [ ] Docs link `step` examples to `state`/`counter`/`gateup` reference pages

### Future Work (out of scope for this PRD)

- State cells holding arrays or records (currently float-only)
- Reactive observers on state cells (`s.on_change(...)`)
- A `static` keyword as syntactic sugar over a hidden state cell, if state cells prove ergonomically heavy
- Method-style chaining for envelopes: `gate.adsr(...).vca(sig)`
- A `change(sig)` builtin (rising or falling) — trivial as EDGE_OP mode 4 if needed

---

## 9. Edge Cases

| Situation | Expected behavior | Rationale |
|---|---|---|
| `counter` reset and trig fire on the same sample | Reset wins — held value becomes `start`, increment skipped that sample | Predictable; matches typical hardware-counter behavior |
| `gateup(constant)` where constant is non-zero | Outputs 1.0 only on the very first sample after compile (rising edge from initial 0) | Matches the established `prev <= 0 && curr > 0` convention; not a special case |
| Empty array: `[].step(trig)` | `wrap(x, 0, 0)` — runtime divides by zero internally; `ARRAY_INDEX` falls back to `out = 0.0` (`arrays.hpp:44-47`) | `len = 0` is degenerate; user gets silence rather than a crash |
| `state(init)` called inside a closure called twice in a patch | Each call site gets its own slot (different semantic-ID paths) | Matches existing builtin state_id behavior; multiple steppers don't share state |
| Same `state(...)` AST position re-visited via recursion | Same slot — recursion shares state | Matches state-pool semantics; if user wants distinct state, they call `state()` from a closure factory |
| `s.set(stream)` where `stream` varies sample-by-sample | The latest sample whose value differs from the slot value at the start of the block is written. For uniformly-varying streams this is the last sample (preserving the original "last sample wins" mental model); for gated writes that return to the slot's prior value mid-block (e.g. `select(gateup(t), idx+dir, idx)`), the rising-edge value is captured. | State cells are scalar registers; the start-of-block snapshot ensures gated writes are not silently dropped when the post-edge samples re-assert the prior slot value. See `docs/reports/2026-04-26-stepper-demo-array-and-state-bugs.md`. |
| `s.get()` before any `set()` | Returns the `init` value passed to `state(...)` | `CellState.initialized` is `false` on first `STATE_OP rate=0`, which writes `init` and flips the flag |
| Hot-swap: code reloaded with same closure structure | Cell slots preserved by semantic-ID hashing | Same as builtin state preservation |
| Hot-swap: `step` closure body changes (e.g., new operations) | Existing slot value preserved if the `state(...)` AST position is still the same | Matches existing hot-swap semantics |
| Hot-swap: `state(0)` edited to `state(5)` at the same AST position | Existing slot value preserved; the new init literal is **ignored** because `CellState.initialized` is already `true`. To force re-init, the user must move the `state(...)` to a different AST position. | Matches builtin hot-swap semantics — state survives code edits as long as the binding slot is structurally unchanged. |
| User defines a closure named `state` / `get` / `set` | Parse error: `E???: '<name>' is a reserved built-in identifier and cannot be redefined` | Avoids the trap where `s.get()` could lex-resolve to a user closure that doesn't understand StateCell args. |
| `arr.foo(args)` where `foo` is not in scope | Error: `E113: Method 'foo' not found — must be a builtin or closure in scope` | UFCS only resolves lexically |
| `arr.foo(args)` where `foo` is in scope but not callable (e.g., `foo` is a number) | Error: `E???: 'foo' is not callable` | Same error as `foo(arr, args)` would produce |
| Negative count from `counter` (impossible in mode 3 as written, but worth noting) | N/A — counter only increments. For ping-pong / decrement use `step_dir` with state cells directly | Keeps `counter` simple |
| Float overflow in `counter` after very long runs | Theoretical: `1.0f + 1.0f` per trigger; at 16th notes / 120 BPM that's ~16M+ years before float precision degrades | Not addressed in this PRD |
| `set()` used in a non-statement position | `set()` returns the new value broadcast as a buffer, so `arr[s.set(s.get()+1)]` works (advances and uses new index in one expression) | Matches expression-oriented language |

---

## 10. Open Questions

- **Q1: ~~Is `CELL_LOAD` / `CELL_STORE` truly a "no new opcode" change?~~** **Resolved.** Cell I/O ships as a single new mode-dispatched opcode `STATE_OP` (rate 0 = init, rate 1 = load, rate 2 = store). See §5.1. The PRD's "no new opcodes" goal was scoped to DSP-level opcodes; one slot-I/O opcode is the minimum and follows the existing `inst.rate` mode-dispatch pattern.

- **Q2: Should `state` accept a non-constant init?** E.g. `state(some_signal)`. The init is sampled once per slot lifetime; using a signal is meaningless for non-trivial signals (only the value at compile-evaluation time matters). Recommend: warn if `init` is not a literal or compile-time-constant expression.

- **Q3: Should `set()` return the new value or unit?** **Resolved.** Returning the value enables expression-position use (`arr[s.set(s.get()+1)]`). `set()` returns the new value broadcast as a buffer (Signal TypedValue).

- **Q4: ~~Method-call resolution order — local closure vs builtin of the same name.~~** **Resolved.** Lexical shadowing wins for normal builtins (a user closure `lp` shadows the `lp` builtin at `sig.lp(1200)`). The three cell-I/O names (`state`, `get`, `set`) are special — they are **reserved** at parse time and cannot be redefined by user closures. See Goal 6 and §9 edge cases. Rationale: silent shadowing of cell I/O would let `s.get()` route to a user closure that doesn't understand `StateCell` args, producing a confusing type error far from the cause.

---

## 11. Testing / Verification Strategy

### Phase 1 — EDGE_OP

Cedar tests in `cedar/tests/test_edge_op.cpp` (Catch2):

```
EDGE_OP rate=0 (classic SAH)
  - holds input value across trigger rising edges
  - identical bytecode-level output to old SAH tests

EDGE_OP rate=1 (gateup)
  - 0→1 transition produces single-sample 1.0 pulse
  - 1→1 produces 0.0 (no edge)
  - 1→0 produces 0.0 (falling edge, not rising)
  - hot-swap: state preserved across recompile

EDGE_OP rate=2 (gatedown)
  - mirror of gateup for falling edges

EDGE_OP rate=3 (counter)
  - increments by 1.0 on each rising edge of trig
  - resets to 0 (default) on rising edge of reset
  - resets to start value when start input is provided
  - reset wins if trig and reset fire on the same sample
  - held value persists across blocks
```

Python in `experiments/test_op_edge.py`: Run each mode for several blocks with synthetic input, save WAV outputs for human evaluation, assert against expected sample counts (e.g., "exactly 4 rising-edge pulses in this 1-second buffer").

### Phase 2 — UFCS

Akkado tests in `akkado/tests/test_methods.cpp`:

```
- arr.lp(1200) compiles to identical bytecode as lp(arr, 1200)
- val.my_closure(2) where my_closure = (a,b) -> a*b returns val*2
- chained: sig.lp(1200).hp(200) ≡ hp(lp(sig, 1200), 200)
- mixed pipe and method: sig.lp(1200) |> hp(%, 200) compiles
- error: undefined method name produces E113 with helpful message
- error: shadowing builtin with non-callable produces "not callable" error
```

### Phase 3 — State cells + STATE_OP

Cedar tests in `cedar/tests/test_state_op.cpp` (Catch2):

```
STATE_OP rate=0 (init)
  - first execution writes inst.inputs[0][0] to slot, sets initialized=true
  - subsequent executions are no-ops (slot value preserved)
  - output buffer is broadcast of slot value

STATE_OP rate=1 (load)
  - output is broadcast of slot value
  - no input reads

STATE_OP rate=2 (store)
  - last sample of inst.inputs[0] is written to slot
  - output buffer is broadcast of new slot value
  - per-block stability: store in block N is visible to load in block N+1
```

Akkado tests in `akkado/tests/test_state.cpp`:

```
- state(0).get() returns 0 on first read
- state(0).set(5).get() returns 5 (chained)
- state cell value persists across blocks
- two distinct state() call sites have independent storage
- state() inside a closure invoked at different sites has independent storage per site
- get(non_state_cell) raises type error
- set(non_state_cell, v) raises type error
- defining `state = (...) -> ...` raises a "reserved identifier" parse error
- defining `get = ...` and `set = ...` raises the same error
- hot-swap: state cell value preserved if AST position unchanged
- hot-swap: editing `state(0)` to `state(5)` at same AST position keeps existing slot value
```

### Phase 4 — Userspace `step` end-to-end

- Compile a patch using `step(arr, trig)` in the akkado
- Audition `web/static/patches/stepper-demo.akk` in the web dev server
- Verify the pattern advances on each beat
- Verify reset variant resets at the expected boundary
- Verify `step_dir` plays forward and backward correctly
- Listen for clicks/discontinuities at array boundaries (should be inaudible — wrap is sample-accurate)

### Build / lint commands

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/cedar/tests/cedar_tests "[edge_op]"
./build/cedar/tests/cedar_tests "[state_op]"
./build/akkado/tests/akkado_tests "[methods]"
./build/akkado/tests/akkado_tests "[state]"

cd experiments && uv run python test_op_edge.py
cd experiments && uv run python test_op_state.py

cd web
bun run build:opcodes      # regenerate opcode metadata after EDGE_OP rename + STATE_OP addition
bun run build:docs         # rebuild docs index after new reference pages
bun run dev                # audition stepper-demo.akk
bun run check              # type-check
```

### Acceptance criteria

- All existing behavioral tests pass. Mechanical churn allowed: any test referencing `Opcode::SAH` or `SAHState` by name needs the rename — but no test logic changes.
- Existing patches (any `.akk` using `sah(in, trig)`) compile and produce bit-identical output.
- All four `step` variants and `step_dir` are implementable and correct in `web/static/patches/stepper-demo.akk`.
- New reference docs link from the F1 lookup index (run `bun run build:docs`).
- Exactly one new DSP-level opcode (`STATE_OP`) is added, alongside the `SAH` → `EDGE_OP` rename. The decision and rationale are captured in §1 Major Design Decisions and §10 Q1.
- The `web/wasm/nkido_wasm.cpp` and `tools/nkido/bytecode_dump.cpp` builds pass after `bun run build:opcodes` regenerates `cedar/include/cedar/generated/opcode_metadata.hpp` (no source changes needed in those files).
