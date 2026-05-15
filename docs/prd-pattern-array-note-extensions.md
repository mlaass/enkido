> **Status: SHIPPED (2026-04-27)** — Phase 1 (this PRD) shipped earlier; Phase 2 ([`prd-pattern-array-note-extensions-phase-2.md`](./prd-pattern-array-note-extensions-phase-2.md)) shipped 2026-04-27 covering Phases 4, 5, 7, 8. One follow-up remains: §5.5a custom-property pipe-binding accessor — see Phase 2 status line for details. The gap analysis is in `docs/audits/prd-pattern-array-note-extensions_audit_2026-04-24.md`.

# PRD: Strudel-Style Pattern System Extensions

## 1. Vision

Extend Akkado with Strudel-compatible patterns, first-class arrays, chord notation, and a rich note object model. This enables expressive live-coding of complex musical structures while maintaining Akkado's modular synthesis capabilities.

**Goals:**
- Adopt Strudel's proven pattern notation and semantics
- Enable chords and arrays as first-class language constructs
- Support MIDI-complete note properties (velocity, aftertouch, pitch bend)
- Provide fluent method chaining on pattern objects
- Build polyphony and voicing in user-space, not as language primitives

## 2. Design Decisions

### 2.1 Array/List Semantics

**Decision**: Explicit `map()` with optional auto-expand sugar.

- **Core primitive**: `map(array, fn)` for explicit iteration
- **Sugar layer**: Auto-expand (`osc(chord)` → N oscillators summed) implemented via compile-time `match` + `map`
- **Rationale**: Low-level control with optional convenience

```akkado
// Explicit (always works)
map([c4, e4, g4], x => osc("saw", x)) |> mix(@) |> out(@, @)

// Sugar (expands at compile time)
osc("saw", [c4, e4, g4]) |> out(@, @)  // Equivalent to above
```

### 2.2 Voice System

**Decision**: Keep synths as mono primitives; build voice concepts in user space.

- Synths are single oscillators (mono)
- Sampler has 32-voice pool (necessary for sample playback)
- Stereo/polyphony are higher-level concepts built on top
- No forced voice allocation at language level

### 2.3 Note Object Model

**Decision**: MIDI-complete + extensible properties.

| Property | Type | Description |
|----------|------|-------------|
| `note` | int/float | MIDI note number or frequency |
| `velocity` | float 0-1 | Note velocity |
| `aftertouch` | float 0-1 | Channel pressure |
| `bend` | float -1 to 1 | Pitch bend |
| `dur` | float | Duration in cycles |
| *custom* | any | User-defined properties |

**Implementation**: PatternEvent struct with named fields, not dictionary. Small array iteration for extensibility.

### 2.4 Chord Syntax

**Decision**: Adopt Strudel's chord symbol format.

```akkado
chord("Am C7 Dm")  // Strudel-compatible
// NOT C4' (old syntax, to be deprecated)
```

**Symbol Format**: `{Root}{Quality}{Extensions}`

| Component | Options |
|-----------|---------|
| Root | C, C#, Db, D, D#, Eb, E, F, F#, Gb, G, G#, Ab, A, A#, Bb, B |
| Quality | (none)=major, m/-=minor, M/^/maj=major, o/dim=diminished, aug/+=augmented, sus=suspended |
| Extensions | 7, 9, 11, 13, 6, 69 |

**Output**: Array of MIDI note numbers (integers).

```akkado
chord("Am")  // → [57, 60, 64] (A3, C4, E4 as MIDI)
```

### 2.5 Voicing System

**Decision**: Full implementation per Strudel spec.

```akkado
chord("Am C G F")
  .anchor("c5")
  .mode("below")
  |> mtof(@) |> osc("saw", @)
```

| Function | Description |
|----------|-------------|
| `anchor(note)` | Center of gravity for voicing |
| `mode(string)` | "below", "above", "duck", "root" |
| Voice leading | Minimize interval distance between chords |
| Voicing dictionaries | Custom voicing shapes |

### 2.6 Pattern Objects & Method Chaining

**Decision**: High priority—patterns as first-class objects.

```akkado
drums = "bd sd [hh hh] cp"
drums.slow(2).rev() |> sampler(@) |> out(@, @)

"bd*4".lp("<4000 2000 1000>")  // Methods on string-as-pattern
```

### 2.7 String-as-Pattern Parsing

**Decision**: Try-parse approach with explicit annotation fallback.

1. Attempt to parse string as mini-notation
2. If valid → Pattern type
3. If not → String type
4. Provide `str("...")` for forcing string interpretation

### 2.8 Pattern Constructors

