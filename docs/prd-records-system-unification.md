> **Status: DONE (2026-05-08)** — All 6 phases shipped. Phase 1: static option-field schema for the 5 visualizers (`cd11ffe`). Phase 2: `akkado_get_shape_index(source, cursor)` WASM export + editor pull-on-idle (`56c2fa9`). Phase 3: statement-level `{x, y} = r`, fn-param `fn f({x, y})`, defaults `{x = 1}`, with E187/E188 (`47e2556`, `997449cd`). Phase 4a: record-valued state cells with E122 widened + E189 shape mismatch (`cf7f81d`). Phase 4b: `cell.field` read/write sugar with E150/E204/E205 (`5b9358c`). Phase 5: shared `extract_options(arena, node, schema)` helper + viz handler migration + records reference docs + record-as-options convention doc (`52ce223`). Phase 6: audit at `docs/audits/prd-records-system-unification_audit_2026-05-08.md` recommends DONE.

# PRD: Records System Unification — Editor Visibility, Destructuring, Options Convention, Mutability

**Version:** 1.0
**Status:** DONE (2026-05-08)
**Author:** Claude (drafted with Moritz)
**Date:** 2026-05-07

---

## 1. Executive Summary

Akkado's records system landed piecewise. Record literals, field access, pipe binding `as e` (incl. inline destructuring `as {x, y}`), and record-to-record spread `{..base, x: 1}` all work today. Pattern events also expose a record-shaped record (currently five fields). But the surface is still spotty:

- The web IDE has zero awareness of record or pattern field names — typing `r.` or `%.` does not autocomplete.
- Standalone destructuring (`{x, y} = r`) and parameter destructuring (`fn f({x, y}) -> …`) are absent — only the pipe-side variant shipped.
- The "record-as-options" pattern works for visualizers (via `extract_options_json`) but is not declared as a convention for other builtin families (samplers, filters, delays).
- Records are immutable by design — and there is no idiomatic "stateful record" form for cases where mutation would be natural. Users either reach for `{..r, x: v}` (which still produces a new value) or fall back to flat scalar state cells.

This PRD owns Phase 2 of the records system: closing those four gaps. It does **not** redescribe the spread spec from `docs/prd-record-argument-spread.md`, which stays authoritative for that work. The previously held-open §3 of `docs/prd-records-and-field-access.md` (extended pattern event fields) shipped on 2026-05-07 (commit `19feea2`) — its newly exposed fields are an upstream dependency this PRD's analyzer shape index will surface verbatim, no further coupling required.

### Key Design Decisions

- **Editor visibility uses two layers in tandem:** a static option-field schema embedded in `BuiltinInfo` (for known builtin record-shaped option params), and an analyzer-driven shape index pulled on idle through a new WASM export (for user records and pattern fields, including source-derived `custom_fields`).
- **Destructuring is added in three forms:** standalone `{x, y} = r`, function parameters `fn f({x, y}) -> …`, and default values `{x = 1, y = 2} = r`. The renaming form (`{x: a}`) is deferred.
- **Record-as-options is declared as a convention but not migrated:** the PRD specifies the canonical pattern (last-positional record arg, `ParamValueType::Record`, fields declared in `BuiltinInfo`) and lists builtin families that should adopt it. Actual conversion of sampler/filter/delay configs is left to follow-up PRDs per family.
- **Mutability is decided in-PRD via a merged design.** Sketch C (record-valued state cells) is the underlying mechanism — value records stay pure, mutable state lives in state cells. Sketch A (`cell.x = 5`) is adopted as bidirectional parser sugar over C: `cell.x` desugars to `get(cell).x`; `cell.x = 5` desugars to `set(cell, {..get(cell), x: 5})`. The sugar applies only when the receiver is a state cell holding a record; pure value records stay immutable.

---

## 2. Problem Statement

### 2.1 Current State

| Capability | Today | Limitation |
|---|---|---|
| Record literal `{x: 1, y: 2}` | ✓ Works (`parser.cpp:1503–1577`, `codegen.cpp:1959–2070`) | — |
| Spread `{..base, x: 1}` | ✓ Works (`codegen.cpp:2006–2072`) | — |
| Field access `r.field`, `%.field`, nested | ✓ Works (`codegen.cpp:2112–2246`) | — (extended fields shipped 2026-05-07) |
| Pipe binding `expr as e`, `as {x, y}` | ✓ Works (`parser.cpp:366–404`) | Pipe-only — no statement-level destructuring |
| Records as builtin params | ✓ Works for viz (`builtins.hpp:70` `type_compatible(Record)`, `codegen_viz.cpp:42–121` `extract_options_json`) | Pattern not generalized; not advertised as a convention |
| Argument spread `f(..r)`, `f(..arr)`, `[..a, b]` | ✗ Not started — see `prd-record-argument-spread.md` | Treated as a prerequisite for parts of this PRD |
| Editor field autocomplete on `r.` / `%.` | ✗ None (`web/src/lib/editor/akkado-completions.ts:1–241`) | Functions/keywords/user-vars only — no record shape awareness |
| Standalone `{x, y} = r` and `fn f({x, y})` | ✗ Not parseable | Only `as` form exists |
| Record mutation `r.x = 5` | ✗ Not parseable; vars immutable (E150) | By design per existing PRD §1.3 — but no idiomatic alternative for stateful state either |
| Extended pattern event fields (note/dur/chance/time/phase/sample_id/aliases) | ✓ Shipped 2026-05-07 (commit `19feea2`) | `voice` deferred under polyphony pivot; `sample` aliases the numeric `sample_id`. Surfaced automatically by this PRD's analyzer shape index. |

### 2.2 Root Causes

1. **Editor never received a record shape feed.** `akkado_get_builtins_json()` exports function signatures but no per-param option-field schema. There is no analogous per-binding shape export at all.
2. **Destructuring landed only where it was cheapest.** `as {x, y}` reused the pipe-binding's symbol scope. Statement-level and parameter destructuring need their own AST and analyzer paths.
3. **The viz options pattern was implemented as a one-off.** `extract_options_json` is hand-written for viz only. There is no shared "extract option fields by builtin" plumbing.
4. **Mutability was deferred without a stand-in.** PRD §1.3 of `prd-records-and-field-access.md` lists "Mutable records" as an explicit non-goal, but no PRD ever specified the alternative for genuinely stateful state with named fields. Users have no idiomatic recipe.

### 2.3 Existing Infrastructure to Build On

| Component | Location | Reuse |
|---|---|---|
| `BuiltinInfo` struct | `akkado/include/akkado/builtins.hpp:76–150` | Extend with `option_fields` |
| `ParamValueType::Record` | `akkado/include/akkado/builtins.hpp:36` (enum), `:70` (`type_compatible`) | Already accepts Record/Pattern; extends to declare field schema |
| `extract_options_json` | `akkado/src/codegen_viz.cpp:42–121` | Generalize into a shared helper or schema-driven extractor |
| `RecordPayload` | `akkado/include/akkado/typed_value.hpp:101–103` | Source-of-truth for analyzer-driven shape dump |
| `PatternPayload::custom_fields` | `akkado/include/akkado/typed_value.hpp:53` | Surfaces source-derived attached props to autocomplete |
| `akkado_get_builtins_json` WASM export | per `prd-editor-autocomplete.md` | Extend to include option-field schema; add sibling `akkado_get_shape_index` |
| `as {x, y}` destructuring parser/codegen | `parser.cpp:366–404`, `codegen.cpp:…` (pipe-binding paths) | Lift destructure logic into shared helper for the new statement/param forms |
| `state()` / `get()` / `set()` and `TypedValue::StateCell` | `typed_value.hpp` (cell_state_id), state-cell plumbing | Sketch C of mutability lives here |
| CodeMirror completion source | `web/src/lib/editor/akkado-completions.ts` | Add a `.`-context branch driven by static + analyzer shape data |

---

## 3. Goals and Non-Goals

### 3.1 Goals

- **G1**: Static schema for builtin record-shaped option params, declared in `BuiltinInfo`, exported through the existing `akkado_get_builtins_json()` WASM call.
- **G2**: Analyzer-driven shape index exported via a new WASM call `akkado_get_shape_index(source) → JSON`, returning record/pattern shapes for every binding visible at top level.
- **G3**: Editor autocomplete shows field-name completions on `r.`, `%.`, and inside record-typed argument literals (e.g. `pianoroll(sig, "name", { …` → suggests viz option fields).
- **G4**: Editor autocomplete includes pattern's `custom_fields` (source-derived attached props from `n"c4".set("cutoff", …)`-style chains).
- **G5**: Standalone destructuring statement `{x, y} = r` binds each named field as an immutable variable.
- **G6**: Function-parameter destructuring `fn f({x, y}) -> …` accepts a record from the caller and destructures inline. Composes with argument spread (`f(..r)` from prerequisite PRD) when shipped.
- **G7**: Default values in destructuring `{x = 1, y = 2} = r` — missing fields fall back to declared defaults.
- **G8**: Record-as-options is documented as a canonical convention with an enumerated list of builtin families that should adopt it. Conversion of those families is explicitly future work owned by per-family PRDs.
- **G9**: Mutability question is resolved in this PRD via a merged design — record-valued state cells (Sketch C) as the underlying mechanism plus bidirectional parser sugar (Sketch A) so users can read with `cell.field` and write with `cell.field = expr` when the receiver is a state cell holding a record. Pure value records remain immutable. Implemented as Phase 4.
- **G10**: Backwards compatible. All existing record/field/pipe-binding code continues to compile unchanged.

