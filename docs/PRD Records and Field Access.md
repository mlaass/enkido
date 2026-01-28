# PRD: Records and Field Access in Akkado

**Version:** 1.0
**Status:** Draft
**Author:** Claude
**Date:** 2026-01-28

---

## 1. Overview

### 1.1 Problem Statement

Currently, pattern events in Akkado produce multiple data streams (trigger, velocity, frequency), but accessing them requires explicit closure parameters:

```akkado
// Current: Must use closure to access multiple event fields
seq("c4 e4 g4", (t, v, f) -> osc("sin", f) * v * ar(t, 0.01, 0.1))

// Desired: Direct field access through pipe
pat("c4 e4 g4") |> osc("sin", %.freq) * %.vel * ar(%.trig, 0.01, 0.1)
```

More broadly, Akkado lacks a general mechanism for structured data with named fields, limiting:
- Grouping related parameters (envelope settings, filter configs)
- Returning multiple values from functions
- Passing configuration objects

### 1.2 Goals

1. Enable field access on pattern event data via `%.field` syntax
2. Introduce record literals `{field: value, ...}` for user-defined structured data
3. Support field access on any record via `expr.field` syntax
4. Maintain compile-time expansion semantics (no runtime record type)
5. Preserve backwards compatibility with existing code

### 1.3 Non-Goals

- Runtime dynamic field access or reflection
- Object-oriented features (methods bound to records, inheritance)
- Mutable records (records are value types, like everything in Akkado)
- Type annotations or explicit record type definitions (for MVP)

---

## 2. Syntax Specification

### 2.1 Record Literals

```ebnf
record_literal = "{" [ record_field { "," record_field } [ "," ] ] "}" ;
record_field   = identifier ":" pipe_expr ;
```

**Examples:**
```akkado
// Simple record
pos = {x: 1.0, y: 2.0}

// Record with expressions
env = {attack: 0.01, decay: 0.1, level: osc("sin", 0.5) * 0.5 + 0.5}

// Nested records
synth = {
    osc: {type: "saw", freq: 440},
    filter: {cutoff: 1000, q: 0.7}
}

// Empty record
empty = {}

// Trailing comma allowed
config = {
    rate: 1.0,
    depth: 0.5,
}
```

### 2.2 Field Access

```ebnf
field_access = primary "." identifier ;
```

Field access has the same precedence as method calls (highest, after atoms).

**Examples:**
```akkado
pos.x                    // Simple field access
synth.osc.freq           // Nested field access
env.attack + env.decay   // In expressions
lp(sig, config.cutoff)   // As function argument
```

### 2.3 Hole Field Access (Single Pipe Stage)

```ebnf
hole = "%" [ "." identifier ] ;
```

**Examples:**
```akkado
pat("c4") |> osc("sin", %.freq)          // Access frequency field
pat("c4") |> %.vel * ar(%.trig, 0.01)    // Multiple field access
pat("c4") |> %                            // Bare hole (backwards compatible)
```

**Limitation:** `%.field` only works in the immediate RHS of a pipe where LHS is a record-producing expression. For multi-stage pipes, use `as` binding.

### 2.4 Pipe Binding with `as`

```ebnf
pipe_expr = or_expr [ "as" identifier ] { "|>" or_expr } ;
```

The `as` keyword binds the LHS value to a name, making it accessible in all subsequent pipe stages:

```akkado
// Bind pattern to 'e', access fields in multiple stages
pat("c4 e4 g4") as e |> osc("sin", e.freq) |> % * e.vel |> lp(%, 1000 * e.vel)

// Works with ANY record-returning expression, not just patterns
fn synth_voice(freq) -> {sig: osc("saw", freq), env: ar(1, 0.01, 0.3)}
synth_voice(440) as v |> lp(v.sig, 1000) * v.env |> out(%, %)

// Can chain multiple bindings
pat("c4") as e |> osc("sin", e.freq) as dry |> reverb(%) |> dry * 0.3 + % * 0.7

// Works with ANY expression type - useful for reusing intermediate values
saw(440) as s |> lp(s, 1000) + lp(s, 2000)  // s used twice without recomputation
noise() as n |> lp(n, 500) * 0.5 + hp(n, 2000) * 0.3
```