**Decision**: Follow Strudel naming conventions.

| Function | Purpose |
|----------|---------|
| `s()` | Sample patterns |
| `note()` | Pitch patterns |
| `chord()` | Chord patterns |
| `pat()` / `seq()` | Alternative pattern constructors |

### 2.9 Polymeter

**Status**: Not currently implemented.

**Decision**: Add `{x y}` and `{x}%n` syntax.

```akkado
"[bd sd] {hh hh hh}"     // Polymeter: 2 vs 3
"{bd sd hh}%5"           // 5-step pattern over cycle
```

## 3. Current Implementation Status

| Feature | Status | Notes |
|---------|--------|-------|
| Mini-notation lexer | ✓ Complete | Tokens, modifiers working |
| Mini-notation parser | ✓ Complete | AST nodes for groups, sequences |
| Pattern evaluation | ✓ Complete | Time division, modifiers |
| Array type + map() | ✓ Complete | Multi-buffer expansion at compile time |
| Chord expansion | ✓ Complete | Chords in mini-notation expand to multi-voice via `pat()`/`chord()` |
| Chord parsing | ✓ Complete | Strudel-compatible `chord("Am")` inside patterns |
| `C4'` standalone syntax | Deprecated | Stub (root only) — use chords inside `pat()`/`chord()` instead |
| Dot-call syntax | ✓ Complete | `pat("c4").slow(2)` desugars to `slow(pat("c4"), 2)` |
| Pattern transforms | ✓ Partial | `slow`, `fast`, `rev`, `transpose`, `velocity`, `bank`, `variant` done. Time/structure modifiers (Phase 7) in progress |
| Polymeter `{x}` | ✓ Complete | Tokens, lexer, parser, AST node, evaluator all implemented and tested |
| TypedValue system | ✓ Complete | Full type tracking replaces ad-hoc maps (see prd-compiler-type-system) |
| Voicing system | Deferred | Algorithms not yet implemented |
| String-as-pattern | Deferred | Explicit `pat()` is the endorsed approach |
| Algorithmic generators | Partial | `euclid(k, n, rot)` exists as Cedar opcode. `run()`, `binary()` not yet implemented |

## 4. Implementation Phases

### Phase 1: Arrays + map() — Foundation

**Goal**: Core array type with explicit mapping.

**Scope**:
1. Array literal syntax: `[a, b, c]`
2. Array type in AST and type system
3. `map(array, fn)` function
4. Array indexing: `arr[0]`, `arr[i]`
5. Array length: `len(arr)`

**Design Questions**:
- Compile-time vs runtime arrays: **Start with compile-time, evaluate runtime needs**
- Fixed-size vs dynamic: **Fixed-size initially**
- DAG flow: Arrays expand to parallel nodes at compile time

**Files to Modify**:
- `akkado/include/akkado/ast.hpp` — Add `ArrayLit` node
- `akkado/src/parser.cpp` — Parse `[a, b, c]`
- `akkado/src/codegen.cpp` — Array handling

**Verification**:
```bash
./build/akkado/tests/akkado_tests "[array]"
```
- `[1, 2, 3]` parses to ArrayLit node
- `map([1,2,3], x => x*2)` produces `[2,4,6]`
- `[c4, e4, g4] |> mtof(@) |> osc("saw", @)` produces 3 oscillators

### Phase 2: Pattern Objects & Method Chaining

**Goal**: Patterns as first-class objects with fluent API.

**Scope**:
1. Pattern type wrapping PatternEventStream
2. Method chaining: `.slow()`, `.fast()`, `.rev()`, etc.
3. String-as-pattern parsing (try-parse)
4. Pattern-as-parameter to methods
5. Return pattern objects from constructors

**Syntax**:
```akkado
drums = "bd sd"
drums.slow(2).rev() |> out(@, @)
```

**Files to Create**:
- `akkado/include/akkado/pattern_object.hpp` — Pattern wrapper type

**Files to Modify**:
- `akkado/include/akkado/pattern_eval.hpp` — Return Pattern objects
- `akkado/src/parser.cpp` — Method call parsing on patterns
- `akkado/src/codegen.cpp` — Pattern method compilation

**Verification**:
- `"bd sd"` auto-parses as pattern
- `"bd sd".slow(2)` returns modified pattern
- Pattern with method chain compiles to valid bytecode

### Phase 3: Chord System (Strudel-Compatible)

**Goal**: `chord()` function with proper expansion.

**Scope**:
1. `chord("Am C7 Dm")` parser for chord symbols
2. Chord symbol → interval lookup
3. Chord expansion to array of MIDI notes
4. Integration with Phase 1 arrays
5. Basic octave handling (default voicing)