### 3.2 Non-Goals

- **Adding any new pattern event fields beyond what `prd-records-and-field-access.md` §3 already shipped (2026-05-07).** This PRD's analyzer shape index surfaces whatever fields are present in `PatternPayload` (currently 11 fixed slots — `freq, vel, trig, gate, type, note, dur, chance, time, phase, sample_id` — plus aliases). The `voice` slot remains deferred under the polyphony pivot; not adding it here.
- **Argument spread itself** (record spread `f(..r)`, array spread `f(..arr)`, array literal spread `[..a, b]`). Owned by `prd-record-argument-spread.md`. Treated here as a prerequisite for parts of G6 and G8 — the option schema must model "this builtin accepts spread" so the editor produces correct hints when the user writes `f(..preset)`.
- **Diagnostic-code reconciliation** between the records PRD's E062–E065 and the implementation's E135/E136/E140. Tracked in the audit's recommended next steps. Out of scope here.
- **Migration of sampler/filter/delay/reverb config to record-as-options.** Convention is declared; per-family migration is future work.
- **Renaming destructuring `{x: a, y: b} = r`.** Deferred — adds parser ambiguity with field-shorthand and is not on the user's path.
- **Type annotations / explicit record types** (e.g. `type Point = {x: Float, y: Float}`). Future.
- **Full LSP / jump-to-def / find-refs.** Future, per `prd-editor-autocomplete.md` non-goals.
- **Mini-notation completions inside pattern strings.** Out of scope.

### 3.3 Prerequisites and Dependencies

| Dep | Status | Used by |
|---|---|---|
| `prd-record-argument-spread.md` | NOT STARTED | G6 (function-param destructuring composes with `f(..r)`), G8 (option schema marks spread-compatible builtins). Strongly recommended to ship before this PRD's Phase 3. If it slips, this PRD's destructuring still ships; spread-aware completions ship later as a follow-up. |
| `prd-editor-autocomplete.md` | DONE | G1, G3 (extends the existing builtins JSON pipeline and CodeMirror completion source). |
| `prd-records-and-field-access.md` §3 | DONE (shipped 2026-05-07, commit `19feea2`) | All 11 fixed pattern fields + aliases now exposed; analyzer shape index surfaces them automatically. `voice` slot deferred separately. |
| `prd-compiler-type-system.md` Phase 1 | DONE | TypedValue + RecordPayload + PatternPayload are the source of truth for the analyzer dump. |

### 3.4 Cross-PRD Sequencing

```
                              [DONE]                                       [DONE]
                                |                                             |
                     prd-editor-autocomplete.md             prd-compiler-type-system.md (Phase 1)
                                |                                             |
                                +---------------+      +----------------------+
                                                |      |
                                                v      v
[NOT STARTED]                        +---------------------------+
prd-record-argument-spread.md  ----> | prd-records-system-       |
   (recommended before Phase 3)      | unification.md (THIS PRD) |
                                     +---------------------------+
                                                |
                                                |
            +-----------+-----------+-----------+-----------+-----------+
            |           |           |           |           |           |
            v           v           v           v           v           v
        Phase 1     Phase 2     Phase 3     Phase 4a    Phase 4b     Phase 5/6
        Static      Analyzer    Destruct.   State       Field        Convention
        schema      shape       (assign,    cells       sugar        + tests +
        (BuiltinInfo,index +    fn-param,   (mechanism: (cell.field, audit
        viz JSON)   pull-on-    defaults)   record-     cell.x=v
                    idle WASM)              valued)
                                            ^
                                            |  4b is optional/
                                            |  deferrable (off-ramp
                                            |  in Phase 4b body)

                          [DONE — 2026-05-07, commit 19feea2]
                          prd-records-and-field-access.md §3
                          (extended pattern event fields)
                          ─── 11 fixed fields + aliases live;
                              analyzer shape index surfaces them
                              with no extra coupling. `voice`
                              deferred under polyphony pivot.

                          [FUTURE — per-builtin-family PRDs]
                          - prd-sampler-options.md       (per Enhanced Sampler G8)
                          - prd-filter-options.md
                          - prd-delay-reverb-options.md
                          ─── each adopts the convention declared in §5.5
```

**Reading the diagram:**
- Solid arrows are hard prerequisites: Phase 1+2 cannot ship without the prior editor-autocomplete and type-system foundations.
- Spread (`prd-record-argument-spread.md`) is a soft prerequisite: shipping it before Phase 3 lets destructuring + spread compose meaningfully (`fn f({x, y})` paired with `f(..r)`). If spread slips, Phase 3 still ships; spread-paired tests come later.
- §3 of the records PRD shipped on 2026-05-07 (commit `19feea2`) — its 11 fixed fields plus aliases flow through `PatternPayload` and surface verbatim through this PRD's analyzer shape index. No coupling needed; the `voice` slot remains deferred separately under the polyphony pivot.
- Per-family options PRDs are downstream of §5.5 and can ship in any order once the convention is documented.

---

## 4. Target Syntax and User Experience

### 4.1 Editor Field Autocomplete (G1–G4)

```akkado
// 1. Builtin option fields (static schema)
saw(220) |> waterfall(%, "harmonics", {
    grad|       // ← editor suggests: gradient, …
    fft: 1024,
    angle|      // ← editor suggests: angle (already typed), height, width, speed
}) |> out(%, %)

// 2. User record fields (analyzer dump)
synth_cfg = {wave: "saw", cutoff: 2000, q: 0.7}
synth_cfg.|     // ← editor suggests: wave, cutoff, q

// 3. Pattern fields, fixed (analyzer dump)
n"c4 e4" |> sine(%.|)    // ← suggests: freq, vel, trig, gate, type,
                                   //   note, dur, chance, time, phase, sample_id
                                   //   (plus aliases: pitch/f, velocity/v,
                                   //   trigger/t, midi/n, sample/s, frequency, p)

// 4. Pattern fields including custom_fields (analyzer dump)
beat = n"c4 e4".set("cutoff", saw(0.5)).set("res", 0.7)
beat |> lp(sine(%.freq), %.|)   // ← suggests: all 11 fixed fields above
                                      //   plus cutoff, res (custom fields from
                                      //   .set() calls)

// 5. Field hints inside spread caller (when spread PRD ships)
preset = {bits: 12, sr: 26040}
samples("my-bank", ..preset, |)       // ← editor suggests remaining unfilled
                                      //   sampler params (bits/sr already in preset)
```

### 4.2 Standalone Destructuring (G5)

```akkado
// Bind multiple fields at once
pos = {x: 1.0, y: 2.0}
{x, y} = pos
sine(x * 100) + sine(y * 100)

// Useful for unpacking function returns
fn make_adsr(a, d) -> {attack: a, decay: d, sustain: 0.7, release: 0.3}
{attack, decay, sustain, release} = make_adsr(0.01, 0.1)
adsr(trig, attack, decay, sustain, release)
```

### 4.3 Function-Parameter Destructuring (G6)

```akkado
// Destructure caller's record inline
fn distance({x, y}) -> sqrt(x * x + y * y)
distance({x: 3, y: 4})        // 5

// With argument spread (when prd-record-argument-spread.md ships)
fn synth({freq, wave, cutoff}) ->
    osc(wave, freq) |> lp(%, cutoff)
config = {freq: 440, wave: "saw", cutoff: 2000}
synth(config)        // direct record arg
synth(..config)      // identical when caller spreads (post-spread-PRD)

// Mixed with regular params
fn lp_voice(freq, {cutoff, q}) ->
    saw(freq) |> lp(%, cutoff, q)
lp_voice(440, {cutoff: 2000, q: 0.7})
```

### 4.4 Default Values in Destructuring (G7)

```akkado
// Field missing from r → fall back to default
{x = 0, y = 0} = {x: 5}            // x = 5, y = 0

// In function params — gives "options-with-defaults" ergonomics
fn synth({freq = 440, wave = "saw", cutoff = 2000, q = 0.7}) ->
    osc(wave, freq) |> lp(%, cutoff, q)

synth({})                                       // all defaults
synth({freq: 220})                              // just override freq
synth({freq: 220, cutoff: 800, q: 0.9})         // mix-and-match

// With spread (post-spread-PRD)
preset = {wave: "saw", cutoff: 2000}
synth({..preset, freq: 220})                    // q = 0.7 default fills
```

### 4.5 Record-as-Options Convention (G8)

