# Vision: Cycles as the Primitive

> **Status: VISION** — Forward-looking architectural direction for the pattern system.
> Not yet a PRD. Captures the paradigm shift and the data/runtime model agreed in design discussion (2026-05-26).
> Decision points marked → are settled; **open** items deferred to PRD work.

## Trigger

The current pattern model compiles mini-notation into a flat event timeline keyed by phase. That representation discards the recursive cycle structure the language actually expresses — which is why transforms compose poorly, polyrhythms / polymeters / nested subdivisions hit edge cases, and `codegen_patterns.cpp` has grown to ~6000 lines of special-case handling. The paradigm is wrong: events are leaves, but we've been treating them as the only thing that exists.

The result is a pattern system that is **inflexible, brittle, and difficult to maintain** — and a wall of open PRDs (microtonal, scale quantize, pattern transforms, patterns-as-first-class-data, runtime event transforms) that all keep tripping over the same architectural issue.

## The wrong paradigm, concretely

```akkado
n"[c4 e4] g4".fast(2)
```

Today this fights the evaluator:
- `[c4 e4]` is a subdivision that runs codegen down one path
- `g4` is a top-level cycle that runs down another
- `.fast(2)` rewrites the resulting flat event list, but the rewrite has to reason about which span semantics were active
- Polyrhythms, polymeters, chord literals, choice operators, and Euclidean patterns each add their own special-case handling in the flat-event evaluator

The recursive structure (a sub-cycle inside a top-level pattern, sped up to play twice per beat) is collapsed before transforms get to see it. Transform composition becomes a series of band-aids on the flattened representation rather than principled operations on tree structure.

## The model: cycles all the way down

