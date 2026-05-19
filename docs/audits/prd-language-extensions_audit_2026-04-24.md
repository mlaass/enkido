# Audit: Akkado Language Extensions (Phases 1, 3, 4)

**PRD:** `docs/prd-language-extensions.md`
**Audit base:** `a625894` (original add commit, via `--follow`)
**Audit head:** `8fc1e05c81ca10d27bde68e6519bc65deb4f9859`
**Audited:** 2026-04-24
**Mode:** Read-only bulk audit

## Summary
- Goals met: 8 of 8
- Critical findings: 1 (Unmet=0, Stubs=0, Coverage Gaps=1, Missing Tests=0)
- Recommended PRD Status: COMPLETE (unchanged)
- One-line verdict: All 8 language features are implemented and well tested; the only residual gap is that Feature 5 (pattern methods) is enforced via legacy E133 checks in `codegen_patterns.cpp` rather than via the builtin-level `param_types` annotation the PRD describes.

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| 1. Dot-call syntax (`x.f(args)` → `f(x, args)`) | Met | Parser: `akkado/src/parser.cpp:991,1004` (`parse_method_call` creates `MethodCall`). Analyzer: `akkado/src/analyzer.cpp:732-733,1021-1022,1098` (`desugar_method_call`). Codegen defensive error kept: `akkado/src/codegen.cpp:1511-1512` (E113). Tests: `test_parser.cpp:547-639,995,1008,1348`; `test_codegen.cpp:1732-1898` (8+ SECTIONs). |
| 2. `const fn` (commit `bda74f9`) | Met | Token: `token.hpp:31,91,107`. AST: `ast.hpp:57,110,234`. Parser: `parser.cpp:241-243,1373-1391,1430`. Symbol table: `symbol_table.hpp:33,37,133-135,208`. Evaluator: `akkado/src/const_eval.cpp` (600 lines) + `include/akkado/const_eval.hpp` (90 lines). Codegen integration: `codegen_functions.cpp:208-215,290-298,507-515`. Tests: `test_const_eval.cpp` (935 lines, ~50 TEST_CASEs). |
| 3. TypedValue refactor (3A `c733f66`, 3B+3C `57bf870`) | Met | Definition: `akkado/include/akkado/typed_value.hpp` (TypedValue struct, ValueType enum, PatternPayload, RecordPayload, ArrayPayload, helpers). Codegen: `codegen.hpp:243-248,273-321` (visit returns TypedValue; `node_types_` cache). Builtins annotations: `builtins.hpp:26-67,82` (`ParamValueType`, `type_compatible`, `param_types` array). Tests: `test_akkado.cpp:1892-2075` (`TypedValue type checking`, `TypedValue integration`). |
| 4. Match destructuring (`cfbb699`) | Met | AST: `ast.hpp:245-246,272-274` (destructure_fields on MatchArmData + PipeBindingData). Parser: `parser.cpp:1242-1356` (match arms), `parse_pipe_binding` for `as {...}`. Analyzer desugar: `analyzer.cpp:609-680` (pipe binding destructure rewrite). Codegen: `codegen.hpp:282-284` (`bind_destructure_fields`). Tests: `test_parser.cpp:730-817`; `test_codegen.cpp:2680-2722`; `test_akkado.cpp:416-462`. |
| 5. Pattern methods (via feature 1 + 3) | Met (with caveat) | Works via dot-call desugar; codegen-level pattern validation lives in `codegen_patterns.cpp:1965,2026,2081,2147,2222,2367,2499,2674,2842,2924` (E133 "first argument must be a pattern"). Tests: `test_codegen.cpp:1836` (`pattern method via dot-call: n"…".slow()`), `1844`; `test_akkado.cpp:1941-1952` (`slow() accepts MiniLiteral pattern`, `slow() rejects non-pattern argument`). Note: The PRD's stated "only new work" was adding `ParamValueType::Pattern` to transform builtins in `builtins.hpp`; inspecting `builtins.hpp:832-863` the `param_types` field is not populated for `slow`, `fast`, `rev`, `transpose`, `velocity`, `bank`, `variant`, `tune`. Behavior equivalent (E133 still raised) but done in codegen rather than as a builtin annotation. |
| 6. Expression defaults (`695269e`) | Met | Parser parses full expression as default node; symbol_table: `symbol_table.hpp:19-21` (`default_node` alongside `default_value`). Evaluator integration: `codegen_functions.cpp:208-215,290-298,507-515` (uses `ConstEvaluator.evaluate(default_node)` when present). Tests: `test_const_eval.cpp:143` (`Const fn: with default parameter`); `test_parser.cpp:1554`. |
| 7. Range patterns (`243d1e3`) | Met | Token: `token.hpp:79,143` (`DotDot`). Lexer: `lexer.cpp:215-218` (`..` vs `...`). AST: `ast.hpp:242-244` (`is_range`, `range_low`, `range_high`). Parser: `parser.cpp:1242-1295,1356` (match arm range parsing). Tests: `test_codegen.cpp:642-780` (7 SECTIONs covering compile-time, half-open semantics, negative ranges, guards, wildcard fallthrough, runtime opcode emission via CMP_GTE / CMP_LT / LOGIC_AND / SELECT). |
| 8. Record spreading (`695269e`) | Met | AST: `ast.hpp:266-267` (`RecordLitData.spread_source`). Parser: `parser.cpp:1491-1509` (parses `..expr` at start of record). Analyzer: `analyzer.cpp:236-280,858-865,992-999` (spread source propagated through clone/substitute). Tests: `test_parser.cpp:1460-1526` (5 SECTIONs); `test_akkado.cpp:2042-2057` (`record spread preserves field types`, `record field access on spread result`). |