```akkado
// Canonical shape: builtin's last positional param is a record-typed
// param with declared option fields. Caller passes inline literal,
// or (post-spread-PRD) spreads a preset record.

// Today (already works for viz):
spectrum(sig, "spec", {fft: 1024, logScale: true, gradient: "viridis"})

// PRD declares the convention; future per-family PRDs adopt it:
samples("my-bank", {bits: 12, sr: 26040, preset: "sp1200"})         // future
filter_lp(sig, 1000, {q: 0.7, dry: 0.3, wet: 0.7})                  // future
delay(sig, 0.5, {feedback: 0.6, dry: 0.3, wet: 0.7, mode: "ping"})  // future
```

### 4.6 Record-Valued State Cells with Field Sugar (G9)

```akkado
// Create a stateful container holding a record
voice = state({freq: 440, vel: 0.5, gate: 0})

// READ — bare field access on the cell desugars to get(cell).field
sine(voice.freq) * voice.vel * voice.gate
// equivalent to: sine(get(voice).freq) * get(voice).vel * get(voice).gate

// WRITE — field assignment on the cell desugars to set(cell, {..get(cell), field: value})
on_note = button("note")
on_note |> { voice.freq = 880; voice.gate = 1 }
// equivalent to: set(voice, {..get(voice), freq: 880, gate: 1})

// The sugar is bidirectional and the receiver type drives the desugaring:
//   value record `r` → `r.x` is plain field access; `r.x = 5` is a parse error
//   state cell `c`   → `c.x` reads (sugar over get); `c.x = 5` writes (sugar over set)

// Self-referential update — read on RHS, write on LHS, all sugar
counter = state({n: 0})
button("inc") |> { counter.n = counter.n + 1 }
// Lowers to: set(counter, {..get(counter), n: get(counter).n + 1})
// Two reads of the cell + one write per trigger. Cheap; cell access is just a state-id lookup.

// Helpers for whole-record updates
fn update(cell, patch) -> set(cell, {..get(cell), ..patch})
on_note |> update(voice, {freq: 880, gate: 1})

// Direct get/set still work — sugar is opt-in, not mandatory
get(voice)                                 // returns the full record
set(voice, {freq: 220, vel: 0.7, gate: 1}) // wholesale replace

// Hot-swap state preservation works as for any state cell (state_id by path hash)
```

### 4.7 Mutability of Pure Value Records (Unchanged)

```akkado
// Value records stay immutable — the sugar does NOT apply here
r = {x: 1, y: 2}
r.x = 5    // E150 — `r` is a value record, not a state cell.
           // To mutate, declare with `state({...})` instead.

// Pure value records can still be "modified" via spread (existing behavior)
r2 = {..r, x: 5}    // r2 = {x: 5, y: 2}, r unchanged
```

---

## 5. Architecture / Technical Design

### 5.1 Static Option Schema (G1)

Extend `BuiltinInfo` to declare per-param option fields when a parameter is `ParamValueType::Record`.

```cpp
// akkado/include/akkado/builtins.hpp

struct OptionField {
    std::string_view name;
    ParamValueType    type;          // Number, String, Bool, Enum, …
    std::string_view  default_repr;  // "1024", "\"viridis\"", "false" (textual)
    std::string_view  description;
    // For Enum: comma-separated allowed values
    std::string_view  enum_values = {};
};

struct OptionSchema {
    std::uint8_t                       param_index;  // which parameter this schema describes
    std::array<OptionField, 16>        fields;       // fixed-size for the typical case
    std::uint8_t                       field_count;
    bool                               accepts_spread = true;  // hint for editor
};

struct BuiltinInfo {
    cedar::Opcode                                       opcode;
    std::uint8_t                                        input_count;
    std::uint8_t                                        optional_count;
    bool                                                requires_state;
    std::array<std::string_view, 6>                     param_names;
    std::array<float, 5>                                defaults;
    std::string_view                                    description;
    // NEW:
    std::array<OptionSchema, 2>                         option_schemas{};
    std::uint8_t                                        option_schema_count = 0;
};
```

The existing `akkado_get_builtins_json()` WASM export gains an `optionFields` block per affected param:

```json
{
  "waterfall": {
    "params": [
      {"name": "in",   "required": true},
      {"name": "name", "required": false},
      {"name": "options", "required": false, "type": "record",
       "optionFields": [
         {"name": "angle",    "type": "number", "default": "180",     "description": "Scroll direction"},
         {"name": "speed",    "type": "number", "default": "40"},
         {"name": "fft",      "type": "enum",   "default": "1024", "values": "256,512,1024,2048"},
         {"name": "gradient", "type": "string", "default": "\"magma\""},
         {"name": "width",    "type": "number", "default": "300"},
         {"name": "height",   "type": "number", "default": "150"}
       ],
       "acceptsSpread": true}
    ]
  }
}
```

The editor consumes this directly — no new WASM call needed for builtin option-field hints.

### 5.2 Analyzer Shape Index (G2, G4)

New WASM export:

```cpp
// web/wasm/nkido_wasm.cpp
//
// `cursor_offset` is the byte offset of the editor caret in `source`.
// Used solely to resolve the `patternHole` field below — top-level
// `bindings` are independent of cursor position. Pass `UINT32_MAX` (or
// any value past end-of-source) to skip patternHole resolution.
WASM_EXPORT const char* akkado_get_shape_index(const char* source,
                                                std::uint32_t cursor_offset);
```

Behavior: parse + analyze the source (same pipeline as the regular compile, but tolerant — partial bindings still indexed). Walk the symbol table; for every binding whose `TypedValue` is `Record`, `Pattern`, or `Array`-of-Record, emit:

```json
{
  "version": 1,
  "sourceHash": "<fnv1a-of-source>",
  "bindings": {
    "synth_cfg": {
      "kind": "record",
      "fields": [
        {"name": "wave",   "type": "string"},
        {"name": "cutoff", "type": "number"},
        {"name": "q",      "type": "number"}
      ]
    },
    "beat": {
      "kind": "pattern",
      "fields": [
        {"name": "freq",  "type": "signal", "fixed": true},
        {"name": "vel",   "type": "signal", "fixed": true},
        {"name": "trig",  "type": "signal", "fixed": true},
        {"name": "gate",  "type": "signal", "fixed": true},
        {"name": "type",  "type": "signal", "fixed": true},
        {"name": "cutoff", "type": "signal", "fixed": false, "source": "set"},
        {"name": "res",    "type": "signal", "fixed": false, "source": "set"}
      ]
    }
  },
  "patternHole": {
    "kind": "pattern",
    "fields": [/* shape of the pattern feeding the % nearest to cursor_offset,
                  if any. Resolution: walk the AST, find the innermost pipe
                  expression containing cursor_offset; if its LHS is a
                  Pattern-typed expression, emit that pattern's shape.
                  Omitted entirely if no enclosing pipe or LHS is not a
                  Pattern. */]
  }
}
```

Shape extraction reuses the existing `RecordPayload::fields` and `PatternPayload::{fields, custom_fields}` directly — no new compile-time data structures needed beyond a small `ShapeIndexBuilder` that walks the analyzer's symbol table after `analyze()` completes.

**Aliases:** for pattern fields with documented aliases (e.g. `freq`/`pitch`/`f`), the dump emits one entry per alias with `aliasOf: "freq"`. The static alias list lives in `typed_value.cpp` next to `pattern_field_index()`.

**Edge case — partial source:** if the source has parse errors but reaches the analyzer, return whatever shapes were successfully bound before the error. Editor still gets useful completions.

### 5.3 Editor Pull-on-Idle (G3)

```ts
// web/src/lib/editor/akkado-shape-index.ts (new)

let lastHash = "";
let cachedIndex: ShapeIndex | null = null;
let timer: number | null = null;

export function scheduleShapeIndex(source: string, cursorOffset: number) {
    if (timer) clearTimeout(timer);
    timer = setTimeout(() => {
        // Hash includes cursor since patternHole depends on it. (Top-level
        // bindings would suffice with a source-only hash if patternHole were
        // computed lazily — kept simple here.)
        const hash = fnv1a(source) ^ cursorOffset;
        if (hash === lastHash) return;
        cachedIndex = JSON.parse(wasm.akkado_get_shape_index(source, cursorOffset));
        lastHash = hash;
    }, 300);
}

export function getShape(name: string) { return cachedIndex?.bindings[name]; }
export function getPatternHoleShape() { return cachedIndex?.patternHole; }
```

`akkado-completions.ts` gains a `.`-context branch:

```ts
// Pseudo-code
if (precedingChar === "." && !insideString) {
    const target = parseTargetExpr(beforeDot);
    if (target.kind === "identifier") {
        const shape = getShape(target.name);
        if (shape) return completionsFromShape(shape);
    }
    if (target.kind === "hole") {  // %.
        const shape = getPatternHoleShape();
        if (shape) return completionsFromShape(shape);
    }
}
if (insideRecordLiteral && enclosingCallParam.acceptsRecord) {
    return completionsFromOptionSchema(enclosingCallParam.optionFields);
}
```