**Key Points:**
- `as` creates an immutable binding visible in all subsequent pipe stages
- Works with ANY expression (scalars, records, arrays, patterns)
- For records, enables field access: `e.freq`, `v.sig`
- For scalars, useful for reusing values without recomputation
- Multiple `as` bindings can coexist in a pipe chain
- `%` still refers to the immediate LHS of each `|>`

### 2.5 Shorthand Field Syntax

When a variable name matches the desired field name:

```akkado
x = 1
y = 2
pos = {x, y}  // Equivalent to {x: x, y: y}

// Mixed shorthand and explicit
freq = 440
{freq, vel: 0.8}  // {freq: 440, vel: 0.8}
```

### 2.6 Distinguishing Field Access from Method Calls

The parser differentiates based on presence of parentheses:
- `x.foo` → field access
- `x.foo()` → method call

Both are parsed in the same `method_expr` rule.

---

## 3. Pattern Event Record

Pattern functions (`pat()`, `seq()`, `note()`) produce a built-in record type with these fields:

### 3.1 Core Fields

| Field | Aliases | Type | Description |
|-------|---------|------|-------------|
| `trig` | `trigger`, `t` | Signal (0/1) | Trigger pulse at event onset |
| `vel` | `velocity`, `v` | Signal (0-1) | Event velocity/amplitude |
| `freq` | `frequency`, `f`, `pitch`, `p` | Signal (Hz) | Frequency for pitch events |
| `note` | `midi`, `n` | Signal (0-127) | Raw MIDI note number |
| `sample` | `s` | Signal (ID) | Sample identifier for sample events |

### 3.2 Extended Fields

| Field | Aliases | Type | Description |
|-------|---------|------|-------------|
| `dur` | `duration` | Signal (cycles) | Event duration |
| `chance` | | Signal (0-1) | Event probability |
| `time` | `t0`, `start` | Signal (cycles) | Event start time within cycle |
| `phase` | `cycle`, `co` | Signal (0-1) | Current position within event |

### 3.3 Unified Event Model (Scalar Fields Only)

**All event fields are scalars.** Chords are "destructured" into multiple simultaneous events, each with its own scalar fields. This provides a uniform model that works for mini-notation, MIDI files, or any note source.

```akkado
// Single note "a4" → 1 event
pat("a4") as e |> osc("sin", e.freq) * e.vel |> out(%, %)

// Chord "Cmaj" → 3 simultaneous events, auto-summed
chord("C") as e |> osc("sin", e.freq) * e.vel |> out(%, %)
// Internally: 3 oscillators created, outputs summed automatically
```

**Event fields** (all scalars, all present on every event):

| Field | Type | Description |
|-------|------|-------------|
| `type` | String | Event type: `"pitch"`, `"sample"`, `"rest"` |
| `freq` | Signal (Hz) | Note frequency (0 for samples unless pitched) |
| `note` | Signal (0-127) | MIDI note number (for pitch or pitched sample playback) |
| `vel` | Signal (0-1) | Note/sample velocity |
| `trig` | Signal (0/1) | Trigger pulse at event onset |
| `dur` | Signal (cycles) | Event duration |
| `time` | Signal (cycles) | Event start time within cycle |
| `phase` | Signal (0-1) | Current position within event |
| `voice` | Signal (int) | Voice index (0, 1, 2, ...) for polyphony |
| `sample` | String | Sample name (`""` for pitch events) |
| `sample_id` | Signal (int) | Numeric sample ID (0 for pitch events) |

**Event types:**
- `"pitch"`: Melodic note with `freq`/`note` set, `sample` empty
- `"sample"`: Sample trigger with `sample`/`sample_id` set, `freq`/`note` optionally for pitching
- `"rest"`: Silence, `trig` = 0

**Pitched sample playback:**
```akkado
// Sample "bd" pitched to C4 - freq/note used for pitch-shifting
pat("bd'c4 bd'e4 bd'g4") as e |>
    sample_play(e.sample_id, e.note) * e.vel
```

### 3.4 Polyphony and Voice Control

**Default behavior:** Multiple simultaneous events are processed independently and auto-summed. A configurable `voices:` parameter limits polyphony.

```akkado
// Default: up to 16 voices, auto-summed
chord("C Am7") as e |> osc("sin", e.freq) * e.vel

// Explicit voice limit
chord("C Am7", voices: 4) as e |> osc("sin", e.freq) * e.vel
```

**Per-voice control:** Use the `.voice` field with `match` or `select` to route voices differently:

