> **Status: NOT STARTED** — No curve notation parsing.

# Timeline Curve Notation PRD

## Executive Summary

This document specifies an ASCII-art curve notation for Akkado that compiles continuous automation curves into Cedar's existing `TIMELINE` opcode. The notation uses 5 value-level characters (`_`, `.`, `-`, `^`, `'`), ramp characters (`/`, `\`), and a smooth modifier (`~`) to express automation envelopes in a compact, visual syntax.

The curve notation integrates fully with the existing mini-notation system -- all grouping, subdivision, alternation, and modifier features work identically. It is accessed via `timeline("...")` or the `t"..."` string prefix.

---

## 1. Problem Statement

### 1.1 What Exists Today

Cedar's `TIMELINE` opcode (`cedar/include/cedar/opcodes/sequencing.hpp:248`) supports:

- Up to 64 breakpoints (`TimelineState::MAX_BREAKPOINTS`)
- Three interpolation modes: linear (0), exponential (1), hold (2)
- Looping with configurable loop length
- Per-sample interpolation via binary search through breakpoints

The `timeline` builtin is registered in `akkado/include/akkado/builtins.hpp:593` with 0 required parameters -- its state is populated by the compiler.

### 1.2 Current Limitation

There is no user-facing syntax to populate TIMELINE breakpoints. The opcode exists and works, but users have no way to define automation curves in Akkado source code. Users who want envelopes must use `adsr()` or construct manual per-sample logic, neither of which provides the flexibility of arbitrary breakpoint automation.

### 1.3 Goal

Provide a compact, visual ASCII-art syntax for defining automation curves that:

1. Compiles to TIMELINE breakpoints at compile time
2. Integrates with existing mini-notation features (grouping, alternation, modifiers)
3. Always outputs 0.0-1.0 (user scales with math)
4. Feels natural alongside existing `pat()` and `seq()` patterns

---

## 2. Syntax Reference

### 2.1 Value Levels

Five characters map to fixed values in the 0.0-1.0 range:

| Char | Value | Mnemonic |
|------|-------|----------|
| `_`  | 0.00  | floor/ground |
| `.`  | 0.25  | low dot |
| `-`  | 0.50  | middle dash |
| `^`  | 0.75  | high caret |
| `'`  | 1.00  | top/peak |

Each character occupies one equal-time slot within its containing group. Adjacent same-value characters extend the hold duration.

### 2.2 Ramp Characters

Two characters express linear interpolation between adjacent levels:

| Char | Visual | Behavior |
|------|--------|----------|
| `/`  | Up     | Linear ramp from preceding level to following level |
| `\`  | Down   | Linear ramp from preceding level to following level |

Both `/` and `\` produce identical interpolation behavior -- they ramp linearly from the value of the preceding atom to the value of the following atom. The visual distinction (`/` vs `\`) serves readability only.

**Multiple consecutive ramps** spread the transition over more time slots:

```akkado
t"__/'''"    // quick ramp: _ to ' over 1 slot
t"__///'''"  // slow ramp: _ to ' over 3 slots
```

### 2.3 Smooth Modifier `~`

The `~` prefix converts a hard level change into a smooth (linear) interpolation:

```akkado
t"__''"      // hard step from 0.0 to 1.0
t"__~''"     // smooth ramp from 0.0 to 1.0 over 1 slot
t"__~-~^''"  // smooth ramp through 0.5, 0.75, then hard step to 1.0
```

Without `~`, a level character holds its value for the entire slot (type = hold). With `~`, the slot linearly interpolates from the previous value to the new value (type = linear).

### 2.4 Integration Syntax

**Function call** -- extends existing `timeline()`:

```akkado
timeline("___/''''\\___")
```

**String prefix** -- syntactic sugar (like `p"..."` for `pat()`):

```akkado
t"___/''''\\___"
```

Both forms are equivalent. The output is always 0.0-1.0. Users scale with math:

```akkado
t"__/''\\__" * 1800 + 200  // ramp from 200 to 2000 Hz
t"_/'\\_" * 0.8 + 0.1      // ramp from 0.1 to 0.9
```

### 2.5 Mini-Notation Compatibility

All existing mini-notation modifiers and grouping work identically with curve atoms:

| Feature | Syntax | Effect |
|---------|--------|--------|
| Subdivision | `t"__[/''''] __"` | Ramp+hold subdivides into one slot |
| Alternation | `t"<__/' ''\\__>"` | Different curves per cycle |
| Repeat | `t"[__/']*4"` | Repeat sub-curve 4 times |
| Slow | `t"__''/2"` | Stretch over 2 cycles |
| Weight | `t"_@3 '"` | Underscore gets 3x time |
| Replicate | `t"'!4"` | Equivalent to `t"'''''"` |
| Chance | `t"_?0.5 '"` | 50% probability of this segment vs holding previous value |

### 2.6 `/` Ambiguity Resolution

`/` is both a ramp atom and the slow modifier prefix. The disambiguation rule:

- `/` followed by a **digit** = slow modifier (e.g., `/2`, `/4`)
- `/` followed by a **non-digit** or at end of input = ramp atom

The mini-notation parser already performs lookahead for modifier detection, so this integrates naturally.

---

## 3. Examples

### 3.1 Basic Curves

```akkado
// Constant value
t"''''"           // hold at 1.0 for entire cycle

