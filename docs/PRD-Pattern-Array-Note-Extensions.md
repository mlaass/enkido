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
map([c4, e4, g4], x => osc("saw", x)) |> mix(%) |> out(%, %)

// Sugar (expands at compile time)
osc("saw", [c4, e4, g4]) |> out(%, %)  // Equivalent to above
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
// NOT 'c4:maj' (old syntax, to be deprecated)
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
  |> mtof(%) |> osc("saw", %)
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
drums.slow(2).rev() |> sampler(%) |> out(%, %)

"bd*4".lpf("<4000 2000 1000>")  // Methods on string-as-pattern
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
| Chord parsing | ✗ Wrong syntax | Uses `'c4:maj'` instead of `chord("Am")` |
| Chord expansion | ✗ Not implemented | Only emits root note |
| Array type | ✗ Not implemented | Blocking chord expansion |
| Polymeter `{x}` | ✗ Not implemented | No curly brace handling |
| Method chaining | ✗ Not implemented | `pat()` returns void |
| Voicing system | ✗ Not implemented | No anchor, mode, voice leading |

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
- `[c4, e4, g4] |> mtof(%) |> osc("saw", %)` produces 3 oscillators

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
drums.slow(2).rev() |> out(%, %)
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
- `akkado/src/lexer.cpp` — Deprecate old `'c4:maj'` syntax
- `akkado/src/codegen.cpp` — Chord expansion via arrays

**Verification**:
- `chord("Am")` → `[57, 60, 64]`
- `chord("C7")` → `[48, 52, 55, 58]`
- Listen: `chord("Am") |> mtof(%) |> osc("saw", %)` sounds like A minor

### Phase 4: Voicing System

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

### Phase 5: Extended Note Properties

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

### Phase 6: Polymeter

**Goal**: `{x y}` and `{x}%n` syntax.

**Scope**:
1. Curly brace tokens in mini-lexer
2. Polymeter AST node
3. LCM-based pattern alignment
4. `polymeterSteps(n, pattern)` functional equivalent

**Syntax**:
```akkado
"[bd sd] {hh hh hh}"   // 2 vs 3 polyrhythm
"{bd sd hh cp}%5"      // 5-step pattern
```

**Files to Modify**:
- `akkado/include/akkado/mini_token.hpp` — Curly brace tokens
- `akkado/src/mini_lexer.cpp` — Polymeter tokens
- `akkado/src/mini_parser.cpp` — Polymeter AST node
- `akkado/src/pattern_eval.cpp` — LCM alignment

### Phase 7: Time & Structure Modifiers

**Goal**: Full Strudel modifier set.

**Categories**:

| Category | Methods |
|----------|---------|
| Speed | `slow(n)`, `fast(n)`, `fastGap(n)`, `cpm(n)` |
| Offset | `early(n)`, `late(n)`, `ribbon(n)` |
| Reorder | `rev()`, `palindrome()`, `iter(n)`, `iterBack(n)`, `ply(n)` |
| Segment | `segment(n)`, `compress(s,e)`, `zoom(s,e)`, `linger(n)`, `swing(n)`, `swingBy(n,div)` |
| Scope | `inside(n,fn)`, `outside(n,fn)` |

**Files to Modify**:
- `akkado/src/pattern_eval.cpp` — All time modifiers
- `akkado/include/akkado/pattern_eval.hpp` — Method declarations

### Phase 8: Algorithmic Generators

**Goal**: Pattern generation functions.

| Function | Description |
|----------|-------------|
| `run(n)` | Integer sequence 0 to n-1 |
| `binary(n)` | Binary representation as pattern |
| `binaryN(n, bits)` | Fixed-width binary |
| `euclid(k, n)` | Euclidean rhythm |
| `euclidRot(k, n, r)` | Rotated euclidean |
| `euclidLegato(k, n)` | Legato euclidean |

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
- `chord("Am C F G") |> mtof(%) |> osc("saw", %)` plays correct progression

### Full System Complete When:
- Strudel code examples work with minimal modification
- Live-coding workflow supports expressive chord/pattern manipulation
- No performance regression in audio path

## 8. Deferred to Object System Revamp

The following features require method chaining and are deferred until the object system is revamped to ensure coherent design across all types (patterns, arrays, chords, audio signals).

### Method Chaining (Deferred)

**Time Manipulation**:
- `.slow(n)` - stretch pattern over n cycles
- `.fast(n)` - compress pattern to 1/n cycles
- `.fastGap(n)` - fast with gaps
- `.cpm(n)` - cycles per minute

**Time Offset**:
- `.early(n)` - shift events earlier
- `.late(n)` - shift events later
- `.ribbon(n)` - continuous offset

**Reordering**:
- `.rev()` - reverse event order
- `.palindrome()` - forward then backward
- `.iter(n)` - rotate pattern
- `.iterBack(n)` - rotate backward
- `.ply(n)` - repeat each event

**Segmentation**:
- `.segment(n)` - sample pattern at n points
- `.compress(start, end)` - time range
- `.zoom(start, end)` - focus on range
- `.linger(n)` - repeat first n
- `.swing(n)` - swing timing
- `.swingBy(amount, division)` - configurable swing

**Scope**:
- `.inside(n, fn)` - apply fn inside n cycles
- `.outside(n, fn)` - apply fn outside n cycles

### Note Properties (Deferred)

- `.velocity(pattern)` - per-note velocity
- `.bend(pattern)` - pitch bend
- `.aftertouch(pattern)` - channel pressure
- `.dur(pattern)` - note duration

### Voicing (Deferred)

- `.anchor(note)` - voicing center of gravity
- `.mode("below" | "above" | "duck" | "root")` - voicing mode
- Voice leading algorithms
- Custom voicing dictionaries via `addVoicings(name, map)`

### String-as-Pattern (Deferred)

```akkado
// Deferred: auto-parsing strings as patterns
"bd sd".slow(2)  // Would require method chaining

// Current: explicit pat() function
pat("bd sd")     // Works now
```

### Rationale

Method chaining should be designed holistically to work consistently across:
- Patterns: `pat("bd sd").slow(2)`
- Arrays: `[1, 2, 3].map(x => x * 2)`
- Chords: `chord("Am").anchor("c5")`
- Audio signals: potentially `osc("saw", 440).lpf(1000)`

Adding method chaining piecemeal would create inconsistent APIs. Better to defer until the object model is designed to support uniform method dispatch across all value types.