**Chord Quality Table**:

| Symbol | Intervals (semitones) |
|--------|----------------------|
| (major) | 0, 4, 7 |
| m, - | 0, 3, 7 |
| 7 | 0, 4, 7, 10 |
| M7, maj7, ^ | 0, 4, 7, 11 |
| m7 | 0, 3, 7, 10 |
| dim, o | 0, 3, 6 |
| dim7, o7 | 0, 3, 6, 9 |
| aug, + | 0, 4, 8 |
| sus2 | 0, 2, 7 |
| sus4 | 0, 5, 7 |
| 6 | 0, 4, 7, 9 |
| 9 | 0, 4, 7, 10, 14 |
| add9 | 0, 4, 7, 14 |

**Files to Create**:
- `akkado/include/akkado/chord_parser.hpp` — Chord symbol parsing
- `akkado/src/chord_parser.cpp`

**Files to Modify**:
- `akkado/include/akkado/music_theory.hpp` — Symbol parsing
- `akkado/src/lexer.cpp` — Deprecate old `C4'` syntax
- `akkado/src/codegen.cpp` — Chord expansion via arrays

**Verification**:
- `chord("Am")` → `[57, 60, 64]`
- `chord("C7")` → `[48, 52, 55, 58]`
- Listen: `chord("Am") |> mtof(@) |> osc("saw", @)` sounds like A minor

### Phase 4: Voicing System — DEFERRED to Phase 2 PRD

> Tracked in [`prd-pattern-array-note-extensions-phase-2.md`](./prd-pattern-array-note-extensions-phase-2.md) §5.4. The summary below is preserved for historical context.

**Goal**: Voice leading and voicing controls.

**Scope**:
1. `anchor(note)` — target pitch center
2. `mode("below" | "above" | "duck" | "root")`
3. Voice leading algorithm (minimize interval distance)
4. Voicing dictionary system
5. `addVoicings(name, map)` for custom dictionaries

**Mode Descriptions**:

| Mode | Behavior |
|------|----------|
| `below` | All notes below anchor |
| `above` | All notes above anchor |
| `duck` | Closest voicing that avoids anchor |
| `root` | Root in bass, others voiced around anchor |

**Files to Create**:
- `akkado/include/akkado/voicing.hpp` — Voice leading algorithms
- `akkado/src/voicing.cpp`

**Verification**:
- `chord("C").anchor("c4").mode("below")` places all notes below C4
- Voice leading minimizes movement between consecutive chords

### Phase 5: Extended Note Properties — DEFERRED to Phase 2 PRD

> Tracked in [`prd-pattern-array-note-extensions-phase-2.md`](./prd-pattern-array-note-extensions-phase-2.md) §5.5. The summary below is preserved for historical context.

**Goal**: MIDI-complete note model.

**Scope**:
1. Extend PatternEvent: velocity, aftertouch, pitch bend
2. Mini-notation modifiers for properties
3. Method accessors: `.velocity()`, `.bend()`
4. Extensible named params mechanism

**Syntax**:
```akkado
"c4 e4 g4".velocity("0.8 0.5 1.0").bend("<0 0.5 0>")
note("c4").velocity(0.7).dur(0.5)
```

**Files to Modify**:
- `akkado/include/akkado/pattern_event.hpp` — Extended properties
- `akkado/src/pattern_eval.cpp` — Property methods
- `akkado/src/codegen.cpp` — Property handling

### Phase 6: Polymeter — ✓ COMPLETE

**Status**: Fully implemented and tested.

**Implemented**:
1. `LBrace`/`RBrace`/`Percent` tokens in `mini_token.hpp`
2. Lexer handles `{`, `}`, `@` in `mini_lexer.cpp`
3. `MiniPolymeter` AST node with `MiniPolymeterData.step_count` in `ast.hpp`
4. Parser: `parse_polymeter()` in `mini_parser.cpp` handles `{x y}` and `{x y}%n`
5. Evaluator: `eval_polymeter()` in `pattern_eval.cpp` — divides parent duration into N equal steps, cycles through children

**Syntax**:
```akkado
"[bd sd] {hh hh hh}"   // 2 vs 3 polyrhythm
"{bd sd hh cp}%5"      // 5-step pattern over cycle
```

**Tests**: `test_mini_notation.cpp` — polymeter tokens, basic parsing, step count, nested polymeter, evaluation

### Phase 7: Time & Structure Modifiers — DEFERRED to Phase 2 PRD

> Tracked in [`prd-pattern-array-note-extensions-phase-2.md`](./prd-pattern-array-note-extensions-phase-2.md) §5.1, §5.2. The 12 transforms below have not been implemented; the table is preserved for historical context.

