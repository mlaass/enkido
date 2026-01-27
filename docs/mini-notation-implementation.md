# Mini-Notation Implementation Specification

This document describes the implementation of mini-notation pattern evaluation and timing in the Akkado language compiler.

## Architecture Overview

The pattern evaluation pipeline transforms mini-notation strings into bytecode for the Cedar VM:

```
Pattern String → Lexer → Parser → AST → Evaluator → Events → Codegen → SEQ_STEP
```

### Pipeline Stages

1. **Lexer** (`mini_lexer.cpp`): Tokenizes the pattern string into tokens (pitches, samples, operators, brackets)
2. **Parser** (`mini_parser.cpp`): Builds an AST using Pratt parsing with operator precedence
3. **Evaluator** (`pattern_eval.cpp`): Expands the AST into a flat timeline of events
4. **Codegen** (`codegen_patterns.cpp`): Emits SEQ_STEP bytecode with event data

### Cycle-Based Evaluation Model

Mini-notation patterns are evaluated on a per-cycle basis. A "cycle" represents one iteration of the pattern, typically 4 beats at default tempo.

For simple patterns, a single cycle evaluation suffices. For patterns with alternating sequences (`<a b c>`), multi-cycle evaluation expands all alternatives into a combined event stream.

## Grouping Constructs

| Construct | Name | Behavior |
|-----------|------|----------|
| `a b c` | Sequence | Items subdivide parent duration equally |
| `[a b c]` | Group/Subdivision | Same as sequence, explicit grouping for nesting |
| `<a b c>` | Slow Concatenation | Each item gets 1 full cycle; pattern spans N cycles |
| `[a, b]` | Polyrhythm | Items play simultaneously |
| `{a b}%n` | Polymeter | Pattern forced to n steps |

### Subdivision (`[]`)

Square brackets create subdivisions within the parent time span:

```
[a b c d] = 4 elements in 1 cycle
  - Each element gets 1/4 of the cycle
  - Events at normalized times 0, 0.25, 0.5, 0.75
  - With cycle_length=4 beats → events at beats 0, 1, 2, 3
```

### Slow Concatenation (`<>`)

Angle brackets create alternating patterns that span multiple cycles:

```
<a b c> = 3 elements spanning 3 cycles
  - Element 'a' plays in cycle 0
  - Element 'b' plays in cycle 1
  - Element 'c' plays in cycle 2
  - Events at normalized times 0, 1, 2
  - With cycle_length=12 beats → events at beats 0, 4, 8
```

### Polyrhythm (`[a, b]`)

Comma-separated items play simultaneously (same start time):

```
[c4, e4, g4] = 3 notes at same time (chord)
  - All events at time 0
  - Each event has full duration
```

### Polymeter (`{}`)

Curly braces with optional `%n` create patterns with fixed step count:

```
{bd sd}%5 = 5 steps cycling through 2 elements
  - bd at step 0, sd at step 1, bd at step 2, sd at step 3, bd at step 4
  - Events at times 0, 0.2, 0.4, 0.6, 0.8
```

## Modifiers

| Symbol | Name | Effect |
|--------|------|--------|
| `*n` | Speed/Fast | Repeat n times in current duration |
| `/n` | Slow | Stretch to n times current duration |
| `!n` | Repeat | Duplicate n times (subdivides time) |
| `@n` | Weight | Affects velocity/amplitude |
| `?n` | Chance | Probability (0-1) of playing |

### Speed Modifier (`*n`)

Speeds up playback by repeating content:

```
c4*2 = c4 plays twice in original duration
[a b]*2 = [a b] plays twice = [a b a b] in same time
```

### Slow Modifier (`/n`)

Stretches the pattern to span more time:

```
[a b c d]/2 = 4 elements spanning 2 cycles
  - Events at times 0, 0.5, 1.0, 1.5 (normalized)
  - cycle_span = 2.0
```

**Important**: The slow modifier stretches TIME within a single evaluation. It does not require multi-cycle evaluation like `<>`.

### Modifier Scope

Modifiers must be inside the pattern string, not outside:

```akkado
pat("[bd sn]/2")  // CORRECT: slows pattern by 2
pat("bd sn")/2    // WRONG: divides signal amplitude by 2
```

## Multi-Cycle Evaluation

### When Multi-Cycle is Needed

Multi-cycle evaluation is required for patterns containing:
- Alternating sequences (`<a b c>`)
- Nested alternations (`<[a b] [c d]>`)

Patterns with only:
- Groups (`[a b c]`)
- Slow modifiers (`[a b]/2`)
- Speed modifiers (`[a b]*2`)

...do NOT require multi-cycle evaluation.

### count_cycles() Function

The `count_cycles()` function analyzes the AST to determine how many cycles a pattern requires:

```cpp
uint32_t PatternEvaluator::count_cycles(NodeIndex node) const;
```

