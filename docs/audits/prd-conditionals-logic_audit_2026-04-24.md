# Audit: Conditionals and Logic

**PRD:** `docs/prd-conditionals-logic.md`
**Audit base:** `4977c77 (2026-04-24)` — the commit that first added the PRD file (renamed from uppercase). PRD was created already marked `Status: DONE`, so feature work predates this base; verification is against HEAD.
**Audit head:** `8fc1e05c81ca10d27bde68e6519bc65deb4f9859`
**Audited:** 2026-04-24
**Mode:** Read-only bulk audit

> **Update 2026-04-26 — all findings resolved.** Re-verified at HEAD `e8c73d6`. Runtime value tests, epsilon test, signal-rate square-wave test, direct opcode unit tests, and Phase 5 documentation (operators page, conditionals reference page, F1 lookup entries for all ten primitives) are all in place. PRD Status remains `DONE`. See [Resolution](#resolution-2026-04-26) for the final state.

## Summary
- Goals met: 4/4 functional Goals met; 20/21 Phase checklist items complete.
- Critical findings: 4 (Unmet=0, Stubs=0, Coverage Gaps=1, Missing Tests=3)
- Recommended PRD Status: MOSTLY COMPLETE
- One-line verdict: Implementation is end-to-end wired (opcodes, VM dispatch, builtins, lexer, parser desugaring, codegen tests) but Phase 5 docs/F1 help is undone and several value-level test-plan items are only realized as codegen-presence checks, so `Status: DONE` is premature.

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| G1 C-style infix (`> < >= <= == != && \|\| !`) | Met | Lexer `akkado/src/lexer.cpp:240-286`; token enum `akkado/include/akkado/token.hpp:120-127,139`; parser precedences `akkado/src/parser.cpp:120-127`; infix dispatch `akkado/src/parser.cpp:498-506`; binary desugar `akkado/src/parser.cpp:913-947`; prefix `!` desugar `akkado/src/parser.cpp:590-604`. |
| G2 Function-call syntax (`gt lt gte lte eq neq band bor bnot select`) | Met | Builtins `akkado/include/akkado/builtins.hpp:540,546,550,554,558,562,566,572,576,580`. |
| G3 Signal-rate operation | Met | Per-sample loops `cedar/include/cedar/opcodes/logic.hpp:20,37,49,61,73,85,97,113`; VM dispatch `cedar/src/vm/vm.cpp:898-927`. |
| G4 Boolean/epsilon convention | Met | `LOGIC_EPSILON = 1e-6f` at `cedar/include/cedar/opcodes/logic.hpp:11`, used at lines 91 and 103. |

## Findings

### Unmet Goals
None.

### Stubs
None found in logic.hpp, lexer, or parser (no TODO/FIXME/XXX/HACK/push_warning/placeholder returns in relevant files).

### Coverage Gaps
- Epsilon behavior not exercised. `akkado/tests/test_codegen.cpp:881-894,977-990` only check that `CMP_EQ`/`CMP_NEQ` opcodes are emitted for `5 == 5` and `5 != 10`; nothing verifies near-equal floats are treated equal (e.g. `0.1 + 0.2 == 0.3`). A regression to a stricter `==` or changed epsilon would pass.

### Missing Tests
- Signal-rate square-wave verification named verbatim in the Test Plan (`check_signal("osc(\"sin\", 1) > 0", …)`) — absent.
- Runtime value assertion for `select` including the negative-falsy case (`select(-1, 100, 50) == 50`) — absent; only opcode presence tested (`test_codegen.cpp:924-942`).
- Runtime value assertions for comparisons/logic (`check_output("gt(10, 5)", 1.0f)`, `band(1, 0) == 0`, `bnot(1) == 0`, etc.) — absent. All current `[codegen][conditionals]` cases are codegen-presence checks only; inverted truth polarity would still pass.

### Missing documentation (Phase 5)
- `web/static/docs/reference/language/operators.md:5` keywords list does not include comparison/logic/conditional/select.
- No `web/static/docs/reference/builtins/conditionals.md` (or similar) exists.
- F1 lookup has no entries for `select`, `gt`, `lt`, `gte`, `lte`, `eq`, `neq`, `band`, `bor`, `bnot` in `web/src/lib/docs/lookup-index.ts` or `manifest.ts`.

### Scope Drift
None. Files changed since base (`akkado/tests/test_codegen.cpp`, `docs/MVP-INCOMPLETE-IMPLEMENTATIONS.md`, `docs/audits/MVP-INCOMPLETE-IMPLEMENTATIONS_audit_2026-04-24.md`) are unrelated to this PRD; all conditionals code predates the audit base.

### Suggestions
1. Resolve the `Status: DONE` vs 21 unchecked boxes contradiction by either checking them off or downgrading Status.
2. Convert codegen-presence tests into value-comparison tests (0.0/1.0 outputs, negative-falsy `select`, epsilon equality).
3. Add a signal-rate test for `sine(1) > 0` producing a square wave as the Test Plan demanded.
4. Add docs page/section and F1 lookup entries for the ten new builtins + five new operators, then re-run `bun run build:docs`.

## PRD Status
- Current: `DONE`
- Recommended: `MOSTLY COMPLETE`
- Reason: Core implementation shipped and tested for codegen, but Phase 5 documentation is entirely undone and the PRD's own Test Plan value assertions are not realized.

---

## Resolution (2026-04-26)

**Re-verified at HEAD `e8c73d6`.** All findings from the original audit have been resolved. PRD Status correctly remains `DONE`.

### Coverage Gaps — RESOLVED
- **Epsilon behavior** — `akkado/tests/test_codegen.cpp:5520-5542` (`Runtime: eq/neq with epsilon`) verifies `eq(0.1 + 0.2, 0.3) == 1.0`, `neq(0.1 + 0.2, 0.3) == 0.0`, and that differences > epsilon (1e-3 vs. 1e-6) compare not-equal. Mirrored at the opcode level by `cedar/tests/test_vm.cpp:1859-1895` (`VM CMP_EQ uses LOGIC_EPSILON`), which uses a `static_assert(LOGIC_EPSILON == 1e-6f)` to force test-author review on epsilon changes.

### Missing Tests — RESOLVED
- **Signal-rate square-wave (`sine(1) > 0`)** — `akkado/tests/test_codegen.cpp:5640-5689` (`Runtime: osc(sin, 1) > 0 produces a square wave`). Runs 400 blocks at 48 kHz (one full 1 Hz period plus margin), asserts every output sample is exactly 0.0 or 1.0, asserts duty cycle is 45–55%, and asserts at least two transitions occurred. (51200 binary-output assertions per run.)
- **Runtime `select` value tests including negative-falsy** — `akkado/tests/test_codegen.cpp:5574-5589` (`Runtime: select picks the right branch`). Covers truthy → `a`, zero → `b`, negative → `b` (the audit-flagged case).
- **Runtime value assertions for comparisons/logic** —
  - `Runtime: gt/lt/gte/lte produce 0.0 or 1.0` (`test_codegen.cpp:5492-5518`) — true/false/equal cases for all four.
  - `Runtime: band/bor/bnot truth tables` (`test_codegen.cpp:5544-5572`) — full truth tables plus negative-falsy assertions for all three.
  - `Runtime: infix syntax matches function-call syntax` (`test_codegen.cpp:5591-5610`) — proves desugaring fidelity.
  - `Runtime: operator precedence` (`test_codegen.cpp:5612-5638`) — `&&` vs `||`, comparison vs logic, arithmetic vs comparison, equality vs comparison.
  - Mirrored at the opcode level by `cedar/tests/test_vm.cpp:1761-1949` (4 `[opcodes][logic]` cases) so opcode regressions surface even if the parser still emits the right bytecode.

### Missing documentation (Phase 5) — RESOLVED
- `web/static/docs/reference/language/operators.md:5` — keywords list now includes `comparison, logic, conditional, gt, lt, gte, lte, eq, neq, band, bor, bnot, ternary, equal, equality, greater, less, and, or, not, boolean`. Page has full precedence table (entries for `>`, `<`, `>=`, `<=`, `==`, `!=`, `&&`, `||`, `!`), comparison/logic sections, conditional-selection section, and an "Operator Desugaring" table mapping every infix form to its builtin.
- `web/static/docs/reference/language/conditionals.md` — full reference page covering all ten primitives (`select`, `gt`, `lt`, `gte`, `lte`, `eq`, `neq`, `band`, `bor`, `bnot`) with parameter tables, truth-convention notes, epsilon explanation, and runnable `akk` examples.
- F1 lookup — `web/src/lib/docs/manifest.ts` has individual entries for `select` (line 528) and all nine other builtins (`gt`, `lt`, `gte`, `lte`, `eq`, `neq`, `band`, `bor`, `bnot` at lines 767–819), each pointing to the matching anchor in `conditionals.md`. The page itself is registered at line 14 / 141.

### Test results at HEAD `e8c73d6`
- `[conditionals][runtime]` — 7 test cases, **51871 assertions, all pass**.
- `[conditionals]` (codegen + runtime) — 14 test cases, **51939 assertions, all pass**.
- `[opcodes][logic]` (cedar) — 4 test cases, **416 assertions, all pass**.
- Full akkado suite — 488 test cases, **136807 assertions, all pass** (no regressions).

### Final PRD Status
- Recommended: `DONE` (now matches current).
- Reason: Implementation, runtime tests (codegen + VM), epsilon coverage, signal-rate Test-Plan item, language reference, conditionals reference page, and F1 lookup are all present and passing.