**Goal**: Strudel-compatible pattern transform functions. All are compile-time transforms on the event list, callable via both functional and dot-call syntax (e.g. `early(pat, 0.25)` or `pat.early(0.25)`).

**Already implemented** (same architecture): `slow`, `fast`, `rev`, `transpose`, `velocity`, `bank`, `variant`

**To implement** — compile-time event transforms following the existing pattern (register in `builtins.hpp` with NOP opcode, add to `is_pattern_call()`, add transform logic in `compile_pattern_for_transform()`, add handler in `codegen_patterns.cpp`, register dispatch in `codegen.cpp`):

| Function | Args | Semantics (per Strudel) |
|----------|------|------------------------|
| `early(pat, n)` | n: cycles | Shift all events earlier by n cycles (wrap around). Equivalent to Tidal's `<~` |
| `late(pat, n)` | n: cycles | Shift all events later by n cycles (wrap around). Equivalent to Tidal's `~>` |
| `palindrome(pat)` | — | Reverse event order every other cycle. Alternates forward/backward playback |
| `iter(pat, n)` | n: subdivisions | Divide pattern into n parts, play sequentially, increment starting subdivision each cycle (wraps). **Requires runtime cycle tracking** |
| `iterBack(pat, n)` | n: subdivisions | Same as `iter` but decrements starting subdivision each cycle |
| `ply(pat, n)` | n: repeats | Repeat each event n times within its original timespan |
| `linger(pat, n)` | n: fraction (0-1) | Keep only the first n fraction of the pattern, repeat to fill the cycle |
| `zoom(pat, start, end)` | start/end: 0-1 | Extract the `[start, end)` time range and stretch it to fill the cycle |
| `compress(pat, start, end)` | start/end: 0-1 | Squeeze the entire pattern into `[start, end)`, leaving silence elsewhere. **Note:** naming conflicts with `comp` audio compressor alias — may need `pcompress` or type-based disambiguation |
| `segment(pat, n)` | n: int | Sample the pattern at n evenly-spaced points per cycle, converting continuous to discrete |
| `swingBy(pat, amount, n)` | amount: 0-1, n: slices | Divide cycle into n slices, delay events in the second half of each slice by `amount` relative to half-slice size. 0=straight, 0.5=half-note delay, 1=wraps to straight |
| `swing(pat, n)` | n: slices | Shorthand for `swingBy(pat, 1/3, n)` — standard jazz swing feel |

**Implementation notes:**
- Most transforms are purely compile-time (modify `cedar::Event` fields in `compile_pattern_for_transform()`)
- `iter`/`iterBack` need runtime cycle awareness — may require generating multiple cycles of events or a new approach
- `palindrome` can be implemented by doubling `cycle_length` and appending reversed events
- `segment` converts continuous patterns to discrete — may need special handling for non-pattern inputs

**Files to Modify**:
- `akkado/include/akkado/builtins.hpp` — Register new NOP builtins
- `akkado/include/akkado/codegen.hpp` — Declare handler functions
- `akkado/src/codegen.cpp` — Register dispatch entries
- `akkado/src/codegen_patterns.cpp` — Handler implementations + `compile_pattern_for_transform()` cases + `is_pattern_call()` entries

### Phase 8: Algorithmic Generators — PARTIAL (remainder DEFERRED to Phase 2 PRD)

> `run`, `binary`, `binaryN` are tracked in [`prd-pattern-array-note-extensions-phase-2.md`](./prd-pattern-array-note-extensions-phase-2.md) §5.3. `euclid` already ships.

**Already implemented:**
- `euclid(hits, steps, rot)` — Cedar opcode `EUCLID`, runtime euclidean rhythm generator with optional rotation
- Euclidean in mini-notation — `(k,n)` syntax inside `pat()`

**To implement** — pattern constructors that generate event sequences at compile time:

| Function | Args | Description |
|----------|------|-------------|
| `run(n)` | n: int | Integer sequence pattern with values 0, 1, 2, ... n-1 as evenly-spaced events |
| `binary(n)` | n: int | Binary representation of n as trigger pattern (1=trigger, 0=rest) |
| `binaryN(n, bits)` | n: int, bits: int | Fixed-width binary pattern (zero-padded to `bits` width) |

**Implementation approach**: These are pattern *constructors* (like `pat()`), not transforms. They generate a `PatternEventStream` at compile-time, then emit `SEQPAT_QUERY`/`SEQPAT_STEP` instructions referencing the generated events.

## 5. Open Questions

