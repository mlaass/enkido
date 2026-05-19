> **Status: TRACKING** — Cleanup spec identifying technical debt to remove.

# Sequencing Functions Audit & Deprecation

## Summary

The Akkado pattern/sequencing API has accumulated redundant and non-functional functions. At the time of this audit, only the `pat` builtin was a true parser keyword that produced working patterns (since superseded by typed pattern literals `n"…"`/`v"…"`/`s"…"`/`c"…"`). Several other functions exist in builtins but are non-functional or legacy. Additionally, Cedar opcodes are completely unreachable from the compiler. This document records the audit findings and deprecation decisions to only remove opcodes and code directly related to the pattern/sequencing API.

## Function Status

| Function | Status | Reason |
|----------|--------|--------|
| `pat` (historical) | **Superseded** | Was the only real parser keyword; compiled to SEQPAT_QUERY/STEP/GATE/TYPE. Since replaced by typed pattern literals `n"…"`/`v"…"`/`s"…"`/`c"…"`. |
| `seq()` | **Remove** | Not a keyword. In builtins as PUSH_CONST placeholder. Only recognized by `is_pattern_expr()` for pattern transform validation. Non-functional standalone. |
| `note()` | **Remove** | Same as `seq()` — non-functional standalone. Identical to the `pat` builtin in intent. |
| `seq_step()` | **Remove** | Legacy opcode. State (events) only populatable via C++ API, never from Akkado. Superseded by SEQPAT system. |
| `timeline()` | **Keep** | Opcode works, Akkado syntax planned for a future PRD. Keep opcode and builtin entry. |
| `chord()` | **Keep** | Has its own codegen handler. Expands chord symbols to MIDI note arrays. Unique functionality. |
| `euclid()` | **Keep** | Euclidean rhythm generator. Unique, self-contained, state computed at runtime from inputs. |
| `trigger()` | **Keep** | Beat-division impulse. Unique, no redundancy. |
| `clock()` | **Keep** | Clock position query. Unique. |
| `lfo()` | **Keep** | Beat-synced LFO. Unique. |
| Pattern transforms (`slow`, `fast`, `rev`, `transpose`, `velocity`, `bank`, `n`) | **Keep** | Compile-time pattern transformations. Work with typed pattern literals. |

## Unreachable Opcodes (6)

These opcodes exist in the Cedar VM but are never emitted by the Akkado compiler.

### Array operations (6)

`ARRAY_CONCAT`, `ARRAY_FILL`, `ARRAY_LEN`, `ARRAY_REVERSE`, `ARRAY_SLICE`, `ARRAY_SUM`

Array ops were implemented in Cedar but the Akkado compiler never gained codegen for them. No builtin entries map to these opcodes.

## Legacy State Structs

- `SeqStepState` — only used by `SEQ_STEP`, remove with opcode
- `TimelineState` — used by `TIMELINE`, **keep** (timeline is being retained)

## Changes Required

### Part 1: Remove deprecated builtins

- `akkado/include/akkado/builtins.hpp` — remove `seq`, `note`, `seq_step` entries (keep `timeline`)
- `akkado/src/codegen_patterns.cpp` — remove `"seq"`, `"note"` from `is_pattern_expr()` and `compile_pattern_for_transform()` (keep `"timeline"`)

### Part 2: Remove unreachable opcodes
- `cedar/include/cedar/opcodes/sequencing.hpp` — remove SEQ_STEP implementation (keep TIMELINE)


### Part 3: Regenerate & rebuild

- `cd web && bun run build:opcodes` — regenerate opcode metadata
- `cd web && bun run build:docs` — regenerate docs index

### Part 4: Update documentation

- `web/static/docs/reference/builtins/sequencing.md` — remove seq_step section (keep timeline)
- `web/static/docs/reference/mini-notation/basics.md` — remove seq(), note() references
- `docs/mini-notation-reference.md` — remove seq(), note() references (keep timeline())

### Part 5: Update tests

- Remove or update tests referencing removed functions/opcodes
- Mark experiment files as deprecated or remove