```akkado
// Different oscillator per voice
chord("C") as e |> match(e.voice) {
    0: osc("sin", e.freq),   // root gets sine
    1: osc("saw", e.freq),   // third gets saw
    _: osc("tri", e.freq),   // fifth gets triangle
} * e.vel |> out(%, %)

// Simple branching with select
chord("C") as e |>
    select(e.voice == 0, osc("sin", e.freq), osc("saw", e.freq)) * e.vel

// Spread voices in stereo
chord("C") as e |>
    osc("sin", e.freq) * e.vel |>
    out(% * (1 - e.voice * 0.3), % * (e.voice * 0.3))
```

### 3.5 Sample Patterns

Sample patterns work identically to pitch patterns:

```akkado
// Simple sample pattern
pat("bd sd bd hh") as e |> sample_play(e.sample_id) * e.vel |> out(%, %)

// Sample with variant (bd:2)
pat("bd:0 bd:1 bd:2") as e |> sample_play(e.sample_id) * e.vel

// Mixed pattern handling via .type
pat("bd c4 sd e4") as e |> match(e.type) {
    "sample": sample_play(e.sample_id),
    "pitch": osc("sin", e.freq),
    _: dc(0)
} * e.vel |> out(%, %)

// Pitched sample playback (sample + pitch)
pat("bd'c2 bd'e2 bd'g2") as e |>
    sample_play(e.sample_id, pitch: e.note) * e.vel
```

**Design rationale:**
- Uniform model works for any note source (mini-notation, MIDI, OSC, etc.)
- No special handling for chords vs single notes or samples vs pitches
- `.type` field enables routing when mixing event types
- Auto-sum is the common case; `.voice` enables advanced routing
- Compile-time voice allocation enables efficient buffer management

### 3.6 Default Field for Bare `%`

When `%` is used without a field in a pattern pipe:
- **Pitch patterns:** Defaults to `%.freq`
- **Sample patterns:** Defaults to `%.sample_id`
- **Note patterns:** Defaults to `%.note`

**Pattern type detection** (auto-detect from content):
- Contains pitch tokens (`c4`, `f#3`, etc.) → pitch pattern
- Contains sample tokens (`bd`, `sd`, `hh`, etc.) → sample pattern
- Explicit: `note("c4 e4")` always pitch, `samp("bd sd")` always sample

---

## 4. Semantic Model

### 4.1 Compile-Time Expansion

Records are expanded at compile-time into multiple named buffers. There is no runtime "record" value—just individual signal buffers.

```akkado
// Source
pos = {x: saw(1), y: saw(2)}
result = pos.x + pos.y

// Conceptual expansion
__pos_x = saw(1)
__pos_y = saw(2)
result = __pos_x + __pos_y
```

### 4.2 Record Type Inference

The compiler infers record types from literals and propagates them through:
- Variable assignments
- Function parameters
- Function return values

```akkado
fn make_env(a, d) -> {attack: a, decay: d}  // Returns record type
env = make_env(0.01, 0.1)                    // env has inferred record type
env.attack                                    // Valid: field exists
env.foo                                       // Error: unknown field
```

### 4.3 Pattern Pipe Transformation

When `%.field` appears in a pipe where LHS is a pattern:

```akkado
// Source
pat("c4 e4") |> osc("sin", %.freq) * %.vel

// Transformed to (conceptually)
seq("c4 e4", (__t, __v, __f) -> osc("sin", __f) * __v)
```

The analyzer:
1. Detects pattern LHS
2. Scans RHS for `%.field` references
3. Builds field→buffer mapping
4. Substitutes hole references with buffer references

---

## 5. Implementation Phases

### Phase 1: Parser Foundation

**Files:** `ast.hpp`, `parser.cpp`

1. Add `NodeType::RecordLit` for `{field: value, ...}`
2. Add `NodeType::FieldAccess` for `expr.field`
3. Add `NodeType::PipeBinding` for `as` binding in pipes
4. Add `HoleData` variant with optional field name
5. Extend `parse_primary()` to handle `{` as record start
6. Extend `parse_method_expr()` to handle field access (no parens)
7. Extend `parse_hole()` to handle `%.field`
8. Extend `parse_pipe_expr()` to handle `as identifier`
9. Support shorthand `{x, y}` syntax (identifier without `:`)

