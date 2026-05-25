> **Status: DRAFT — 7 phases.** Filed 2026-05-26 as the codegen-
> monolith follow-up to
> [`docs/audits/parser-codegen-interop_audit_2026-05-25.md`](audits/parser-codegen-interop_audit_2026-05-25.md).
> Sibling PRDs:
> [`docs/prd-parser-codegen-correctness.md`](prd-parser-codegen-correctness.md)
> (in flight) covers the 6 critical correctness findings, and
> [`docs/prd-parser-codegen-hardening.md`](prd-parser-codegen-hardening.md)
> (also filed 2026-05-26) covers front-end hardening + parallelism
> prep. **This PRD** owns the audit's codegen-monolith findings: F4
> (visit() Call branch — 1,180 LOC in a single switch arm), F9
> (pattern-transform handler clones — ~1,500 LOC of near-duplicates),
> F10 (StateInitData manual construction at 19 sites), and the
> viz/param family data-driven collapse (PRD-7 portion: codegen_viz.cpp
> 417 LOC → ~100, codegen_params.cpp 416 LOC → ~120).
>
> **Status: DRAFT.** Outline is complete; each phase needs reviewer
> sign-off on the proposed file layout + builder API surface before
> implementation begins. Phases land independently after Phase 1
> ships.
>
> **Total LOC reduction target: ~4,500 LOC** across `codegen*.cpp`
> (audit's stated payoff). Single largest pure-cleanup in the front-
> end.

# PRD: Codegen Sprawl Cleanup

## Executive Summary

`akkado/src/codegen*.cpp` is the front-end's accretion hot zone — 110
commits to `codegen.cpp` alone, 83 to `codegen_patterns.cpp`, 41 to
`codegen_functions.cpp` (82% of all codegen commits in three files
per audit §3.4). The audit identified four major monolithic patterns
that consume more codegen LOC than any other single source of bloat
in the compiler:

1. **`visit()` Call branch is 1,180 LOC** inside a single switch arm
   (`codegen.cpp:966-2143`). 7 inlined sub-paths (default-fill,
   chord-expand, stereo-native, SAMPLE_PLAY scalar, FM-detection,
   ADSR/delay rate-field special-casing, generic). A 100-entry
   `special_handlers` static map. Every new builtin adds a row to this
   monolith.
2. **Pattern-transform handlers are ~1,500 LOC of near-clones.**
   `handle_bank_call` / `handle_variant_call` / `handle_transport_call`
   / `handle_tune_call` / 8+ others. Same 30-line block thrice,
   differing only in the inner mutator (`codegen_patterns.cpp:3583-4908`).
3. **`StateInitData` constructed field-by-field at 19 sites.** 14 in
   `codegen_patterns.cpp` alone. Every new `StateInitData::Type` field
   requires touching all 19 sites; missing one silently breaks one
   transform family.
4. **Viz + param families repeat the same 6-step emit pattern.**
   `codegen_viz.cpp` (417 LOC) has 5 handlers each implementing
   validate→visit→name→options→state_id→PROBE. `codegen_params.cpp`
   (416 LOC) has 4 handlers in the same shape. Both collapse to a
   single data-driven emitter + `BuiltinInfo.kind` tag.

Plus: **606 bare `0xFFFF` sentinels** across the 10 codegen `.cpp`
files; **177 buffer-allocation failure blocks** each ~4 lines repeating
the same error-emit pattern; **126 `cedar::Instruction X{}`
declarations** without a builder. These are bundled into an
**`InstructionBuilder`** that absorbs the boilerplate.

**Key Design Decisions (locked unless flagged [OPEN]):**

- **Builders first, file split second, handler consolidation third.**
  `InstructionBuilder` and `StateInitBuilder` ship before the file
  split because (a) they shrink every site that the file split
  touches, reducing diff churn, and (b) they're the right abstraction
  to use inside the new file structure from day one.
- **`special_handlers` table → `BuiltinInfo::codegen_handler` member
  function pointer.** The 100-entry static map in `codegen.cpp:1068`
  goes away; per-builtin dispatch lives on the `BuiltinInfo` itself.
  Adding a new special-handler becomes a one-line addition.
- **One file per concern, not per legacy split.** Audit's proposed
  layout (`codegen_visit_dispatch.cpp`, `codegen_call_dispatch.cpp`,
  `codegen_bus.cpp`, `codegen_records.cpp`) is the target. Existing
  `codegen_patterns.cpp` further splits into `pattern_compiler.cpp`,
  `codegen_pattern_transforms.cpp`, `codegen_pattern_io.cpp`.
- **`PatternTransformEmitter` is the canonical helper for pattern-
  transform handlers.** Takes `(transform_name, payload_mutator,
  BuiltinInfo*)` and a per-transform lambda. ~900 LOC reduction.
- **`BuiltinInfo::kind` enum classification.** New enum:
  `Function | Visualization | Param | PatternTransform | StereoNative |
  …`. The Call branch reads `info->kind` and dispatches to one of N
  emitters. Eliminates the inlined-into-Call-branch sub-paths.
- **`emit_extended_params_init` is the precedent.** Audit calls out
  `codegen.cpp:35-73` (the only existing centraliser) as bug-free.
  Phase 1 generalises its pattern.
- **No bytecode change.** Every phase ships byte-identical output
  (verified via the correctness PRD's Phase 0 snapshot harness).
- **`inst.rate` stays reserved for its legitimate uses** (audio-rate
  vs control-rate, small fixed enums ≤4 values, compile-time count
  fields). No new runtime-tunable params bit-packed into `inst.rate`.
  Existing offenders (compressor attack/release, freeverb damping,
  ping-pong mix, etc.) are listed in this PRD's appendix as future
  per-family migration follow-ups, **not** in scope here.

---

## 1. Problem Statement / Current State

### 1.1 F4 — `visit()` Call branch is 1,180 LOC

Site: `codegen.cpp:966-2143` — `case NodeType::Call:` inside the
2,059-line `visit()` function at `codegen.cpp:203-2261`.

Inside the Call branch:

| Sub-path | Approximate line span |
|---|---|
| `special_handlers` lookup + dispatch | 1068-1180 |
| Default-fill PUSH_CONST blocks (5 copies) | 1335, 1761, 1862, 1989, +616 (BUILTIN_VARIABLES) |
| Spread expansion | (calls into `expand_call_arguments`, now read-only post correctness Phase 1a) |
| Chord-expansion branch | 1600-1700ish |
| Stereo-native branch | 1700-1830 (~130 LOC) |
| SAMPLE_PLAY scalar branch | 1830-1900 |
| FM-detection retrofit | 1900-1989 |
| ADSR/delay rate-field special-casing | 1989-2050 |
| Generic emission tail | 2050-2143 |

Other monoliths in `codegen.cpp`:

- `emit_bus_epilogue` — 234 LOC (`:2739`).
- `handle_field_access` — 202 LOC (`:3488`).
- `handle_record_literal` — 113 LOC (`:3375`).
- `reorder_spread_named_args` — 146 LOC (`:3230`), to be consolidated
  by `prd-parser-codegen-hardening.md` Phase 7 — but the leftover
  shape lives here.

### 1.2 F9 — Pattern-transform boilerplate (~1,500 LOC)

Sites in `codegen_patterns.cpp`:

| Handler | LOC | Line |
|---|---|---|
| `handle_bank_call` | 161 | :3583 |
| `handle_variant_call` | 213 | :3744 |
| `handle_transport_call` | 193 | :3957 |
| `handle_tune_call` | 71 | :4150 |
| `handle_palindrome_call` | ~30 | :4221 |
| `handle_compress_call` | ~50 | :4280 |
| `handle_zoom_call` | ~45 | :4350 |
| `handle_segment_call` | ~40 | :4420 |
| `handle_iter_call` | ~50 | :4500 |
| `handle_iterBack_call` | ~50 | :4580 |
| `handle_anchor_call` | ~40 | :4670 |
| `handle_mode_call` | ~50 | :4750 |
| `handle_voicing_call` | ~110 | :4810 |

Shared shape (every one of the above):

```
get_pattern_arg → compile_pattern_for_transform → push_path → allocate
value/velocity/trigger triple → emit SEQPAT_QUERY → emit SEQPAT_STEP →
push StateInitData.
```

Compare `codegen_patterns.cpp:3636-3660` vs `:3795-3820` vs
`:3974-4000` — same 30-line block thrice, differing only in the inner
mutator.

Each accreted "we forgot to copy field X across all handlers" follow-up
commit (`b485c3f`, `c56e65f`, `c3802b6`, `2f710ef` per audit) becomes
impossible after consolidation.

### 1.3 F10 — `StateInitData` manual construction at 19 sites

19 `state_inits_.push_back` calls total; 14 in `codegen_patterns.cpp`
alone. Representative duplication:

- `codegen_patterns.cpp:1369-1380` (SequenceProgram) is byte-for-byte
  repeated at `:1719`, `:1869`, `:2867`, `:3685`, `:3900`, `:4118`.
- `codegen_higher_order.cpp:787-793` (EventTransform).
- RateScale, Reorder, Fanout, Timeline all replicate the same
  boilerplate per `StateInitData::Type`.

Every commit that adds a field to `StateInitData` must touch all 19
sites; missing one silently breaks one transform family. The audit
points at `emit_extended_params_init` (`codegen.cpp:35-73`) as the
existing centraliser — bug-free precedent.

### 1.4 Instruction-emission boilerplate (audit §3.4 InstructionBuilder)

Counts across the 10 codegen .cpp files:

- 126 sites with `cedar::Instruction X{}` declarations.
- 606 bare `0xFFFF` sentinels.
- 48 manual `inputs[0] = 0xFFFF` inits.
- 177 `buffers_.allocate()` sites each followed by ~4 lines of:

  ```cpp
  if (out == BUFFER_UNUSED) {
      error("E101", "Buffer pool exhausted", …);
      return …;
  }
  ```

- The `"Buffer pool exhausted"` string literal appears **167 times**.
- `set_unused_inputs()` helper at `helpers.hpp:156` is called **once**.
- `Instruction::make_unary` / `make_binary` factories are used only
  for COPY (~25 sites).

### 1.5 Viz + param family sprawl (audit §3.4 PRD-7 portion)

`codegen_viz.cpp` (417 LOC) has 5 handlers (`handle_pianoroll_call`,
`handle_oscilloscope_call`, `handle_waveform_call`,
`handle_spectrum_call`, `handle_waterfall_call`), all the same shape:

```
validate signal arg → visit → name → options → push_path → state_id →
push VisualizationDecl → emit PROBE
```

`codegen_params.cpp` (416 LOC) has 4 handlers (param/button/toggle/
select) in a structurally identical shape with different `ParamDecl`
types.

Both collapse to a single emitter parameterised over
`BuiltinInfo::kind` + per-kind metadata.

### 1.6 File layout inconsistency (audit §3.4)

Two parallel layouts coexist:

- `akkado/include/akkado/codegen/` — subdirectory with 6 small
  `inline` helper headers.
- `akkado/include/akkado/codegen.hpp` — 1,423 LOC monolith.
- `akkado/src/codegen_*.cpp` — flat layout.

`codegen/literals.hpp` is entirely dead (per audit §3.1, deleted by
hardening PRD Phase 1). Some `.cpp` files (e.g. `codegen_state.cpp`)
include none of the `codegen/*` helper headers.

Either fold everything in `codegen/` back into `codegen.hpp` or move
all sources under `src/codegen/`. **Decision pending: this PRD's
Phase 3 picks the canonical layout in the reviewer round.**

---

## 2. Goals and Non-Goals

### Goals

1. **`visit()` is ≤ 250 LOC; Call branch ≤ 80 LOC + dispatch table.**
   Sub-paths split into named per-builtin-kind emitters.
2. **`PatternTransformEmitter` exists; pattern-transform handlers
   reduce from ~1,500 LOC to ~600 LOC.**
3. **`StateInitBuilder` exists; every `state_inits_.push_back` goes
   through it.** Adding a field to a `StateInitData::Type` becomes a
   one-line builder method addition.
4. **`InstructionBuilder` exists; instruction-allocation boilerplate
   reduces from ~177 sites × ~6 LOC to ~177 sites × 1 LOC.** Single
   buffer-allocation-failure path.
5. **`special_handlers` table is gone**; per-builtin dispatch lives on
   `BuiltinInfo::codegen_handler`.
6. **Viz + param handlers reduce from 833 LOC combined to ~220 LOC
   combined.** Data-driven dispatch via `BuiltinInfo::kind`.
7. **`codegen.cpp` reduces from 3,898 LOC to ~600 LOC.**
   `codegen_patterns.cpp` reduces from 5,953 LOC to ~3,000 LOC.
   Combined codegen reduction target: ~4,500 LOC.
8. **No bytecode change.** Snapshot harness reports byte-identical
   output for every fixture after every phase.

### Non-Goals

- **Critical correctness findings** (F1, F2, F3, F7, F8, F12, F14) —
  handled by `prd-parser-codegen-correctness.md`.
- **Front-end hardening + parallelism prep** (F6, F11, F13, F15, §3.3,
  §3.5) — handled by `prd-parser-codegen-hardening.md`.
- **`inst.rate` deprecation for runtime-tunable params.** Existing
  offenders are listed in the appendix as future per-family
  migrations.
- **Cedar VM changes.** All instructions stay byte-identical; only
  the C++ that emits them is refactored.
- **Per-statement parallel codegen.** `node_types_` dual-role split
  is a separate future PRD (catalogued by hardening PRD).

---

## 3. Architecture / Technical Design

### 3.1 `InstructionBuilder`

```cpp
// New: akkado/include/akkado/codegen/instruction_builder.hpp
namespace akkado::codegen {

class InstructionBuilder {
public:
    explicit InstructionBuilder(cedar::Opcode op);

    /// Set an input buffer at slot N (0-4). Defaults to 0xFFFF (unused)
    /// for any slot not set.
    InstructionBuilder& input(int slot, std::uint16_t buf);
    InstructionBuilder& inputs(std::initializer_list<std::uint16_t> bufs);

    InstructionBuilder& output(std::uint16_t buf);
    InstructionBuilder& rate(std::uint8_t r);
    InstructionBuilder& state_id(std::uint16_t id);
    InstructionBuilder& imm_f(float f);
    InstructionBuilder& imm_i(std::int32_t i);

    /// Emit through CodeGenerator (which enforces source_locations parity
    /// per correctness PRD Phase 3). Allocates the output buffer via the
    /// generator's allocator; on failure, emits the E101 diagnostic and
    /// returns BUFFER_UNUSED.
    std::uint16_t emit(CodeGenerator& cg);

private:
    cedar::Instruction inst_{};
};

} // namespace akkado::codegen
```

Usage (today → after):

```cpp
// Before:
cedar::Instruction inst{};
inst.opcode = cedar::Opcode::SIN;
inst.inputs[0] = freq_buf;
inst.inputs[1] = 0xFFFF; inst.inputs[2] = 0xFFFF;
inst.inputs[3] = 0xFFFF; inst.inputs[4] = 0xFFFF;
inst.output = buffers_.allocate();
if (inst.output == BUFFER_UNUSED) {
    error("E101", "Buffer pool exhausted", loc);
    return BUFFER_UNUSED;
}
emit(inst);
return inst.output;

// After:
return InstructionBuilder(cedar::Opcode::SIN)
    .input(0, freq_buf)
    .emit(*this);
```

Single buffer-allocation-failure path: `InstructionBuilder::emit`
allocates, emits E101 via the generator if needed, returns
`BUFFER_UNUSED` on failure. The 167 repeated `"Buffer pool exhausted"`
literals collapse to one site.

### 3.2 `StateInitBuilder`

```cpp
// New: akkado/include/akkado/codegen/state_init_builder.hpp
namespace akkado::codegen {

class StateInitBuilder {
public:
    /// Type-specific factory methods — one per StateInitData::Type.
    /// Each returns a typed sub-builder with fluent setters.
    static SequenceProgramBuilder sequence_program(std::uint16_t state_id);
    static EventTransformBuilder  event_transform (std::uint16_t state_id);
    static RateScaleBuilder       rate_scale      (std::uint16_t state_id);
    static ReorderBuilder         reorder         (std::uint16_t state_id);
    static FanoutBuilder          fanout          (std::uint16_t state_id);
    static TimelineBuilder        timeline        (std::uint16_t state_id);
    static MiniLiteralBuilder     mini_literal    (std::uint16_t state_id);
    static ExtendedParamsBuilder  extended_params (std::uint16_t state_id);
    // … one per StateInitData::Type variant arm
};

class SequenceProgramBuilder {
public:
    SequenceProgramBuilder& cycle_length(float c);
    SequenceProgramBuilder& sequences(std::vector<SequenceData> s);
    SequenceProgramBuilder& transforms(std::vector<TransformSpec> t);
    // … fluent setters per field

    void publish(CodeGenerator& cg);   // Pushes onto cg.state_inits_
};
```

Usage (today → after):

```cpp
// Before (one of 7 byte-for-byte copies of this block in codegen_patterns.cpp):
StateInitData init{};
init.type = StateInitData::Type::SequenceProgram;
init.state_id = state_id;
init.sequence_program.cycle_length = cycle_len;
init.sequence_program.sequences   = std::move(sequences);
init.sequence_program.transforms  = std::move(transforms);
// (continues with ~10 more field assignments) …
state_inits_.push_back(std::move(init));

// After:
StateInitBuilder::sequence_program(state_id)
    .cycle_length(cycle_len)
    .sequences  (std::move(sequences))
    .transforms (std::move(transforms))
    .publish(*this);
```

`emit_extended_params_init` (`codegen.cpp:35-73`) becomes a one-line
wrapper around `StateInitBuilder::extended_params(...).publish(*this)`.

### 3.3 `BuiltinInfo::kind` + `codegen_handler`

```cpp
// In akkado/include/akkado/builtins.hpp
enum class BuiltinKind : std::uint8_t {
    Function,           // Default — plain UGen / arithmetic / etc.
    StereoNative,       // Returns stereo (pair of buffers)
    Visualization,      // pianoroll, oscilloscope, waveform, spectrum, waterfall
    Param,              // param, button, toggle, dropdown/select
    PatternTransform,   // bank, variant, transport, tune, palindrome, …
    SampleScalar,       // sample("name", …) scalar form
    Sequencer,          // seq, timeline, pat
    Bus,                // bus, mixer
    Special,            // [OPEN] tentatively retained for chord-expand /
                        //        FM-detection cases — TBD in Phase 4 PR
                        //        review
};

using CodegenHandler = std::uint16_t (CodeGenerator::*)(
    NodeIndex call_node,
    const BuiltinInfo& info);

struct BuiltinInfo {
    // … existing fields …
    BuiltinKind     kind             = BuiltinKind::Function;
    CodegenHandler  codegen_handler  = nullptr;   // nullptr → default per-kind emitter
};
```

The 100-entry `special_handlers` table in `codegen.cpp:1068` is
deleted. Each builtin that needs custom codegen sets its
`codegen_handler` member directly in `BUILTIN_FUNCTIONS`. The Call
branch dispatch becomes:

```cpp
if (info->codegen_handler) {
    return (this->*info->codegen_handler)(call_node, *info);
}
switch (info->kind) {
    case BuiltinKind::Visualization:    return emit_visualization(call_node, *info);
    case BuiltinKind::Param:            return emit_param        (call_node, *info);
    case BuiltinKind::PatternTransform: return emit_pattern_transform(call_node, *info);
    case BuiltinKind::StereoNative:     return emit_stereo_native(call_node, *info);
    case BuiltinKind::Sequencer:        return emit_sequencer    (call_node, *info);
    case BuiltinKind::Bus:              return emit_bus_call     (call_node, *info);
    case BuiltinKind::SampleScalar:     return emit_sample_scalar(call_node, *info);
    case BuiltinKind::Function:
    default:                            return emit_function     (call_node, *info);
}
```

### 3.4 `PatternTransformEmitter`

```cpp
// New: akkado/src/codegen/pattern_transform_emitter.cpp
namespace akkado {

/// Per-transform configuration consumed by the canonical emitter.
struct PatternTransformConfig {
    const char* transform_name;   // "bank", "variant", "tune", etc.
    cedar::Opcode query_opcode;   // SEQPAT_QUERY_BANK / _VARIANT / etc.
    /// Mutator: takes the prepared TransformSpec scaffolding and fills in
    /// the per-transform payload.
    std::function<void(TransformSpec& spec, NodeIndex arg)> payload_mutator;
};

/// Emit a pattern-transform call. Used by all 13 handlers in
/// codegen_patterns.cpp.
std::uint16_t emit_pattern_transform(
    CodeGenerator& cg,
    NodeIndex call_node,
    const PatternTransformConfig& config,
    const BuiltinInfo& info);

} // namespace akkado
```

The 13 `handle_*_call` handlers each shrink to ~10 lines of config
setup + a single call to `emit_pattern_transform`.

### 3.5 Viz + param data-driven emitters

```cpp
// In BuiltinInfo
struct VisualizationMeta {
    cedar::VisualizationType viz_type;   // Pianoroll, Oscilloscope, …
    int signal_input_slot;               // Which input is the signal
    bool stereo_signal;                  // Pair of signal slots?
};

struct ParamMeta {
    ParamDecl::Kind decl_kind;           // Slider, Button, Toggle, Select
    int name_arg_slot;                   // Where the "name" string lives
    int default_arg_slot;                // Where the default value lives
};

struct BuiltinInfo {
    // …
    std::optional<VisualizationMeta> viz_meta;
    std::optional<ParamMeta>         param_meta;
};
```

`emit_visualization(call_node, info)` reads `info.viz_meta`, runs the
canonical 6-step emit pattern. `emit_param(call_node, info)` reads
`info.param_meta`, runs the canonical 4-step emit pattern.

`codegen_viz.cpp` (417 LOC) collapses to a single ~80 LOC emitter +
~20 LOC per-builtin metadata in `builtins.hpp`. `codegen_params.cpp`
(416 LOC) similarly.

### 3.6 File layout decision

**[OPEN — Phase 3 reviewer round]** Two options:

**Option A: subdir layout** — move all sources under `src/codegen/`:

```
akkado/src/codegen/
  visit_dispatch.cpp    (outer switch + literal handlers, ~250 LOC)
  call_dispatch.cpp     (Call branch + per-kind emitters, ~400 LOC)
  bus.cpp               (handle_bus_call, handle_mixer_call,
                         inline_mixer_closure, scan_closure_for_sinks,
                         emit_bus_epilogue, ~620 LOC)
  records.cpp           (handle_record_literal, handle_field_access,
                         pipe-binding, destructure, ~750 LOC)
  pattern_compiler.cpp  (SequenceCompiler, ~1199 LOC)
  pattern_transforms.cpp(handle_*_call + PatternTransformEmitter,
                         ~700 LOC)
  pattern_io.cpp        (midi, soundfont, smooch, wt_load, samples,
                         ~500 LOC)
  higher_order.cpp      (emit_event_transform split into 3-4 stages,
                         ~500 LOC)
  arrays.cpp            (apply_lambda + array ops, ~800 LOC)
  stereo.cpp            (handle_pingpong + stereo handlers, ~700 LOC)
  state.cpp             (~500 LOC)
  control_flow.cpp      (~400 LOC)
  viz.cpp               (~80 LOC after collapse)
  params.cpp            (~100 LOC after collapse)
  functions.cpp         (~3000 LOC; future split out of scope)
```

**Option B: flat layout, keep names** — rename existing files to make
the split clearer (`codegen_visit_dispatch.cpp`, etc.) but keep them
all flat in `akkado/src/`.

Option A is the cleaner long-term home; Option B is less disruptive
to existing tooling (find/grep, IDE bookmarks). PR reviewer picks one
and the rest of this PRD adapts in the implementation phase.

---

## 4. Per-Phase Implementation Detail

### Phase 1 — `InstructionBuilder` (~1 week)

**Scope.** Introduce `InstructionBuilder`. Single buffer-allocation-
failure path. Migrate all ~126 instruction-emission sites.

**Files touched.**

- NEW `akkado/include/akkado/codegen/instruction_builder.hpp` +
  `.../src/codegen/instruction_builder.cpp`.
- Mechanical rewrites in every `codegen*.cpp` (126 sites). Each commit
  should be ≤ 10 site migrations to keep diffs reviewable.
- `akkado/tests/test_codegen.cpp` — unit tests for builder + smoke
  test that bytecode is byte-identical.

**Exit criteria.**

- `grep -n '0xFFFF' akkado/src/codegen` returns ≤ 50 hits (down from
  606).
- `grep -n '"Buffer pool exhausted"' akkado/src/codegen` returns ≤ 5
  hits (down from 167; some left for non-builder paths to be migrated
  in Phase 2).
- Snapshot harness byte-identical.

---

### Phase 2 — `StateInitBuilder` (~1 week)

**Scope.** Introduce `StateInitBuilder` with one factory per
`StateInitData::Type`. Migrate all 19 `state_inits_.push_back` sites.

**Files touched.**

- NEW `akkado/include/akkado/codegen/state_init_builder.hpp` +
  `.../src/codegen/state_init_builder.cpp`.
- 19 site migrations across `codegen.cpp`, `codegen_patterns.cpp`,
  `codegen_higher_order.cpp`, `codegen_stereo.cpp` (per audit F10).
- `emit_extended_params_init` becomes a one-line builder wrapper.

**Exit criteria.**

- `grep -n 'state_inits_.push_back' akkado/src/codegen` returns one
  hit (inside `StateInitBuilder::publish`).
- Adding a new field to e.g. `SequenceProgram` requires touching one
  builder method + one site, not 7.
- Snapshot harness byte-identical.

---

### Phase 3 — Codegen file split + layout decision (~2 weeks)

**Scope.** Pick the file layout (Option A or B in §3.6). Split
`codegen.cpp` (3,898 LOC) and `codegen_patterns.cpp` (5,953 LOC) per
the chosen layout. **No behavior change**; pure restructure.

**Approach.** One commit per file extraction. Each commit:

1. Cuts the targeted functions out of the source file.
2. Pastes into the new file.
3. Adds the necessary includes.
4. Verifies build + snapshot harness byte-identical.

**Files touched.** Per §3.6 (Option A or B).

**Exit criteria.**

- `codegen.cpp` reduces to ≤ 600 LOC (per audit's "~250 lines remain"
  target — conservative ceiling at 600).
- `codegen_patterns.cpp` reduces to ≤ 1,300 LOC (the SequenceCompiler
  + minimal residue).
- All `codegen/` source files have a clear single concern visible in
  the filename.
- Snapshot harness byte-identical.

---

### Phase 4 — `visit()` Call branch refactor + `BuiltinInfo::kind` (~2 weeks)

**Scope.** Replace the 100-entry `special_handlers` static map with
`BuiltinInfo::codegen_handler` member pointers. Add `BuiltinInfo::kind`.
Split the 1,180-LOC Call branch into per-kind emitters
(`emit_visualization`, `emit_param`, `emit_pattern_transform`,
`emit_stereo_native`, `emit_sequencer`, `emit_bus_call`,
`emit_sample_scalar`, `emit_function`).

**Files touched.**

- `akkado/include/akkado/builtins.hpp` — add `BuiltinKind`,
  `CodegenHandler`, populate `kind` / `codegen_handler` on every entry
  (data migration; ~250 LOC additions).
- `akkado/src/codegen/call_dispatch.cpp` (NEW per Phase 3) — house
  the per-kind emitters.
- `akkado/src/codegen/visit_dispatch.cpp` — Call branch reduces to
  ~80 LOC of dispatch logic.
- Delete `special_handlers` map from `codegen.cpp:1068`.

**Exit criteria.**

- `grep -n 'special_handlers' akkado/src/codegen` returns zero hits.
- Per-builtin custom codegen path is one line in `BUILTIN_FUNCTIONS`,
  not "edit a separate dispatch table".
- The Call branch handler stack is named, navigable, and ≤ 80 LOC at
  the dispatch site.
- Snapshot harness byte-identical.

---

### Phase 5 — Pattern-transform consolidation (~2 weeks)

**Scope.** Introduce `PatternTransformEmitter`. Migrate all 13
`handle_*_call` pattern-transform handlers (bank/variant/transport/
tune/palindrome/compress/zoom/segment/iter/iterBack/anchor/mode/
voicing).

**Files touched.**

- `akkado/src/codegen/pattern_transforms.cpp` (per Phase 3 layout) —
  add the `PatternTransformEmitter`; collapse handlers.
- `akkado/include/akkado/builtins.hpp` — each pattern-transform
  builtin gets a `kind = PatternTransform` tag + per-transform
  metadata (e.g. opcode, payload-mutator pointer).

**Exit criteria.**

- Combined pattern-transform handler LOC reduces from ~1,500 to ~600
  (recorded in PR description).
- Adding a new pattern transform is a ~10-line addition (one
  `BuiltinInfo` entry + one `PatternTransformConfig` registration).
- Snapshot harness byte-identical.

---

### Phase 6 — Viz + param family data-driven dispatch (PRD-7 portion, ~1 week)

**Scope.** Collapse `codegen_viz.cpp` (417 LOC) + `codegen_params.cpp`
(416 LOC) using `BuiltinInfo::kind = Visualization | Param` +
`viz_meta` / `param_meta` metadata. Single per-kind emitter for each.

**Files touched.**

- `akkado/src/codegen/viz.cpp` (per Phase 3 layout) — ~80 LOC
  emitter.
- `akkado/src/codegen/params.cpp` — ~100 LOC emitter.
- `akkado/include/akkado/builtins.hpp` — populate `viz_meta` /
  `param_meta` on the 9 affected builtins.

**Exit criteria.**

- Combined viz + param LOC reduces from 833 to ~180 (recorded).
- Adding a new visualizer or param control is a ~5-line
  `BUILTIN_FUNCTIONS` entry, no new C++ handler.
- Snapshot harness byte-identical.

---

### Phase 7 — Final pass + audit close-out (~1 week)

**Scope.** Wrap up any remaining codegen-monolith items not absorbed
by Phases 1-6:

- `emit_bus_epilogue` (234 LOC) — split into named sub-helpers.
- `handle_field_access` (202 LOC) — split.
- `handle_record_literal` (113 LOC) — split.
- `emit_event_transform` (`codegen_higher_order.cpp:436-820`, 385 LOC)
  — split into 3-4 stages.
- `apply_lambda` cache-thrash (`codegen_arrays.cpp:76-230`) — preserve
  per-iteration `node_types_` correctness but stop the
  move/clear/restore thrash by scoping a per-lambda overlay.
- Audit-doc close-out edits per `prd-parser-codegen-hardening.md` §12
  protocol (this PRD mirrors that protocol).

**Files touched.** Per the residual list above + audit doc.

**Exit criteria.**

- No single function in `codegen/` exceeds 250 LOC.
- `apply_lambda` no longer mutates `node_types_` per iteration via
  move/clear/restore.
- Audit findings F4, F9, F10, and the viz/param portion of audit §3.4
  all carry RESOLVED tags pointing at this PRD.
- Snapshot harness byte-identical.

---

## 5. Phase Dependencies and Order

```
Phase 1 (InstructionBuilder) ─┐
                              v
            Phase 2 (StateInitBuilder)
                              │
                              v
            Phase 3 (file split)
                              │
                              ├──> Phase 4 (Call branch + kind)
                              │           │
                              │           v
                              │    Phase 5 (pattern transforms)
                              │           │
                              │           v
                              │    Phase 6 (viz + param)
                              │           │
                              │           v
                              │    Phase 7 (close-out)
                              v
```

- **Phase 1 first** — every later phase touches instruction emission.
- **Phase 2 second** — pairs naturally with Phase 1.
- **Phase 3 third** — gives the rest of the phases a clean structure
  to land into.
- **Phases 4 → 5 → 6 → 7 sequential** — each depends on the prior
  (Phase 5 uses the per-kind emitter from Phase 4; Phase 6 uses
  the same dispatch; Phase 7 wraps up residue).

Estimated total effort: **8-10 weeks single-engineer**, with some
phases parallelisable across two reviewers if reviewing in parallel
threads.

---

## 6. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| Cedar VM | **No change** | Bytecode byte-identical |
| `BUILTIN_FUNCTIONS` table | **Modified** | Gains `kind` + `codegen_handler` + `viz_meta` / `param_meta` fields |
| `special_handlers` static map | **Removed** | Replaced by per-builtin handler pointers |
| `InstructionBuilder` | **New** | Single emit path; absorbs ~600 sentinel sites + 177 failure blocks |
| `StateInitBuilder` (+ per-Type sub-builders) | **New** | Eliminates 19 push-back duplications |
| `PatternTransformEmitter` | **New** | ~900 LOC reduction in pattern-transform handlers |
| Per-kind emitters (`emit_visualization` / `emit_param` / `emit_pattern_transform` / `emit_stereo_native` / `emit_sequencer` / `emit_bus_call` / `emit_sample_scalar` / `emit_function`) | **New** | Replace Call branch sub-paths |
| `codegen.cpp` | **Significantly shrunk** | 3,898 → ~600 LOC |
| `codegen_patterns.cpp` | **Significantly shrunk** | 5,953 → ~1,300 LOC |
| `codegen_viz.cpp` / `codegen_params.cpp` | **Significantly shrunk** | 417/416 → ~80/100 LOC |
| File layout under `codegen/` | **Modified** | Per Phase 3 reviewer decision (subdir vs flat) |
| Source-locations parity (correctness PRD F2) | **Preserved** | `InstructionBuilder::emit` routes through `CodeGenerator::emit()` |
| `inst.rate` usage | **Unchanged** | Existing offenders catalogued (Appendix A); future per-family migrations |

---

## 7. Testing Strategy

Every phase ships at least one **precise builder unit test** plus
relies on the snapshot harness (correctness PRD Phase 0) for the
byte-identical-bytecode guarantee.

### Builder unit tests

- `test_codegen.cpp [P1]`: `InstructionBuilder` with input/output/rate/
  imm_f/imm_i — emit, decode, assert fields.
- `test_codegen.cpp [P1]`: `InstructionBuilder::emit` on a full
  buffer pool returns `BUFFER_UNUSED` and emits exactly one E101
  diagnostic.
- `test_codegen.cpp [P2]`: each `StateInitBuilder` factory produces a
  `StateInitData` with the same field layout as the manual form.

### Snapshot harness (every phase)

Every phase's PR must pass the snapshot harness from
`prd-parser-codegen-correctness.md` Phase 0 with no snapshot
updates. Any intentional bytecode change requires reviewer sign-off
+ snapshot baseline update in-PR.

### Cross-cutting invariants (still hold)

- `instructions_.size() == source_locations_.size()` in
  `generate()` epilogue.
- `arena_structural_hash(ast_->arena)` unchanged across `generate()`.

---

## 8. Edge Cases

### Phase 1 (`InstructionBuilder`)

- **`set_unused_inputs()` helper.** Used once in the codebase
  (helpers.hpp:156). After Phase 1, this is dead. Delete in the same
  PR.
- **`Instruction::make_unary` / `make_binary` factories.** Used ~25
  times for COPY. Migrate to `InstructionBuilder` for consistency or
  retain as ergonomic shortcuts that wrap the builder — TBD in Phase
  1 PR.
- **Custom-bit-pack opcodes (e.g. compressor attack/release in
  `inst.rate`).** Phase 1's builder doesn't change `inst.rate`
  semantics; existing bit-pack code stays the same. Future migrations
  per Appendix A.

### Phase 2 (`StateInitBuilder`)

- **`ExtendedParams<N>` template instantiation.** Builder must accept
  the runtime N value or be templated per-N. Audit's
  `emit_extended_params_init` (`codegen.cpp:35-73`) shows the
  existing solution; preserve.
- **Move-only fields.** `StateInitData` arms contain `std::vector`s
  and other move-only types. Builder must support move-from values
  cleanly (e.g. `.sequences(std::move(seqs))`).

### Phase 3 (file split)

- **Cyclic include risk.** `codegen.hpp` declares the `CodeGenerator`
  class; per-emitter `.cpp` files include it for member access. If
  emitters depend on each other (e.g. `emit_pattern_transform` calls
  into `emit_function`), the new layout must keep them in the same
  translation unit OR forward-declare via the class.
- **Static helpers used across emitters.** Move into a shared
  `codegen/helpers.cpp` (extends the existing `codegen/helpers.hpp`).

### Phase 4 (`BuiltinInfo::kind` + dispatch)

- **`Special` kind tentatively retained.** Chord-expand + FM-detection
  paths might resist clean `kind`-driven dispatch. Phase 4 PR reviewer
  decides whether to add a `codegen_handler` fn-pointer per Special
  case or carve a `BuiltinKind::Special` umbrella.
- **Alias-vs-canonical builtin handler.** `BUILTIN_ALIASES` entries
  share the underlying builtin; the alias resolution path must point
  the `codegen_handler` at the canonical entry, not duplicate.

### Phase 5 (pattern-transform consolidation)

- **Per-transform diagnostics.** Some handlers emit transform-specific
  E-class errors. `PatternTransformConfig` must accept a per-transform
  diagnostic emitter, or the canonical emitter must consult an enum.
- **`voicing` is the largest holdout** (~110 LOC). It does
  dictionary-lookup + apply, not the canonical query/step shape.
  Decision: leave `voicing` outside `PatternTransformEmitter` and
  document why, OR add a "custom payload" escape hatch. TBD in Phase
  5 PR.

### Phase 6 (viz + param)

- **Stereo signal viz inputs.** Some visualizers (oscilloscope,
  spectrum) accept stereo. `viz_meta.stereo_signal = true` toggles
  the per-emitter handling.
- **`dropdown` vs `select` naming.** Builtins use both names today
  (alias). Phase 6 preserves the alias.

### Phase 7 (close-out)

- **`apply_lambda` correctness.** Per-iteration `node_types_`
  scoping must NOT regress: the lambda body sees its own arg's
  TypedValue, not the outer scope's. The overlay approach: push a
  `node_types_` frame at iteration entry, pop at exit. No
  cross-iteration leak.
- **`emit_bus_epilogue` split.** The 234-LOC function has subtle
  ordering invariants (master bus crossfade, soft-clip
  insertion). Split must preserve invariants — covered by the
  snapshot harness.

---

## 9. Cross-Reference: Audit Findings → Phase Map

| Finding | Phase | Notes |
|---|---|---|
| F4 (visit() Call branch monolith) | 3 + 4 | File split + Call branch refactor |
| F9 (pattern-transform clones) | 5 | `PatternTransformEmitter` |
| F10 (StateInitData duplication) | 2 | `StateInitBuilder` |
| §3.4 InstructionBuilder | 1 | First phase |
| §3.4 viz + param sprawl (PRD-7 portion) | 6 | Data-driven emitters |
| §3.4 `emit_bus_epilogue` / `handle_field_access` / etc. | 7 | Close-out |
| §3.4 `apply_lambda` cache-thrash | 7 | Overlay scoping |
| §3.4 `codegen_patterns.cpp` SequenceCompiler extract | 3 | File split |
| §3.4 `emit_event_transform` 385-LOC split | 7 | Close-out |
| §3.4 stereo handler cluster | 3 + 7 | File split + minor refactor |
| §3.5 file layout inconsistency | 3 | Phase 3 picks canonical layout |

---

## 10. Sourcing for Key Design Decisions

| Decision | Where set |
|---|---|
| Two-PRD split | Hardening PRD Round 1 Q1 (mirrored here) |
| All 4 codegen-sprawl PRDs in scope | Hardening PRD Round 1 Q2 (mirrored here) |
| Filename: `prd-codegen-sprawl-cleanup.md` | Hardening PRD Round 3 Q2 |
| Builders ship first | This PRD draft (rationale: shrink every site the file split touches) |
| `BuiltinInfo::kind` + `codegen_handler` over `special_handlers` map | This PRD draft (audit-recommended) |
| File layout (Option A vs B) | **[OPEN]** Phase 3 reviewer round |

---

## 11. Per-Phase Documentation Maintenance Protocol

Mirrors `prd-parser-codegen-hardening.md` §12. Each phase PR updates:
**this PRD's** status block, the **audit doc**'s relevant finding
header + PRD-shortlist row.

For audit edits, this PRD ships the close-out edits for **F4, F9,
F10, §3.4 viz+param + InstructionBuilder + file-layout items**. The
hardening PRD owns its own finding edits.

---

## 12. Open Questions

- **[OPEN]** Phase 3: subdir layout (`src/codegen/*.cpp`) vs flat
  (`src/codegen_*.cpp`)? Reviewer-decided.
- **[OPEN]** Phase 4: `BuiltinKind::Special` carve-out vs full
  `codegen_handler` per-builtin even for FM-detection / chord-expand?
- **[OPEN]** Phase 5: `voicing` inside or outside
  `PatternTransformEmitter`?
- **[OPEN]** Phase 7: `apply_lambda` overlay impl details — scoped
  copy on push, restore on pop, or per-iteration substitute via
  symbol-table push?

---

## 13. Appendix A — `inst.rate` runtime-tunable offenders (future per-family migrations)

Out of scope for this PRD. Listed so future contributors know which
opcodes still bit-pack runtime-tunable params into `inst.rate` and
should migrate to `ExtendedParams<N>`:

- Compressor attack/release.
- Limiter (multiple params).
- Freeverb damping + mod_depth.
- Dattorro size_mod.
- Distort_comb damp.
- Delays ping-pong mix.

Each is a per-family migration following the canonical
ExtendedParams pattern in `docs/extended-params-mechanism.md`.

---

## 14. Follow-ups

- **Future per-statement codegen parallelism** — depends on splitting
  `node_types_` dual role (cache vs channel). Catalogued by
  `prd-parser-codegen-hardening.md`. Not addressed here.
- **`codegen_functions.cpp` (3,061 LOC) split** — out of scope for
  this PRD; future cleanup.
- **`codegen_arrays.cpp` (1,722 LOC) split** — out of scope; future
  cleanup (post-`apply_lambda` overlay refactor in Phase 7).
