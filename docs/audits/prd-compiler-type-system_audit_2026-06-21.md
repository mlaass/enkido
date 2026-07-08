# Audit: Akkado Compiler Type System

**PRD:** `docs/prd-compiler-type-system.md`
**Audit base:** `4977c77` (2026-04-24, PRD introduced under lowercase name) — feature commits land through `2f5d0c3`/`67bafa4`/`eea7f39` (2026-06-08)
**Audit head:** `379a356` (2026-06-21)
**Audited:** 2026-06-21

## Verdict

**Clean pass.** Every in-scope goal is Met with cited code and a passing test. No
Unmet Goals, Stubs, Coverage Gaps, or Missing Tests. The only deferred work
(builtin overload resolution) is intentionally spun out to
`prd-builtin-overload-resolution.md` and is out of scope for this PRD.

## Goal Verification

| Goal (phase) | Status | Evidence |
|---|---|---|
| Phase 1 — `TypedValue` struct + `ValueType` enum (11 members) | Met | `akkado/include/akkado/typed_value.hpp:15-35` (enum), `:154-171` (struct) |
| Phase 1 — `visit()` returns `TypedValue` | Met | `akkado/include/akkado/codegen.hpp:469` |
| Phase 1 — ad-hoc maps subsumed by `node_types_`; only `stereo_outputs_` remains | Met | `codegen.hpp:1331` (`node_types_`), `:1395` (`stereo_outputs_`); old map names survive only in the comment at `:1329-1330` |
| Phase 2 — `ParamValueType` enum | Met | `typed_value.hpp:44-54` |
| Phase 2 — `type_compatible()` | Met | `akkado/include/akkado/builtins.hpp:58-78` |
| Phase 2 — `param_types` / `args_are_signal` on `BuiltinInfo` | Met | `builtins.hpp` (referenced throughout); used at `codegen.cpp:1637-1639` |
| Phase 2 — E160 type-check loop in `visit_call()` | Met | `codegen.cpp:1689-1713` |
| Phase 3 — `Symbol` carries `TypedValue` (so `as` propagates types) | Met | `akkado/include/akkado/symbol_table.hpp:188` (`std::optional<TypedValue> typed_value`) |
| Phase 3 — closures propagate types | Met | `codegen_functions.cpp` (each_voice/closure param field access) |
| Phase 3 — runtime-event-source Pattern typing (`is_runtime_event_source`, `midi()`→`poly()`) | Met | `typed_value.hpp` PatternPayload `is_runtime_event_source`; `codegen.cpp:11241` test path |
| Phase 4 — coerce-friendly Signal fallback for unannotated `args_are_signal` slots | Met | `codegen.cpp:1679-1713` |
| Phase 4 — Pattern→Signal coercion (mono passes, sample passes, poly non-sample → E160) | Met | `codegen.cpp:1620-1649` |
| Phase 4 — uppercase PascalCase type-annotation names | Met | `param_value_type_name()` `builtins.hpp:32-44` |
| Phase 4 — `transport()` arg-0 Pattern check (E133) | Met | `codegen_patterns.cpp:3259/3267` |
| Phase 4 — match-arm `ValueType` agreement (E160) | Met | `codegen_functions.cpp:2451` |
| Phase 4 — `poly()`/`legato()` reject non-pattern input (E423) | Met | `codegen_functions.cpp:2862` |
| User-fn parameter type-annotation matrix (E184) | Met | `codegen_functions.cpp:581-694` |

## Findings

### Unmet Goals
None.

### Stubs
None. No `TODO`/`FIXME`/`XXX`/`HACK`/not-implemented markers in `typed_value.hpp`
or `builtins.hpp`. The E423 path the PRD status block flagged as the one gap this
audit predecessor found-and-fixed is present and tested (`codegen_functions.cpp:2852-2868`).

### Coverage Gaps
None. Each PRD claim traces to a specific failing-on-regression test:

| Claim | Test |
|---|---|
| `type_compatible()` per `ParamValueType` | `test_param_type_annotations.cpp:46,71,319` + `test_akkado.cpp:2186` |
| E160 implicit-signal-slot reject (Function/StateCell) | `test_param_type_annotations.cpp:556` |
| E160 explicit-annotation strictness (Array→E160) | `test_param_type_annotations.cpp:592` |
| Pattern→Signal coerce (mono pass / poly reject) | `test_param_type_annotations.cpp:180-208` |
| E423 poly/legato non-pattern input | `test_param_type_annotations.cpp:629-653` |
| E133 transport arg-0 Pattern | `test_param_type_annotations.cpp:602`, `test_transport.cpp:54` |
| E184 user-fn annotation matrix (all types) | `test_param_type_annotations.cpp:338-536` |
| match-arm ValueType agreement | `test_param_type_annotations.cpp:679,693` |
| `as` binding TypedValue propagation | `test_transport.cpp:54`, `test_chord.cpp:709-736` |
| closure param type propagation | `test_higher_order.cpp:182-191`, `test_param_type_annotations.cpp:264-295` |
| Pattern/Record field access + aliases | `test_analyzer.cpp:356`, `test_shape_index.cpp:116`, `test_state.cpp:361` |
| Array multi-buffer expansion | `test_codegen.cpp:6262-6302` |

### Missing Tests
None. The PRD's revised acceptance ("a type-mismatch test exists per
`ParamValueType` that is enforceable") is satisfied by the 37 TEST_CASEs in
`test_param_type_annotations.cpp`.

### Scope Drift
None worth flagging. The diff touches all five Key Files the PRD named
(`codegen.hpp`, `codegen.cpp`, `codegen_patterns.cpp`, `builtins.hpp`,
`codegen_builtins.cpp`) plus `typed_value.hpp`/`symbol_table.hpp`/
`codegen_functions.cpp` — the latter are the natural homes for the
TypedValue struct, Symbol field, and user-fn annotation matrix and are
consistent with the design.

### Convention Drift
None.

### Suggestions
- The two enforcement paths (generic `param_types`→E160 vs per-handler
  diagnostics) remain split by design; their unification is the explicit job of
  `prd-builtin-overload-resolution.md`. No action needed here.

## Decisions Recorded

- **Status flip approved by user:** `MOSTLY SHIPPED` → `COMPLETE`, with a note
  that overload resolution lives in its own PRD.
- No source fixes were required during this audit — the implementation was
  already complete and tested.

## Tests Added / Extended

None. The audit found full coverage; no fixes were made, so no regression tests
were needed.

## Test Run

| Test | Result |
|---|---|
| `akkado_tests "[type-annotation]"` | Pass (220 assertions, 41 cases) |
| `akkado_tests "[transport]"` | Pass (13 assertions, 4 cases) |
| `cmake --build build --target akkado_tests` | Build OK |

## PRD Status

- Before: `MOSTLY SHIPPED`
- After: `COMPLETE` (in-scope Phases 1–4 done + tested; overload resolution
  tracked separately in `prd-builtin-overload-resolution.md`)