### 5.4 Destructuring (G5–G7)

#### 5.4.1 AST

```cpp
// akkado/include/akkado/ast.hpp

struct DestructureField {
    std::string     name;           // canonical field name
    NodeIndex       default_value;  // NULL_NODE when no default
};

struct DestructureAssignmentData {
    std::vector<DestructureField> fields;
    NodeIndex                     source;  // RHS expression
};

struct DestructureParamData {
    std::vector<DestructureField> fields;  // appears as a single param
};

enum class NodeType {
    // …
    DestructureAssignment,  // {x, y} = expr
    DestructureParam,       // appears in fn signature
};
```

#### 5.4.2 Parser

- Statement-level: when `parse_statement()` sees `{` followed by an `identifier` not followed by `:` (i.e. shorthand), peek for `}` then `=`. If matched, parse as `DestructureAssignment`. If not, fall back to record literal expression statement.
- Function param: extend `parse_fn_signature()` to accept `{ field [= default] [, …] }` as a single parameter slot. Internally creates a `DestructureParam` node attached to the function signature.
- Default values: after each field name, accept optional `=` expression for the fallback.

The pipe-side `as {x, y}` parser already validates the destructuring pattern syntax (`parser.cpp:366–404`). Lift its field-list parser into a shared `parse_destructure_fields()` helper used by all three forms.

#### 5.4.3 Codegen

For statement-level: each field becomes an immutable binding to the analyzer-visible buffer for that record field. If the field is missing from the source record AND a default is declared, emit the default expression and bind to its buffer. If missing AND no default, emit **E187** ("destructure source missing required field 'foo'") — see §10.0 reserved codes table.

For function params: at call site, the call's argument is required to be Record-typed. Before lowering the function body, emit per-field bindings into the function's local scope with the same semantics as statement-level. Composes naturally with argument spread — `f(..r)` builds a synthetic record from spread args, then destructures normally.

### 5.5 Record-as-Options Convention (G8)

The PRD declares the canonical pattern. Future per-family PRDs adopt it.

**Pattern:**
1. Last (or last-N) positional parameter typed as `ParamValueType::Record`.
2. Field schema declared in `OptionSchema` on the builtin's `BuiltinInfo`.
3. Codegen reads the literal via a shared helper `extract_options(node, schema) -> OptionsPayload` that generalizes `codegen_viz.cpp::extract_options_json`. Helper validates field names against the schema, emits warnings for unknown fields (W160-style — reuse from spread PRD), fills defaults.
4. Builtin handler consumes a typed `OptionsPayload` instead of poking AST nodes directly.

**Builtin families recommended for adoption (each as a follow-up PRD):**

| Family | Builtins | Likely option fields |
|---|---|---|
| Sampler | `samples`, `sample_play` | `bits, sr, preset, loop_mode, pitch_algo, grain_size, grain_overlap, grain_window, grain_jitter` |
| Filters | `lp, hp, bp, moog, svflp, …` | `q, dry, wet, oversample` |
| Delays / reverbs | `delay, comb, freeverb, dattorro, lexicon` | `feedback, dry, wet, mode, damping, size` |
| Visualizers | `pianoroll, oscilloscope, waveform, spectrum, waterfall` | already done — codify the schema |

### 5.6 Mutability — Record-Valued State Cells + Field Sugar (G9)

This work has two layers: (1) the type-system extension that lets state cells hold records, and (2) the parser/analyzer sugar that lets users say `cell.field` instead of `get(cell).field` and `cell.field = v` instead of `set(cell, {..get(cell), field: v})`. The sugar is the surface; the cells are the mechanism. Both ship in Phase 4.

#### 5.6.1 Type-System Layer (mechanism)

The state-cell mechanism already exists (`TypedValue::cell_state_id` and surrounding plumbing). Extending it to record-valued cells is mostly type-system work.

**Existing behavior to relax.** `codegen_state.cpp:60` and `:154` currently emit `E122 "state() initial value must be a number or signal"` (and the parallel rejection on `set()`). Phase 4a explicitly **widens** that check to admit `Record` alongside `Number`/`Signal`; `Function`, `Array`, `StateCell`, and `Void` continue to reject. The error code stays E122; the message is updated to "must be a number, signal, or record".

**Type rules:**
- `state(initial)` where `typeof(initial) = Record` returns `StateCell<Record<…>>` and remembers the cell's record shape.
- `get(cell)` returns the `Record` payload of the cell — supports field access naturally (`get(cell).freq`).
- `set(cell, value)` requires `typeof(value) = Record` with the same field shape as the cell's declared type. Shape mismatch → **E189** (new — see §10.0 reserved codes).

**State preservation across hot-swap:** the cell's `state_id` (FNV-1a path hash) already tracks identity; the record value held inside is just data. No new hot-swap logic.

**Stdlib helper** (declared in language docs, not a new opcode):

```akkado
fn update(cell, patch) -> set(cell, {..get(cell), ..patch})
```

A computed-field-name `modify(cell, field, fn_)` form would require runtime field-name resolution, which `prd-records-and-field-access.md` §Q5 marks as "Not supported." Deferred.

#### 5.6.2 Parser Sugar Layer (surface)

Two new desugarings, both gated on the receiver being a state cell holding a record.

**Read sugar:** `cell.field` → `get(cell).field`

- Recognized at codegen time: `handle_field_access` already dispatches on the receiver's `ValueType`. Add a branch: when the receiver is `StateCell<Record>`, emit a synthetic `get` and continue field-access codegen on the resulting record.
- Pure value records (`r = {x: 1}; r.x`) are unaffected — they take the existing Record branch.

**Write sugar:** `cell.field = expr` → `set(cell, {..get(cell), field: expr})`

- New AST node `FieldAssignment { receiver: NodeIndex, field: string, value: NodeIndex }`.
- Parser: when `parse_statement()` (or expression-statement parser) sees `expr.identifier =`, build a `FieldAssignment`. Distinguish from `expr.identifier(...)` (method call) and `expr.identifier` (field read) by the trailing `=`.
- Analyzer: validate that `receiver` is `StateCell<Record>` AND the named `field` exists on the cell's record shape. If receiver is a value record → **E150** (existing code, message extended: "Cannot assign to field of immutable value record; declare with state(...) instead"). If receiver is a state cell but field absent → reuse **E136** "Unknown field" with the cell's available field list.
- Codegen: lower to `set(receiver, {..get(receiver), field: value})`. The `{..get(receiver), field: value}` part reuses existing record-spread codegen verbatim.

**No nested-field write in v1.** `cell.inner.x = 5` is rejected at analyze time with **E204** ("Nested field assignment is not supported; use `set(cell, {..get(cell), inner: {..get(cell).inner, x: v}})`"). The sugar only applies to a one-level field on a state cell.

**No pipe-position write in v1.** `expr |> cell.x = 5` is rejected at parse time with **E205** ("Field assignment is a statement, not an expression — wrap in a block: `expr |> { cell.x = 5 }`"). Documented in §10.

**Disambiguation rule (§10.1 echo):** when both static schema and analyzer disagree on a binding's shape, static wins. For sugar resolution specifically: the analyzer determines whether a binding is `StateCell<Record>` vs. `Record`; that's analyzer-only territory (no static schema collision possible) so the rule does not bite here.

---

## 6. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| `BuiltinInfo` | **Modified** | Add `OptionSchema` array + count |
| `akkado_get_builtins_json()` | **Modified** | Emit `optionFields` per record-typed param |
| `akkado_get_shape_index()` | **New** | New WASM export; walks analyzer symbol table |
| Analyzer `ShapeIndexBuilder` | **New** | Builds shape JSON from `TypedValue` payloads |
| Parser — `parse_statement()` | **Modified** | Recognize `{x, y} = expr` |
| Parser — `parse_fn_signature()` | **Modified** | Accept `{x, y}` as a param slot |
| Parser — shared destructure helper | **New** | `parse_destructure_fields()` |
| Codegen — destructure assignment | **New** | Emits per-field bindings |
| Codegen — destructure param | **New** | Validates Record arg, emits per-field locals in fn body |
| Codegen — `extract_options` helper | **New** | Generalized from `codegen_viz.cpp::extract_options_json` |
| Codegen — viz handlers | **Modified** | Migrate to `extract_options` (no behavior change) |
| Type system — record-valued state cells | **Modified** | `state()`/`get()`/`set()` accept Record payload |
| Editor — `akkado-shape-index.ts` | **New** | Pull-on-idle WASM caller |
| Editor — `akkado-completions.ts` | **Modified** | `.`-context branch + record-literal-context branch |
| `prd-record-argument-spread.md` | **Stays** | Authoritative for spread; this PRD only consumes |
| `prd-records-and-field-access.md` §3 | **Stays (now DONE)** | Shipped 2026-05-07; this PRD's analyzer surfaces fields verbatim |
| `prd-editor-autocomplete.md` | **Stays** | Authoritative for the existing autocomplete; this PRD extends |

### 6.1 Unchanged Surfaces

