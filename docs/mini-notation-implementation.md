> **Status: REFERENCE** — Implementation spec. Fully implemented and current.

# Mini-Notation Implementation Specification

This document describes the implementation of mini-notation pattern evaluation and timing in the Akkado language compiler.

## Architecture Overview

The pattern evaluation pipeline transforms mini-notation strings into bytecode for the Cedar VM:

```
Pattern String → Lexer → Parser → AST → Evaluator → Events → Codegen → SEQPAT_*
```

### Pipeline Stages

1. **Lexer** (`mini_lexer.cpp`): Tokenizes the pattern string into tokens (pitches, samples, operators, brackets)
2. **Parser** (`mini_parser.cpp`): Builds an AST using Pratt parsing with operator precedence
3. **Evaluator** (`pattern_eval.cpp`): Expands the AST into a flat timeline of events
4. **Codegen** (`codegen_patterns.cpp`): Emits the SEQPAT opcode family (`SEQPAT_QUERY`/`STEP`/`GATE`/`TYPE`/`FIELD`/`PHASE`) sharing one `SequenceState`

### Cycle-Based Evaluation Model

Mini-notation patterns are evaluated on a per-cycle basis. **One cycle equals one beat**; BPM directly sets the cycle rate. Every top-level element occupies one full cycle (per-cycle alternation) — `"a b c d"` plays four cycles in sequence, one element per cycle. This is a **deliberate divergence from Strudel/Tidal**, which subdivides one cycle by element count. To pack multiple events into a single cycle, use the explicit subdivision form `[a b c d]`.

For simple patterns, a single cycle evaluation suffices. For patterns with alternating sequences (top-level or `<a b c>`), multi-cycle evaluation cycles through the children in turn.

## Grouping Constructs

| Construct | Name | Behavior |
|-----------|------|----------|
| `a b c` | Top-level alternation | Each item plays for one cycle; pattern spans N cycles (synonym of `<a b c>`) |
| `[a b c]` | Group/Subdivision | Items subdivide one cycle equally |
| `<a b c>` | Alternation | Synonym of top-level; each item gets 1 full cycle; pattern spans N cycles |
| `[a, b]` | Polyrhythm | Items play simultaneously |
| `{a b}%n` | Polymeter | Pattern forced to n steps |

### Top-level Alternation (default)

Spaces at the top level of a mini-notation string create per-cycle alternation:

```
"a b c d" = 4 elements spanning 4 cycles
  - Element 'a' plays in cycle 0
  - Element 'b' plays in cycle 1
  - Element 'c' plays in cycle 2
  - Element 'd' plays in cycle 3
  - With cycle_length=1 beat → one element per beat
```

### Subdivision (`[]`)

Square brackets pack their children into one cycle:

```
[a b c d] = 4 elements in 1 cycle
  - Each element gets 1/4 of the cycle
  - Events at normalized times 0, 0.25, 0.5, 0.75
  - With cycle_length=1 beat → events at beats 0, 0.25, 0.5, 0.75
```

### Alternation (`<>`)

Angle brackets behave identically to the top-level form — they're a documented synonym:

```
<a b c> = 3 elements spanning 3 cycles
  - Element 'a' plays in cycle 0
  - Element 'b' plays in cycle 1
  - Element 'c' plays in cycle 2
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
s"[bd sn]/2"  // CORRECT: slows pattern by 2
s"bd sn"/2    // WRONG: divides signal amplitude by 2
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
- `MiniPolyrhythm`, `MiniPolymeter`: returns max of children's cycle counts
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

**One cycle = one beat**, so normalized cycle time IS beat time — there is
no conversion factor. Top-level compilation is a hybrid dispatch
(`compile_top_level_pattern` in `codegen_patterns.cpp`):

- **All top-level weights == 1** (no `/n` or `@n` on any top-level
  element): the per-cycle-alternation path is used and `cycle_length_`
  stays at its **1.0 base** — elements alternate one-per-cycle at runtime,
  and `.slow(N)` / `.fast(N)` scale from that 1.0 base.
- **Any non-unit weight** (a top-level `/n` or `@n`): children subdivide
  time proportionally and `cycle_length_ = sum of top-level weights`.

### Examples

| Pattern | cycle_length | Behavior |
|---------|--------------|----------|
| `c4 e4 g4` | 1.0 (base) | Per-cycle alternation: c4 in cycle 0, e4 in cycle 1, g4 in cycle 2 |
| `<c4 e4 g4>` | 1.0 (base) | Identical (synonym of top-level) |
| `[c4 e4 g4 b4]/2` | 2.0 | Events at beats 0, 0.5, 1.0, 1.5 |
| `a [b c]/2 d` | 4.0 | Weights 1 + 2 + 1; children packed proportionally |

## SEQPAT Opcode Family

Pattern playback in the Cedar VM is handled by the SEQPAT opcode family,
all reading one shared `SequenceState`
(`cedar/include/cedar/opcodes/sequence.hpp`):

- `SEQPAT_QUERY` — advances the sequence against the beat clock once per
  block; the other opcodes read its result
- `SEQPAT_STEP` — current event value (freq / sample id)
- `SEQPAT_GATE` — gate signal (high for event duration, 1-sample drop at
  each onset)
- `SEQPAT_TYPE`, `SEQPAT_FIELD`, `SEQPAT_PHASE` — event kind, extended
  fields (`vel`, `dur`, `note`, `chance`, …), phase within the event

### Trigger Logic (conceptual)

1. Compute the beat position within the pattern:
   `beat_pos = fmod(beat, cycle_length)`
2. When `beat_pos` crosses the next event's start time, fire its onset
   (trigger pulse + gate drop-and-raise)
3. Output the event's value / velocity / extended fields
4. Wrap detection: when `beat_pos` resets below the previous position,
   playback restarts from the first event

See `SequenceState` for the full current structure — events carry
per-event extended fields and multi-voice chord values
(`OutputEvent.values[]`, prd-pattern-event-arrays), not just
time/value/velocity triples.

## Known Limitations

1. **Modifiers outside quotes**: Modifiers like `/2` or `*4` outside pattern strings are treated as arithmetic operators, not pattern modifiers.

2. **Memory for large patterns**: Alternation patterns create event lists proportional to the number of cycles. Deep nesting or large alternations may use significant memory.

3. **Pre-evaluated cycles**: All cycles are evaluated at compile time. There are no "infinite" patterns - the total cycle count is bounded by the AST structure.

4. **Random choice evaluation**: The `|` choice operator selects randomly at evaluation time. Multi-cycle evaluation will make different random choices per cycle.

## UI Feedback (Implemented)

Active-step highlighting shipped: pattern events carry source locations
(`akkado::serialize_sequences_json()` / `serialize_mini_ast_json()` export
them), and the web editor highlights the playing step via
`web/src/lib/stores/pattern-highlight.svelte.ts` and
`Panel/PatternDebugPanel.svelte` (AST visualization, sequence events,
source-location mapping). See CLAUDE.md "Pattern Debug Serialization".