// Step function
t"____''''"       // 0.0 for first half, 1.0 for second half

// Triangle LFO
t"_/'\\_"         // ramp up then ramp down

// Sawtooth
t"_///''"         // slow ramp up
t"'//_"           // slow ramp down

// Smooth fade in
t"__~.~-~^''"     // gradual ramp through 0.25, 0.5, 0.75

// Square wave
t"''''____"       // high-low
```

### 3.2 Musical Use Cases

```akkado
// Filter sweep on a pad
osc("saw", 220) |> lp(@, t"__/''\\__" * 3000 + 200) |> out(@, @)

// Tremolo
osc("sin", 440) * t"['^]*8" |> out(@, @)

// Sidechain-style ducking (4 pumps per cycle)
drums = pat("bd _ _ _")
synth = osc("saw", C4') * t"[_/'']*4" |> out(@, @)

// Panning automation
sig = osc("saw", 220)
sig |> out(sig * t"_/'\\__", sig * t"__/'\\_ ")

// Envelope on pluck
pat("c4 e4 g4") as e |>
    osc("sin", e.freq) * t"'/\\___" * e.vel |>
    out(@, @)
```

### 3.3 With Mini-Notation Features

```akkado
// Alternating curves per cycle
t"<_/'' ''\\_ >"       // up on odd cycles, down on even

// Subdivided ramp
t"__ [/''''] __"        // ramp+hold compressed into one beat

// Repeated envelope
t"[_/'\\_ ]*4"          // 4 attack-release envelopes per cycle

// Slow automation over 4 cycles
t"____/''''\\____/4"    // entire curve stretches over 4 cycles

// Weighted timing
t"_@3 /"                // hold at 0 for 3/4, ramp for 1/4
```

---

## 4. Technical Approach

### 4.1 New Token Types (Mini Lexer)

Add token types to the mini-notation lexer for curve-specific characters:

```cpp
// In MiniTokenType enum (mini_token.hpp)
CurveLevel,    // _, ., -, ^, '  (value level atom)
CurveRamp,     // /, \           (ramp atom)
CurveSmooth,   // ~              (smooth modifier prefix)
```

Each `CurveLevel` token carries its value:

```cpp
struct MiniCurveLevelData {
    float value;  // 0.0, 0.25, 0.5, 0.75, or 1.0
};
```

#### Lexer Conflict Resolution

Several curve characters overlap with existing mini-notation tokens:

| Char | Existing Use | Curve Use | Resolution |
|------|-------------|-----------|------------|
| `_`  | `Elongate` token | 0.0 level | Context: only curve in `timeline()` mode |
| `-`  | Not used in mini-notation | 0.5 level | No conflict |
| `/`  | `Slow` modifier | Ramp atom | Lookahead: `/` + digit = slow; `/` + non-digit = ramp |
| `~`  | `Rest` token | Smooth prefix | Context: only curve in `timeline()` mode |
| `'`  | Not used in mini-notation | 1.0 level | No conflict |
| `.`  | Not used in mini-notation | 0.25 level | No conflict |
| `^`  | Not used in mini-notation | 0.75 level | No conflict |
| `\`  | Not used in mini-notation | Ramp atom | No conflict |

**Context-based disambiguation**: The mini-lexer must know whether it is lexing a curve string or a standard pattern. This is determined by the calling context -- `timeline()` calls set a `curve_mode` flag on the lexer. In curve mode:

- `_` emits `CurveLevel(0.0)` instead of `Elongate`
- `~` emits `CurveSmooth` instead of `Rest`
- All other curve characters emit their respective tokens

### 4.2 New AST Node Kinds

Add curve-specific atom kinds to the mini-notation AST:

```cpp
// In MiniAtomKind enum (ast.hpp)
enum class MiniAtomKind : std::uint8_t {
    Pitch,
    Sample,
    Rest,
    Elongate,
    Chord,
    CurveLevel,    // Value level: _, ., -, ^, '
    CurveRamp,     // Ramp: /, \
};
```

Extend `MiniAtomData` to store curve information:

```cpp
struct MiniAtomData {
    MiniAtomKind kind;
    // ... existing fields ...
    float curve_value = 0.0f;    // For CurveLevel: 0.0, 0.25, 0.5, 0.75, 1.0
    bool curve_smooth = false;   // true if preceded by ~ modifier
};
```

### 4.3 Parser Changes

In the mini-parser, add handlers for curve tokens in `parse_atom()`:

```cpp
if (match(MiniTokenType::CurveSmooth)) {
    // ~ prefix: next atom must be a CurveLevel
    if (!match(MiniTokenType::CurveLevel)) {
        error("Expected level character after ~");
    }
    auto& data = std::get<MiniCurveLevelData>(previous().value);
    // Create CurveLevel atom with smooth=true
    return make_curve_atom(data.value, /*smooth=*/true);
}

if (match(MiniTokenType::CurveLevel)) {
    auto& data = std::get<MiniCurveLevelData>(previous().value);
    return make_curve_atom(data.value, /*smooth=*/false);
}

if (match(MiniTokenType::CurveRamp)) {
    return make_ramp_atom();
}
```

Update `is_atom_start()` to include curve token types.

### 4.4 Evaluation Changes

The pattern evaluator (`pattern_eval.cpp`) generates `PatternEvent` objects from atoms. For curve notation, add a new event type:

```cpp
// In PatternEventType enum (pattern_event.hpp)
enum class PatternEventType : std::uint8_t {
    Pitch,
    Sample,
    Rest,
    Chord,
    Elongate,
    CurveLevel,    // Holds a value for its duration
    CurveRamp,     // Interpolates between neighbors
};
```

Extend `PatternEvent` with curve fields:

```cpp
struct PatternEvent {
    // ... existing fields ...
    float curve_value = 0.0f;     // For CurveLevel
    bool curve_smooth = false;    // Linear interp to this value
    // curve_type derived: smooth/ramp -> linear (0), plain level -> hold (2)
};
```

In `eval_atom()`, handle the new atom kinds:

```cpp
case MiniAtomKind::CurveLevel:
    event.type = PatternEventType::CurveLevel;
    event.curve_value = atom_data.curve_value;
    event.curve_smooth = atom_data.curve_smooth;
    break;

case MiniAtomKind::CurveRamp:
    event.type = PatternEventType::CurveRamp;
    break;
```

### 4.5 `t"..."` String Prefix

Add the `t"..."` prefix following the same pattern as `p"..."`:

**Lexer** (`akkado/src/lexer.cpp`): In `lex_identifier()`, after the `p"..."` check:

```cpp
// Check for timeline string prefix: t"..." or t`...`
if (source_[start_] == 't' && current_ == start_ + 1) {
    char next = peek();
    if (next == '"' || next == '`') {
        return make_token(TokenType::Timeline);
    }
}
```

**Token** (`akkado/include/akkado/token.hpp`): Add `Timeline` to `TokenType` enum.

**Parser** (`akkado/src/parser.cpp`): Handle `TokenType::Timeline` in `parse_prefix()` and `parse_mini_literal()`, routing to timeline-specific parsing that sets curve mode on the mini-lexer.

---

## 5. Compilation Model

### 5.1 Curve Events to Breakpoints

After pattern evaluation produces a flat list of curve events, convert them to `TimelineState::Breakpoint` entries:

```
Input:  t"__/''\\__"
         ↓
Eval:   [CurveLevel(0.0, t=0.000, d=0.125),
         CurveLevel(0.0, t=0.125, d=0.125),
         CurveRamp(     t=0.250, d=0.125),
         CurveLevel(1.0, t=0.375, d=0.125),
         CurveLevel(1.0, t=0.500, d=0.125),
         CurveRamp(     t=0.625, d=0.125),
         CurveLevel(0.0, t=0.750, d=0.125),
         CurveLevel(0.0, t=0.875, d=0.125)]
         ↓
Breakpoints:
         [{time=0.000, value=0.0, curve=hold},
          {time=0.250, value=0.0, curve=linear},  // ramp start
          {time=0.375, value=1.0, curve=hold},     // ramp end
          {time=0.625, value=1.0, curve=linear},   // ramp start
          {time=0.750, value=0.0, curve=hold}]     // ramp end
```

### 5.2 Conversion Algorithm

```
function events_to_breakpoints(events):
    breakpoints = []
    for each event in events:
        if event is CurveLevel:
            if event.curve_smooth:
                // ~ prefix: interpolate FROM previous value TO this value
                bp = {time: event.time, value: event.curve_value, curve: linear}
            else:
                // Hard hold at this value
                bp = {time: event.time, value: event.curve_value, curve: hold}
            breakpoints.append(bp)

        else if event is CurveRamp:
            // Ramp: interpolate from previous level to next level
            // Value is resolved by looking at surrounding CurveLevel events
            prev_value = find_previous_level(events, event) or 0.0
            next_value = find_next_level(events, event) or 0.0
            bp = {time: event.time, value: next_value, curve: linear}
            breakpoints.append(bp)

    // Optimization: merge consecutive holds at the same value
    breakpoints = merge_same_value_holds(breakpoints)

    return breakpoints
```

### 5.3 Ramp Value Resolution

Ramp characters (`/`, `\`) don't carry their own value -- they bridge between adjacent levels:

1. **Find preceding level**: Walk backward through events to find the nearest `CurveLevel`. If none exists, default to 0.0.
2. **Find following level**: Walk forward through events to find the nearest `CurveLevel`. If none exists, default to 0.0 (wraps to start if looping).
3. **Multiple consecutive ramps**: Each ramp covers a proportional portion of the transition. For `_///'''`:
   - 3 ramps between 0.0 and 1.0
   - Ramp 1: value = 0.33, Ramp 2: value = 0.67, Ramp 3: value = 1.0

### 5.4 Breakpoint Optimization

Before emitting, optimize the breakpoint list:

1. **Merge consecutive same-value holds**: `CurveLevel(0.0), CurveLevel(0.0)` becomes a single breakpoint at the earlier time
2. **Clamp to MAX_BREAKPOINTS (64)**: If the curve generates more than 64 breakpoints after optimization, emit a compile warning and truncate

### 5.5 Code Generation

In `codegen_patterns.cpp`, when the pattern source is a `timeline()` call or `t"..."` literal:

1. Evaluate the curve string through the mini-notation pipeline (with curve_mode=true)
2. Convert events to breakpoints using the algorithm above
3. Emit a `TIMELINE` opcode with `state_id` pointing to pre-populated `TimelineState`
4. Set `loop = true` and `loop_length` to the cycle length in beats (typically 4.0 for 1 cycle)

```cpp
// In codegen_patterns.cpp
TimelineState state;
state.loop = true;
state.loop_length = cycle_length_beats;  // 4.0 for 1 cycle

for (const auto& bp : breakpoints) {
    if (state.num_points >= TimelineState::MAX_BREAKPOINTS) {
        warn("Timeline curve exceeds 64 breakpoints, truncating");
        break;
    }
    state.points[state.num_points++] = {
        .time = bp.time * cycle_length_beats,  // Convert 0-1 to beats
        .value = bp.value,
        .curve = bp.curve_type  // 0=linear, 2=hold
    };
}

// Emit via StateInitData
emit_timeline_opcode(state);
```

### 5.6 Curve Type Mapping

| Source | Breakpoint Curve Type | Cedar Value |
|--------|----------------------|-------------|
| Level char without `~` | Hold | 2 |
| Level char with `~` prefix | Linear | 0 |
| Ramp char (`/`, `\`) | Linear | 0 |

Exponential interpolation (curve type 1) is not exposed in the initial notation. It could be added later with a modifier (e.g., `e~` for exponential smooth).

---

## 6. Edge Cases & Ambiguity Resolution

### 6.1 Empty String

```akkado
t""  // Compile error: empty curve notation
```

### 6.2 Single Character

```akkado
t"'"   // Constant 1.0 for entire cycle → single breakpoint {time=0.0, value=1.0, curve=hold}
t"_"   // Constant 0.0 for entire cycle
```

### 6.3 Ramp at Start

```akkado
t"/'''"  // No preceding level → ramp from 0.0
         // Breakpoints: {time=0.0, value=1.0, curve=linear}, {time=0.25, value=1.0, curve=hold}
```

### 6.4 Ramp at End

```akkado
t"___/"  // No following level → ramp to 0.0 (wraps if looping)
         // Breakpoints: {time=0.0, value=0.0, curve=hold}, {time=0.75, value=0.0, curve=linear}
```

### 6.5 Adjacent Ramps

```akkado
t"_//_"   // 2 ramps between 0.0 and 0.0 → each ramp covers proportional portion
          // With no level change, ramps interpolate to 0.0 (no-op)

t"_//'"   // 2 ramps from 0.0 to 1.0
          // Ramp 1: {time=0.25, value=0.5, curve=linear}
          // Ramp 2: {time=0.50, value=1.0, curve=linear}
```

### 6.6 `~` on First Character

```akkado
t"~'"  // No preceding value → smooth ramp from 0.0 to 1.0
       // Same as t"/'"
```

### 6.7 `~` on Ramp

```akkado
t"~/"  // ~ only applies to level characters, not ramps
       // Compile error: "~ modifier must precede a level character (_, ., -, ^, ')"
```

### 6.8 `/` Ambiguity

```akkado
t"__/2"    // / followed by digit → slow modifier, not ramp
           // Stretches "__" over 2 cycles

t"__/'"    // / followed by ' → ramp atom
           // Ramp from 0.0 to 1.0

t"__/''/2" // First / is ramp (followed by '), /2 is slow
           // Curve "ramp to 1.0" stretched over 2 cycles
```

### 6.9 `_` Disambiguation

In standard mini-notation, `_` is `Elongate` (extend previous note). In curve mode, `_` is `CurveLevel(0.0)`. The mini-lexer's `curve_mode` flag ensures the correct interpretation.

### 6.10 `~` Disambiguation

In standard mini-notation, `~` is `Rest` (silence). In curve mode, `~` is `CurveSmooth` (smooth prefix). Same `curve_mode` flag resolves this.

### 6.11 Breakpoint Limit

`TimelineState::MAX_BREAKPOINTS = 64`. After optimization (merging consecutive same-value holds), most practical curves fit easily. A 64-step curve like `t"_._._._._._._._."` (alternating values) produces 64 breakpoints before merging. If exceeded, emit a compile-time warning and truncate.

---

## 7. Files to Modify

### Mini-Notation Pipeline

| File | Changes |
|------|---------|
| `akkado/include/akkado/mini_token.hpp` | Add `CurveLevel`, `CurveRamp`, `CurveSmooth` token types; add `MiniCurveLevelData` struct; extend `MiniTokenValue` variant |
| `akkado/src/mini_lexer.cpp` | Add curve_mode flag; lex `_`, `.`, `-`, `^`, `'` as `CurveLevel` in curve mode; lex `/` with lookahead for ramp vs slow; lex `\` as `CurveRamp`; lex `~` as `CurveSmooth` in curve mode |
| `akkado/include/akkado/mini_parser.hpp` | Declare curve atom parsing methods |
| `akkado/src/mini_parser.cpp` | Parse `CurveLevel`, `CurveRamp`, `CurveSmooth` tokens into AST atoms; update `is_atom_start()` |

### AST

| File | Changes |
|------|---------|
| `akkado/include/akkado/ast.hpp` | Add `CurveLevel`, `CurveRamp` to `MiniAtomKind`; add `curve_value`, `curve_smooth` fields to `MiniAtomData` |

### Pattern Evaluation

| File | Changes |
|------|---------|
| `akkado/include/akkado/pattern_event.hpp` | Add `CurveLevel`, `CurveRamp` to `PatternEventType`; add `curve_value`, `curve_smooth` fields to `PatternEvent` |
| `akkado/src/pattern_eval.cpp` | Handle `CurveLevel` and `CurveRamp` atoms in `eval_atom()` |

### Code Generation

| File | Changes |
|------|---------|
| `akkado/src/codegen_patterns.cpp` | Add `events_to_breakpoints()` conversion; emit TIMELINE opcode for curve patterns; populate `TimelineState` via `StateInitData` |

### Lexer & Parser (Main)

| File | Changes |
|------|---------|
| `akkado/include/akkado/token.hpp` | Add `Timeline` token type |
| `akkado/src/lexer.cpp` | Lex `t"..."` / `t\`...\`` as `Timeline` + `String` tokens |
| `akkado/src/parser.cpp` | Handle `TokenType::Timeline` in `parse_prefix()` / `parse_mini_literal()`; route to curve-mode mini-parsing |

### Builtins

| File | Changes |
|------|---------|
| `akkado/include/akkado/builtins.hpp` | Update `timeline` builtin entry if parameter count or signature changes |

### Existing Cedar (No Changes Required)

| File | Status |
|------|--------|
| `cedar/include/cedar/opcodes/dsp_state.hpp` | `TimelineState` already supports 64 breakpoints, linear/exponential/hold |
| `cedar/include/cedar/opcodes/sequencing.hpp` | `op_timeline` already implements interpolation and looping |

---

## 8. Testing Strategy

### 8.1 Lexer Tests (`test_lexer.cpp`)

```akkado
// t"..." prefix tokenization
t"__/''\\__"          // Timeline + String tokens
t`__/''\__`           // Timeline + backtick String

// Standalone 't' remains Identifier
t = 5                 // Identifier("t"), not Timeline
total = 10            // Identifier("total"), not Timeline prefix
```

### 8.2 Mini-Lexer Tests (`test_mini_lexer.cpp`)

```
// Curve mode tokens
"_"      → CurveLevel(0.0)
"."      → CurveLevel(0.25)
"-"      → CurveLevel(0.5)
"^"      → CurveLevel(0.75)
"'"      → CurveLevel(1.0)
"/"      → CurveRamp
"\"      → CurveRamp
"~"      → CurveSmooth

// / ambiguity
"_/'"    → CurveLevel, CurveRamp, CurveLevel
"_/2"    → CurveLevel, Slow(2)

// ~ prefix
"~'"     → CurveSmooth, CurveLevel(1.0)

// Non-curve mode (standard)
"_"      → Elongate
"~"      → Rest
```

### 8.3 Parser Tests (`test_mini_parser.cpp`)

```akkado
// Basic curve parsing
t"___"               // 3 CurveLevel atoms, all value=0.0
t"_/'"               // CurveLevel(0.0), CurveRamp, CurveLevel(1.0)
t"~'"                // CurveLevel(1.0, smooth=true)

// With grouping
t"[_/'] __"          // Subdivision: ramp compressed into 1 slot
t"<_' '_ >"          // Alternation between two curves

// With modifiers
t"_*4"               // Repeat _ four times
t"'!3"               // Replicate: '''
t"__''/2"            // Slow: stretch over 2 cycles
```

### 8.4 Evaluation Tests (`test_pattern_eval.cpp`)

```
// Event generation
t"____"       → 4 CurveLevel events, each at value=0.0, duration=0.25

t"__/'"       → [CurveLevel(0.0, t=0.0),
                 CurveLevel(0.0, t=0.25),
                 CurveRamp(t=0.5),
                 CurveLevel(1.0, t=0.75)]

// Timing
t"_@3 '"      → CurveLevel(0.0, t=0.0, d=0.75),
                 CurveLevel(1.0, t=0.75, d=0.25)
```

### 8.5 Codegen Tests (`test_codegen.cpp`)

```
// Breakpoint generation
t"___"        → 1 breakpoint: {time=0.0, value=0.0, curve=hold}

t"__''"       → 2 breakpoints: {time=0.0, value=0.0, curve=hold},
                                {time=0.5, value=1.0, curve=hold}

t"_/'"        → 3 breakpoints: {time=0.0, value=0.0, curve=hold},
                                {time=0.33, value=1.0, curve=linear},
                                {time=0.67, value=1.0, curve=hold}

// Verify TIMELINE opcode emitted with correct state_id
// Verify loop=true and loop_length matches cycle length
```

### 8.6 End-to-End Tests (`test_akkado.cpp`)

```akkado
// Constant output
t"''''" |> out(@, @)    // Should produce ~1.0 for all samples

// Zero output
t"____" |> out(@, @)    // Should produce ~0.0 for all samples

// Scaling
t"''''" * 440 |> out(@, @)  // Should produce ~440.0

// Integration with audio graph
osc("sin", 440) * t"''''____" |> out(@, @)  // Gated sine
```

### 8.7 Build & Run

```bash
cmake --build build && ./build/akkado/tests/akkado_tests "[timeline]"
```

---

## 9. Implementation Checklist

### Phase 1: Lexer & Token Infrastructure
- [ ] Add `Timeline` to `TokenType` enum in `token.hpp`
- [ ] Add `t"..."` / `t\`...\`` detection in `lexer.cpp` (following `p"..."` pattern)
- [ ] Add `token_type_name()` entry for `Timeline`
- [ ] Add `CurveLevel`, `CurveRamp`, `CurveSmooth` to `MiniTokenType` in `mini_token.hpp`
- [ ] Add `MiniCurveLevelData` struct to `mini_token.hpp`
- [ ] Extend `MiniTokenValue` variant with `MiniCurveLevelData`
- [ ] Add `curve_mode` flag to `MiniLexer`
- [ ] Implement curve-mode lexing for `_`, `.`, `-`, `^`, `'`, `/`, `\`, `~`
- [ ] Implement `/` lookahead disambiguation (digit = slow, non-digit = ramp)
- [ ] Lexer tests for all curve tokens and ambiguity cases

### Phase 2: AST & Parser
- [ ] Add `CurveLevel`, `CurveRamp` to `MiniAtomKind` enum in `ast.hpp`
- [ ] Add `curve_value`, `curve_smooth` fields to `MiniAtomData`
- [ ] Add curve atom parsing in `mini_parser.cpp` (`parse_atom()` and helpers)
- [ ] Update `is_atom_start()` for curve tokens
- [ ] Handle `TokenType::Timeline` in main parser (`parse_prefix()`, `parse_mini_literal()`)
- [ ] Pass `curve_mode=true` when parsing timeline string content
- [ ] Parser tests for curve atoms, grouping, and modifiers

### Phase 3: Pattern Evaluation
- [ ] Add `CurveLevel`, `CurveRamp` to `PatternEventType`
- [ ] Add `curve_value`, `curve_smooth` fields to `PatternEvent`
- [ ] Handle curve atoms in `eval_atom()`
- [ ] Verify subdivision, alternation, and modifiers work with curve events
- [ ] Evaluation tests for event generation and timing

### Phase 4: Code Generation
- [ ] Implement `events_to_breakpoints()` conversion function
- [ ] Implement ramp value resolution (preceding/following level lookup)
- [ ] Implement multiple-ramp proportional interpolation
- [ ] Implement breakpoint optimization (merge same-value holds)
- [ ] Emit TIMELINE opcode with populated `TimelineState`
- [ ] Set `loop=true` and `loop_length` based on cycle span
- [ ] Add compile warning for >64 breakpoints
- [ ] Codegen tests for breakpoint generation
- [ ] End-to-end tests for audio output

### Phase 5: Documentation
- [ ] Add curve notation to mini-notation reference (`web/static/docs/mini-notation-reference.md`)
- [ ] Add `t"..."` prefix to language reference
- [ ] Run `bun run build:docs` to rebuild docs index