These shipped surfaces are explicitly preserved as-is and verified by regression tests:

- **Cedar VM opcodes.** No new opcodes; no opcode behavior changes. Whole feature is compile-time.
- **Existing record literal `{x: 1}` and spread `{..base, x: 1}`.** Parser path (`parser.cpp:1503–1577`) and codegen (`codegen.cpp:1999–2110`) untouched.
- **Existing field access `r.field`, `%.field`.** `handle_field_access` (`codegen.cpp:2112+`) gains a `StateCell<Record>` branch but the existing Record/Pattern branches are unchanged.
- **Existing pipe-binding destructure `as {x, y}`.** Parser path (`parser.cpp:366–404`) gets refactored to share `parse_destructure_fields()` — same observable behavior; the existing tests act as a regression gate.
- **All extended pattern fields shipped 2026-05-07.** Surfaced verbatim; no remapping or alias rewiring.
- **State cell hot-swap identity (`state_id` path-hash).** Same machinery, now also carries Record-shaped values.

---

## 7. Mutability Design Discussion

Records are immutable today. `prd-records-and-field-access.md` §1.3 lists "Mutable records" as an explicit non-goal. This section walks through the design space and lands on the merged design adopted as G9.

### 7.1 The Two Surface Sketches

Initially these looked like competing alternatives:

- **Sketch A** — direct mutation `r.x = 5`. Familiar imperative ergonomics.
- **Sketch C** — record-valued state cells: `voice = state({...})`, then `get(voice).x` and `set(voice, {..get(voice), x: 5})`. **Extends** existing state-cell infra: cell-identity, `state_id`, hot-swap, and StatePool plumbing all reuse verbatim; the `E122` type-acceptance check in `state()`/`set()` is widened to admit `Record` (currently Number/Signal only — see §5.6.1).

Read literally as separate semantics, A and C conflict: A makes records mutable values; C keeps records pure and puts mutability inside state cells. Picking A would require a new runtime record representation, complicate hot-swap identity, and reverse `prd-records-and-field-access.md` §1.3.

### 7.2 The Reframing — A as Sugar over C

A and C are not actually competing semantics. A can be reframed as **parser sugar over C**, and the conflict dissolves:

| Surface form | Desugars to | Receiver type required |
|---|---|---|
| `cell.field` (read) | `get(cell).field` | `StateCell<Record>` |
| `cell.field = expr` (write) | `set(cell, {..get(cell), field: expr})` | `StateCell<Record>` |
| `r.field` (read) | unchanged — plain field access | `Record` (value) |
| `r.field = expr` (write) | E150 — value records are immutable | `Record` (value) |

Under this reframing:
- Pure value records stay immutable, exactly as `prd-records-and-field-access.md` §1.3 specified.
- State cells gain `r.x`-style ergonomics that match Sketch A's surface goal.
- The semantic engine is still Sketch C — `set` produces a new record value, the cell atomically swaps it in, hot-swap identity comes from `state_id`.
- The analyzer disambiguates by receiver type. Users do not need to think about which form to type — for a state cell, `cell.x` does the right thing in both read and write position.

### 7.3 Why This Beats Either Sketch in Isolation

| Axis | Sketch A (literal) | Sketch C (literal) | Merged design |
|---|---|---|---|
| Ergonomics for "change one field" | Best (`r.x = 5`) | Verbose without sugar (`set(cell, {..get(cell), x: 5})`) | Best (`cell.x = 5`) |
| Value-type invariant preserved | No | Yes | Yes |
| Hot-swap identity | New machinery needed | Reuses `state_id` | Reuses `state_id` |
| Cedar VM impact | Likely changes | None | None |
| Implementation cost | Large | Small | Small (sugar is parser + analyzer + codegen lowering, no new opcodes) |
| Composes with destructuring / spread / pipe binding | Conflicts | Yes | Yes |

The merged design pays only the cost of the parser-sugar layer (one new AST node `FieldAssignment`, two desugaring paths in codegen) and gets the surface ergonomics that made Sketch A appealing in the first place.

### 7.4 What the Sugar Does NOT Do

To keep the spec tight, several adjacent ergonomics are explicitly deferred:

- **Nested-field write** `cell.inner.x = 5` — defer (would need walking the path, building the rebuilt record, and re-setting; non-trivial). For now: `set(cell, {..get(cell), inner: {..get(cell).inner, x: 5}})` or refactor to flatter records.
- **Pipe-position write** `expr |> cell.x = 5` — assignments are statements, not expressions. Wrap in a block (`expr |> { cell.x = 5 }`) to use in pipe context.
- **Mutation of pure value records** — explicitly E150 with a hint pointing the user at `state(...)`.
- **`modify(cell, field, fn_)`** — requires computed field names, deferred per existing records PRD §Q5.
- **Type coercion on assign** — `cell.freq = 880` requires the value to match the field's declared type. No implicit Number↔Signal lifting beyond what already exists for record literals.

### 7.5 Decision

**Adopt the merged design as G9.** Phase 4 implements both layers: record-valued state cells (mechanism, §5.6.1) and the bidirectional field sugar (surface, §5.6.2). Pure value records stay immutable; mutability lives in state cells; the surface syntax for both reads and writes is uniform `cell.field`.

Revisit deferred items (nested-field write, pipe-position write, `modify`) only if user pain emerges after Phase 4 ships.

---

## 8. File-Level Changes

| File | Change |
|---|---|
| `akkado/include/akkado/builtins.hpp` | Add `OptionField`, `OptionSchema` structs; extend `BuiltinInfo` with `option_schemas` array |
| `akkado/src/builtins.cpp` (or wherever viz signatures are declared) | Populate `option_schemas` for all current viz builtins |
| `akkado/include/akkado/ast.hpp` | Add `DestructureField`, `DestructureAssignmentData`, `DestructureParamData`, NodeType entries |
| `akkado/src/parser.cpp` | New `parse_destructure_fields()` helper; refactor `as {x, y}` to use it; extend `parse_statement()` and `parse_fn_signature()` |
| `akkado/src/analyzer.cpp` | Validate destructure source is Record; check field existence; resolve defaults; emit **E187** (missing required), **E188** (duplicate field) per §10.0 |
| `akkado/src/codegen.cpp` | Codegen for `DestructureAssignment` and `DestructureParam`; new shared `extract_options(node, schema)` helper |
| `akkado/src/codegen_viz.cpp` | Migrate to shared `extract_options` helper (behavior-preserving) |
| `akkado/src/codegen_functions.cpp` | Function-param destructuring: bind locals before emitting body |
| `akkado/include/akkado/typed_value.hpp` | Allow `Record` as the held type of `StateCell`; type-rule extensions; track cell's record shape for sugar disambiguation |
| `akkado/src/codegen_state.cpp` | Widen `state()`/`set()` E122 acceptance to admit Record (currently Number/Signal only); return Record in `get()`; emit **E189** on shape mismatch |
| `akkado/include/akkado/stdlib.hpp` (and the file that registers stdlib `fn` defs) | Add `update(cell, patch)` defined as `fn update(cell, patch) -> set(cell, {..get(cell), ..patch})`. Lands once argument spread (`..patch`) ships; until then, document the helper but require users to inline. |
| `akkado/include/akkado/ast.hpp` | Add `FieldAssignment` NodeType + data (receiver: NodeIndex, field: string, value: NodeIndex) for the write-side sugar |
| `akkado/src/parser.cpp` | Recognize `expr.identifier = expr` in statement context; build `FieldAssignment`. Reject pipe-position field assignment with **E205** |
| `akkado/src/analyzer.cpp` | Validate `FieldAssignment` receiver is `StateCell<Record>` and field exists; **E150** (existing code, message extended) for value-record receivers; reuse **E136** for unknown field; emit **E204** for nested-field write |
| `akkado/src/codegen.cpp` | Codegen for `FieldAssignment` (lower to `set(receiver, {..get(receiver), field: value})`); add read-side sugar branch in `handle_field_access` for `StateCell<Record>` receivers |
| `akkado/include/akkado/typed_value.hpp` — `pattern_field_index()` aliases | Static alias-list extension for shape-index emission (no new fields, just expose alias mapping) |
| `web/wasm/nkido_wasm.cpp` | Add `akkado_get_shape_index()` export; extend `akkado_get_builtins_json()` to emit `optionFields` |
| `web/src/lib/editor/akkado-shape-index.ts` | NEW — pull-on-idle WASM caller, source-hash cache |
| `web/src/lib/editor/akkado-completions.ts` | New `.`-context branch; new record-literal-context branch using static option schemas |
| `web/src/lib/editor/signature-help.ts` | Optional: surface option-field defaults inline when cursor is inside a record-typed param |
| `akkado/tests/test_parser.cpp` | Tests: standalone destructure, fn-param destructure, defaults |
| `akkado/tests/test_codegen.cpp` | Tests: destructure binds correct buffers, defaults trigger on missing, record-valued state cells |
| `akkado/tests/test_analyzer.cpp` | Tests: shape index includes records, patterns, custom_fields, aliases |
| `akkado/tests/test_builtins_json.cpp` (or similar) | Tests: `optionFields` appear for viz builtins; spread flag set correctly |
| `web/tests/` (if integration test infra exists) | Tests: editor surfaces field hints on `r.`, `%.`, inside record literal |