**Grammar additions:**
```ebnf
record_literal = "{" [ record_field { "," record_field } [ "," ] ] "}" ;
record_field   = identifier [ ":" pipe_expr ] ;  // shorthand when no ":"
pipe_expr      = or_expr [ "as" identifier ] { "|>" or_expr } ;
hole           = "%" [ "." identifier ] ;
```

**Deliverable:** Parser accepts new syntax, produces correct AST

### Phase 2: Symbol Table & Type Tracking

**Files:** `symbol_table.hpp`, potentially new `types.hpp`

1. Add `SymbolKind::Record`
2. Add `RecordTypeInfo` to track field names and types
3. Extend `Symbol` to store record type info
4. Add field lookup methods

**Deliverable:** Symbol table can represent record-typed variables

### Phase 3: Semantic Analysis - Records

**Files:** `analyzer.cpp`

1. Analyze `RecordLit` nodes, infer field types
2. Validate `FieldAccess` nodes (field exists, type compatible)
3. Propagate record types through assignments
4. Handle nested records

**Deliverable:** Analyzer validates record usage, reports field errors

### Phase 4: Semantic Analysis - Pipe Bindings & Pattern Fields

**Files:** `analyzer.cpp`

1. Process `as` bindings: create symbol for bound name, track scope through pipe chain
2. Detect record-typed LHS in pipes (patterns or user records)
3. For `%.field`: validate field exists on record type, resolve to buffer reference
4. For `binding.field`: resolve through symbol table to record's field buffer
5. Handle multiple `as` bindings in same pipe chain

**Key insight:** `as` creates a symbol visible in all subsequent pipe stages. The symbol references a record type, enabling field access.

**Deliverable:** `as` binding and `%.field` syntax work correctly

### Phase 5: Code Generation - Records

**Files:** `codegen.cpp`

1. Generate buffer per record field
2. Track field→buffer mapping per record variable
3. Resolve field access to correct buffer
4. Handle nested field access

**Deliverable:** Records compile to working bytecode

### Phase 6: Code Generation - Pattern Fields

**Files:** `codegen_patterns.cpp`

1. Emit all pattern event buffers (not just trig/vel/freq)
2. Add buffers for extended fields (dur, chance, time, phase)
3. Wire `%.field` to correct buffer

**Deliverable:** Full pattern field access works

### Phase 7: Testing & Documentation

1. Parser tests for all new syntax
2. Analyzer tests for type inference and validation
3. Codegen tests for buffer allocation
4. Integration tests with audio output
5. Update language specification

---

## 6. Edge Cases & Error Handling

### 6.1 Unknown Field Access

```akkado
pos = {x: 1, y: 2}
pos.z  // Error: E060 - Unknown field 'z' on record. Available: x, y
```

### 6.2 Field Access on Non-Record

```akkado
x = 42
x.field  // Error: E061 - Cannot access field on non-record value
```

### 6.3 Pattern Field Outside Pattern Pipe

```akkado
%.freq  // Error: E062 - Pattern field access '%.freq' outside pattern pipe
x = %.vel  // Error: E062
```

### 6.4 Unknown Pattern Field

```akkado
pat("c4") |> %.foo  // Error: E063 - Unknown pattern field 'foo'. Available: trig, vel, freq, ...
```

### 6.5 Duplicate Field Names

```akkado
r = {x: 1, x: 2}  // Error: E064 - Duplicate field 'x' in record literal
```

### 6.6 Empty Field Name

```akkado
r = {: 1}  // Syntax error: Expected field name
```

### 6.7 Shadowing in Nested Records

```akkado
outer = {x: 1, inner: {x: 2}}
outer.x        // 1
outer.inner.x  // 2  (no shadowing issue, different paths)
```

### 6.8 Records in Arrays

```akkado
points = [{x: 0, y: 0}, {x: 1, y: 1}]
map(points, p -> p.x + p.y)  // Valid: each element is a record
```

### 6.9 Mixing Bare % and %.field

```akkado
pat("c4") |> % + %.vel  // % defaults to %.freq, so: %.freq + %.vel
```

### 6.10 Pattern Field on Non-Pattern Pipe

```akkado
saw(440) |> %.freq  // Error: E065 - Pattern field '%.freq' used but LHS is not a pattern
```

### 6.11 Chained Pipes with Pattern Fields

```akkado
pat("c4") |> lp(osc("sin", %.freq), 1000) |> % * %.vel
//                                           ^-- Error: %.vel not valid here
// The second pipe's LHS is the lp() output, not a pattern
```