### Pattern Object Representation
How does a Pattern object serialize to bytecode? Options:
1. Multiple SEQ_STEP instructions
2. Pattern reference instruction + pattern table
3. Inline pattern data in instruction stream

### Auto-Expand Implementation
Exact mechanism for user-space auto-expand sugar:
1. Macro-like `match` on types?
2. Compiler intrinsic with user-definable behavior?
3. Overload resolution based on array parameter?

### String Parsing Ambiguity
Edge cases where valid mini-notation could also be a valid string:
- `"bd"` — sample name or pattern?
- `"c4"` — note or string?

**Resolution**: Context-dependent. In pattern contexts, parse as pattern. Provide `str("bd")` escape hatch.

## 6. File Summary

### New Files

| File | Purpose |
|------|---------|
| `akkado/include/akkado/pattern_object.hpp` | Pattern wrapper type |
| `akkado/include/akkado/chord_parser.hpp` | Chord symbol parsing |
| `akkado/include/akkado/voicing.hpp` | Voice leading algorithms |
| `akkado/src/chord_parser.cpp` | Chord parser implementation |
| `akkado/src/voicing.cpp` | Voicing implementation |

### Modified Files

| File | Changes |
|------|---------|
| `akkado/include/akkado/ast.hpp` | ArrayLit, Pattern nodes |
| `akkado/include/akkado/mini_token.hpp` | Curly brace tokens |
| `akkado/include/akkado/pattern_event.hpp` | Extended properties |
| `akkado/include/akkado/music_theory.hpp` | Chord symbols |
| `akkado/src/mini_lexer.cpp` | Polymeter tokens |
| `akkado/src/mini_parser.cpp` | Polymeter, modifiers |
| `akkado/src/parser.cpp` | Method chaining, arrays |
| `akkado/src/pattern_eval.cpp` | Time modifiers |
| `akkado/src/codegen.cpp` | Array expansion, pattern methods |

## 7. Success Metrics

### Phase 1 Complete When:
- `[1, 2, 3]` parses and type-checks
- `map()` transforms arrays correctly
- Array elements expand to parallel DAG nodes

### Phase 3 Complete When:
- All common chord symbols parse correctly
- `chord("Am C F G") |> mtof(@) |> osc("saw", @)` plays correct progression

### Full System Complete When:
- Strudel code examples work with minimal modification
- Live-coding workflow supports expressive chord/pattern manipulation
- No performance regression in audio path

## 8. Resolved and Remaining Deferrals

### Resolved: Method Chaining via Dot-Call Desugaring

The original blocker for Phase 7 was method chaining. This has been resolved via **dot-call desugaring**: `pat("c4").slow(2)` is parsed and desugared to `slow(pat("c4"), 2)` at the AST level. This works for all pattern transforms, array operations, and audio functions uniformly.

All time/structure modifiers can be implemented as regular functions and automatically gain dot-call syntax. No special object system needed.

```akkado
// Both forms work identically today:
slow(pat("c4 e4"), 2)       // functional
pat("c4 e4").slow(2)        // dot-call (desugars to above)

// Chaining works:
pat("c4 e4").slow(2).rev()  // desugars to rev(slow(pat("c4 e4"), 2))
```

### Resolved: TypedValue System

The `TypedValue` struct, `ValueType` enum, and `visit()` returning typed values are fully implemented (see prd-compiler-type-system). This replaces the old ad-hoc maps and enables type-aware compilation. Pattern, Record, Array, Signal, Number, String, Function, and Void types are all tracked.

### Still Deferred

**Voicing system** (Phase 4):
- `.anchor(note)` / `anchor(chord, note)` — voicing center of gravity
- `.mode("below" | "above" | "duck" | "root")` — voicing mode
- Voice leading algorithms
- Custom voicing dictionaries via `addVoicings(name, map)`
- Can be implemented as regular functions with dot-call. Deferred due to algorithm complexity, not architecture.

**String-as-Pattern auto-parsing**:
- `"bd sd".slow(2)` — auto-parsing bare strings as patterns
- Current approach: explicit `pat("bd sd").slow(2)`
- Deferred: design question about disambiguation, not a technical blocker

**Scope modifiers** (lower priority):
- `inside(n, fn)` — apply fn inside n cycles
- `outside(n, fn)` — apply fn outside n cycles
- These take function arguments and apply them at different time scales. Requires more design thought around how closures interact with pattern transforms.

**Extended note properties** (partially done):
- `velocity(pat, value)` — implemented as pattern transform
- Mini-notation velocity suffix `c4:0.8` — implemented
- `.bend(pattern)`, `.aftertouch(pattern)`, `.dur(pattern)` — deferred, can be added as transforms following same pattern