### 8.1 Files Explicitly NOT Changed

| File | Reason |
|---|---|
| Cedar VM opcodes | Compile-time only feature throughout |
| `prd-records-and-field-access.md` | Stays authoritative; §3 shipped 2026-05-07 — fields surface automatically |
| `prd-record-argument-spread.md` | Stays authoritative; treated as soft prerequisite |

---

## 9. Implementation Phases

### Phase 1 — Static Option Schema (3–4 days) — **SHIPPED 2026-05-07**

**Goal:** Builtin record-shaped option params declare their fields; editor surfaces them on `, |` inside the record literal.

**Files:** `builtins.hpp`, `builtins.cpp`, `nkido_wasm.cpp`, `akkado-completions.ts`.

**Deliverables:**
- `OptionSchema`/`OptionField` structs.
- All current viz builtins populate schemas (no behavior change beyond the export).
- `akkado_get_builtins_json()` emits `optionFields`.
- Editor completion source recognizes "inside record literal that is the Nth arg of builtin X" and produces field-name completions.

**Verification:**
- New unit tests assert `optionFields` JSON shape for `pianoroll`, `waterfall`, `spectrum`, etc.
- Manual: type `waterfall(sig, "x", { gra` in the editor; completion popup shows `gradient`.

### Phase 2 — Analyzer Shape Index (3–5 days)

**Goal:** New WASM export returns shapes for all top-level bindings + the pattern shape at the cursor's pipe `%`.

**Files:** `nkido_wasm.cpp`, new `ShapeIndexBuilder` in `analyzer.cpp` (or sibling .cpp), `akkado-shape-index.ts`, `akkado-completions.ts`.

**Deliverables:**
- `akkado_get_shape_index(source)` returns JSON per §5.2.
- Pattern shapes include fixed fields (5), `custom_fields` from SEQPAT_PROP, and pattern-field aliases.
- Editor pulls on idle (300ms), caches by source hash, uses results for `r.` and `%.` completion.

**Verification:**
- Unit tests for `ShapeIndexBuilder` covering: simple record, nested record, pattern with `.set()`-derived custom fields, partial source after parse error.
- Manual: define `cfg = {a: 1, b: 2}` in editor; type `cfg.|`; popup shows `a` and `b`.
- Manual: define `beat = n"c4".set("cutoff", saw(0.5))`; type `beat |> sine(%.|`; popup shows `freq, vel, trig, gate, type, cutoff` (+ aliases).

### Phase 3 — Destructuring (4–6 days)

**Goal:** `{x, y} = r`, `fn f({x, y}) -> …`, `{x = 1, y = 2} = r` all parse, analyze, and codegen.

**Files:** `ast.hpp`, `parser.cpp`, `analyzer.cpp`, `codegen.cpp`, `codegen_functions.cpp`, tests.

**Deliverables:**
- AST nodes for assignment-form and param-form destructure.
- Shared `parse_destructure_fields()` used by all three forms (incl. existing `as {x, y}`).
- Defaults evaluated lazily — only when source field is missing.
- Diagnostic **E187** for source missing required (no-default) fields; **E188** for duplicate fields. (See §10.0 reserved codes.)

**Verification:**
- Parser tests for all three forms incl. trailing comma, mixed defaults.
- Codegen tests asserting bindings reach correct buffers.
- Integration: `fn synth({freq = 440, wave = "saw"}) -> osc(wave, freq); synth({})` compiles to a saw at 440 Hz.

### Phase 4a — Record-Valued State Cells, Mechanism Only (3 days)

**Goal:** `state(record_literal)`, `get(cell).field`, `set(cell, new_record)`, and `update(cell, patch)` all work. No sugar yet.

**Files:** `typed_value.hpp`, `analyzer.cpp` (type rules), `codegen_state.cpp` (or equivalent), stdlib for `update`, tests.

**Deliverables:**
- `state()` accepts Record TypedValue; type-tracks the record shape on the cell.
- `get()` returns the held Record; field access works through it (existing record-field-access path; no sugar dispatch yet).
- `set()` validates shape match; emits **E189** on mismatch.
- `update(cell, patch)` defined as a stdlib `fn` using existing spread (lands once spread PRD ships; until then, document the helper but require users to inline `set(cell, {..get(cell), ..patch})`).
- Hot-swap state preservation works for record-valued cells (verify via existing hot-swap test infra). Cross-shape reloads fall back to the new initial record (no silent partial-merge).

**Verification:**
- Unit tests for shape-mismatched `set()` (**E189**).
- Unit tests for `set()` given a non-Record value (**E122**, message widened).
- Integration: button trigger updates voice via explicit `set(voice, {..get(voice), gate: 1})`; verify audio.
- Hot-swap test: cell value persists across structural code reload using `update()`.

### Phase 4b — Field Sugar over Record-Valued State Cells (3 days)

**Goal:** `cell.field` reads and `cell.field = expr` writes work as bidirectional sugar over the Phase 4a mechanism. Phase 4a tests still pass; the sugar produces equivalent observable behaviour.

**Prerequisite:** Phase 4a shipped.

**Files:** `ast.hpp` (FieldAssignment), `parser.cpp`, `analyzer.cpp` (sugar disambiguation, deferred-feature errors), `codegen.cpp` (field-access sugar branch + FieldAssignment lowering), tests.

**Deliverables:**
- `FieldAssignment` AST node added.
- Parser recognizes `cell.field = expr` in statement context; rejects in expression-only contexts (e.g. as RHS of `=`).
- Read sugar: `handle_field_access` branches on `StateCell<Record>` receivers and emits a synthetic `get`.
- Write sugar: `FieldAssignment` lowers to the equivalent of `set(receiver, {..get(receiver), field: value})`, reusing existing record-spread codegen.
- Analyzer rejects nested-field write (`cell.inner.x = 5`) with a clear "deferred" error.
- Analyzer rejects field assignment on value records with E150 + a hint pointing at `state(...)`.

**Verification:**
- Unit tests for `cell.x` read sugar producing the same buffer as `get(cell).x`.
- Unit tests for `cell.x = v` write sugar producing the same effect as `set(cell, {..get(cell), x: v})`.
- Unit tests for self-referential update `cell.x = cell.x + 1` lowering correctly.
- Unit tests for E150 on `r.x = v` where `r` is a value record (with the state(...) hint).
- Unit tests for nested-field-write rejection.
- Unit tests for parse-rejection on pipe-position field assignment outside a block.
- Integration: button trigger toggles `gate` field via `voice.gate = 1`; verify audio matches the Phase 4a integration test using `update()`.

**Off-ramp:** if real-world usage of Phase 4a (with explicit `get`/`set`/`update`) proves ergonomic enough that the sugar feels unnecessary, Phase 4b can be deferred to a future PRD without affecting the rest of this work. The mechanism stands on its own.

### Phase 5 — Convention Documentation + Migration of Viz (2 days)

**Goal:** Document the record-as-options convention in `docs/` (language reference + agent guide); migrate viz handlers to the shared `extract_options` helper without changing behavior.

**Files:** language reference docs, `codegen_viz.cpp`, `codegen.cpp` (helper).

**Deliverables:**
- Convention doc with the four recommended families enumerated as future PRD candidates.
- Viz handlers use `extract_options(node, schema)`; existing viz tests still pass.

### Phase 6 — Tests, Docs, Audit (1–2 days)

**Goal:** Comprehensive test coverage; PRD audit per nkido convention.

**Files:** test files across all phases; `docs/audits/prd-records-phase-2_audit_<date>.md` once shipped.

---

## 10. Edge Cases

### 10.0 Reserved Diagnostic Codes