Mini-notation describes a **recursive tree of cycles**. A cycle has a length and a list of events. An event has an offset, a duration, and content — which is either a leaf value (a number) or *another cycle* (a sub-pattern occupying that event's span). That's the entire data model:

```
Cycle    = { length: Beats, events: Event[] }
Event    = { offset: Beats, duration: Beats, content: Number | Cycle }
Pattern  = (i: int) -> CycleMap
CycleMap = { value: Cycle, vel?: Cycle, ... }   // value always present; others optional
```

Three things to note:

1. **Atoms are just numbers.** Pitches, sample IDs, raw values — all stored as `f32`. Their *interpretation* (Hz, MIDI note, sample index) is a query-time concern, not a data-model concern. The cycle is namespace-agnostic.

2. **Structural concepts emerge from the data, not from special node types.** A chord is multiple events sharing an offset. A polyrhythm is multiple events sharing an offset. A rest is an absent event. Elongation is an event with longer duration. These were all special cases in the old model; here they're emergent properties of the same primitive.

3. **Continuous-signal patterns aren't a separate shape.** Timeline curves (`t"…"`) become `v"…" |> interp(shape)` — a value pattern piped through an interpolation operator. The cycle data stays pure; the curve shape is a downstream signal-generation concern.

### What stores vs what projects vs what derives

The same field-access vocabulary covers three different categories of data, which were conflated in the old model:

| Category | Origin | Examples |
|---|---|---|
| **Stored** (parallel Cycles in the record) | Pattern composition introduces them | `value` (always), `vel` (optional), future: `pan`, `dur`, etc. |
| **Projected** (computed from `value` at query time) | Pattern type metadata | `freq` (mtof of `value` for note patterns), `note`, `sample_id` |
| **Derived from timing** (computed at extractor time) | Event onset/duration | `gate`, `trig` |

The vast majority of patterns are `{value: Cycle}`. Velocity, pan, and other per-event metadata appear as parallel cycles in the record only when composition introduces them. Parallel channels have **independent timing** — a vel cycle can have a different event layout than its sibling value cycle, sampled at value-event onsets.

### Music theory leaves the compiler

`mtof`, scale quantization, tuning systems, transpose, and similar concerns become **userspace stdlib transforms** over event streams. The compiler stops privileging 12-TET equal temperament. Microtonal tuning, just intonation, scale-quantized patterns, and exotic temperaments fall out for free because the cycle data is just numbers — the user gets to decide what they mean:

```akkado
n"c4 e4 g4" |> mtof(@) |> osc("sin", @)              // 12-TET sugar
n"0 2 4 5"  |> scale(@, "major", "c4") |> mtof(@) |> osc("sin", @)
n"c4 e4 g4" |> tuning(@, "just") |> osc("sin", @)    // JI in userspace
n"60 60.5 61" |> mtof(@) |> osc("sin", @)            // microtonal direct
```

This subsumes the open `prd-microtonal-extension.md`, `prd-scale-quantize.md`, and the music-theory portions of `prd-runtime-event-transforms.md`.

## Patterns are closures, transforms are macros

A Pattern is **a function**: given a cycle index, return a CycleMap. Mini-notation parse output becomes a function body that closes over the cycle's structure. The same function call site is invoked once per cycle boundary; the returned CycleMap is consumed by the walker for that cycle's playback.

Transforms — `.fast`, `.slow`, `.rev`, `.every`, `.transpose`, `.velocity`, `.bank`, `.variant`, and future combinators — are **compile-time macros** that rewrite the closure body. Two regimes:

**Static path** (no runtime state in the source pattern): the macro can fully evaluate the resulting CycleMap at compile time. A pattern like `n"c4 e4 g4".fast(2)` compiles to a function whose body is `return &baked_cycle_42`. The walk reads constant data. Zero VM work beyond a single load.

**Dynamic path** (source pattern reads `param`, signal-driven, or otherwise has runtime state): the macro rewrites the closure body to construct Cycles in arena at call time, optionally invoking the original closure and using small runtime helpers — `cycle_slice(c, start, end)`, `cycle_reverse(c)`, `cycle_scale(c, factor)`. These are inline functions or stdlib procedures, **not opcodes**.

Composition is then trivial — transforms are just function rewrites; chaining transforms is function-rewrite composition. No bespoke wrapping infrastructure. This subsumes `prd-pattern-array-transforms.md` and most of `review-patterns-as-first-class-data.md`.

## The VM lands cheap

→ **Two new opcodes:**

- `PATTERN_TICK` — invoked at cycle boundaries. Calls the pattern's L2 function with the current cycle index, captures the returned CycleMap pointer.
- `CYCLE_WALK` — given a Cycle pointer, recursively walks the tree (iteratively with a small stack), emits leaf events into the event-stream buffer with absolute offset/duration in the playback timeline.

→ **L2 BLOCK_CALL reused.** Pattern functions are L2 closures. No new calling convention.

→ **Cycle storage: arena per invocation.** Allocate from a per-cycle bump arena, freed when the cycle's playback completes. ~16 bytes per event, ~32 bytes per nested cycle header. Hundreds of events per cycle is trivial cost. Static patterns bake to constant Cycles in bytecode; the function body returns a pointer to bake data and the walk reads it directly. No allocation churn in the common case.

→ **Extractors stay.** The existing `SEQPAT_*` extractors are renamed/repurposed to consume the event-stream buffer that `CYCLE_WALK` produces. Three flavors:
- Read stored data: `SEQPAT_VALUE`, `SEQPAT_VEL` (reads from named cycle in the record)
- Project from `value`: `SEQPAT_FREQ`, `SEQPAT_NOTE`, `SEQPAT_SAMPLE_ID` (consult pattern kind metadata)
- Derive from timing: `SEQPAT_GATE`, `SEQPAT_TRIG`

### What retires

- `MIDI_QUERY` opcode (formerly `SEQ_STEP`)
- The flat-event evaluator (`pattern_eval.cpp` becomes much smaller or goes away)
- ~6000 lines of `codegen_patterns.cpp` special-case handling for nodes, modifiers, polyrhythms, polymeters, Euclidean, choice
- Compile-time chord expansion as a leaf-atom phenomenon (chord literals become parser sugar emitting multiple events at offset 0)
- Compile-time mtof in the parser (notes stored as numbers; conversion is userspace)

### What changes vs stays for the editor / web UI

The pattern debug surfaces (`pattern_debug.hpp`, `serialize_mini_ast_json`, the Panel/PatternDebugPanel) become **richer**: the AST is now the same structure as the runtime Cycle tree. AST visualization, event preview, and live state inspection converge on a single representation.

## Migration sketch (vision-level only)

This is a frontend rewrite, not a VM overhaul. The migration moves in three stages:

1. **Build the new pipeline alongside the old.** New opcodes (`PATTERN_TICK`, `CYCLE_WALK`), arena Cycle representation, closure-based codegen for mini-notation. Behind a feature gate.

2. **Port mini-notation node-by-node.** Atoms, subdivisions, top-level alternation, polyrhythm/parallel, polymeter, Euclidean, choice, modifiers. Run both pipelines in parallel for testing; assert bytecode-output equivalence on the existing fixture suite where semantics overlap.

3. **Retire the flat-event path.** Delete `pattern_eval.cpp` evaluator, `MIDI_QUERY` opcode, and the bulk of `codegen_patterns.cpp`. Userspace stdlib gains `mtof`, `scale`, `tuning`, `quantize`, `interp`, `interp_smooth`, etc. as event-stream transforms.

Downstream consumers (extractors, the audio graph, hot-swap state preservation, the web UI's pattern debug) are touched lightly — their interface is the event-stream buffer, which is largely the same shape on both sides of the migration.

## What this subsumes

These open PRDs and reviews collapse partially or fully into this work:

- `prd-pattern-array-transforms.md` — transforms become compile-time macros over closures
- `prd-pattern-event-arrays.md` — events live in arena Cycles, decomposable
- `prd-patterns-as-scalar-values.md` — patterns are L2 functions / closures
- `prd-runtime-event-transforms.md` — phases 2-5 may become trivial
- `prd-microtonal-extension.md` — falls out from value-numbers + userspace mtof
- `prd-scale-quantize.md` — stdlib transform
- `prd-mini-notation-micro-timing.md` — duration is first-class per-event
- `review-patterns-as-first-class-data.md` — most gaps close
- Portions of `prd-cycle-length-cleanup.md` and `prd-beats-per-cycle.md`

The PRD for this work will need to coordinate with the maintainers of those documents and explicitly mark which ones close.

## Out of scope for this vision

These decisions are deferred to the PRD:

- Concrete bytecode layout for `PATTERN_TICK` and `CYCLE_WALK`
- The exact L2 function calling convention for returning a CycleMap pointer
- The full set of compile-time transform macros and their semantics (especially `slow`, which involves Cycle slicing)
- RNG seeding strategy for `MiniChoice` (deterministic via cycle index hash?)
- The static-bake optimizer's exact triggers and code paths
- Interp shape primitives for `t"…"` migration (linear, smooth, cubic, step)
- Hot-swap state preservation through pattern-structure changes (the recursive tree gives a natural structural-hash story)
- Sample-bank resolution timing (parse-time vs late-binding)
- Channel-set in the CycleMap beyond `value` / `vel` — what's the full canonical set?

## Decisions settled in this discussion

1. **Cycles are the primitive.** Events are leaves; sub-cycles are events whose content is another cycle. → Settled
2. **Atoms are just numbers.** No Pitch/Sample/Chord/Rest/Elongate type variants. → Settled
3. **Pattern = closure** `(cycle_index) -> CycleMap`. → Settled
4. **Output is a record of parallel cycles** with `value` always present and optional named channels. → Settled
5. **Stored / projected / derived** are three distinct categories — only stored data lives in the cycle data model. → Settled
6. **Music theory is userspace.** mtof, scale, tuning, etc. become stdlib transforms; compiler is theory-agnostic. → Settled
7. **Parallel channels have independent timing.** Each is its own Cycle; vel sampled at value-event onsets. → Settled
8. **Transforms are compile-time macros.** Function-body rewriting; no transform opcodes; runtime helpers only when source is dynamic. → Settled
9. **VM surface: 2 new opcodes + L2 reuse.** `PATTERN_TICK` + `CYCLE_WALK`. → Settled
10. **Storage: arena per invocation, static patterns bake to constants.** → Settled

## Next step

A PRD that takes this vision and turns it into a concrete migration plan: opcode specifications, calling convention, transform macro semantics (especially `slow`), fixture parity strategy, deletion targets, and the coordination plan for the dependent PRDs listed above.