Logic:
- `MiniAtom`: returns 1
- `MiniGroup`: returns max of children's cycle counts
- `MiniSequence` (`<>`): returns N * max(child_cycles) where N = number of children
- `MiniModified`: returns child's cycle count (modifiers don't add cycles)
- `MiniPolyrhythm`: returns max of children's cycle counts
- `MiniChoice`: returns max of children's cycle counts

### Multi-Cycle Evaluation Process

For patterns requiring multiple cycles:

```cpp
PatternEventStream evaluate_pattern_multi_cycle(NodeIndex root, const AstArena& arena) {
    PatternEvaluator evaluator(arena);
    uint32_t num_cycles = evaluator.count_cycles(root);

    if (num_cycles <= 1) {
        return evaluator.evaluate(root, 0);  // Single cycle
    }

    PatternEventStream combined;
    for (uint32_t cycle = 0; cycle < num_cycles; cycle++) {
        PatternEventStream cycle_events = evaluator.evaluate(root, cycle);

        // Offset times by cycle number
        for (auto& event : cycle_events.events) {
            event.time += static_cast<float>(cycle);
        }

        combined.events.insert(combined.events.end(),
                               cycle_events.events.begin(),
                               cycle_events.events.end());
    }

    combined.cycle_span = static_cast<float>(num_cycles);
    combined.sort_by_time();
    return combined;
}
```

### Example: `<c4 e4 g4>`

1. `count_cycles(<c4 e4 g4>)` returns 3
2. Evaluate cycle 0 → `c4` at time 0
3. Evaluate cycle 1 → `e4` at time 0 → offset to time 1.0
4. Evaluate cycle 2 → `g4` at time 0 → offset to time 2.0
5. Combined: c4@0, e4@1, g4@2
6. `cycle_span = 3.0`

## Timing Conversion

### Normalized Time to Beat Time

Events are evaluated with normalized times (0.0 - cycle_span). Conversion to beats:

```
beat_time = normalized_time * 4.0
cycle_length = 4.0 * cycle_span
```

### Examples

| Pattern | cycle_span | cycle_length | Event Beat Times |
|---------|------------|--------------|------------------|
| `c4 e4 g4` | 1.0 | 4 beats | 0, 1.33, 2.67 |
| `<c4 e4 g4>` | 3.0 | 12 beats | 0, 4, 8 |
| `[c4 e4 g4 b4]/2` | 2.0 | 8 beats | 0, 2, 4, 6 |

## SEQ_STEP Opcode

The SEQ_STEP opcode handles pattern playback in the Cedar VM.

### State Structure

```cpp
struct SeqStepState {
    std::vector<float> times;      // Event times in beats
    std::vector<float> values;     // Event values (freq, sample_id)
    std::vector<float> velocities; // Event velocities
    float cycle_length;            // Total pattern length in beats
    uint32_t current_index;        // Current playback position
};
```

### Trigger Logic

The opcode fires triggers based on current beat position:

1. Calculate `beat_pos = std::fmod(beat, cycle_length)`
2. If `beat_pos >= times[current_index]`, fire trigger
3. Output current value and velocity
4. Advance `current_index`, wrap at end of pattern

### Wrap Detection

Pattern wrapping is detected when beat position resets:

```cpp
if (beat_pos < previous_beat_pos) {
    current_index = 0;  // Reset to beginning
}
```

## Known Limitations

1. **Modifiers outside quotes**: Modifiers like `/2` or `*4` outside pattern strings are treated as arithmetic operators, not pattern modifiers.

2. **Memory for large patterns**: Alternation patterns create event lists proportional to the number of cycles. Deep nesting or large alternations may use significant memory.

3. **Pre-evaluated cycles**: All cycles are evaluated at compile time. There are no "infinite" patterns - the total cycle count is bounded by the AST structure.

4. **Random choice evaluation**: The `|` choice operator selects randomly at evaluation time. Multi-cycle evaluation will make different random choices per cycle.

## UI Feedback Design (Future Phase)

### Goal

Highlight the currently playing step in the pattern editor.

### Challenges

- Pattern is compiled to flat event list; original source structure is lost
- Need to map beat position back to source location
- Multiple patterns may play simultaneously

### Proposed Solution

Add source mapping to events:

```cpp
struct PatternEvent {
    // ... existing fields
    SourceLocation source_loc;  // For UI highlighting
    uint32_t step_index;        // Index in original pattern
};
```

SEQ_STEP outputs current step index:

```cpp
struct SeqStepState {
    // ... existing fields
    uint32_t current_step;      // For UI feedback
};
```

### UI Integration

1. Codegen preserves `step_index` from pattern evaluation
2. SEQ_STEP updates `current_step` on each trigger
3. WASM bindings expose `get_current_step(pattern_id)` API
4. UI polls at display rate (60fps) and highlights corresponding source

### Visual Highlighting Options

- Background color pulse on current step
- Cursor/playhead indicator in pattern text
- Piano roll style with moving marker
- Oscilloscope showing triggered note
