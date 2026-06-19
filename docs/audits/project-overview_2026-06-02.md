# Project Overview — nkido

_Snapshot: 2026-06-02_

## Repository activity

| Metric                    | Value                  |
| ------------------------- | ---------------------- |
| Total commits             | 770                    |
| Days with commits         | 89                     |
| First commit              | 2026-01-08             |
| Most recent commit        | 2026-05-29             |
| Active span               | ~143 days              |
| Average commits / active day | ~8.7                |

## Tests

Counts are static — collected by grepping the Catch2 test sources rather
than executing the suites.

### Test cases (`TEST_CASE` / `SCENARIO` / `TEMPLATE_TEST_CASE` / `TEST_CASE_METHOD`)

| Component | Files | Test cases |
| --------- | ----: | ---------: |
| cedar     | 35    | 331        |
| akkado    | 51    | 1,142      |
| **Total** | **86** | **1,473** |

### Assertion macro invocations (`REQUIRE`, `CHECK`, and the `*_THROWS*` / `*_THAT` variants)

| Component | Assertion sites |
| --------- | --------------: |
| cedar     | 1,702           |
| akkado    | 8,821           |
| **Total** | **10,523**      |

> Note: these are source-level macro counts. Catch2's runtime assertion
> totals are much higher because most macros execute inside loops — for
> reference, a prior cedar run reported 341,416 runtime assertions
> across the 330 collected test cases.

## Components

- **cedar** — stack-based bytecode VM and DSP graph engine
  (95+ opcodes, 128-sample blocks @ 48 kHz)
- **akkado** — Strudel/Tidal-flavoured DSL that compiles to cedar
  bytecode via Pratt parser → AST → DAG → bytecode

## Contributors

- Moritz Laass
- Claude (paired sessions)