## Findings

### Unmet Goals
None.

### Stubs
None detected.

### Coverage Gaps
- Feature 5 partial implementation path: The PRD Feature 5 says "Add type annotations to pattern transform builtins in `builtins.hpp` (param 0 = Pattern type)" — but `builtins.hpp:832-863` leaves `param_types` unset (defaults to `Any`) for `slow`/`fast`/`rev`/`transpose`/`velocity`/`bank`/`variant`/`tune`. Equivalent validation is performed in `codegen_patterns.cpp` via E133 errors, so the behavior still matches the PRD test cases, but the declarative mechanism chosen in the PRD was not adopted. Low-severity: no user-visible gap.

### Missing Tests
No features are without tests. Spot check:
- Dot-call: 8+ parser SECTIONs plus end-to-end codegen tests.
- const fn: 50+ TEST_CASEs in `test_const_eval.cpp` (935 lines), covering scalar/array, recursion limit, non-pure rejection, non-const arg rejection, math builtins, comparisons, match-in-const-fn, range patterns in const fn, division by zero.
- TypedValue: ~25 SECTIONs across `test_akkado.cpp:1892-2075` and `test_types.cpp` (309 lines, stereo type rules).
- Destructuring: parser + codegen + end-to-end SECTIONs.
- Expression defaults: covered in `test_const_eval.cpp:143` and combined scenarios.
- Range patterns: 7 SECTIONs in `test_codegen.cpp:642-780` including opcode emission tests.
- Record spreading: 5 parser SECTIONs + 2 integration SECTIONs.
- Pattern methods: covered via dot-call + pattern-arg E133 tests.

### Scope Drift
- Added a `const` variable form (`ConstDecl` AST node, `const x = expr`) which is not in the PRD (the PRD only describes `const fn`). Reasonable extension of the same compile-time interpreter, and tested in `test_const_eval.cpp:37-80`. Extra scope, not missing scope.

### Suggestions
- Optional: populate `ParamValueType::Pattern` on pattern-transform builtins in `builtins.hpp:832-863` to align with the PRD's stated implementation path for Feature 5. The E133 runtime check in `codegen_patterns.cpp` would become redundant but the declarative table aids future tools (LSP, docs, web signature help).
- Document the `ConstDecl` (`const x = expr`) extension either in this PRD or a follow-up note, since it co-exists with `const fn` in the grammar.

## PRD Status
- Current: `COMPLETE`
- Recommended: `COMPLETE`
- Reason: All 8 features have source evidence and targeted test coverage. The referenced commits (`bda74f9`, `c733f66`, `57bf870`, `cfbb699`, `0dc05a3`, `695269e`, `243d1e3`) all resolve. The only deviation from the PRD text (pattern-method type annotations on builtins) has an equivalent enforcement path already shipped and tested, so the feature promise is met end-to-end.