The records audit (`audits/prd-records-and-field-access_audit_2026-05-05.md`) flagged drift between PRD-spec'd codes (E060–E065) and emitted ones (E135/E136/E140). To prevent recurrence, this PRD nails down its codes against actual current usage. Codes were chosen from gaps in the existing distribution surveyed at PRD draft time — see `grep -rh 'error("E[0-9]' akkado/src/`.

| Code | Reuse / new | Site | Message |
|---|---|---|---|
| **E122** | Existing — message widened | `codegen_state.cpp` | "state() / set() initial value must be a number, signal, or record" (was: "must be a number or signal"). The pre-existing rejection of Function/Array/StateCell stays. |
| **E136** | Existing — reused | analyzer / codegen | "Unknown field 'X' on state cell. Available: …" — same family as the existing pattern/record unknown-field. |
| **E150** | Existing — message extended | analyzer | Currently "Cannot reassign immutable variable 'X'" (`analyzer.cpp:215, 513`); add a sibling case "Cannot assign to field of immutable value record. Declare with `state({...})` to allow mutation." |
| **E187** | New | analyzer (destructure) | "Destructure source missing required field 'X' (no default declared)." Distinct from the existing **E141** ("Destructure field 'X' not found in record/pattern"), which fires from the `as {x, y}` pipe-binding path; E187 fires from the new statement-level / fn-param destructure paths. |
| **E188** | New | parser/analyzer (destructure) | "Duplicate field 'X' in destructure pattern." |
| **E189** | New | analyzer (state cell) | "State cell shape mismatch — expected fields {…}, got {…}." Replaces the conflicting E170 originally drafted. |
| **E204** | New | analyzer (field-assign sugar) | "Nested field assignment is not supported in v1; rewrite as `set(cell, {..get(cell), inner: {..get(cell).inner, field: v}})`." |
| **E205** | New | parser (field-assign sugar) | "Field assignment is a statement, not an expression. Wrap in a block: `expr |> { cell.x = 5 }`." |
| **W160** | Existing (per spread PRD) | analyzer (option fields) | "Field 'X' has no matching option in `<builtin>`." Reused from the spread PRD's W160 to keep severity consistent across the two surfaces (per §12.5 recommendation). |

**Codes deliberately NOT used.** **E170** (already in use at `analyzer.cpp:200, :507`, `codegen.cpp:606`, `codegen_viz.cpp:147` for read-only-builtin assignment & `pianoroll()` arg validation) and **E180** (already in use at `codegen_arrays.cpp:668` for `spread()` count and `codegen_stereo.cpp:100` for `mono()` arg) — both originally drafted into this PRD; both reassigned to the E187–E189 / E204–E205 ranges above to avoid clashes.

### 10.1 Editor / Shape Index

- **Source has parse error.** `akkado_get_shape_index` returns whatever was bound before the error, plus an `errors: […]` array. Editor still serves completions for valid bindings.
- **Cursor inside string literal.** Suppress `.`-context completion entirely (current behavior — preserve).
- **Binding shadowed within a function body.** Shape index reports outer-scope shape only (top-level bindings). Function-local shadowing is out of scope for v1 — same restriction as the existing user-variable extraction.
- **Pattern at `%` is ambiguous (no clear LHS).** `patternHole` field omitted. Editor falls back to no completion rather than guessing.
- **Static schema collides with analyzer-emitted shape for the same builtin.** Static wins — it represents the contract; the analyzer-emitted one represents the actual bound value.

### 10.2 Destructuring

| Input | Expected |
|---|---|
| `{x, y} = {x: 1}` (no default for `y`) | **E187**: "destructure source missing required field 'y'" |
| `{x = 0, y = 0} = {x: 5}` | `x = 5, y = 0` |
| `{x, y} = 42` | **E140**: "destructure source is not a record" (existing) |
| `{x, y} = {x: 1, y: 2, z: 3}` | OK; `z` ignored (no warning — extra fields are common when destructuring a wider record) |
| `fn f({x, y = 1, z}) -> …` | OK at definition. Caller must supply a record where `x` and `z` are present; `y` falls back. |
| `{x, x} = r` | **E188**: "duplicate field 'x' in destructure pattern" |
| Nested destructure `{a, b: {c}} = r` | **Out of scope for v1.** Defer. |

### 10.3 Record-as-Options Convention

- **Caller passes a record with extra fields.** W160 warning per spread PRD's W160 (reuse same code) — "field 'foo' has no matching option in <builtin>".
- **Caller passes an empty record `{}`.** All defaults apply. No warning.
- **Caller passes a non-record value where a record is expected.** Existing E140 path.
- **Spread caller `f(..preset)` where `preset` is missing required option fields.** Existing spread PRD handles via W160 (extra) and E105 (missing required). No new behavior.

### 10.4 State-Cell Records and Field Sugar

| Input | Expected |
|---|---|
| `s = state({x: 1}); set(s, {x: 2, y: 3})` | **E189**: "state cell shape mismatch — expected {x}, got {x, y}" |
| `s = state({x: 1}); set(s, 42)` | **E122** (widened): "state cell holds Record, set() given Number" |
| `s = state({x: 1}); get(s) + 1` | Existing field-access required: error E061 "Cannot apply '+' to Record" |
| Hot-swap: previous `voice = state({freq: 440})`, new code adds field `voice = state({freq: 440, vel: 0.5})` | Cell shape changed; existing hot-swap handles by re-initializing or mapping by ID hash — verify in Phase 4. **If the previous shape's serialized state cannot be migrated, fall back to the new initial record (no silent partial-merge).** |
| `s = state({x: 1}); s.x` | Read sugar: equivalent to `get(s).x` |
| `s = state({x: 1}); s.x = 5` | Write sugar: equivalent to `set(s, {..get(s), x: 5})` |
| `s = state({x: 1}); s.y = 5` | **E136**: "Unknown field 'y' on state cell. Available: x" |
| `r = {x: 1}; r.x = 5` | **E150** (existing, message extended): "Cannot assign to field of immutable value record. Declare with `state({...})` to allow mutation." |
| `s = state({inner: {x: 1}}); s.inner.x = 5` | **E204**: "Nested field assignment is not supported in v1. Use `set(s, {..get(s), inner: {..get(s).inner, x: 5}})`." |
| `expr |> s.x = 5` | **E205** (parse): "Field assignment is a statement, not an expression. Wrap: `expr |> { s.x = 5 }`." |
| `s = state({x: 1}); s.x = s.x + 1` | OK. Lowers to `set(s, {..get(s), x: get(s).x + 1})` — two reads + one write of the cell. Cheap; no special handling needed. |
| `s = state({x: 1}); t = s; t.x = 5` | OK. `t` and `s` alias the same cell (cell identity flows through the binding). `get(s).x == 5` after the write. |

### 10.5 Custom Pattern Fields

- **Same field name from multiple `.set()` calls.** Last wins (existing `custom_fields` map semantics).
- **Custom field name collides with a fixed field.** Resolved by inspecting current behavior in `akkado/src/typed_value.cpp:66–84` (`pattern_field()`): the lookup checks the **fixed-field table first**, falling back to `custom_fields` only if no fixed match. Therefore `n"c4".set("freq", x)` *silently allocates a `custom_fields["freq"]` buffer that is unreachable via `%.freq` or `pat.freq`* — the fixed `freq` buffer wins at every access site. The collision is data-loss-quiet; for editor autocomplete this PRD treats it as a correctness issue and **deduplicates by name in the shape-index emitter** (fixed entry exported, custom entry suppressed) so the autocomplete cannot mislead the user into typing a colliding `.set()` and assuming `%.freq` will read it. A user-visible warning at the `.set()` site is recommended as a follow-up but explicitly out of scope for this PRD (pattern-build-time emitter changes).
- **Custom field referenced before `.set()` chain completes.** Already handled — codegen visits the chain before emitting field accesses.

---

## 11. Testing / Verification Strategy

### 11.1 Unit Tests (per phase)

- Phase 1: 6+ tests for `OptionSchema` JSON emission across viz builtins.
- Phase 2: 10+ tests for `ShapeIndexBuilder` covering record, nested record, pattern fixed fields, pattern custom fields, aliases, partial-source recovery.
- Phase 3: 15+ parser tests, 10+ codegen tests, defaults coverage, **E187** + **E188** cases.
- Phase 4: 15+ tests for record-valued state cells AND field sugar — including **E189** (shape mismatch), **E122** (non-Record `set` value), hot-swap survival, read-sugar equivalence with `get()`, write-sugar equivalence with `set(...{..get(...)...})`, **E150** on value-record assignment, **E136** on unknown field, **E204** on nested-field write, **E205** on pipe-position write, alias semantics.

### 11.2 Integration Tests

```cpp
TEST_CASE("editor surfaces user record fields") {
    const char* src = R"(
        cfg = {wave: "saw", cutoff: 2000}
        osc(cfg.wave, 440) |> lp(%, cfg.cutoff) |> out(%, %)
    )";
    auto idx = json::parse(akkado_get_shape_index(src));
    auto fields = idx["bindings"]["cfg"]["fields"];
    CHECK(field_named(fields, "wave"));
    CHECK(field_named(fields, "cutoff"));
}

TEST_CASE("editor surfaces pattern custom fields") {
    const char* src = R"(
        beat = n"c4 e4".set("filt", saw(0.5))
    )";
    auto idx = json::parse(akkado_get_shape_index(src));
    auto fields = idx["bindings"]["beat"]["fields"];
    CHECK(field_named(fields, "freq"));
    CHECK(field_named(fields, "filt"));
}

TEST_CASE("destructure with defaults") {
    auto result = akkado::compile(R"(
        {x = 0, y = 0} = {x: 5}
        out(x, y)
    )");
    CHECK(result.success);
    // verify x=5, y=0 reaches output
}

TEST_CASE("record-valued state cell update") {
    auto result = akkado::compile(R"(
        v = state({freq: 440, gate: 0})
        button("note") |> set(v, {..get(v), gate: 1})
        sine(get(v).freq) * get(v).gate |> out(%, %)
    )");
    CHECK(result.success);
}

TEST_CASE("record-valued state cell shape mismatch") {
    auto result = akkado::compile(R"(
        v = state({freq: 440})
        set(v, {freq: 880, vel: 0.5})
    )");
    CHECK_FALSE(result.success);
    CHECK(has_diagnostic(result, "E189"));
}

TEST_CASE("state cell read sugar matches get()") {
    auto sugared = akkado::compile(R"(
        v = state({freq: 440, vel: 0.5})
        out(sine(v.freq) * v.vel, %)
    )");
    auto explicit_ = akkado::compile(R"(
        v = state({freq: 440, vel: 0.5})
        out(sine(get(v).freq) * get(v).vel, %)
    )");
    CHECK(sugared.success);
    CHECK(explicit_.success);
    // Bytecode equivalence (or at least same observable behaviour) expected.
}

TEST_CASE("state cell write sugar matches set + spread") {
    auto sugared = akkado::compile(R"(
        v = state({freq: 440, gate: 0})
        button("note") |> { v.gate = 1 }
    )");
    auto explicit_ = akkado::compile(R"(
        v = state({freq: 440, gate: 0})
        button("note") |> set(v, {..get(v), gate: 1})
    )");
    CHECK(sugared.success);
    CHECK(explicit_.success);
}

TEST_CASE("field assignment on value record is E150") {
    auto result = akkado::compile(R"(
        r = {x: 1}
        r.x = 5
    )");
    CHECK_FALSE(result.success);
    CHECK(has_diagnostic(result, "E150"));
}

TEST_CASE("nested-field write is deferred") {
    auto result = akkado::compile(R"(
        s = state({inner: {x: 1}})
        s.inner.x = 5
    )");
    CHECK_FALSE(result.success);
    // Diagnostic message includes "nested field assignment" / "deferred".
}
```

### 11.3 Manual / UX Verification

1. Run dev server (`cd web && bun run dev`), open editor.
2. Type `cfg = {a: 1, b: 2}` then on a new line `cfg.` — popup must show `a` and `b`.
3. Type `n"c4".set("foo", 1) as e |> sine(e.` — popup must include `freq`, `vel`, …, plus `foo`.
4. Type `waterfall(sig, "x", { gra` — popup must show `gradient`.
5. Define `synth = fn({freq = 440, wave = "saw"}) -> osc(wave, freq)`; call `synth({})` — should compile and produce a saw at 440 Hz.
6. Define a button-triggered state-cell update; verify audio responds and that hot-reloading the patch preserves the cell value.

### 11.4 Build / Run Commands

```bash
# Backend
cmake --build build
./build/akkado/tests/akkado_tests "[records]"
./build/akkado/tests/akkado_tests "[shape-index]"
./build/akkado/tests/akkado_tests "[destructure]"
./build/akkado/tests/akkado_tests "[state-cell-record]"
./build/akkado/tests/akkado_tests "[field-sugar]"

# WASM (after Phase 1+2)
cd web && bun run build:wasm

# Editor
cd web && bun run dev      # Manual test
bun run check              # Type-check
```

### 11.5 Success Criteria / Done Definition

The PRD is considered complete when **all** of the following hold:

**Phase-level gates** — each phase ships only when its own gate passes:

| Phase | Gate |
|---|---|
| 1 | All viz builtins emit `optionFields` in `akkado_get_builtins_json()`. Editor surfaces field hints inside record-typed builtin args. New unit tests pass. |
| 2 | `akkado_get_shape_index(source)` returns shapes for all top-level Record / Pattern / Array-of-Record bindings, including pattern `custom_fields` and aliases. Editor pull-on-idle wired up; manual UX checks §11.3.2–§11.3.3 pass. |
| 3 | `{x, y} = r`, `fn f({x, y}) -> …`, and `{x = 1, y = 2} = r` all parse, analyze, and codegen. **E187** emitted on missing required fields with no default; **E188** on duplicate fields. All existing pipe-side `as {x, y}` tests still pass (regression gate). |
| 4a | `state(record_literal)`, `get(cell)`, `set(cell, ...)`, and stdlib `update()` all work end-to-end. **E122** message widened; **E189** on shape mismatch. Hot-swap test passes for record-valued cells. |
| 4b | `cell.field` and `cell.field = expr` produce observably equivalent behaviour to the explicit `get`/`set` forms (verified by paired tests). **E150** (existing, message extended) on value-record assignment with the `state(...)` hint. **E204** for nested-field-write rejection. **E205** parse error on pipe-position write outside a block. |
| 5 | Convention documented in language reference + agent guide. Viz handlers migrated to the shared `extract_options` helper with all existing viz tests still green (behaviour-preserving migration gate). |
| 6 | Audit doc `docs/audits/prd-records-system-unification_audit_<date>.md` written and recommends Status `DONE`. |

**Whole-PRD gates** — must all hold simultaneously before flipping the Status header to `DONE`:

1. **All phase gates above pass.**
2. **No regressions:** `./build/akkado/tests/akkado_tests` (full suite) is green; `./build/cedar/tests/cedar_tests` is green (modulo the one pre-existing skip noted in the records audit).
3. **Manual UX checks §11.3.1–§11.3.6 all pass** in the dev server with a fresh browser session.
4. **Backwards compatibility:** every test that passed on the merge base of this PRD's branch still passes on its head. The grep audit `grep -r "{ *\.\." akkado/tests/` and equivalent for record-literal forms shows no test removed or weakened.
5. **Documentation up to date:** language reference reflects the new destructuring and state-cell sugar. Agent guide entry exists for the record-as-options convention.
6. **All five Open Questions in §12 are resolved or explicitly deferred** with a rationale captured in the PRD or follow-up.
7. **Off-ramp honoured:** if Phase 4b was deferred per its own off-ramp, that decision is recorded in the audit and the PRD Status reflects partial completion (e.g. `PARTIAL — Phase 4b deferred`).

**What `DONE` does NOT require:**
- §3 of `prd-records-and-field-access.md` shipped — that is an independent track per §3.3/§3.4.
- Per-family options PRDs (sampler, filter, delay) shipped — convention only is in scope here.
- Spread PRD shipped — soft prerequisite; if it slips, this PRD ships without spread-paired tests and a follow-up adds them.

---

## 12. Open Questions

### 12.1 Custom field shadowing a fixed pattern field

**Resolved during PRD review (2026-05-07).** Investigation of `typed_value.cpp:66–84` shows fixed fields shadow custom fields silently — the `.set("freq", …)` value is computed but unreachable. This PRD's editor shape index deduplicates by name (fixed wins, custom suppressed) so autocomplete agrees with codegen. A user-visible warning at the `.set()` site is recommended as a follow-up but is out of scope here. See §10.5.

### 12.2 Nested destructuring

`{a, b: {c, d}} = r` would let the user reach into a nested record in one statement. Useful but adds parser ambiguity (nested `{}` vs. block). Deferred — revisit if user requests.

### 12.3 Renaming destructure `{x: a, y: b} = r`

Conflicts with field-shorthand (`{x, y}`) at parse time. Resolvable but cost > value at the moment. Deferred.

### 12.4 `modify(cell, field, fn_)` helper

Requires computed-field-name resolution — currently rejected by `prd-records-and-field-access.md` §Q5. Revisit if state-cell ergonomics warrant.

### 12.5 Schema-driven option validation severity

Today the spread PRD proposes W160 (warning) for unknown fields. Should record-as-options use W160 too, or escalate to error? Inconsistent severity across the two surfaces would be confusing — recommend W160 throughout for now.

---

## 13. Future Extensions

- **Type annotations** for records (`type Config = {a: Number, b: Number}`). Would let the static schema and analyzer dump merge into a single mechanism.
- **Record methods** — bind functions to record types. Substantial scope; explicitly future per existing records PRD §10.3.
- **Mini-notation completion** inside pattern strings. Out of scope per `prd-editor-autocomplete.md`.
- **Cursor-aware shape pull** — future optimization on the analyzer dump's pull-on-idle.
- **Conversion of sampler/filter/delay families** to record-as-options. Each needs its own PRD.

---

## 14. References

- `docs/prd-records-and-field-access.md` — DONE as of 2026-05-07; all §3 extended pattern fields shipped except `voice` (deferred under polyphony pivot).
- `docs/audits/prd-records-and-field-access_audit_2026-05-05.md` — recommended PARTIAL pre-shipment; tracks diagnostic-code drift (E060–E065 spec'd vs E135/E136/E140 emitted) — the precedent that motivates §10.0's reserved-codes table here.
- `docs/prd-record-argument-spread.md` — spread feature spec; prerequisite for parts of this PRD.
- `docs/prd-editor-autocomplete.md` — DONE; this PRD extends its WASM-export pipeline.
- `docs/prd-compiler-type-system.md` — Phase 1 done; TypedValue is the source of truth for shape index.
- `akkado/include/akkado/typed_value.hpp` — `RecordPayload`, `PatternPayload`.
- `akkado/src/codegen_viz.cpp` — existing record-as-options implementation to generalize.
