> **Status: IN PROGRESS — Phases 1 + 2a + 2b + 3 + 4 shipped.**
>
> - **Phase 1** (substrate: packed `EVENT_MAP` / `EVENT_FILTER` opcodes +
>   runtime `transpose` / `velocity`) — shipped 2026-05-22, commit `79b4b24`.
> - **Phase 2a** (closure-taking `event_map` / `event_filter` builtins +
>   Cedar opcode closure rework) — shipped 2026-05-23, commit `694eb84`.
> - **Phase 2b** (stdlib `akkado/stdlib/event_transforms.ak` modifier
>   migration + delete C++ handlers) — shipped 2026-05-23, commits
>   `582a28a` → `b485c3f`. Migrated: `transpose`, `velocity`, `dur`,
>   `bend`, `aftertouch`, `early`, `late`, `swing`, `swingBy`. `tune`
>   deferred (its `tune("31edo", pattern)` semantics don't fit the
>   `event_map` shape; needs a separate `EVENT_OUT_MICRO` slot).
> - **Phase 3** (rate scaling: `EVENT_RATE_SCALE` opcode + runtime
>   `fast` / `slow`) — shipped 2026-05-23. The opcode mutates the upstream
>   `SequenceState.cycle_length` each block to `original / rate`; every
>   `SEQPAT_*` opcode reads `cycle_length` for cycle scaling so a single-
>   field write covers QUERY / STEP / GATE / FIELD / PHASE / TYPE / PROP /
>   VALUES uniformly — no external-clock plumbing. Signal-rate factors are
>   now supported (`fast(p, lfo)`). Constant factors fold the reciprocal
>   for `slow` at compile time; signal-rate `slow` emits a runtime `DIV`.
>   Multi-use stays independent: each `fast`/`slow` call recompiles its
>   inner pattern into a fresh SequenceState via
>   `compile_pattern_for_transform`, so `slow(f, 2)` and `fast(f, 2)` on
>   the same `f` don't interfere. Composition `fast(slow(p, 2), 3)` lands
>   at the expected 1.5× speed: inner `slow`'s compile-time `cycle_length`
>   accumulation feeds the outer `fast`'s ERS, which captures the
>   compile-time-accumulated value on first block. **Bug fix shipped with
>   Phase 3**: SEQPAT_STEP's wrap-detect heuristic was hardcoded to
>   `last_beat_pos - 0.5f`, assuming `cycle_length = 1.0`. With
>   `cycle_length < 1` (any `fast(N)` for N>1), wraps were never detected
>   — silently exposing a pre-existing bug now that runtime `fast` actually
>   sets `cycle_length < 1`. Fixed by scaling the threshold to
>   `cycle_length * 0.5f` (`sequencing.hpp:492`); regression test in
>   `akkado/tests/test_fast_slow.cpp [regression]`.
> - **Phase 4** (structural transforms: `EVENT_REORDER` + `EVENT_FANOUT`
>   opcodes; runtime `rev` / `palindrome` / `zoom` / `compress` / `ply` /
>   `linger` / `segment` / `iter` / `iterBack`) — shipped 2026-05-23.
>   `EVENT_REORDER` (kind selector REV/PALINDROME/ITER/ITER_BACK/ZOOM/COMPRESS
>   in `inst.rate` low nibble) and `EVENT_FANOUT` (PLY/LINGER/SEGMENT) both
>   reuse `SequenceState` as their downstream-OutputEvents holder, mirroring
>   the EVENT_MAP / EVENT_FILTER substrate. Continuous parameters (zoom/
>   compress endpoints, linger frac) are signal-rate; integer-cardinality
>   parameters (`ply n`, `segment n`, `iter n`) stay compile-time constants.
>   `iter` / `iterBack` migrated fully into `EVENT_REORDER(ITER)`: the
>   `iter_n` / `iter_dir` fields on `SequenceState` and `StateInitData`,
>   `init_sequence_iter_state` on the VM, and the SEQPAT_QUERY rotation
>   block (`sequencing.hpp`) are deleted. The 3 legacy `[vm][sequence][iter]`
>   tests in `cedar/tests/test_vm.cpp` were removed; coverage is preserved
>   end-to-end via `cedar/tests/test_event_reorder.cpp` (10 cases) and
>   `akkado/tests/test_reorder.cpp` (32 cases including composition with
>   EVENT_MAP / EVENT_RATE_SCALE / EVENT_FANOUT chains). Composition with
>   upstream runtime EVENT_MAP (e.g. `n"…".transpose(5).palindrome()`)
>   works for free because `EVENT_REORDER`/`EVENT_FANOUT` read the
>   upstream's `OutputEvents` each block via `resolve_output_events`.
>   `compile_pattern_for_transform`'s recursive compile-time fold is kept
>   (matches Phase 3 fast/slow precedent): nested constant-only chains
>   `rev(fast(p, 2))` fold the inner `fast` into the inner SequenceProgram's
>   `cycle_length` and emit only the outer `EVENT_REORDER`. `bend`/runtime-
>   EVENT_MAP composition under structural transforms now works end-to-end
>   (the Phase 2b regression note about `slow(bend(p, 0.3), 2)` is
>   superseded by Phase 3 + 4 shipping; the runtime EVENT_MAP is preserved
>   through all chains). A new helper, `emit_pattern_query_only`, factors
>   out the inner SEQPAT_QUERY emission so the 7 Phase-4 handlers skip the
>   wasted SEQPAT_STEP/extended-field/SEQPAT_PROP chain that
>   `emit_pattern_with_state` would otherwise emit for the inner state.
> - **Phase 5 (corrected scope, 2026-05-24)** — chord-array support
>   inside `event_map` closures (`e.notes` / `e.freqs` read + write) +
>   stdlib `scale` / `key` / `voice` / `invert` on top of `event_map`
>   (no new opcodes) + `scales.ak` generator + `TypedValue` Phase B
>   cleanup. The original Phase 5 framing ("`EVENT_QUANTIZE` opcode +
>   chord expansion via `EVENT_FANOUT`") is **dropped** — under the
>   foundational principle that opcodes are for primitive operations
>   and DSP work, not language constructs, scale/key/voice/invert
>   belong in stdlib akkado just like Phase 2b's `transpose` /
>   `velocity` / `dur` / `bend` migration.
> - **Phase 5 followup (2026-05-24)** — two latent bugs surfaced during
>   Phase 5 and worked around at the time are now fixed at the root:
>   (1) the closure-literal body walker mis-classified bare-identifier
>   bodies (`(h) -> h`) as a phantom param across 6 codegen walkers —
>   commit `e457b7c` consolidates them onto one canonical
>   `closure_body()` helper using the parser's structural "body is last
>   child" guarantee; (2) `BLOCK_CALL` / `BLOCK_BIND` / `SKIP_IF_*` /
>   `LOOP_STATIC` dispatched only from `execute_program`'s main loop,
>   silently no-op'ing inside every other subprogram-body runner —
>   commit `d95aa8b` extracts a `VM::execute_step()` helper and threads
>   it through `run_voice_pool`, `run_foreach_per_iteration`,
>   `run_foreach_shared`, `run_event_map_closure`,
>   `run_event_filter_closure`, and `execute_block_call`'s own body
>   loop. With (2) closed, the Commit F "inline `floor(n+0.5)` /
>   `fmod(fmod(n,12)+12,12)` per branch because BLOCK_CALL doesn't
>   dispatch" workaround is retired: commit `405be8f` regenerates
>   `scale_quantize.ak` using shared `fn snap` / `fn pc12` helpers,
>   shrinking the file ~13%.
>
> The Phase 3 (`EVENT_RATE_SCALE`) and Phase 4 (`EVENT_REORDER` /
> `EVENT_FANOUT`) opcodes that shipped are architecturally a partial
> regression against the same principle: each is a kind-dispatched
> mega-opcode with hardcoded C++ per named transform (REV / PALINDROME /
> ITER / ZOOM / COMPRESS, PLY / LINGER / SEGMENT, fast/slow factor
> scaling). They are functionally working stopgaps. Their correct
> redesign — closures over `Array<Event>` returning `Array<Event>`, with
> rev/palindrome/ply/etc. as stdlib akkado on top — is a substantial
> separate design captured in
> [`prd-pattern-array-transforms.md`](prd-pattern-array-transforms.md)
> and is **explicitly out of scope for Phase 5**. Phase 5 leaves the
> Phase 3+4 opcodes untouched.
>
> The one hard external dependency — runtime closure / first-class fn
> infrastructure (§0,
> [`prd-runtime-functions-control-flow.md`](prd-runtime-functions-control-flow.md))
> — shipped its L1→L3 phases plus §4.2 `BLOCK_BIND` (commits `41bb96c`,
> `9776ded`, `16b166c`, `ad6aed6`, `5b7746b`, `21f73ef`, `9690a6a`,
> `7a31d9d`). All §11 open questions are resolved; decisions are recorded
> inline in §11. `prd-cycle-length-cleanup.md` (§0.5) is **not** a hard
> prerequisite and may land in any order. `prd-pattern-event-arrays.md`
> (§0.6) was a soft dependency for chord-wide closures and is complete.

# PRD: Runtime Event-Stream Transforms (Pattern-Modifier Rework)

## Executive Summary

Today, every Akkado pattern modifier (`transpose`, `tune`, `fast`, `slow`, `early`, `late`, `rev`, `ply`, `swing`, `velocity`, `dur`, `bend`, `palindrome`, `zoom`, `segment`, `compress`, `iter`, `iterBack`, `linger`, `swingBy`, `bank`, `variant`, `aftertouch`, ~30 total) is a **compile-time AST transform**: the handler in `akkado/src/codegen_patterns.cpp` mutates a `std::vector<cedar::Event>` during codegen and emits `Opcode::NOP`. This forces every parameter to collapse to a constant, blocks parity with live MIDI streams, duplicates event-list-walk logic across 30+ handlers, and prevents any composability with user signals.

This PRD specifies a **rework into runtime event-stream transforms**: per-event modifiers become stdlib akkado functions on top of two generic closure-taking primitive opcodes that operate on `OutputEvents` — the existing runtime boundary already shared by `SequenceState` and `MidiQueueState`. The substrate is **two generic closure-taking opcodes** — `EVENT_MAP` (per-event field rewrite) and `EVENT_FILTER` (per-event drop). Every property modifier (`transpose`, `velocity`, `dur`, `bend`, `swing`, `early`, `late`, `scale`, `key`, `voice`, `invert`, `degrade`, `mask`, …) is a 1-line `akkado/stdlib/event_transforms.ak` definition on top of these.

Whole-pattern transforms (`rev`, `palindrome`, `ply`, `linger`, `segment`, `zoom`, `compress`, `iter`, `iterBack`, `fast`, `slow`) are categorically different — they need to see the full array of events at once and cannot be expressed per-event. Phases 3 and 4 shipped `EVENT_RATE_SCALE`, `EVENT_REORDER`, `EVENT_FANOUT` as kind-dispatched stopgap opcodes for these. The correct redesign — closures over `Array<Event>` returning `Array<Event>`, with these transforms as stdlib akkado — is the subject of the follow-up PRD [`prd-pattern-array-transforms.md`](prd-pattern-array-transforms.md). The kind-dispatched opcodes stay in place until that follow-up lands.

**Key design decisions (locked — see §11 for resolved open questions):**

- **Opcodes are primitives, not language constructs.** Any transform expressible from generic per-event closure primitives belongs in stdlib akkado, not as a dedicated opcode. The Phase 3+4 opcodes for whole-pattern transforms are a temporary exception until the follow-up PRD lands a proper array-of-events substrate.
- **Replace, don't coexist.** Compile-time modifier handlers are deleted, not deprecated. Constants and signals lower to the same runtime path. (Migration story per §8.)
- **Two-opcode closure substrate** for per-event transforms: `EVENT_MAP` and `EVENT_FILTER`. Both take a closure handle; both produce a new `OutputEvents` stream. Everything else built on these is stdlib.
- **Closure-taking `event_map`** as the high-level surface; existing closures-as-compile-time-AST are insufficient and the design depends on the runtime closure PRD (§0).
- **Signal sampling at event onset.** When a closure body references an external buffer (`e.note + lfo`), the buffer is indexed at the event's emission sample, not block-start and not re-sampled mid-event.
- **`fast`/`slow` are whole-pattern operations.** Phases 3+4 shipped them as the `EVENT_RATE_SCALE` opcode; the follow-up PRD will migrate them to stdlib on top of the array-of-events substrate.
- **Scope includes** core modifiers + scale quantize (snap to scale/key) + voicing/chord expansion/inversion + filter/predicate ops (`degrade`, `mask`) — **all delivered as stdlib akkado on top of `event_map` / `event_filter`**, not as new opcodes.
- **Phased delivery in 5 phases** (see §9), each independently testable.
- **Assumes the cycles-pure clock model has landed** (it has — the "1 cycle = 1 beat" headline shipped with the 2026-05-19 parser revert, commit `9d99490`). Every `e.time` reference and the `fast`/`slow` phase rescaler are written in cycles-pure terms. This is **not** a hard dependency on `prd-cycle-length-cleanup.md`; per §0.5 the `cycle_length` field is live and stays.

---

## 0. Hard Dependency: Runtime Closure Infrastructure — SHIPPED

When this PRD was drafted, Cedar had **no runtime function dispatch** — no CALL/RET opcodes, no first-class function values, no closure objects in the state pool. Akkado closures (`(e) -> ...`) existed only as compile-time AST nodes, inlined at every call site via `handle_user_function_call` (`akkado/src/codegen_functions.cpp`).

**That gap is now closed.** [`prd-runtime-functions-control-flow.md`](prd-runtime-functions-control-flow.md) shipped its L1→L3 phases plus §4.2 `BLOCK_BIND`. The runtime surface the `event_map(events, (e) -> ...)` design depends on now exists:

- **`FOREACH_EVENT`** — the per-event dispatch opcode. Iterates an upstream event stream and invokes a subprogram block per event, with the lambda parameter delivered as a **full event record** (`n.freq` / `n.vel` / `n.dur` / `n.note` / `n.chance` / `n.time` map to convention slots; `n.gate` / `n.trig` are synthesized per iteration; `E408` for fields the event model cannot supply). This is the dispatch primitive the `EVENT_*` opcodes in §3.1 build on.
- **`BLOCK_CALL` / `BLOCK_BIND`** — runtime subprogram dispatch: a user `fn` body compiles once into `ProgramSlot::blocks[]` and dispatches at runtime. `BLOCK_CALL` carries args in `inputs[0..4]`; `BLOCK_BIND` extends the convention to 6..32 params. This is what lets `event_map(events, (e) -> ...)` lower to a real runtime closure instead of per-site inlining.
- **Subprogram table** — `ProgramSlot::blocks[]` + the block-aware load handshake + `set_state_id_xor` per-call-site isolation. Closure bodies are runtime-resident and hot-swap-stable via a `block:<fn>@callsite_<N>` path hash.
- **VM allocators** VOICE_POOL / PER_ITERATION / SHARED, and the higher-order DSL surface (`each()`, `each_voice()`, `reduce()`).

**Mapping to this PRD's design.** Where earlier drafts said "Closure" and "INVOKE_CLOSURE", the shipped names are `BlockRef` and `FOREACH_EVENT` / `BLOCK_CALL`. §3.1's "closure handle stored in `StateInitData`" is a `block_id` into the subprogram table. The escape-hatch codegen-inlining fallback (specialised `TransformKind` enum + per-event mini-VM) described in earlier drafts is **no longer needed and is dropped**. See §11 OQ-8 for the full coupling-surface list.

---

## 0.5. Related Cleanup: Clock-Phase & MIDI-Streaming (separate PRD)

> **Revised 2026-05-20 — this section was rewritten after a false premise was
> found.** It previously named [`prd-cycle-length-cleanup.md`](prd-cycle-length-cleanup.md)
> a **hard prerequisite** on the assumption that it removes the "now-dead"
> `cycle_length` field, leaving `EventStreamPayload` free of it. That premise
> is wrong: `cycle_length` is **live** — the `slow`/`fast`/`palindrome`/`linger`
> pattern transforms compute it (`akkado/src/codegen_patterns.cpp`), tests
> verify the computed values, and the runtime scales event times by it. The
> field **stays**. `prd-cycle-length-cleanup.md` has been rescoped to the
> genuinely-dead plumbing only (MIDI `1e6f` sentinel → `streaming: bool`;
> `beat_phase`/`bar_phase` → one `cycle_offset`; a comment/terminology pass).

[`prd-cycle-length-cleanup.md`](prd-cycle-length-cleanup.md) is **not a hard
prerequisite** of this PRD. Its rescoped work — the MIDI `streaming` flag and
the clock-phase collapse — is largely orthogonal to runtime event transforms.
The two PRDs can land in either order; the only soft coupling is the shared
`EventStreamPayload`/`EventSourcePayload` surface, which **carries
`cycle_length`** in both designs.

> **Note (2026-05-20):** the original dependency named here was `prd-cycles-pure-clock-model.md`. That PRD has been split. Its headline — "1 cycle = 1 beat", `samples_per_cycle()` dropping `* 4` — already shipped with the 2026-05-19 parser revert (commit `9d99490`). The remaining work was split into (1) `prd-cycle-length-cleanup.md`, the clock-phase / MIDI-streaming cleanup, and (2) `prd-beats-per-cycle.md`, an optional user-facing time-signature knob. **This PRD does NOT depend on `prd-beats-per-cycle.md`.** Every `e.time` reference below is already cycle phase `[0, 1)` post-revert.

**Time-semantics points still valid post-revert** (independent of the cleanup PRD):

- §3.1's signal-sampling formula in the original draft referenced `spb` (samples per beat). Post-revert that's `spc` (samples per cycle), and the event-onset sample is derived directly from the scheduler.
- §3.3's stdlib `early` / `late` are already cycle-phase by design (`mod 1.0`).
- §3.3's `swing` / `swingBy` definitionally assumed a beat-grid; under cycle = beat they need an explicit grid-subdivision argument (see §11 OQ-10).

**Corrections forced by the false-premise discovery — sections needing rework**:

- §3.4's Phase B `EventStreamPayload` cleanup must **keep** `cycle_length`, not drop it. The field is live; the unified payload carries it.
- §11 OQ-7's interaction between `fast`/`slow` and `cycle_length` does **not** evaporate — `fast`/`slow` *are* the `cycle_length` mutation. OQ-7 remains a live design question and must be answered, not deferred.
- Any other section that assumed `e.time` arithmetic could ignore `cycle_length` must be re-checked: `slow`-wrapped patterns produce `cycle_length != 1.0`.

> **Corrected 2026-05-21.** §3.1, §3.4, §7 and §11 OQ-7 below have been
> reworked to keep `cycle_length` live. No pre-correction wording remains.

---

## 0.6. Soft Dependency: Pattern Event Arrays (separate PRD)

A third PRD — **`prd-pattern-event-arrays.md`** ("Pattern Event Arrays — Userspace Access to Chord & Polyphonic Data") — is a **soft** dependency, gating **§11 OQ-1 only**. It surfaces `OutputEvent.values[]` as a first-class dynamic-array value type (`DynArray`), adds `notes(e)` / `freqs(e)` accessors (with `e.notes` / `e.freqs` UFCS sugar), makes `len()` polymorphic, and — per its own Q3 — defines `map()` over runtime-varying-length arrays.

**Why soft, not hard**: property modifiers that rewrite only the *primary* voice (`transpose`, `velocity`, `dur`, `bend` acting on `e.note`) need nothing from it. Only **chord-wide** transforms — rewriting every note in `values[]` — need the dynamic-array model. Per §11 OQ-1, the chord-wide closure form `(e) -> {notes: map(e.notes, (v) -> v + 7)}` is only well-defined once Pattern Event Arrays lands: that PRD owns both the `DynArray` type and `map`-over-`DynArray`.

**Shared boundary**: both PRDs operate on the same `OutputEvents` wire format. Pattern Event Arrays' `notes(e)` accessor reads `OutputEvent.values[]`; every `EVENT_*` opcode in this PRD reads and writes `OutputEvents`. Consequently `notes(e)` composes downstream of a transform chain for free — `n"[c4,e4,g4]".transpose(7) … e.notes` works with no extra wiring, because PatternPayload still points at the final transform's `state_id`.

**Sequencing**: Pattern Event Arrays is unblocked today (its own prerequisite, `prd-userspace-state-and-edge-primitives.md`, shipped 2026-04-26) and should land before this PRD's **Phase 2** (`event_map` closures). **Phase 1** (constant-only `EVENT_MAP` / `EVENT_FILTER`, primary-voice) does not depend on it.

> **PRD reviewers**: unlike §0 / §0.5, descoping this dependency does not block the PRD — it only forces OQ-1 to collapse to "primary-voice-only, no chord-wide closure form."

---

## 1. Current State

### 1.1 What exists today

**~30 pattern modifiers, all compile-time.** Handlers live in `akkado/src/codegen_patterns.cpp`. Each one walks a `std::vector<cedar::Event>` and mutates fields, then re-serialises into a `cedar::Sequence` via `SequenceCompiler`. The opcode emitted is `Opcode::NOP`.

Examples (file:line refs in `akkado/src/codegen_patterns.cpp`):

| Modifier      | Handler                       | Line  |
|---------------|-------------------------------|-------|
| `transpose`   | `handle_transpose_call`       | 3238  |
| `tune`        | `handle_tune_call`            | 4297  |
| `fast`        | `handle_fast_call`            | 3117  |
| `slow`        | `handle_slow_call`            | 3056  |
| `velocity`    | `handle_velocity_call`        | 3311  |
| `dur`         | `handle_dur_call`             | 3637  |
| `bend`        | `handle_bend_call`            | 3629  |
| `early`       | `handle_early_call`           | 4367  |
| `late`        | `handle_late_call`            | 4430  |
| `rev`         | `handle_rev_call`             | 3178  |
| `palindrome`  | `handle_palindrome_call`      | 4493  |
| `ply`         | `handle_ply_call`             | 4560  |
| `linger`      | `handle_linger_call`          | 4624  |
| `zoom`        | `handle_zoom_call`            | 4693  |
| `segment`     | `handle_segment_call`         | 4764  |
| `compress`    | `handle_compress_call`        | 5487  |
| `swing`       | `handle_swing_call`           | 5380  |
| `swingBy`     | `handle_swing_by_call`        | 5431  |
| `iter`        | `handle_iter_call`            | 4844  |
| `iterBack`    | `handle_iter_back_call`       | 4905  |
| `bank`        | `handle_bank_call`            | 3738  |
| `variant`     | `handle_variant_call`         | 3899  |

**Runtime event boundary already exists.** Both `SequenceState` (`cedar/include/cedar/opcodes/sequence.hpp:190` ff) and `MidiQueueState` (`cedar/include/cedar/opcodes/midi.hpp:46`) publish an `OutputEvents` array (`sequence.hpp:122-184`) per block. `SEQPAT_QUERY` fills it; `SEQPAT_STEP` / `SEQPAT_GATE` / `SEQPAT_PROP` read from it and destructure into per-field signal buffers (FREQ, VEL, TRIG, GATE, NOTE, DUR, CHANCE, TIME, PHASE, SAMPLE_ID — see `akkado/include/akkado/typed_value.hpp:108-119`). `poly()` and `soundfont` read OutputEvents directly via `StatePool::resolve_output_events`.

**TypedValue payloads.** `PatternPayload` carries per-field buffer indices, `voice_freqs[]`, `custom_fields`, plus a `state_id` pointing to the SequenceState. `EventSourcePayload` (`typed_value.hpp:144`) wraps MIDI streams — just a state_id + cycle_length, no buffers. MIDI events get baked to mono buffers for `as e` field access via the `is_runtime_event_source` flag.

### 1.2 The gap

1. **No signals as parameters.** `transpose(p, lfo)` is impossible.
2. **No MIDI parity.** Modifiers only work on baked patterns; live MIDI has no equivalent path.
3. **Implementation sprawl.** 30+ near-identical handlers; bug fixes need fanning out.
4. **No user composability.** You can't write a new modifier in akkado userland that operates per-event.
5. **No first-class chord/voicing/scale ops.** Things like "snap to minor scale" or "expand to chord" must be retrofitted; today they would each need their own compile-time handler.

---

## 2. Goals and Non-Goals

### 2.1 Goals

1. Introduce **two generic closure-taking Cedar opcodes** — `EVENT_MAP` (per-event field rewrite) and `EVENT_FILTER` (per-event drop) — operating on `OutputEvents`.
2. Introduce **`event_map` and `event_filter`** closure-taking Akkado builtins.
3. **Rewrite ~all per-event property modifiers as 1-line stdlib akkado** (`akkado/stdlib/event_transforms.ak`) on top of `event_map` / `event_filter`. Delete the corresponding C++ handlers.
4. **Achieve uniform pattern/MIDI parity** for per-event transforms. Every per-event transform works identically on patterns, MIDI streams, and any future event source.
5. **Add new per-event capabilities as stdlib**: `scale`/`key` (pitch quantize), `voice`/`invert` (chord-array expansion/reflection), `degrade`/`mask` (probabilistic / pattern-gated filter).
6. **Ship the closure substrate extensions** needed for chord-array transforms: `e.notes` / `e.freqs` read inside `event_map` closures + DynArray-typed return fields (`notes` / `freqs`) for write-back.
7. **Ship docs and examples** covering the new model end-to-end.

**Out of scope (deferred to [`prd-pattern-array-transforms.md`](prd-pattern-array-transforms.md)):** whole-pattern transforms that need the full event array — `rev`, `palindrome`, `ply`, `linger`, `segment`, `zoom`, `compress`, `iter`, `iterBack`, `fast`, `slow`. Phases 3+4 shipped these as kind-dispatched `EVENT_RATE_SCALE` / `EVENT_REORDER` / `EVENT_FANOUT` opcodes; that follow-up PRD will migrate them to stdlib on top of an array-of-events substrate.

### 2.2 Non-Goals (deferred)

- **Runtime closure infrastructure itself.** Owned by §0's separate PRD.
- **Removing pattern compile-time baking entirely.** `n"c4 e4 g4"` still bakes a `cedar::Sequence` at compile time — only the *modifier* layer becomes runtime.
- **Replacing `bank` / `variant` sample resolution with runtime asset loading.** Sample assets must still resolve at compile time so `RequiredSamples` propagation works. (See §11 OQ-3 for nuance.)
- **A `with`-record-literal sugar syntax.** Explicitly rejected by the user in favour of closure-only ergonomics.
- **Per-sample mid-event signal re-sampling for closure parameters.** Event-onset latching only (§3.1).

---

## 3. Design

### 3.1 Runtime substrate — two generic closure-taking Cedar opcodes

Per-event transforms split by *closure shape*, not by modifier name. Each opcode reads upstream `OutputEvents` via `StatePool::resolve_output_events` (which already handles `SequenceState` + `MidiQueueState` uniformly — `cedar/include/cedar/opcodes/state_pool.hpp:128`) and publishes its own `OutputEvents`:

| Opcode             | Shape                                                  | Used by                                                                       |
|--------------------|--------------------------------------------------------|-------------------------------------------------------------------------------|
| `EVENT_MAP`        | per-event field rewrite via closure                    | `event_map`, `transpose`, `velocity`, `dur`, `bend`, `tune`, `aftertouch`, `early`, `late`, `swing`, `swingBy`, **`scale`, `key`, `voice`, `invert`** |
| `EVENT_FILTER`     | predicate drop via closure                             | `event_filter`, **`degrade`, `mask`, `filter_events`**                       |

That's the entire per-event closure substrate. Every transform in the "Used by" column is stdlib akkado on top of one of these two opcodes — no per-transform C++ handler, no per-transform opcode.

**Phase 3+4 stopgap opcodes — deferred to follow-up PRD.** Phases 3 and 4 shipped three additional kind-dispatched opcodes for whole-pattern transforms that don't fit the per-event closure shape:

| Opcode (stopgap)   | Kinds                                                  | Used by                                                                       |
|--------------------|--------------------------------------------------------|-------------------------------------------------------------------------------|
| `EVENT_RATE_SCALE` | (no kinds; takes a factor signal)                      | `fast`, `slow`                                                                |
| `EVENT_REORDER`    | REV / PALINDROME / ITER / ITER_BACK / ZOOM / COMPRESS  | `rev`, `palindrome`, `iter`, `iterBack`, `zoom`, `compress`                   |
| `EVENT_FANOUT`     | PLY / LINGER / SEGMENT                                  | `ply`, `linger`, `segment`                                                    |

These three opcodes are functionally working but architecturally a regression against the foundational principle — each is a kind-switch over hardcoded named transforms with per-transform C++ bodies. The correct redesign uses closures over `Array<Event>` returning `Array<Event>`, with rev / ply / fast / etc. as stdlib akkado on top. See [`prd-pattern-array-transforms.md`](prd-pattern-array-transforms.md). Until that PRD lands, the three opcodes stay in place untouched.

Common conventions (apply to all of `EVENT_MAP` / `EVENT_FILTER` and the stopgap opcodes):

- **State**: each instance owns a `SequenceState` (reuses the existing struct — no new `DSPState` variant). `OutputEvents` is the wire format.
- **Upstream `state_id`**: packed across `inputs[2]+inputs[3]` (32 bits in two 16-bit slots). Leaves `inputs[0..1]` for the two most-common signal parameters; 3+-param transforms use `ExtendedParams<N>` per `docs/extended-params-mechanism.md`.
- **Closure handle**: stored in `StateInitData` as a runtime closure reference (delivered by the closure PRD per §0).
- **Signal sampling at event onset**: when a closure body reads an external buffer (`e.note + lfo`), the opcode indexes the buffer at the event's scheduled onset sample within the current block. The scheduler provides that per-event sample offset directly — no `samples-per-beat` arithmetic (the cycles-pure headline shipped 2026-05-19). `cycle_length` remains live but is not consulted on this path. Latched per event; no mid-event re-sampling.
- **Downstream**: PatternPayload/EventStream pointers are rewired to the final transform's state_id; `as e |> osc(@, e.freq)` keeps working because SEQPAT_STEP still reads OutputEvents and refills per-field buffers.
- **Per-block memory budget**: each opcode lazily sizes its `OutputEvents` buffer to `upstream_cap × fanout_factor`. No hard chain-length cap (§11 OQ-4).

### 3.2 Closure-taking builtin

```akkado
event_map(events, (e) -> {note: e.note + 7, vel: e.vel * 0.8})
```

- **Signature**: `(EventStream, Closure((Event) -> Record)) -> EventStream`.
- **Return contract**: the returned record's fields override matching event fields; unspecified fields pass through unchanged.
- **Allowed inside body**: field reads on `e`, arithmetic + math builtins, external buffer references (sampled at event onset), calls to whitelisted pure helpers (`scale_to`, `quantize`, `mod`, `if`).
- **Forbidden inside body** (compile-time errors):
  - Stateful opcodes (proposed `E170`).
  - State cells / `state(init)` / `get(s)` / `set(s, v)` (proposed `E171`).
  - `out()` calls (proposed `E173`).
  - Nested `event_map` is **allowed** — it just produces a transformed stream the outer can consume.
- **Lowering**: codegen emits a single `EVENT_MAP` instruction with the closure reference in `StateInitData`. The runtime opcode body invokes the closure for each upstream event, copies/merges field outputs, writes the new event to its `OutputEvents`.

`event_filter` mirrors `event_map`:

```akkado
event_filter(events, (e) -> e.vel > 0.5)
```

- **Signature**: `(EventStream, Closure((Event) -> Bool)) -> EventStream`.
- **Lowering**: `EVENT_FILTER` opcode invoking the predicate per event; events are copied through only if it returns truthy.

### 3.3 Stdlib redefinitions

Move ~all property modifiers out of C++ codegen handlers and into `akkado/stdlib/event_transforms.ak`:

```akkado
fn transpose(events, n)   = event_map(events, (e) -> {note: e.note + n})
fn velocity(events, v)    = event_map(events, (e) -> {vel:  e.vel * v})
fn dur(events, d)         = event_map(events, (e) -> {dur:  e.dur * d})
fn bend(events, b)        = event_map(events, (e) -> {bend: b})
fn aftertouch(events, a)  = event_map(events, (e) -> {at:   a})
fn early(events, t)       = event_map(events, (e) -> {time: (e.time - t) mod 1.0})
fn late(events, t)        = event_map(events, (e) -> {time: (e.time + t) mod 1.0})
fn tune(events, cents)    = event_map(events, (e) -> {micro: e.micro + cents})
```

`e.time` is **cycle phase** `[0, 1)` within the current cycle (matching Strudel's `co`), so `early(p, 0.25)` shifts by a quarter-cycle. For closures that need cross-cycle reasoning, `e.cycle` exposes the absolute cycle count as a float (e.g., `3.25` = quarter-way through cycle 3). Field naming is locked per §11 OQ-9: `e.time` = phase, `e.cycle` = absolute count. `swing` / `swingBy` take an explicit grid-subdivision argument (`grid:`, default `8`) under cycles-pure — the old implicit "8th-note grid" assumed 4 beats per cycle and no longer holds; per §11 OQ-10:

```akkado
fn swing(events, amount, grid = 8) =
  event_map(events, (e) -> {time: e.time + swing_offset(e.time * grid, amount)})
```

Phase 5 (2026-05-24) adds chord-array-aware per-event transforms on the same stdlib substrate, enabled by extending the closure machinery to read `e.notes` / `e.freqs` (DynArray views onto chord arrays) and to write back DynArray-typed `notes` / `freqs` fields:

```akkado
fn scale(events, name)     = event_map(events, (e) -> {note: degree_to_note(e.note, parse_scale_root(name), parse_scale_intervals(name))})
fn key(events, name)       = event_map(events, (e) -> {note: snap_to_scale(e.note, parse_scale_root(name), parse_scale_intervals(name))})
fn voice(events, intervals) = event_map(events, (e) -> {notes: map(intervals, (i) -> e.note + i)})
fn invert(events, axis)    = event_map(events, (e) -> {notes: map(e.notes, (n) -> 2 * axis - n)})
fn degrade(events, p)      = event_filter(events, (e) -> random() > p)
fn mask(events, pattern)   = event_filter(events, (e) -> pattern_active_at(pattern, e.time))
```

`scale` / `key` resolve their interval list against `akkado/stdlib/scales.ak` — a generated catalog of named scale interval lists produced by `web/scripts/generate-scales.ts` from the tonal.js scale-type table. See sibling PRD [`prd-scale-quantize.md`](prd-scale-quantize.md) for the full scale/key semantics.

Whole-pattern transforms (`rev`, `palindrome`, `ply`, `linger`, `segment`, `zoom`, `compress`, `iter`, `iterBack`, `fast`, `slow`) currently lower to the Phase 3+4 stopgap opcodes (§3.1). The follow-up PRD [`prd-pattern-array-transforms.md`](prd-pattern-array-transforms.md) will migrate them to stdlib akkado on top of an array-of-events substrate.

`bank`/`variant` keep their compile-time sample-resolution path (sample-ref propagation is unchanged) but stamp `type_id` at runtime through `EVENT_MAP` with a small `BANK_SET` helper. See §11 OQ-3.

### 3.4 TypedValue evolution (two phases)

- **Phase A (compatible)**: add `upstream_state_id` and `transform_chain` fields to `PatternPayload` (`akkado/include/akkado/typed_value.hpp:50-120`). Existing `as e |> osc(@, e.freq)` keeps working — `SEQPAT_FIELD` just points at the final transform's `state_id`.
- **Phase B (cleanup)**: collapse `EventSourcePayload` into a unified `EventStreamPayload`, deprecate `ValueType::EventSource`. Pattern and MIDI become the same type. Per §0.5, the unified payload **carries `cycle_length`** — the field is live (the `slow`/`fast`/`palindrome`/`linger` transforms compute it and the runtime scales event times by it), so both `PatternPayload` and the future `EventStreamPayload` keep it.

---

## 4. Worked Examples

### 4.1 Static transpose (constant)

```akkado
n"c4 e4 g4".transpose(7) |> osc("sin", @.freq) |> out(@)
```

Codegen:
- `n"c4 e4 g4"` → `SEQPAT_QUERY` populating `SequenceState A` per block.
- `transpose(p, 7)` lowers (via stdlib) to `event_map(p, (e) -> {note: e.note + 7})`.
- `EVENT_MAP` opcode reads `SequenceState A`'s `OutputEvents`, applies the closure (constant +7), writes to `SequenceState B`.
- Downstream `SEQPAT_STEP` reads `SequenceState B`, fills FREQ buffer with transposed frequencies.

### 4.2 Signal-driven transpose

```akkado
lfo = osc("sin", 0.2) * 6
n"c4 e4 g4".transpose(lfo) |> osc("sin", @.freq) |> out(@)
```

Closure body becomes `(e) -> {note: e.note + lfo[event_offset]}`. The runtime `EVENT_MAP` opcode, for each event firing at sample-offset `t`, reads `lfo[t]` and adds it to the event's note before writing.

### 4.3 MIDI parity

```akkado
midi("ctrl1").transpose(12).velocity(0.7) |> poly(@, instr, 8)
```

`midi(...)` produces a `MidiQueueState` publishing `OutputEvents`. The same `EVENT_MAP` opcodes chain on top. `poly` reads the final state's `OutputEvents` via `StatePool::resolve_output_events`. No special-casing for MIDI.

### 4.4 Continuous rate scaling

```akkado
n"c d e f g".fast(osc("sin", 0.1) * 1.5 + 2) |> ...
```

`fast(p, sig)` lowers to `EVENT_RATE_SCALE` which produces a modulated phase signal that replaces `SEQPAT_QUERY`'s clock input (uses the existing external-clock path at `sequencing.hpp:372-380`).

### 4.5 Composable user code

```akkado
fn arp_up(events, steps) =
  event_map(events, (e) -> {note: e.note + (cycle_count() mod steps) * 12})

n"c4 g4".arp_up(3) |> osc("saw", @.freq) |> out(@)
```

Userland-defined modifier in 1 line, working on patterns or MIDI.

---

## 5. Migration Table

| Modifier               | Old handler (codegen_patterns.cpp:line) | New form                                                                                          |
|------------------------|------------------------------------------|---------------------------------------------------------------------------------------------------|
| `transpose`            | `handle_transpose_call:3238`             | stdlib `event_map`                                                                                |
| `tune`                 | `handle_tune_call:4297`                  | stdlib `event_map`                                                                                |
| `velocity`             | `handle_velocity_call:3311`              | stdlib `event_map`                                                                                |
| `dur`                  | `handle_dur_call:3637`                   | stdlib `event_map`                                                                                |
| `bend`                 | `handle_bend_call:3629`                  | stdlib `event_map`                                                                                |
| `aftertouch`           | (delegates to property_transform)        | stdlib `event_map`                                                                                |
| `early` / `late`       | `handle_early_call:4367` / `:4430`       | stdlib `event_map` on `time` field                                                                |
| `swing` / `swingBy`    | `handle_swing_call:5380` / `:5431`       | stdlib `event_map` (helper: `swing_offset(grid_pos, amount)`)                                     |
| `fast` / `slow`        | `handle_fast_call:3117` / `:3056`        | **stopgap builtin → `EVENT_RATE_SCALE`** — deferred to [`prd-pattern-array-transforms.md`](prd-pattern-array-transforms.md) for stdlib redesign |
| `rev`                  | `handle_rev_call:3178`                   | **stopgap builtin → `EVENT_REORDER`** — deferred to follow-up PRD                                 |
| `palindrome`           | `handle_palindrome_call:4493`            | **stopgap builtin → `EVENT_REORDER`** — deferred to follow-up PRD                                 |
| `ply`                  | `handle_ply_call:4560`                   | **stopgap builtin → `EVENT_FANOUT`** — deferred to follow-up PRD                                  |
| `linger`               | `handle_linger_call:4624`                | **stopgap builtin → `EVENT_FANOUT`** — deferred to follow-up PRD                                  |
| `zoom`                 | `handle_zoom_call:4693`                  | **stopgap builtin → `EVENT_REORDER`** — deferred to follow-up PRD                                 |
| `segment`              | `handle_segment_call:4764`               | **stopgap builtin → `EVENT_FANOUT`** — deferred to follow-up PRD                                  |
| `compress`             | `handle_compress_call:5487`              | **stopgap builtin → `EVENT_REORDER`** — deferred to follow-up PRD                                 |
| `iter` / `iterBack`    | `handle_iter_call:4844` / `:4905`        | **stopgap builtin → `EVENT_REORDER`** (rotation by cycle index) — deferred to follow-up PRD       |
| `bank` / `variant`     | `handle_bank_call:3738` / `:3899`        | hybrid: keep compile-time sample-ref propagation; `type_id` stamped at runtime via `EVENT_MAP`    |
| `degrade` / `mask`     | (new)                                    | **stdlib `event_filter`** (Phase 5)                                                               |
| `scale` / `key`        | (new)                                    | **stdlib `event_map`** on top of `akkado/stdlib/scales.ak` catalog (Phase 5)                      |
| `voice` / `invert`     | (new)                                    | **stdlib `event_map`** with chord-array (`notes`) DynArray return field (Phase 5)                 |

---

## 6. Error Codes (proposed)

| Code | Site                                             | Meaning                                                                                |
|------|--------------------------------------------------|----------------------------------------------------------------------------------------|
| `E170` | `event_map` / `event_filter` closure validation | Closure body invokes a stateful opcode                                                 |
| `E171` | …                                              | Closure body touches a state cell (`state` / `get` / `set`)                            |
| `E172` | …                                              | Reserved (was: nested `event_map`); nested is allowed, code reserved for future        |
| `E173` | …                                              | Closure body calls `out()` or any sink                                                 |
| `E174` | `event_map` closure return validation           | Closure body returns a non-record value, or returns a record with a field name not in the allowed slot set (`note`/`vel`/`dur`/`bend`/`at`/`time`/`chance`/`notes`/`freqs`/...) |
| `E180` | event-transform chain                           | **Reserved, not emitted.** §11 OQ-4 resolved to no hard chain-length cap; code held for a future budget guard. |
| `E181` | `EVENT_FANOUT`                                  | Output event count exceeds allocated capacity                                          |

Scale-quantize error codes (`E184`–`E186`) are owned by the sibling PRD [`prd-scale-quantize.md`](prd-scale-quantize.md). `E182` is no longer reserved by this PRD — the `EVENT_QUANTIZE` opcode it was reserved for has been dropped from the substrate.

---

## 7. Critical Files

> The cycles-pure headline (`spb`/`* 4` math) already shipped with the 2026-05-19 revert, so the files below are written in cycles-pure terms. `cycle_length` plumbing is **live and stays** (per §0.5) — none of the work below removes it. The remaining `prd-cycle-length-cleanup.md` work (MIDI `streaming` flag, clock-phase collapse) is orthogonal and may land in either order.

**Cedar (engine):**
- `cedar/include/cedar/vm/instruction.hpp` — `EVENT_MAP` (218), `EVENT_FILTER` (219), and the stopgap `EVENT_RATE_SCALE` (221), `EVENT_REORDER` (222), `EVENT_FANOUT` (223) shipped. No new opcodes for Phase 5.
- `cedar/include/cedar/opcodes/event_transforms.hpp` — `op_event_map`, `op_event_filter` (shipped); Phase 5 extends `op_event_map`'s prologue/epilogue to populate `e.notes` / `e.freqs` DynArray views and apply chord-array overlays.
- `cedar/include/cedar/opcodes/state_pool.hpp:128` — `resolve_output_events` (existing — covers transform-owned `SequenceState`s).
- `cedar/include/cedar/dsp_state.hpp` — `DSPState` variant (reuses existing `SequenceState`; no new variant).

**Akkado (compiler):**
- `akkado/include/akkado/builtins.hpp:1291-1383` — replace handler bindings for all listed modifiers.
- `akkado/src/codegen_patterns.cpp` — delete handlers for the modifiers in §5; some become small shims emitting `EVENT_*` opcodes; most disappear in favour of stdlib akkado.
- `akkado/include/akkado/typed_value.hpp:50-120` — `PatternPayload` extensions (Phase A); unify to `EventStreamPayload` (Phase B).
- `akkado/src/codegen_functions.cpp:69-733` — interplay with closure handling (depends on §0 work).
- `akkado/stdlib/event_transforms.ak` — **NEW** file: stdlib redefinitions of property modifiers.

**Web / docs:**
- `web/static/docs/concepts/` — new "event streams" concept doc.
- `web/static/docs/builtins/` — update every modifier doc page; add `event_map`, `event_filter`.
- `web/src/lib/docs/` — rebuild `lookup-index.ts` via `bun run build:docs`.
- `web/wasm/` — `bun run build:opcodes` after new opcodes land.

**Tests:**
- `cedar/tests/test_event_*.cpp` — per-opcode unit tests.
- `akkado/tests/test_event_map.cpp` — closure-lowering correctness.
- `experiments/test_op_event_*.py` — long-form WAV renders (≥300 s simulated per CLAUDE.md:338).

---

## 8. Migration Story for User Code

Every existing example, demo patch, test, and doc using a modifier in §5 will route through the new path. Because constants and signals lower through the *same* `EVENT_MAP`, **constant-arg behaviour is preserved**; only internal mechanics change.

**Compatibility risks:**
- Timing of event mutations: old compile-time path applied modifications before `cedar::Sequence` was baked; new runtime path applies them after each block's `SEQPAT_QUERY`. For pure field rewrites (transpose, velocity, dur, bend) the observable behaviour is identical. For modifiers that interacted with `cycle_length` (`fast`, `slow`, `palindrome`), behaviour must be verified case-by-case — see §11 OQ-5.
- `iter` / `iterBack` already used runtime state for per-cycle rotation; behaviour should be preserved exactly.
- `bank` / `variant`: sample-ref propagation is preserved (still compile-time); runtime `type_id` stamping must produce identical sample routing.

**Migration approach** — **hard cutover** (§11 OQ-5, resolved). One PR per phase; old compile-time modifier handlers are deleted, not deprecated. No `--legacy-modifiers` flag. Documentation tells users that any behavioral regression from the old compile-time path is a bug to file.

**Phase 2b shipped regressions** (2026-05-23). The stdlib `fn` form removed
compile-time validation surfaces that the C++ handlers carried. Documented
here so users can recognize and report them:

| Pre-Phase 2b | Phase 2b behavior |
|---|---|
| `velocity(p, 1.5)` → E131 (out of [0,1]) | compiles; stdlib fn is a passthrough |
| `transpose([…], 5)` → E133 | E184 (stream annotation rejects Array) |
| `transpose(x, 5)` for Signal-bound `x` → E133 | E184 |
| `transpose("c4 e4", 12)` → coerced to pattern | E184 (no auto-coercion) |
| `bend(p, s"bd sd")` → E160 | compiles silently |
| `transpose(s"…", 12)` → no-op | emits EVENT_MAP (audibly still a no-op since SAMPLE_PLAY reads sample_id, not note) |
| `slow(bend(p, 0.3), 2)` → bend preserved | bend silently lost (slow is still compile-time; resolves in Phase 4) |
| `swing(p)` → grid=4 default | grid=8 default (per §11 OQ-10); pass `grid: 4` to restore |
| `swingBy(p, a)` → grid=4 default | grid=8 default; same workaround |

---

## 9. Phasing

| Phase | Deliverable                                                                                                                                                                                | Tests                                                                                       |
|-------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------|
| **0a** | ~~External PRD: `cycle_length` plumbing cleanup~~ — **not a hard prerequisite** (§0.5); may land in any order, no longer gates Phase 1                                                     | Owned by `prd-cycle-length-cleanup.md`                                                      |
| **0b** | **External PRD: runtime closure / first-class fn infrastructure — ✅ SHIPPED** (L1→L3 + §4.2 `BLOCK_BIND`)                                                                                  | Shipped by `prd-runtime-functions-control-flow.md`                                          |
| **1** | Substrate: `EVENT_MAP` + `EVENT_FILTER` opcodes; manually-wired `transpose` and `velocity` in C++ codegen (no closures yet, constants only)                                                | `cedar/tests/test_event_map.cpp`, `experiments/test_op_event_map.py` (≥300 s)               |
| **2a** | `event_map` / `event_filter` builtins taking closures; Cedar `EVENT_MAP`/`EVENT_FILTER` opcode closure rework. **✅ SHIPPED 2026-05-23** (`694eb84`).                                                  | `cedar/tests/test_event_map.cpp`, `akkado/tests/test_event_map.cpp` for closure plumbing     |
| **2b** | Migrate property modifiers to `akkado/stdlib/event_transforms.ak`; delete corresponding C++ handlers. **✅ SHIPPED 2026-05-23** (`582a28a` → `b485c3f`). Migrated `transpose`, `velocity`, `dur`, `bend`, `aftertouch`, `early`, `late`, `swing`, `swingBy`. `tune` deferred (different semantics). | `akkado/tests/test_event_map.cpp [phase2b]` (11 tests); existing `[event-map]` tests updated for closure-form encoding |
| **3** | `EVENT_RATE_SCALE` opcode for `fast`/`slow` with signal input. **✅ SHIPPED 2026-05-23.** **Architecturally a stopgap** — kind-dispatched mega-opcode that the follow-up PRD will migrate to stdlib on top of an array-of-events substrate. | Rate-scaled WAV experiments                                                                 |
| **4** | `EVENT_FANOUT` + `EVENT_REORDER`: migrate `rev`, `palindrome`, `ply`, `linger`, `iter`, `iterBack`, `zoom`, `segment`, `compress`. **✅ SHIPPED 2026-05-23**. **Architecturally a stopgap** like Phase 3 — kind-dispatched mega-opcodes deferred to follow-up PRD for proper stdlib redesign. Reuses `SequenceState` as downstream holder (no new `DSPState` variant); kind selector packs into `inst.rate` low nibble; ITER direction in bit 4. iter/iterBack legacy fields (`iter_n`/`iter_dir` on SequenceState + StateInitData) deleted. | `cedar/tests/test_event_reorder.cpp` (10 cases), `cedar/tests/test_event_fanout.cpp` (7 cases), `akkado/tests/test_reorder.cpp` (32 cases) |
| **5 (corrected, 2026-05-24)** | Chord-array support inside `event_map` closures (`e.notes` / `e.freqs` read + DynArray-typed return fields for write-back); stdlib `scale` / `key` / `voice` / `invert` on top of `event_map` (no new opcodes); `web/scripts/generate-scales.ts` → `akkado/stdlib/scales.ak`; stdlib `degrade` / `mask` if `random()` and pattern-active-at-time are available; `TypedValue` Phase B cleanup (unify `EventSourcePayload` → `EventStreamPayload`). The Phase 3+4 stopgap opcodes are **not** refactored — that's [`prd-pattern-array-transforms.md`](prd-pattern-array-transforms.md). | `cedar/tests/test_event_map.cpp [chord-notes-read][chord-notes-write]`; `akkado/tests/test_scale_quantize.cpp`; scale/key/voice WAV experiments; full-suite green after `TypedValue` cleanup |

Each phase ships Catch2 tests + ≥300 s rendered-WAV experiments per CLAUDE.md.

---

## 10. Verification

End-to-end checks once Phase 5 lands:

- `cmake --build build && ./build/cedar/tests/cedar_tests "[event-transform]" && ./build/akkado/tests/akkado_tests "[event-map]" "[scale]" "[key]"` — all unit tests pass.
- `cd experiments && ./run_all.sh` — every `test_op_event_*.py` produces a clean WAV and a green pass; new ≥300 s renders for `scale`/`key`/`voice`/`invert`.
- **Manual** (web dev server): paste `n"c4 e4 g4".transpose(7).velocity(0.8) |> osc("sin", @.freq) * @.vel |> out(@)`, verify transposed playback.
- **Manual MIDI parity**: `midi("ctrl1").transpose(12) |> poly(@, instr, 8)` — confirms MIDI parity.
- **Manual scale quantize**: `n"c4 c#4 d4 d#4 e4 f4 f#4 g4" |> key("d:minor") |> osc("sin", @.freq) |> out(@)` — confirms quantization.
- **Manual chord voicing**: `n"c4 e4 g4" |> voice([0, 3, 7]) |> poly(@, instr, 8)` — confirms chord-array write from `event_map` closure.
- **Manual composability**: a user-defined akkado modifier (`fn arp_up(events, steps) = ...`) compiles, runs on patterns AND MIDI.

---

## 11. Resolved Design Decisions

All open questions were resolved 2026-05-21. The decisions below are final and locked; implementation follows them directly.

**OQ-1. Chord events vs. field arithmetic. → RESOLVED: primary-voice-only in Phase 1–2; chord-wide form kept specced, gated on Pattern Event Arrays before Phase 2.**
`(e) -> {note: e.note + 7}` transposes only the **primary voice**. Chord-wide transforms use the dynamic-array form `(e) -> {notes: map(e.notes, (v) -> v + 7)}`, where `e.notes` is the `notes(e)` accessor (UFCS sugar) from `prd-pattern-event-arrays.md` returning a `DynArray`. **Use `e.notes` consistently — no separate `e.values` spelling.** Pattern Event Arrays is NOT STARTED today; it is a soft dependency (§0.6) that must land before this PRD's **Phase 2**. Phase 1 (constant-only, primary-voice) does not need it. If Pattern Event Arrays slips past Phase 2, the chord-wide form ships in a follow-up — it does not block the rest of this PRD.

**OQ-2. Closure return-value passthrough merge. → RESOLVED: shallow overlay; unspecified fields pass through unchanged.**
The closure's returned record is a shallow overlay: each field present in the returned record overrides the matching event field; every field absent passes through from the upstream event unchanged. No deep merge, no field deletion. A closure returning `{}` is an identity transform.

**OQ-3. `bank` / `variant` runtime path. → RESOLVED: hybrid.**
`RequiredSamples` propagation stays compile-time (sample assets must load before audio starts). The event's `type_id` is **runtime-stamped** via `EVENT_MAP` plus a small `BANK_SET` helper, enabling `events.bank(other_bank_signal)`. The compile-time sample-ref propagation path is unchanged; only the per-event `type_id` field becomes runtime-writable. The §5 migration-table row for `bank`/`variant` already reflects this hybrid.

**OQ-4. Per-block OutputEvents memory budget. → RESOLVED: lazy sizing, no hard cap.**
Each `EVENT_*` opcode lazily sizes its `OutputEvents` buffer to `upstream_cap × fanout_factor`. There is **no hard chain-length limit**. Worst-case 30-chain × 4096-event ≈ 24 MB fits the 128 MB arena comfortably. `E181` (`EVENT_FANOUT` per-op capacity overflow) still guards the per-opcode bound; `E180` is reserved-but-unused (see §6) in case a global budget guard is wanted later.

**OQ-5. Migration period. → RESOLVED: hard cutover.**
Option A. One PR per phase; the old compile-time modifier handlers are **deleted**, not deprecated. No `--legacy-modifiers` flag. Documentation tells users that any behavioral regression from the old compile-time path is a bug to file. §8's Option B is dropped.

**OQ-6. Specialised codegen lowering for closed-form RHS. → RESOLVED: uniform dispatch only.**
Every `event_map` closure lowers to the same `EVENT_MAP` + `FOREACH_EVENT` dispatch path, including closed-form bodies like `e.X + const`. No specialised `TransformKind` fast path. A per-event fast path that skips the dispatcher is filed as a **future, non-blocking optimization PRD** — it is not in scope here and must not gate any phase.

**OQ-7. `fast` / `slow` and downstream `co`. → RESOLVED: Strudel semantics, locked.**
`EVENT_RATE_SCALE` rescales the cycle-phase signal feeding the upstream `SEQPAT_QUERY`. `cycle_length` is **live** — `fast`/`slow` are precisely the transforms that compute it (per §0.5), and `EVENT_RATE_SCALE` is its runtime expression. Downstream `co` (cycle offset) reports the **consumer's** cycle phase, unchanged by `fast(p, 2)`; the inner pattern simply produces 2× as many cycles' worth of events per outer cycle. This matches Strudel and is locked.

**OQ-8. Closure-PRD coupling surface. → RESOLVED: enumerated below (dependency shipped).**
This PRD consumes the following surfaces from the now-shipped `prd-runtime-functions-control-flow.md`:
- `FOREACH_EVENT` opcode — per-event subprogram dispatch; the dispatch primitive under every `EVENT_*` opcode.
- `BLOCK_CALL` / `BLOCK_BIND` opcodes + the `ProgramSlot::blocks[]` subprogram table — closure bodies compiled once, dispatched at runtime; `block_id` is the "closure handle" in §3.1's `StateInitData`.
- The block-aware program-load handshake + `set_state_id_xor` per-call-site state isolation + `block:<fn>@callsite_<N>` hot-swap path hashing.
- The event-record lambda parameter convention (`n.freq`/`n.vel`/`n.dur`/`n.note`/`n.chance`/`n.time` → convention slots; `n.gate`/`n.trig` synthesized; `E408` for unsupplyable fields).
- VM allocators VOICE_POOL / PER_ITERATION / SHARED and the `each()` / `reduce()` higher-order surface (used by chord/voicing fanout in Phase 5).
All are SHIPPED. `prd-runtime-functions-control-flow.md` §13 already cross-references this PRD as its consumer.

**OQ-9. `e.time` vs `e.cycle` field naming. → RESOLVED: `e.time` = phase, `e.cycle` = absolute.**
`e.time` is cycle phase `[0, 1)` within the current cycle (matches Strudel's `co`, matches the existing `e.time` field meaning so no rename). `e.cycle` is the absolute cycle count as a float (e.g. `3.25`). The `e.phase`/`e.t` alternatives are rejected. Locked before stdlib ships.

**OQ-10. `swing` / `swingBy` grid spec. → RESOLVED: explicit `grid:` arg, default 8.**
`swing(p, amount, grid = 8)` / `swingBy(p, amount, grid = 8)` take an explicit subdivision count, defaulting to `8` (preserves observable behaviour for typical 8-step patterns). The implicit-from-pattern and `grid_fraction` forms are rejected — explicit and predictable beats magic. Stdlib definition in §3.3.

**OQ-11. Cycles-pure-PRD coupling surface. → RESOLVED: enumerated below.**
The cycles-pure headline ("1 cycle = 1 beat", `samples_per_cycle()` without `* 4`) already shipped with the 2026-05-19 revert (commit `9d99490`). This PRD assumes:
- `spc` (samples-per-cycle) accessor and the scheduler's per-event onset-sample timestamp — used by §3.1 signal sampling at event onset.
- `e.time` = cycle phase `[0, 1)`, `e.cycle` = absolute cycle count (§3.3, OQ-9).
- `EventStreamPayload` / `EventSourcePayload` **carry `cycle_length`** — the field is live (corrected per §0.5; the earlier "dropped" premise was false).
None of this depends on `prd-cycle-length-cleanup.md`; that PRD's remaining work (MIDI `streaming` flag, clock-phase collapse) is orthogonal.

---

## 12. Next Step

Phases 1, 2a, 2b, 3, 4 are shipped. Phase 5 (corrected scope, 2026-05-24) is **in progress**:

1. ✅ **`prd-runtime-functions-control-flow.md`** (§0) — L1→L3 + §4.2 `BLOCK_BIND` SHIPPED.
2. ✅ **§11 open questions** — all 11 resolved (2026-05-21); decisions locked inline in §11.
3. ✅ **`prd-pattern-event-arrays.md`** (§0.6) — SHIPPED 2026-05-21; provides the `DynArray` value type that Phase 5's chord-array closure extension builds on.
4. **`prd-cycle-length-cleanup.md`** (§0.5) — not a hard prerequisite; may land in any order.
5. **`prd-pattern-array-transforms.md`** — NEW follow-up PRD owning the redesign of the Phase 3+4 stopgap opcodes. Not started; not blocking Phase 5.

**Implementation order for Phase 5** (see `/home/moritz/.claude/plans/phase-4-fully-unified-snowglobe.md` for the full commit-level plan):
- Commit A: PRD rewrites + follow-up PRD stub
- Commit B: verify stdlib loader handles top-level constants
- Commit C: `scales.ak` generator + catalog
- Commit D: chord-array read inside `event_map` closures (`e.notes` / `e.freqs`)
- Commit E: chord-array write from `event_map` closures (DynArray-typed return fields)
- Commit F: stdlib `key` and `scale`
- Commit G: stdlib `voice` and `invert`
- Commit H: stdlib `degrade` and `mask` (deferred — needs per-event `random()` + `pattern_active_at_time` primitives from the follow-up PRD)
- Commit I: `TypedValue` Phase B cleanup
- Commit J (followup, `e457b7c`): canonical closure-body recovery — 6 broken walkers migrated onto one structural helper, fixes the bare-identifier-body silent-failure that Commit E worked around.
- Commit K (followup, `d95aa8b`): `VM::execute_step` unifies subprogram-body dispatch so BLOCK_CALL / BLOCK_BIND / SKIP_IF / LOOP_STATIC dispatch from inside every body context, not just `execute_program`'s main loop.
- Commit L (followup, `405be8f`): `scale_quantize.ak` regenerated using shared `fn snap` / `fn pc12` helpers — Commit F's inline-math workaround retired.

Once Phase 5 ships, the next workstream is the follow-up PRD's array-of-events substrate to retire the Phase 3+4 stopgap opcodes.
