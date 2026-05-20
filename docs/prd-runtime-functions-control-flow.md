> **Status: L1 SHIPPED · L2 PARTIAL · L3 NOT STARTED** — Brainstorm-converged design (2026-05-17); reviewed and corrected 2026-05-20. PRs split L1 → L2 → L3 as three independent rollouts.
>
> - **L1 (Phase 1) — SHIPPED.** `when()` (commit `41bb96c`) and `loop(N){body}` (commit `9776ded`): `SKIP_IF_ZERO`/`SKIP_IF_NONZERO`/`LOOP_STATIC` opcodes + Akkado surface.
> - **L2 (Phase 2) — PARTIAL.** Shipped: the `#inline` annotation (lexer `#` token + statement-position parsing), recursion rejection (E240/E244/E246/E249), and the `BLOCK_CALL`/`RET` opcode reservations (compile-time-expansion markers; the VM hard-errors if one is reached). **Deferred to L3:** the `Subprogram` side-table and the `expand_block_calls()` machinery — under the chosen compile-time-expansion model the table has no L2 consumer and is not the runtime-resident shape L3's `FOREACH_EVENT` needs, so it is co-designed with L3.
> - **L3 (Phase 3) — NOT STARTED.**
>
> Source: design framework at `~/.claude/plans/there-is-no-harmonic-flute.md`.

# Cedar Runtime Functions, Callable Blocks & Control Flow PRD

## Executive Summary

Cedar today has **no** runtime function/closure infrastructure: no `CALL`/`RET`, no `JMP`/`JMPE`, no loops, no if/then/else. The VM is a flat sequential dispatch loop over a topo-sorted instruction list. All Akkado user `fn`s are inlined at every call site by the compiler. `POLY_BEGIN`/`POLY_END` is the *only* program-level "skip and re-execute" mechanism in the VM today and exists as a hard-coded special case.

This PRD generalizes that special case into a uniform "callable block" mechanism and adds the minimum control-flow primitives needed to unlock four user-facing capabilities the current model blocks:

1. **Code reuse / size reduction** — a shared subprogram table lets one `fn` body back many call-sites.
2. **Conditional graph topology** — forward `SKIP_IF` opcodes bypass whole instruction ranges when a control value is zero, instead of always-evaluating both branches and muxing with `SELECT`.
3. **Recursive / self-similar structures** — *deferred to a follow-up PRD*; the design admits compile-time-unrolled recursion at a later phase.
4. **Higher-order DSL** — `notes.each_voice(n => osc("sin", n.freq))` over dynamic event streams via a generalized `FOREACH_EVENT` opcode (POLY's algorithm, not its API). The `.map(...)` surface name is reserved for per-event *field rewrite* per the companion [prd-runtime-event-transforms.md](prd-runtime-event-transforms.md); see §13 for the disambiguation.

Cedar's two load-bearing invariants are preserved: **bounded audio-callback time** and **zero allocation in the audio path**. The "RT for audio, non-RT for setup" relaxation is the key enabler — allocation, state-slot expansion, and per-callsite specialization happen at hot-swap/init time; the audio callback only executes a flat instruction stream.

### Key Decisions

- **Hybrid layer model.** A few targeted VM primitives (L1 control flow + L2 subprogram table + L3 event iterator) plus compile-time work in Akkado. Not a full Turing-complete VM rewrite.
- **Buffer addressing: swap-time expansion by default.** The compiler emits `BLOCK_CALL` at every call site; a swap-prepare pass inlines the body with absolute buffer indices patched. Audio path stays bit-identical to today's flat dispatch loop.
- **Convention slots for event iterators.** `FOREACH_EVENT` (and its POLY/map/each specializations) use fixed param-buffer slots (POLY's existing 5-slot freq/gate/vel/trig/out scheme generalized) for true body sharing — no swap-time expansion, true table dispatch.
- **Shared block by default; `#inline` opts back.** `fn foo(...)` becomes a shared block; `#inline fn foo(...)` keeps the current per-site inlining. Default flips so bytecode shrinks automatically; `#inline` exists for hot-path micro-bodies where dispatch overhead matters.
- **POLY migration in same PRD.** POLY = `FOREACH_EVENT + voice allocator + BLOCK_CALL voice_body` from day one. Bit-exact behavior; no legacy code path.
- **No recursion in v1.** Recursive `fn` definitions are rejected at compile time with a clear error. Compile-time-unrolled recursion (`#max_depth(N)`) is a deliberate follow-up PRD.
- **Forward-only skips.** L1 control flow has `SKIP_IF_ZERO`/`SKIP_IF_NONZERO`/`LOOP_STATIC` only. No backward JMP, no arbitrary control flow — cycles at the instruction-stream level remain impossible.

---

## 1. Problem Statement

### 1.1 Current State Inventory

The brainstorm exploration confirmed:

| Capability | Today | File:line |
|---|---|---|
| Sample-level signal mux | `SELECT(cond, a, b)` (both branches always execute) | `cedar/include/cedar/opcodes/logic.hpp:20` |
| Comparators + logic | `CMP_GT/LT/GTE/LTE/EQ/NEQ`, `LOGIC_AND/OR/NOT` | `cedar/include/cedar/vm/instruction.hpp` (opcodes 141–149) |
| Compile-time mode dispatch | `inst.rate` baked once (LFO shape, EDGE_OP mode, CLOCK mode, INTERP_TIME mode, ARRAY_INDEX wrap/clamp) | `cedar/include/cedar/opcodes/{edge_op,state_op,sequencing,utility,arrays}.hpp` |
| Special-case "subroutine" | `POLY_BEGIN { body } POLY_END` — VM has `execute_poly_block()` that re-runs the inlined body once per active voice | `cedar/src/vm/vm.cpp:323-595`, `akkado/src/codegen_functions.cpp:1992-2285` |
| User-defined functions | Parsed by Akkado, **fully inlined** at every call site (see [prd-advanced-functions.md](prd-advanced-functions.md) — explicitly preserves "no runtime function objects, no new VM opcodes") | `akkado/src/codegen.cpp`, `akkado/src/codegen_functions.cpp` |
| Conditional logic | Sample-rate signal selection only — the [conditionals PRD](prd-conditionals-logic.md) is **DONE** (opcodes 140–149 shipped: `SELECT`, `CMP_*`, `LOGIC_*`; infix syntax + `select()` builtin) | This PRD adds **block-rate conditional bypass** (`when()`), which is *not* the same as the "compile-time if/else statements" that PRD listed in Future Work — it's orthogonal. See §7.3 for the relationship |
| State pool | Fixed 512-slot open-addressing table, FNV-1a hashed semantic IDs, pre-allocated based on compiled graph | `cedar/include/cedar/dsp/constants.hpp` (`MAX_STATES = 512`), `cedar/include/cedar/vm/state_pool.hpp` |
| VM dispatch loop | Flat `while (ip < program.size())` for-loop; the only branch is `POLY_BEGIN` → `execute_poly_block()` returns next ip | `cedar/src/vm/vm.cpp:313-322` |

### 1.2 Concrete Limitations

| User wants… | Today's workaround | Cost |
|---|---|---|
| A reusable `fn` called from N sites | Per-site re-inlining | Bytecode and i-cache pressure grow linearly with N; state slot count multiplies; large patches blow past `MAX_STATES = 512` |
| Bypass an effect chain when a toggle is off | `SELECT(toggle, sig * 0, sig \|> reverb(@))` | The reverb runs every block regardless; CPU not saved |
| Tree-like / nested allpass / fractal feedback | Manually unroll, copy-paste body | Unreadable; state slot count explodes; depth must be statically chosen by hand |
| `notes.each_voice(n => osc("sin", n.freq))` over a runtime event stream | Manually expand into a fixed-size POLY block | API doesn't compose with arbitrary higher-order operators |

### 1.3 Why Now

`POLY_BEGIN` already is, structurally, a hard-coded subroutine call: header marker → inlined body → `POLY_END` resumption point, with `execute_poly_block()` acting as the caller. The mechanism is half-built. Generalizing it costs much less than building it from scratch and unifies four otherwise-disconnected wishlist items behind one mental model.

---

## 2. Goals & Non-Goals

### Goals

- **Audio callback stays bounded-time and allocation-free** post-shipping. A 300-second simulated render shows zero allocation events on the audio thread.
- **L1**: Conditional bypass measurably cuts CPU vs the `SELECT`-equivalent when the condition is false (`when(mute, @ * 0, @ |> reverb(@))` benchmark).
- **L2**: A user `fn` called from N sites produces audio bit-exact to the current per-site-inlined behavior, and pre-expansion bytecode is measurably smaller than current.
- **L2**: Hot-swap state preservation works per-call-site via the existing semantic-path scheme extended with a `block@callsite_N` component.
- **L3**: POLY reimplemented as `FOREACH_EVENT + voice allocator + BLOCK_CALL voice_body` is bit-exact to today's POLY across all existing `akkado/tests/` cases.
- **L3**: `n"…".each_voice(n => osc(...))` renders correctly in both `nkido-cli render` and the web UI, with state preserved across hot-swap.
- **No new audio-thread allocations.** Every new state slot, every new block-table entry, every voice-pool entry is allocated at swap-prepare or init time only.

### Non-Goals

- **True recursion.** Rejected at compile time in v1 with `E240: recursive fn '<name>' not supported — see follow-up PRD`. Compile-time-unrolled recursion (`#max_depth(N)`) is a deliberate follow-up.
- **Frame-relative addressing.** Defer until a use case can't be served by swap-time expansion or convention slots. (Sketched in §5.3 as the "if we ever need it" option.)
- **First-class fn values escaping the compile unit.** Block-refs in v1 are restricted: capture-only-by-buffer-index-and-constant, consumed by stdlib higher-order operators, do not survive across hot-swap as values.
- **Backward JMP / arbitrary control flow.** Forward skips only. No cycles in the instruction stream.
- **Run-time growth of state pool, subprogram table, or voice pool.** All sizing finalized at swap-prepare time.
- **`inst.rate` runtime dispatch.** This PRD does *not* repurpose `inst.rate` for runtime mode selection. It stays as the canonical channel for compile-time mode dispatch (see [extended-params-mechanism.md](extended-params-mechanism.md) §5).
- **Removing `SELECT` or the existing sample-rate logic operators.** They remain the right tool for per-sample signal muxing; L1 conditional-skip is the right tool for block-rate program-flow conditionals.

### 2.1 What Stays vs What Changes

| Subsystem | Stays the same | Changes |
|---|---|---|
| VM dispatch loop (`vm.cpp:313-322`) | Flat `while (ip < program.size())` for-loop | Adds skip-handling for `SKIP_IF_*` and `LOOP_STATIC`; adds `BLOCK_CALL` (pre-expansion only; post-swap the audio path sees zero) and `FOREACH_EVENT` dispatch into subprogram table |
| `execute_poly_block()` (`vm.cpp:323-595`) | Algorithm (voice allocation, per-voice body re-execution, voice mixing) | Renamed to `execute_foreach_event()` with `VOICE_POOL` allocator branch; `PER_ITERATION` and `SHARED` branches added |
| State pool (`state_pool.hpp`) | Fixed-size open-addressing table, FNV-1a hashed semantic IDs, pre-allocated at swap-prepare | No structural change; `MAX_STATES` re-audited (currently 512) |
| Path-hash scheme (`codegen.hpp:354-355`, `codegen.cpp`) | `push_path()`/`pop_path()` mechanism, FNV-1a hash, `funcname#N` per-call counter | Adds `block:<name>@callsite_<N>` component for `BLOCK_CALL` sites; adds `foreach:<name>@callsite_<N>/iter_<voice_slot>` for `FOREACH_EVENT` iterations |
| Triple-buffer hot-swap + micro-crossfade ([cedar-vm-hot-swap-implementation.md](cedar-vm-hot-swap-implementation.md)) | Atomic pointer swap at block boundaries, semantic-ID rebinding, 5–10ms crossfade | Unchanged — block bodies hash into the same path scheme |
| Bytecode wire format | Header, main program, state inits | Adds `Subprograms` side-table (§8); old programs with no `fn`s remain backward-compatible |
| User-fn machinery (`codegen_functions.cpp`) | Front-end parsing of `fn`, parameter typing, signature handling | Backend emission switches from per-site inlining to `Subprogram` + `BLOCK_CALL`; per-site inlining retained behind `#inline` annotation |
| POLY user-facing API | `poly(events, instr, max_voices)` signature, instrument 3-arg contract `(freq, gate, vel)`, stereo support | Bit-exact behavior; internally dispatched through `FOREACH_EVENT(VOICE_POOL)` instead of `POLY_BEGIN`/`POLY_END`. `POLY_BEGIN`/`POLY_END` opcodes retained as no-op aliases for one release cycle, then removed |
| `SELECT` and sample-rate logic operators (opcodes 140–149, `prd-conditionals-logic.md`) | All shipped behavior | Unchanged — `when()` is additive, not a replacement |
| `inst.rate` policy ([extended-params-mechanism.md](extended-params-mechanism.md) §5) | Compile-time mode dispatch only; no runtime bit-packing | Unchanged — new opcodes follow the rule (FOREACH_EVENT `allocator_kind` is a compile-time enum ≤4 values, the legitimate `rate` use) |
| Audio-path allocation invariant | Zero runtime allocations on audio thread | Unchanged — every new state slot, subprogram-table entry, voice-pool entry sized at swap-prepare time |
| `nkido-cli`, `bytecode_dump.cpp`, web bytecode disassembler | Existing disassembly UX | Extended to render `Subprogram` side-table and toggle expanded/unexpanded views (Phase 2 UX detail) |
| Akkado lexer | Existing tokens | Adds `#` as annotation-prefix lexeme |
| Akkado parser | Existing precedence table, expression-vs-statement positions | Adds annotation parsing at statement position; adds `loop()` contextual keyword + block-expression form (see §7.6) |

---

## 3. Mental Model: POLY_BEGIN Generalized

The unifying insight: **`POLY_BEGIN` is already a de-facto subroutine call.**

Today's encoding (`cedar/src/vm/vm.cpp:313-322`):

```
[POLY_BEGIN rate=body_len, state_id=X]   ← header marker; body_len = compile-time-known
[body op 1]                               ← inlined body
[body op 2]
...
[body op N]
[POLY_END]
[next op]                                 ← VM resumes here after voice loop completes
```

`execute_poly_block(program, ip)` (`vm.cpp:323-595`) is structurally:

```
1. read event source (SequenceState or MidiQueueState) via state_id_X
2. for each gate-on event: allocate a voice (lifecycle: allocate on gate-on, release on gate-off)
3. for each active voice:
     bind voice's per-voice buffers into the param slots (freq, gate, vel, trig, out)
     re-run instructions program[ip+1 .. ip+body_len] in the VM
4. mix all voice outputs into the POLY block's output buffer
5. return ip + body_len + 2  (skip past POLY_END)
```

The PRD generalizes this into:

- A **subprogram table** — a side-array of (block_id → instruction_list) separate from the main program.
- A `BLOCK_CALL block_id, frame_id, state_id_prefix` opcode that dispatches into a subprogram.
- A `RET` opcode terminating each subprogram body.
- **Specialized callers** — different policies for *how* and *how many times* a block gets invoked:
  - `SKIP_IF_ZERO buf, offset` — call zero or one time depending on a predicate (well, "skip" not "call", but the semantic is the same: conditional execution).
  - `LOOP_STATIC count, body_len` — call N times in sequence.
  - `FOREACH_EVENT event_src, block_id, allocator_kind` — call once per event/voice with per-iteration state.
- **POLY = FOREACH_EVENT + voice allocator + BLOCK_CALL voice_body**, no longer a special-case opcode.

Audio-path execution stays the same flat for-loop dispatch it is today; calls are either pre-resolved at swap time into inlined sequences (L2 default) or dispatched through the subprogram table at runtime (L3 event iterators).

---

## 4. Layered Design

The three layers are designed to be **independently shippable** (one PR per layer). L2 depends on L1 only in the sense that L1 ships first; L3 depends on L2's subprogram table machinery.

### 4.1 Layer 1 — Forward Control Flow Primitives

Three new opcodes. All are **forward-only**: they shift `ip` ahead, never back. Topological sort and the DAG model are unaffected.

| Opcode | Encoding | Semantics |
|---|---|---|
| `SKIP_IF_ZERO` | `inputs[0] = predicate_buf`, `rate = offset` (uint8) or `inputs[1] = offset_const_buf` for `offset > 255` | If `predicate_buf[0] == 0.0f` at block entry, advance `ip` by `offset + 1`; else execute next instruction normally |
| `SKIP_IF_NONZERO` | symmetric | dual |
| `LOOP_STATIC` | `rate = body_len` (uint8); count carried in `StateInitData::LoopStaticCount { count: u32 }` entry following the instruction (compile-time constant, not a buffer index) | Execute the next `body_len` instructions `count` times in sequence. Each iteration sees fresh state-pool reads (loop body is *not* a separate frame; state is shared across iterations — caveat for stateful opcodes inside a loop) |

**Predicate is sampled once per block** — at `program[ip].inputs[0][0]`. This is the cheapest possible model and aligns with how `inst.rate`-style mode selection works today (per-block, not per-sample). User-visible semantics in Akkado:

```akkado
// L1: block-rate conditional bypass
sig |> when(mute_toggle, @ * 0, @ |> reverb(@))
// codegen:
//   [SKIP_IF_NONZERO mute_toggle, offset_to_else]
//   [body for `@ |> reverb(@)` branch]
//   [SKIP forward to merge]
//   [body for `@ * 0` branch]
//   [merge: assign output buffer]
```

A new builtin `when(cond, true_branch, false_branch)` provides the Akkado surface.

**Relationship to `select()` and "compile-time if/else":** three distinct concepts, each with its own scope.

| Construct | Status | Rate | Both branches eval? | Use case |
|---|---|---|---|---|
| `select(cond, a, b)` (also infix `?:` deferred) | **DONE** (`prd-conditionals-logic.md`) | sample | yes | per-sample modulation, smooth crossfades |
| `when(cond, a, b)` | **THIS PRD** (L1) | block | no — only taken branch runs | mute-bypass, A/B routing, expensive-effect gating |
| Compile-time `if/else` (constant-folded) | **FUTURE** (per `prd-conditionals-logic.md` §Future Work) | n/a | n/a (resolved at compile time) | array operations, conditional codegen, constant-folded patches |

`when()` is orthogonal to the deferred compile-time if/else: that one resolves at codegen, eliminating one branch entirely from bytecode; `when()` keeps both branches in bytecode but uses `SKIP_IF_*` opcodes to skip the not-taken branch at runtime. Users who want behavior closest to "C-style if/else" today should reach for `select()` (always evaluates both — the right choice when both branches are stateful or cheap) or `when()` (skips at block rate — the right choice when one branch is expensive and rarely taken).

**Caveats documented in user-facing docs:**
- `when()` decides at *block* boundaries (every 128 samples / 2.67ms). Not suitable for per-sample switching.
- State inside the not-taken branch is *not* advanced. If a reverb is in the false branch and the condition flips back, its tail picks up where it left off — usually the desired behavior, but worth flagging.
- `LOOP_STATIC` shares state across iterations; do not use it to instantiate N independent stateful UGens (that's what L3 `each` is for).

**Edge-case behavior (explicit):**

The non-trivial cases need committed semantics, not just docs caveats:

| Case | Behavior in v1 |
|---|---|
| **(a) Feedback / decay state in skipped branch** (e.g., a reverb tail in the `false` branch when `cond` flips to `true`) | Reverb tail frozen — does *not* ring out while skipped. When `cond` flips back, tail resumes from its frozen sample. Documented as user-visible behavior; flagged in `when()` doc as "use `select()` if you need the tail to decay regardless". |
| **(b) Sequencer / pattern state in skipped branch** | Pattern state (e.g., a pattern literal inside the skipped branch) is *not* advanced — events for that block are lost. Cycle position resumes from its frozen value on un-skip. This is the same model as (a): skipped subgraphs are frozen, not silenced. Documented in `when()` doc as "patterns inside `when()` lose events while bypassed — pattern logic that needs to keep ticking belongs outside the bypass". |
| **(c) Hot-swap mid-skip** | Skipped state still participates in semantic-ID rebinding. The next-compile's matching subgraph receives the frozen state — its DSP picks up where the pre-swap version left off, even though the swap happened during a block when it wasn't running. Falls out of the existing path-hash scheme; no new mechanism required. |
| **(d) Chained `when(a, when(b, …))`** | Skips compose: the outer `SKIP_IF_*` advances `ip` past the entire inner construct (offset is computed at codegen). No special handling at runtime. Branches are independent — entering the outer's false branch does not implicitly enter the inner's false branch. |

### 4.2 Layer 2 — Subprogram Table & BLOCK_CALL

**Bytecode model additions:**

> **Codebase note.** There is no `struct Bytecode` today. The runtime program
> form is `ProgramSlot` (`cedar/include/cedar/vm/program_slot.hpp`) — a
> cache-line-aligned struct holding a **fixed** `std::array<Instruction,
> MAX_PROGRAM_SIZE>` plus a `std::array<std::uint32_t, MAX_STATES>` of state
> IDs, loaded via `ProgramSlot::load(std::span<const Instruction>)` from a flat
> instruction span. There is no header, no magic bytes, and no side-table in
> the current format. The struct below is therefore a **proposed new wire-format
> type**, not an extension of something existing — the `// existing` notes mean
> "conceptually present today as the flat instruction stream / state-init list",
> not "a field on an existing `Bytecode` struct". How a variable-length
> `Subprogram` side-table coexists with the fixed-array `ProgramSlot` runtime
> model is an open design question — see §11.

```cpp
// PROPOSED — new type. Today's equivalent is the flat instruction span
// consumed by ProgramSlot::load() (program_slot.hpp).
struct Bytecode {
    std::vector<Instruction> main;        // today: the flat instruction span
    std::vector<StateInit>   state_inits; // today: the state-init list
    // NEW:
    std::vector<Subprogram>  blocks;      // side-table of callable blocks
};

struct Subprogram {
    BlockId                   id;             // stable hash of block source identity
    std::vector<Instruction>  body;           // topo-sorted, ends with RET
    FrameDescriptor           frame;          // logical-slot → buffer-binding template
    std::vector<BlockId>      callees;        // for swap-time call-graph walk
};

struct FrameDescriptor {
    uint8_t param_count;                       // number of logical input slots
    uint8_t local_buf_count;                   // intermediate buffers the body needs
    uint8_t output_count;                      // number of logical output slots
};
```

**New opcodes:**

| Opcode | Encoding | Semantics |
|---|---|---|
| `BLOCK_CALL` | `inputs[0..4] = bound buffer indices for first 5 logical slots`; `output = output_buf`; `state_id = callsite_disambiguator`; `block_id` carried via following `StateInitData::BlockCallTarget { block_id: u32 }` entry (NOT bit-packed into `rate`). `rate` reserved for compile-time mode dispatch per CLAUDE.md §'Extended Parameter Patterns' | (Pre-swap-expansion) Marker for swap-time expansion. (Post-expansion) replaced by an inlined copy of the body |
| `BLOCK_BIND` | `rate = slot_index` (uint8, range 5..255); `inputs[0] = buf_idx`; emitted as a leading sequence before `BLOCK_CALL` when a call site needs to bind logical slots beyond `inputs[0..4]` | Bind one additional logical input slot to a buffer before the immediately following `BLOCK_CALL`. Consumed and erased by `expand_block_calls()`. Multiple `BLOCK_BIND`s precede a single `BLOCK_CALL`; order doesn't matter (slot index is explicit) |
| `RET` | no operands | Terminates a subprogram body. Required at end of every `Subprogram::body` |

**Compile-time (Akkado):**

1. Lower every `fn` definition (non-`#inline`) into a `Subprogram` entry. The fn's parameters become logical slots 0..N; its return value becomes the output slot.
2. At each call-site, emit `BLOCK_CALL` with the bound buffer indices from the call's argument expressions.
3. For call-sites with more than 5 inputs (beyond the `inputs[0..4]` slot bank), emit a leading sequence of `BLOCK_BIND` opcodes — one per slot 5..N, each carrying `rate = slot_index` and `inputs[0] = buf_idx` — immediately before the `BLOCK_CALL`. The body reads slot K via the convention layout established by the `FrameDescriptor`. (Most user fns fit in 5; this is a rare path.) The `expand_block_calls()` pass consumes both the `BLOCK_BIND` prefix and the `BLOCK_CALL` itself in a single rewrite step.

**Swap-time expansion (default addressing strategy):**

A new pass `expand_block_calls(bytecode)` runs in the swap-prepare phase (before bytecode handoff to the audio thread):

```
for each BLOCK_CALL instruction in main:
    look up Subprogram body
    rewrite the body's buffer indices: logical slot K → actual buffer bound at this call site
    rewrite state IDs: add "block@callsite_N" component to the semantic path
    splice the body in-place, replacing the BLOCK_CALL
    (RET instruction is dropped)
```

After this pass, `bytecode.main` looks identical to today's per-site-inlined output. The audio thread never sees a `BLOCK_CALL`. The `bytecode.blocks` table is retained only for L3 `FOREACH_EVENT` dispatch and for debug introspection — for purely L2 use it becomes dead weight at audio time (acceptable: the table is small, lives outside the hot loop).

**State IDs with shared blocks:**

Today (`akkado/include/akkado/codegen.hpp:351`, `akkado/src/codegen.cpp`): semantic path is built by `push_path()` / `pop_path()` during codegen; state IDs are FNV-1a hashes of the path. For function calls today, the path component is `"funcname#N"` where N is a per-call counter (`codegen_functions.cpp:1056-1057`).

Extension: at each `BLOCK_CALL` site, the call-site path component is `"block:foo@callsite_N"` (or any stable scheme — the only constraint is *stability across compiles when source is unchanged*). After swap-time expansion, each call-site's expanded body sees state IDs derived from that path. Result: per-call-site state slots, fully recoverable across hot-swap as long as source identity is preserved.

**Why expansion instead of true dispatch?**

| Factor | Swap-time expansion (chosen) | True dispatch (deferred) |
|---|---|---|
| Audio-path complexity | Zero change — same flat loop | Adds return-IP stack, BLOCK_CALL dispatch overhead |
| Bytecode size | Same as today's inlining | Smaller (body stored once) |
| State slot count | Same as today's inlining | Same (state IDs are per-call-site either way) |
| Cache locality | Identical to today (body inlined at use site) | Cold call into subprogram table |
| Debug story | Standard disassembly works | Need to render subprogram table separately |

For L2, expansion is strictly better than dispatch. We pay the bytecode-size cost (which today's inlining already pays) and gain zero audio-path complexity. L3 uses true dispatch only where it earns its keep (per-voice / per-event repetition where the body genuinely runs N times).

### 4.3 Layer 3 — Event Iteration & Higher-Order DSL

#### 4.3.1 `FOREACH_EVENT`

```
FOREACH_EVENT event_src_state_id, allocator_kind, output
  + StateInitData::ForeachConfig { block_id: u32, max_iterations: u16, frame_descriptor_id: u16 }
```

Encoding (slot layout, per CLAUDE.md §'Extended Parameter Patterns' — no bit-packing of runtime-tunable values into `inst.rate`):

| Field | Type | Carried in | Meaning |
|---|---|---|---|
| `event_src_state_id` | uint16 | `state_id` (the opcode's own state_id slot serves dual purpose: identifies this FOREACH instance, and the upstream is resolved via its own state_id stored in `StateInitData`) | State ID of upstream `SequenceState` / `MidiQueueState` / `ArrayState` |
| `allocator_kind` | uint8 (enum, compile-time, ≤4 values) | `inst.rate` (legitimate use per CLAUDE.md: "small fixed enum modes ≤4 values") | `VOICE_POOL` (POLY semantics: gate-on allocates, gate-off releases, multiple events overlap), `PER_ITERATION` (each event gets a fresh state slice, released at end of block), `SHARED` (all iterations share state — e.g., a fold accumulator) |
| `output` | uint16 | `inst.output` | Mix bus for all iterations |
| `block_id` | uint32 | `StateInitData::ForeachConfig` entry following the instruction | Subprogram to invoke per event |
| `max_iterations` | uint16 | `StateInitData::ForeachConfig` | Pre-sized cap on concurrent voices/iterations (used to pre-allocate state slots at swap-prepare time) |
| `frame_descriptor_id` | uint16 | `StateInitData::ForeachConfig` | Index into a parallel `Bytecode::frame_descriptors[]` table; describes convention-slot layout |

`inst.inputs[0..4]` are reserved for the convention-slot buffer indices the body reads from (freq/gate/vel/trig/out for POLY; user-defined layout for `map`/`each`/`fold` via the `FrameDescriptor`). This matches POLY's existing 5-slot scheme.

VM execution (audio path, but allocator state pre-sized at swap time):

```
1. Read events from upstream state (already populated by sequencer/MIDI in the same block).
2. Per allocator_kind:
   - VOICE_POOL: walk events; on gate-on, find a free voice slot in the pre-allocated pool;
     on gate-off, mark released. Active voice set updated.
   - PER_ITERATION: allocate one slot per event from a pre-sized arena (cleared each block).
   - SHARED: single slot, all iterations reference it.
3. For each active iteration:
     bind iteration's per-iter buffers into the convention slots (freq/gate/vel/trig/out + N user slots)
     dispatch into bytecode.blocks[block_id].body
     execute through to RET
4. Mix all iteration outputs into the FOREACH_EVENT's output buffer.
```

**Overflow behavior when live events exceed `max_iterations`** — committed
semantics, matching today's POLY allocator (`PolyAllocState::allocate_voice`,
`cedar/include/cedar/opcodes/dsp_state.hpp:541` — "Find first inactive voice …
No voice stealing — return nullptr if all busy"):

| Allocator | When the pool/arena is full | Rationale |
|---|---|---|
| `VOICE_POOL` | **Drop the new gate-on event** — no voice stealing in v1. The note simply does not sound. Bit-exact with POLY today. | Voice stealing is a deliberate v1 non-goal; it would change POLY behavior, violating the bit-exactness gate. A stealing policy can be a follow-up. |
| `PER_ITERATION` | **Drop events beyond the `max_iterations` cap** for that block; process the first `max_iterations`. | Arena is pre-sized at swap time; runtime growth is a non-goal (§12). |
| `SHARED` | N/A — single slot, no overflow possible. | — |

Dropped events are silent (no error, no diagnostic). `max_iterations` is sized
generously at swap-prepare time from the upstream pattern/voice cap; overflow
is an edge case, not an expected path. A future PRD may add a voice-stealing
policy for `VOICE_POOL`.

**Addressing strategy: convention slots** (the POLY exception to L2's swap-time expansion). The block body reads its inputs from fixed param-buffer indices — exactly as POLY does today with its 5-slot scheme. No swap-time expansion; true dispatch into the subprogram table.

**Why convention slots here?**

- The body genuinely runs N times per block, where N is dynamic. Inlining N copies at swap time would require knowing N at swap time, which we don't.
- The voice pool / per-iteration arena needs to share buffer layout across all iterations — convention slots are the natural mechanism.
- POLY already does this and the model works.

#### 4.3.2 POLY Migration

POLY is reimplemented entirely as a `FOREACH_EVENT` specialization:

```
poly(input_events, instrument, max_voices=8)
// today: emits POLY_BEGIN { instrument body inlined } POLY_END
// after: emits FOREACH_EVENT(event_src=input_events, block_id=instrument_block,
//                            allocator_kind=VOICE_POOL, output=poly_output)
//        + StateInitData::PolyAlloc(max_voices) entry as today
//        + Subprogram entry for `instrument` body
```

`POLY_BEGIN` and `POLY_END` opcodes are removed (or kept as no-op aliases for one release cycle for old saved bytecode — TBD in §10 rollout). The `execute_poly_block()` function in `vm.cpp:323-595` is refactored into `execute_foreach_event()` with the `VOICE_POOL` allocator branch reproducing its exact behavior.

**Bit-exactness criterion:** every existing test in `akkado/tests/` that touches POLY must pass unchanged. This is enforced as gate-1 of L3 merge.

#### 4.3.3 Block-Ref Values & Higher-Order DSL

A new lightweight compile-time value type:

```cpp
struct BlockRef {
    BlockId block_id;
    std::array<uint16_t, MAX_CAPTURES> captured_buf_indices;
    uint8_t capture_count;
};
```

**Capture semantics (v1 — minimal):**
- Captures buffer indices and compile-time constants only.
- Closes over a single lexical scope at definition time; rebinding at call-time is not supported.
- Does **not** survive across hot-swap as a value. (Captured buffer *identities* are stable across hot-swap via the semantic-path scheme; the BlockRef value itself is a compile-time artifact.)
- Cannot be returned from a fn (no first-class fn values escaping the compile unit). Cannot be stored in a record field. Cannot recurse.

These restrictions exist to keep v1 shippable; they can relax in follow-ups as use cases prove out.

**Stdlib higher-order operators (all implemented via `FOREACH_EVENT`):**

```akkado
notes.each_voice(n => osc("sin", n.freq) * 0.5) // PER_ITERATION allocator
notes.each(n => osc("sin", n.freq) |> out(@))   // PER_ITERATION, no return
notes.fold(0.0, (acc, n) => acc + n.freq)       // SHARED allocator, single accumulator slot
```

Each is a thin compile-time wrapper that emits a `FOREACH_EVENT` with the appropriate allocator and binds the lambda body as the `block_id`. The lambda's parameter list maps to the convention slots. `each_voice` (this PRD) is distinct from `map` (the companion event-transforms PRD); see §13.

**Restriction in v1**: the operand of `each_voice`/`each`/`fold` must be one of:
- a pattern result (`n"…"`, `v"…"`, `s"…"`, `c"…"`, `seq(...)`),
- a MIDI input event stream,
- a literal array of records.

(I.e., something that maps to a `SequenceState` / `MidiQueueState` / `ArrayState`.) Higher-order over arbitrary signals is out of scope — that's already covered by per-sample operators and L2 user fns.

---

## 5. Buffer Addressing Strategy (Concrete)

### 5.1 Swap-Time Expansion (Default for L2)

Used by every non-`#inline` user `fn` call. The body lives once in `bytecode.blocks`; at swap-prepare time it is inlined into `bytecode.main` at every `BLOCK_CALL` site with buffer indices and state IDs patched. Audio thread sees a flat, indistinguishable-from-today instruction stream.

Cost: bytecode size grows linearly with call-site count (same as today's inlining). Benefit: zero audio-path complexity, zero new VM machinery for the L2 use case, identical runtime characteristics.

### 5.2 Convention Slots (For Event Iterators)

Used by `FOREACH_EVENT` and its specializations (POLY, `each_voice`, `each`, `fold`). The block body reads inputs from a fixed bank of param-buffer indices — for POLY today these are 5 slots (freq, gate, vel, trig, out); for `each_voice(n => ...)` the layout is one slot per record field accessed (`n.freq`, `n.vel`, etc.) plus an output slot.

The convention slot layout is part of the `FrameDescriptor` of each block. The caller writes event fields into the slots before dispatching into the body; the body reads from them. True body sharing in memory across all iterations.

### 5.3 Frame-Relative Addressing (Deferred)

Would let true CALL/RET work without swap-time expansion or fixed convention slots. Each callable block carries an `inst.frame_offset` field; the VM maintains a frame pointer; opcodes inside a block reference `frame + N` instead of absolute buffer indices.

**Out of scope for v1.** Deferred until a use case forces it. Most likely trigger: true runtime recursion (the follow-up PRD).

---

## 6. State Pool & Hot-Swap

### 6.1 State ID Computation

Today (`akkado/include/akkado/codegen.hpp:351`, `akkado/src/codegen.cpp`): semantic path built via `push_path()` / `pop_path()` stack; state ID = FNV-1a hash of joined path. Function calls already add `"funcname#N"` components (`codegen_functions.cpp:1056-1057`).

This PRD extends the path scheme with:

- For `BLOCK_CALL` (L2 swap-time expansion): inside the expanded body, the path stack pushes `"block:<fn_name>@callsite_<N>"` where `<N>` is the call-site index in source order. State IDs in the expanded body hash this extended path.
- For `FOREACH_EVENT` (L3): the iteration's path component is `"foreach:<block_name>@callsite_<N>/iter_<voice_slot_idx>"`. Voice/iteration slots are stable across hot-swap as long as `max_voices` doesn't shrink.

### 6.2 MAX_STATES Sizing

Currently 512 (`cedar/include/cedar/dsp/constants.hpp`). With L2 swap-time expansion, state slot count for a 10-call-site fn equals 10 × (slots-per-body-copy) — same as today's per-site inlining. So L2 does *not* increase pressure relative to today.

L3 `FOREACH_EVENT` with `VOICE_POOL` allocator at `max_voices=N` consumes N × (state-slots-per-voice) slots. POLY today does the same.

**Audit task before shipping:** instrument the swap-prepare pass to report peak state-slot consumption across representative user patches. If any patch exceeds 512, bump `MAX_STATES` to 1024 — pure constant change, no architectural impact.

### 6.3 Hot-Swap Behavior

Triple-buffer hot-swap and micro-crossfade mechanisms ([cedar-vm-hot-swap-implementation.md](cedar-vm-hot-swap-implementation.md)) are unchanged. Block bodies hash into the same path scheme. Semantic rebinding works as today.

Renaming a `fn` (e.g., `fn foo` → `fn bar`) invalidates the state for that fn cleanly — the path changes, the hash changes, the old state pool entry is GC'd to the fading pool, fresh state is allocated. Document as user-visible behavior: "renaming a fn loses its DSP state, same as renaming any other named identifier."

---

## 7. Akkado Surface Language

### 7.1 `fn` — Shared Block by Default

```akkado
// Default: shared subprogram, called via BLOCK_CALL, swap-time-expanded
fn lp_chain(input, cutoff) {
    input |> lp(@, cutoff) |> hp(@, 30)
}

sig1 |> lp_chain(@, 800) |> out(@)
sig2 |> lp_chain(@, 1200) |> out(@)
// Both call-sites share one Subprogram entry pre-expansion;
// after swap-prepare, sig1 and sig2 each see their own inlined copy with their own state.
```

### 7.2 `#inline` — Opt Back into Per-Site Inlining

```akkado
// For hot-path micro-bodies where you genuinely want zero dispatch overhead
#inline fn fast_mix(a, b) {
    a * 0.5 + b * 0.5
}
```

`#inline` matters mostly as a performance escape hatch. The default's bytecode-size win matters more for most user code. Two-instruction bodies in inner loops are a small minority; explicit annotation is fine.

### 7.3 `when` — Block-Rate Conditional Bypass (L1)

```akkado
// Bypass the reverb chain entirely when muted
sig |> when(mute_toggle, @ * 0, @ |> reverb(@))

// Skip a chain on a one-shot trigger
hit_chain |> when(hit_button, @ |> dist(@, 4) |> lp(@, 800), @)
```

Distinct from `select(cond, a, b)` (sample-rate, both branches always evaluated) and from the deferred compile-time `if/else` (resolved at codegen time, eliminates one branch from bytecode). See §4.1's "Relationship to `select()`" table for the full three-way comparison. The compiler emits `SKIP_IF_NONZERO` / `SKIP_IF_ZERO` opcodes.

### 7.4 `for` / `loop` — Bounded Static Iteration (L1)

```akkado
// Apply 4 series allpass stages — body shares state across iterations (caveat in §4.1)
sig |> loop(4) { @ |> allpass(@, 800, 0.7) } |> out(@)
```

L1 only. For N independent stateful UGen instantiations, use L3 `each`:

```akkado
freqs = [220, 330, 440, 550]
freqs.each(f => osc("sin", f) * 0.25) |> out(@)
```

### 7.5 Higher-Order DSL (L3)

```akkado
// each_voice: per-event UGen instantiation; mixed output of all per-event signals
n"c4 e4 g4" as notes
notes.each_voice(n => osc("sin", n.freq) * 0.5) |> out(@)

// each: side-effecting, no return aggregation (each iteration calls out() itself)
notes.each(n => osc("sin", n.freq) |> out(@))

// fold: shared accumulator
notes.fold(0.0, (acc, n) => acc + n.freq)
```

Lambda parameter names map to convention slots; record field access (`n.freq`) maps to the per-iteration field-slot indices.

> **Naming note.** `each_voice` is the per-event signal-mixer. `map`, in this codebase, belongs to the companion `prd-runtime-event-transforms.md` and means per-event *field rewrite* (closure returns a record). See §13 for the rationale. Picking distinct names avoids two PRDs claiming the same `.map(...)` surface.

### 7.6 Parser Changes

This PRD adds three new pieces of Akkado surface syntax. None require changes to operator precedence; all extend the parser at parse-statement and parse-primary positions.

**`#inline` annotation (statement-prefix):**

A `#` token followed by an identifier at statement position parses as an *annotation* attached to the next `fn` declaration. Annotations are a small open-ended set; v1 defines `#inline` only.

```akkado
#inline fn fast_mix(a, b) { a * 0.5 + b * 0.5 }
```

`#` is reserved as the annotation-prefix lexeme. Outside annotation-context, `#` is reserved (currently produces a lex error). Future annotations (`#cold`, `#deprecated`, …) reuse the same lexical rule. This avoids any clash with the canonical hole token `@`.

**`loop(N) { body }` block-expression:**

`loop` is a contextual keyword (parsed as identifier in non-statement positions, treated as a control-flow form when followed by `(`). The body is a brace-delimited expression block whose value is the final expression (so it composes with `|>`).

```akkado
sig |> loop(4) { @ |> allpass(@, 800, 0.7) } |> out(@)
```

The count argument must be a **compile-time constant**: an integer literal, or a `let`-bound name whose initializer constant-folds to an integer. For v1, "constant-folds" means: integer literal, arithmetic on integer literals, or another `loop`-eligible name. A small compile-time evaluator (already implicit in mini-notation pre-processing) is extended to handle these cases. Non-foldable RHS produces `E251` (see §7.7).

**`when(cond, true_branch, false_branch)` builtin:**

`when` is a regular builtin call — no parser change. Lowering to `SKIP_IF_*` opcodes happens at codegen time. (The block-rate semantics matter at codegen, not at parse time.)

### 7.7 Compile-Time Errors

Codes use the **E240–E251** block. This range is confirmed free against both
shipped akkado codes (which run through E232 — `E200`–`E205` and `E230`–`E232`
are **already in use**, so the originally-drafted E200-block collided) and the
`E170`–`E182` block reserved by `prd-runtime-event-transforms.md`.

| Code | Trigger | Message |
|---|---|---|
| `E240` | Recursive `fn` definition (`fn foo() -> foo()`) | "recursive fn '<name>' not supported in v1 — see follow-up PRD" |
| `E241` | Block-ref captured into a record field or returned from a fn (escapes compile unit) | "block-ref values cannot escape the compile unit in v1" |
| `E242` | `each_voice`/`each`/`fold` applied to a non-event-stream operand | "each_voice/each/fold operand must be a pattern, MIDI input, or array literal" |
| `E243` | `loop()` (lowering to `LOOP_STATIC`) with non-constant count | "loop count must be a compile-time constant (integer literal or const-folded name)" |
| `E244` | `#inline` on a recursive fn | (same as E240) |
| `E245` | `BLOCK_CALL` body exceeds expansion size limit (configurable; default 1024 instructions per expanded body) | "expanded fn body exceeds N instructions — add #inline or split" |
| `E246` | `#inline` annotation applied to anything that isn't a `fn` declaration | "#inline must precede a `fn` declaration" |
| `E247` | `when()` branches have mismatched output arity (one returns 1 channel, the other returns 2) | "when() branches must have matching output channel count" |
| `E248` | `BLOCK_BIND` slot index ≥ `frame.param_count` for the target block | "slot index N is beyond the target block's parameter count" |
| `E249` | Unknown annotation (e.g., `#unknown_name`) | "unknown annotation '#<name>' — see docs for supported annotations" |
| `E250` | `#max_depth(N)` on a non-recursive fn (placeholder for the recursion follow-up; rejected in v1) | "#max_depth annotation reserved for future recursion support" |
| `E251` | `loop()` count is a non-foldable expression (e.g., references a runtime signal) | "loop count must constant-fold to an integer" |

---

## 8. Bytecode Layout (Wire Format)

> **Codebase note.** There is **no serialized bytecode wire format today** — no
> `CEDR` magic, no header, no versioned container. Programs reach the VM as a
> flat `std::span<const Instruction>` loaded into `ProgramSlot::load()`
> (`program_slot.hpp`). The layout below is therefore an **entirely new
> artifact** introduced by this PRD, not an extension of an existing format.
> The whole `Header` + `Subprograms` container is net-new; treat this section
> as a greenfield format design. Whoever implements it must also decide how
> the runtime side consumes it (see §11 open question on `Subprogram` storage).

```
Header
  magic: "CEDR"
  version: u16
  flags: u16
Main program
  count: u32
  Instruction[count]
State inits
  count: u32
  StateInit[count]
Subprograms                          ← NEW (L2+)
  count: u16
  Subprogram[count]:
    block_id: u32
    frame_descriptor: 4 bytes (param_count, local_buf_count, output_count, reserved)
    body_count: u32
    Instruction[body_count]
```

Subprogram table is empty for programs that use no `fn`s — backward-compatible with the current saved-bytecode format for trivial programs. Bumped version flag indicates L2+ bytecode.

---

## 9. Phased Rollout

Three independent PRs. Each one is shippable on its own.

### Phase 1 — L1: Forward Control Flow (PR1)

**Scope:**
- New opcodes: `SKIP_IF_ZERO`, `SKIP_IF_NONZERO`, `LOOP_STATIC`.
- VM dispatch loop extended to handle skip offsets (no jumps backward).
- New Akkado builtin: `when(cond, true_branch, false_branch)` and `loop(count) { body }`.
- Codegen lowers `when` / `loop` into the new opcodes.
- Web UI `bytecode_dump.cpp` updated to disassemble new opcodes.
- Generated opcode metadata regenerated (`web && bun run build:opcodes`).
- Tests: bit-exact equivalence vs `select(cond, a, b)` when cond is constant; CPU benchmark showing measurable saving when condition flips.

**Files touched:**
- `cedar/include/cedar/vm/instruction.hpp` — opcode enum additions
- `cedar/include/cedar/opcodes/control_flow.hpp` — NEW
- `cedar/src/vm/vm.cpp` — dispatch loop skip-handling
- `akkado/include/akkado/builtins.hpp` — `when`, `loop` builtin entries
- `akkado/src/codegen.cpp` or NEW `akkado/src/codegen_control_flow.cpp` — lowering
- `web/wasm/nkido_wasm.cpp`, `tools/nkido-cli/bytecode_dump.cpp` — disassembly support
- `experiments/test_op_when.py`, `experiments/test_op_loop.py` — NEW
- `akkado/tests/test_control_flow.cpp` — NEW

**Acceptance:**
- `when(toggle_off, expensive, cheap)` shows ≥ 80% CPU reduction vs `select` equivalent.
- All existing tests pass.

**Rollback plan:** L1 is fully additive (no existing opcodes or builtins changed). Revert the PR; new opcodes disappear from the enum; `when`/`loop` builtins disappear from the builtin table. No saved-bytecode compatibility risk — programs using `when()`/`loop()` fail to compile post-revert (clean failure mode).

### Phase 2 — L2: Subprogram Table & BLOCK_CALL (PR2)

> **Implementation note (2026-05-20).** L2 was split during implementation.
> The **surface-language half shipped** (see the Status block at the top):
> the `#inline` annotation, recursion rejection (E240/E244/E246/E249), and the
> `BLOCK_CALL`/`RET` opcode reservations. The **`Subprogram` table +
> `expand_block_calls()` machinery is deferred to L3.** Reason: the agreed
> model expands fn bodies at *compile time* inside Akkado codegen (not a
> swap-prepare pass), which makes the runtime bytecode byte-identical to
> today's per-site inlining. A `Subprogram` side-table therefore has **no L2
> consumer** and the AST-indexed shape it would take in L2 is not the
> runtime-resident shape L3's `FOREACH_EVENT` dispatch needs — so it is
> co-designed with L3. The original L2 scope below is retained for the
> historical record; items struck through are deferred.

**Scope:**
- ~~Bytecode format extension: `Subprogram` table.~~ *(deferred to L3)*
- New opcodes: `BLOCK_CALL`, `RET`, ~~optional `BLOCK_BIND`~~ — shipped as
  compile-time-expansion marker reservations; `BLOCK_BIND` dropped (unnecessary
  under compile-time expansion).
- ~~Akkado codegen emits `Subprogram` entries for all `fn` definitions.~~
  *(deferred to L3)*
- ~~Swap-prepare pass: `expand_block_calls()`.~~ *(deferred to L3)*
- ~~State-ID path scheme extended with `"block:<name>@callsite_<N>"`.~~
  *(deferred — L2 keeps the existing `name#N` scheme, so bytecode + state IDs
  stay byte-identical to today.)*
- New Akkado annotation: `#inline`. **— SHIPPED**
- Compile error for recursive `fn` definitions. **— SHIPPED**

**Files touched:**
- `cedar/include/cedar/vm/instruction.hpp` — opcode enum additions
- `cedar/include/cedar/vm/bytecode.hpp` — **NEW** file: the serialized wire-format container of §8 (no `Bytecode` type or wire format exists today; see §4.2 / §8 codebase notes)
- `cedar/include/cedar/vm/program_slot.hpp` — runtime `ProgramSlot`; needs a decision on how the `Subprogram` side-table is held at audio time (see §11)
- `cedar/src/vm/swap_prepare.cpp` — NEW (or wherever bytecode handoff lives — needs investigation)
- `akkado/src/codegen.cpp`, `akkado/src/codegen_functions.cpp` — fn-as-shared-block emission
- `akkado/include/akkado/codegen.hpp` — extended path scheme
- `web/wasm/nkido_wasm.cpp`, `tools/nkido-cli/bytecode_dump.cpp` — render subprogram table + expanded/unexpanded views
- `akkado/tests/test_block_call.cpp` — NEW
- `docs/concepts/callable-fns.md` — NEW user-facing doc

**Acceptance:**
- A 10-call-site fn produces audio bit-exact to current per-site-inlined behavior.
- Pre-expansion bytecode for a 10-call-site fn is ≥ 5× smaller than post-expansion (proves the table works).
- Hot-swap state preservation works per-call-site (existing hot-swap test harness extended).
- Recursive `fn` rejected at compile time with clear error.

**Rollback plan:** Higher-risk than L1 because codegen for *all* `fn` definitions changes. Mitigation: ship behind a compiler flag `--inline-fns` (default off after acceptance passes); the flag forces per-site inlining (existing path) for an entire compile unit. Revert path: flip default, revert bytecode-format version bump, drop `BLOCK_CALL`/`BLOCK_BIND`/`RET` from the opcode enum, drop `Subprograms` table from the wire format. Saved-bytecode risk: any program saved with the new format would fail to load post-revert; users must recompile from source. Acceptable since saved bytecode is not the primary distribution channel.

### Phase 3 — L3: FOREACH_EVENT, POLY Migration, Higher-Order DSL (PR3)

**Scope:**
- New opcode: `FOREACH_EVENT` with three allocator kinds (`VOICE_POOL`, `PER_ITERATION`, `SHARED`).
- VM: `execute_foreach_event()` consolidating `execute_poly_block()`'s logic.
- POLY codegen migrated: `poly(...)` emits `FOREACH_EVENT(VOICE_POOL)` + Subprogram for instrument body.
- `POLY_BEGIN` / `POLY_END` opcodes retained as no-op aliases for one release cycle, then removed.
- `BlockRef` value type in Akkado codegen.
- Stdlib higher-order operators: `map`, `each`, `fold` as builtins emitting `FOREACH_EVENT`.

**Files touched:**
- `cedar/include/cedar/vm/instruction.hpp` — `FOREACH_EVENT` opcode
- `cedar/include/cedar/opcodes/foreach_event.hpp` — NEW
- `cedar/src/vm/vm.cpp` — `execute_foreach_event()`, POLY routing
- `akkado/src/codegen_functions.cpp:1992-2285` — POLY migration to FOREACH_EVENT emission
- `akkado/src/codegen_higher_order.cpp` — NEW (map/each/fold)
- `akkado/include/akkado/builtins.hpp` — map/each/fold builtin entries
- All POLY tests in `akkado/tests/` — must pass unchanged
- `experiments/test_op_foreach_event.py` — NEW
- `docs/concepts/higher-order-dsl.md` — NEW user-facing doc

**Acceptance:**
- All existing POLY tests pass bit-exact.
- `n"…".each_voice(n => osc(...))` renders correctly in both `nkido-cli render` and the web UI.
- A 300-second simulated render shows zero allocation events on the audio thread.
- Hot-swap state preservation works across POLY migration (existing PolyAllocState path-hash unchanged).

**Rollback plan:** Highest-risk phase because POLY behavior changes substrate. Mitigation: ship `FOREACH_EVENT` alongside existing `POLY_BEGIN`/`POLY_END` for one release cycle with a compiler flag `--legacy-poly` (default off after gate-1 acceptance: all POLY tests pass bit-exact). If a regression is detected post-release: flip default; existing `execute_poly_block()` code path is still intact. To revert fully: drop `FOREACH_EVENT` opcode + executor + higher-order builtins + Subprogram entries that target poly bodies; restore POLY codegen at `codegen_functions.cpp:1992-2285`. Saved-bytecode programs targeting `FOREACH_EVENT` will fail post-revert (clean recompile required). The `--legacy-poly` flag is the recommended single-knob disable for hot-fix scenarios.

---

## 10. Verification & Test Plan

Per-phase acceptance criteria are listed above. Cross-cutting verification:

### 10.1 Long-Render Allocation Check

Per project DSP methodology, every PR ships a ≥ 300-second simulated audio render exercising the new feature. Allocation tracking on the audio thread is mandatory (add an instrumentation guard if it doesn't already exist).

### 10.2 Bit-Exactness

L2 PR: bit-exact equivalence vs current per-site inlining for representative patches.
L3 PR: bit-exact equivalence vs current POLY for all `akkado/tests/` POLY cases.

These are gate-1 merge criteria for their respective PRs.

### 10.3 Hot-Swap Preservation

Extend the existing hot-swap test harness with:
- A program that defines a `fn` called from 3 sites; verify per-call-site state preservation across a swap that modifies an unrelated part of the program.
- A program with a `n"…".each_voice(...)` block; verify per-iteration state preservation across pattern edits.
- A program using `poly()` (post-migration); verify all existing PolyAllocState preservation invariants hold.

### 10.4 CPU Benchmark (L1)

`when(toggle_off, expensive_chain, sig)` vs `select(toggle_off, expensive_chain, sig)`:
- Measure block render time over 1000 blocks.
- Acceptance: `when` shows ≥ 80% reduction when condition is false.

### 10.5 Bytecode Size Benchmark (L2)

A test patch with a 30-instruction `fn` called from 10 sites:
- Today's per-site inlining: ~300 instructions in `main`.
- L2 pre-expansion: ~10 `BLOCK_CALL` instructions in `main` + 1 × 30-instruction body in `blocks`. ≥ 5× smaller pre-expansion.
- Post-swap-expansion: ~300 instructions (matches today by construction).

### 10.6 Existing Test Suite

All of `cedar_tests`, `akkado_tests`, and `experiments/run_all.sh` must pass unchanged at each phase boundary. Regressions block the merge.

---

## 11. Open Questions (For Resolution During Implementation)

1. **MAX_STATES sizing.** `MAX_STATES` is already 512. Measure peak slot consumption across representative patches in Phase 2 — is 512 still enough, or do we need to bump to 1024? Constant change only, but worth getting right once.
2. **`Subprogram` dedup at swap time?** If two call-sites have identical buffer bindings, dedupe the expanded copies? Probably not worth it in v1; expansion size matches today's inlining cost.
3. **Block-ref capture exhaustivity.** v1 captures buffer indices + constants. Should it also capture other block-refs? Records? Defer to L3 follow-up; reject explicitly in v1 with clear error.
4. **`SKIP_IF_ZERO` predicate semantics.** Currently spec'd as "first sample of block." Should it also support "any sample in block is nonzero" / "all samples" / "RMS > threshold"? Decided per use case in Phase 1; document the chosen semantics explicitly.
5. **POLY backward-compat for saved bytecode.** Keep `POLY_BEGIN`/`POLY_END` as no-op aliases for one release cycle, or break compat immediately? Default proposal: alias for one cycle, drop in the release after L3 ships.
6. **`#inline` for very small fns.** Heuristic auto-inlining (≤ 3 instructions auto-inlined)? Recommend explicit-only in v1 for predictability; revisit if pre-expansion bytecode-size wins are smaller than expected in practice.
7. **Hot-swap semantics for renamed blocks.** Document user-visible behavior in Phase 2 user docs: renaming a `fn` loses its state, same as renaming any other named identifier. (Already implied by the existing FNV-1a path-hash scheme.)
8. **CLI/web debug surface for subprogram tables.** Phase 2 needs `bytecode_dump.cpp` and the web UI to render both subprogram tables and expanded/unexpanded views. UX details TBD in Phase 2 design.
9. **Where does `swap_prepare.cpp` live?** Investigate existing swap-prepare codepath during Phase 2 implementation; place new expansion pass next to it. May not need a new file.
10. **L3 `map` over a literal array — codegen path.** Literal arrays today (`[220, 330, 440]`) — does the existing codegen produce an `ArrayState`, or does it need extension to support `FOREACH_EVENT(ARRAY)` source? Investigate in Phase 3.
11. **Runtime storage of the `Subprogram` side-table.** The runtime program form, `ProgramSlot` (`program_slot.hpp`), is a cache-line-aligned struct holding a *fixed* `std::array<Instruction, MAX_PROGRAM_SIZE>` with no slot for a side-table. L2's swap-time expansion sidesteps this — `BLOCK_CALL` sites are inlined away, so the `blocks` table is not needed at audio time. But L3's `FOREACH_EVENT` dispatches into `bytecode.blocks[block_id]` *at audio time* and genuinely needs the bodies resident. Phase 3 must decide: a second fixed array on `ProgramSlot` for block-body instructions? A separately-loaded region? Does `MAX_PROGRAM_SIZE` absorb block bodies, or get a companion `MAX_BLOCK_PROGRAM_SIZE`? Resolve before Phase 3 design; it also drives how the §8 wire format is deserialized into the runtime structure.

---

## 12. Out of Scope (Explicit Non-Goals, Restated)

- **True recursion.** Compile-time-unrolled recursion with `#max_depth(N)` annotation is a follow-up PRD. v1 rejects recursive `fn` definitions at compile time.
- **Frame-relative addressing.** Defer until a use case can't be served by swap-time expansion or convention slots. Most likely triggered by the recursion follow-up.
- **First-class fn values escaping the compile unit.** BlockRef values in v1 are capture-only-by-buffer-index-and-constant, single-call-site, consumed by stdlib higher-order operators only.
- **Backward JMP / arbitrary control flow.** Forward skips only; no cycles at the instruction-stream level.
- **Run-time growth of state pool, subprogram table, voice pool.** All sizing finalized at swap-prepare time. Audio path is allocation-free.
- **`inst.rate` runtime dispatch.** Stays as compile-time mode-dispatch channel per [extended-params-mechanism.md](extended-params-mechanism.md) §5.
- **Removing `SELECT` / sample-rate logic operators.** They remain the right tool for per-sample signal muxing.

---

## 13. Relation to `prd-runtime-event-transforms.md`

The companion draft [prd-runtime-event-transforms.md](prd-runtime-event-transforms.md) declares (its §0) a **hard external dependency** on a separate "runtime closure infrastructure" PRD: a `Closure` runtime value type, an `INVOKE_CLOSURE` opcode, and Akkado codegen support for closure values as `TypedValue` variants. **This PRD is that infrastructure PRD.** The mapping:

| event-transforms PRD term | This PRD's mechanism |
|---|---|
| `Closure` runtime value | `BlockRef` (`{block_id, captured_buf_indices, capture_count}`) — see §4.3.3 |
| `INVOKE_CLOSURE` opcode | `BLOCK_CALL` (and, when invoked per-event, `FOREACH_EVENT` dispatching into the bytecode's subprogram table) |
| Closure-as-`TypedValue` codegen | BlockRef as a compile-time value flowing through Akkado's existing `TypedValue` (new `ValueType::BlockRef` variant) — see §4.3.3 |

**Required adjustment to BlockRef semantics for event-transforms compatibility.** §4.3.3 lists v1 BlockRef restrictions including "does not survive across hot-swap as a value" and "cannot be stored in a record field". The event-transforms PRD requires closure handles stored in `StateInitData` so its `EVENT_MAP` opcode can dispatch the closure per event. To satisfy that, this PRD relaxes the restriction *only* for: (1) BlockRefs consumed directly by an `EVENT_MAP` / `EVENT_FILTER` / `FOREACH_EVENT` opcode at the call site they were declared in, (2) the BlockRef is stored as a `BlockId + capture-vector` reference in `StateInitData`, not as a heap-allocated value. The "no escape across hot-swap boundaries" and "no return from a fn" restrictions stand; only the "no storage in StateInitData" restriction lifts. Concretely: `event_map(events, (e) -> {...})` is supported; `fn make_xform() = (e) -> {...}` returning a closure is not.

**Disambiguation of `notes.map(...)`.** Both PRDs proposed a `map` method on event streams with different semantics:

| PRD | Name proposed | Semantics |
|---|---|---|
| **This PRD** (§7.5) | `notes.map(n => osc(...))` | Per-event UGen instantiation, mixed output. Closure body is a signal-producing expression; output is the sum across all currently-active events. |
| **event-transforms PRD** (§3.2) | `notes.map(n => {note: ...})` | Per-event field rewrite. Closure body returns a record; output is an event stream with rewritten fields. |

These are different operations and need different surface names. **Resolution (committed in this PRD):** this PRD's `notes.map(...)` is renamed to **`notes.each_voice(...)`** (matches the L3 stdlib op set: `each`, `each_voice`, `fold`). The event-transforms PRD keeps `notes.map(...)` for field rewrites. Both PRDs need to update their respective §7.5 / §3.3.

The event-transforms PRD also needs to (a) drop its §0 from "hard external dependency" to "depends on this PRD", and (b) cross-link this section. That edit belongs in the event-transforms PRD, not here, but is required before either ships.

---

## 14. References

| Doc | Relation |
|---|---|
| [prd-runtime-event-transforms.md](prd-runtime-event-transforms.md) | Companion draft; depends on this PRD for closure infrastructure (see §13) |
| [prd-conditionals-logic.md](prd-conditionals-logic.md) | **DONE** — sample-rate `SELECT` + comparators; this PRD adds `when()` (block-rate, distinct from `select`), NOT the "compile-time if/else" that PRD listed as future work |
| [prd-advanced-functions.md](prd-advanced-functions.md) | Existing user-fn machinery; explicitly states "no runtime function objects, no new VM opcodes" — this PRD is where we change that |
| [prd-compile-time-functions.md](prd-compile-time-functions.md) | The compile-time fn model this PRD extends |
| [prd-closure-pipe-operator.md](prd-closure-pipe-operator.md) | Closure capture model for block-refs |
| [extended-params-mechanism.md](extended-params-mechanism.md) | `inst.rate` policy referenced in non-goals |
| [cedar-vm-hot-swap-implementation.md](cedar-vm-hot-swap-implementation.md) | Hot-swap mechanism that block state-IDs participate in |
| [cedar-architecture.md](cedar-architecture.md) | Existing VM dispatch loop, state pool, DAG model |
| [dsp-experiment-methodology.md](dsp-experiment-methodology.md) | 300-second long-render requirement |
| `cedar/src/vm/vm.cpp:313-595` | Main dispatch loop + `execute_poly_block()` to be refactored |
| `cedar/include/cedar/dsp/constants.hpp` | `MAX_STATES = 512` — bump candidate to 1024 if audit warrants |
| `akkado/src/codegen_functions.cpp:1992-2285` | Current POLY codegen — port target |
| `akkado/include/akkado/codegen.hpp:351` | Path-hash mechanism extended for blocks |

---

## 15. Glossary

- **Block** — a callable subprogram body (a sequence of Cedar instructions terminated by `RET`). Stored in `bytecode.blocks`.
- **BLOCK_CALL** — opcode marker for a call site. Pre-swap-expansion: a dispatch instruction. Post-swap-expansion (L2): replaced by an inlined copy of the body.
- **Block-ref** — a compile-time value representing a closure: `(block_id, captured_buf_indices)`. Consumed by `FOREACH_EVENT`-based stdlib operators only in v1.
- **Convention slots** — fixed param-buffer indices that a block body reads its inputs from. Used by `FOREACH_EVENT` (and POLY) to enable true body sharing across iterations.
- **FOREACH_EVENT** — L3 opcode that dispatches a block once per event/voice/iteration. Three allocator kinds: `VOICE_POOL` (POLY semantics), `PER_ITERATION` (fresh state per event), `SHARED` (fold-style accumulator).
- **Frame descriptor** — metadata on a `Subprogram` describing its logical slot count, local buffer count, and output count. Used by swap-time expansion to allocate buffers and by `FOREACH_EVENT` to set up convention slots.
- **Frame-relative addressing** — addressing scheme where opcodes reference buffers via `frame + N` instead of absolute indices. Out of scope for v1.
- **Subprogram table** — `bytecode.blocks`: the side-array of callable block bodies, separate from `bytecode.main`.
- **Swap-time expansion** — the swap-prepare pass that inlines `BLOCK_CALL` sites with absolute buffer indices and state IDs patched. Audio thread sees flat dispatch only.
- **`#inline`** — Akkado annotation opting back into per-site inlining for a `fn`. Default is shared block.
- **`when()`** — block-rate conditional bypass builtin (L1). Distinct from sample-rate `select()`.