Clarification needed: Should field references "flow through" multiple pipe stages, or only work in the immediate RHS of a pattern?

---

## 7. Resolved Design Decisions

### D1: Field Reference Scope in Chained Pipes ✓

**Decision:** Explicit `as` binding required for multi-stage access.

```akkado
// Single stage: %.field works
pat("c4") |> osc("sin", %.freq) * %.vel

// Multi-stage: use 'as' binding
pat("c4") as e |> osc("sin", e.freq) |> % * e.vel
```

### D2: Chord Field Access ✓

**Decision:** `%.freq` always returns an array (unified model).

- Single note: `%.freq` = `[440.0]`
- Chord: `%.freq` = `[261.6, 329.6, 392.0]`

Use `map()` and `sum()` to handle uniformly.

### D3: Shorthand Field Syntax ✓

**Decision:** Include in MVP.

```akkado
x = 1
y = 2
pos = {x, y}  // Same as {x: x, y: y}
```

### D4: Record Equality ✓

**Decision:** Not supported. Records are for structuring, not comparison.

---

## 8. Open Questions (Remaining)

### Q1: Field Access on Arrays of Records (Deferred)

```akkado
points = [{x: 1}, {x: 2}]
points.x  // What should this return?
```

**Decision:** Not supported in MVP. Use explicit `map(points, p -> p.x)`.
Future versions may add auto-mapping.

### Q2: Destructuring Assignment

```akkado
{x, y} = pos  // Extract fields to local variables
```

**Recommendation:** Defer to follow-up (requires more parser work)

### Q3: Destructuring in Function Parameters

```akkado
fn distance({x, y}) -> sqrt(x^2 + y^2)
```

**Recommendation:** Defer to follow-up

### Q4: Record Spread/Merge

```akkado
base = {x: 1, y: 2}
extended = {...base, z: 3}  // {x: 1, y: 2, z: 3}
```

**Recommendation:** Defer to future (complex semantics)

### Q5: Computed Field Names

```akkado
field = "x"
r = {[field]: 1}  // Dynamic field name
```

**Recommendation:** Not supported (conflicts with compile-time expansion)

### Q6: Nested Pattern Field Access

```akkado
chord("C") |> %.chord.root
```

Should chord data be accessible as nested fields?

**Recommendation:** Defer - use flat top-level fields for MVP

---

## 9. Backwards Compatibility

### 8.1 Existing Code

All existing code continues to work unchanged:
- `pat("c4") |> osc("sin", %)` - bare `%` defaults to `%.freq`
- `seq("c4", (t, v, p) -> ...)` - explicit closure still works
- `{}` - currently unused, no conflict

### 8.2 Block Syntax

Closure blocks use `{}`:
```akkado
(x) -> { y = x + 1; y * 2 }
```

Record literals also use `{}`:
```akkado
r = {x: 1}
```

**Disambiguation:** Records require `identifier:` inside braces. Blocks contain statements.
- `{ x: 1 }` → record (has `identifier:`)
- `{ x = 1; x }` → block (has `=` and `;`)
- `{}` → empty record (could also be empty block, but empty blocks are useless)

### 8.3 Method Calls

Current method call syntax (parsed but not codegen'd):
```akkado
x.method()
```

New field access syntax:
```akkado
x.field
```

No conflict - methods require `()`.

---

## 10. Future Extensions

### 9.1 Type Annotations (Future)

```akkado
type Point = {x: Float, y: Float}
fn distance(p: Point) -> Float
```

### 9.2 Default Field Values (Future)

```akkado
type Config = {rate: Float = 1.0, depth: Float = 0.5}
c = Config{}  // Uses defaults
```

### 9.3 Record Methods (Future)

```akkado
type Point = {
    x: Float,
    y: Float,
    fn magnitude(self) -> sqrt(self.x^2 + self.y^2)
}
```

---

## 11. Success Criteria

1. **Parser:** All new syntax parses correctly with proper AST representation
2. **Analyzer:** Type inference works, field errors caught at compile time
3. **Codegen:** Records expand to correct buffer allocations
4. **Pattern fields:** `%.field` syntax works with full field set
5. **Tests:** >90% coverage of new functionality
6. **Docs:** Language specification updated
7. **Backwards compatible:** All existing tests pass
