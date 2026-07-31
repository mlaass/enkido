> **Status: IN PROGRESS — Phase 1-5 SHIPPED (2026-07-31), 4 phases remaining.**
>
> - **Phase 5 (lex_primitives extract) — SHIPPED 2026-07-31.** New
>   `akkado/include/akkado/lex_primitives.hpp` + `src/lex_primitives.cpp`
>   with `CursorBase` (nav + F8-correct line/column tracking),
>   classifiers, `scan_number`, `scan_velocity_suffix` (replaces the 3
>   in-file near-clones), and `pitch_to_midi`. Both lexers now inherit
>   `CursorBase`. **Divergences from the spec above:** (1) namespace is
>   `akkado::lex_primitives` — `akkado::lex` is already the convenience
>   function; (2) `pitch_to_midi(char, int, int)` instead of the
>   string_view-parsing sketch — both call sites scan char-by-char with
>   lexer-specific lookahead, that signature is the actually-shared
>   tail; (3) `scan_number` takes `NumericScanOpts` (sign / exponent /
>   greedy-dot / seen-dot) because the four numeric scanners have
>   deliberately different dot/exponent behavior — all preserved and
>   unit-locked; invalid exponents are now rejected by pure lookahead
>   (fixes a latent column-drift on v"…" rollback). LOC: lexer.cpp
>   607→490 (−117), mini_lexer.cpp 902→721 (−181); combined −298 meets
>   the ≥250 combined target, the per-file ≥150/≥200 stretch numbers
>   were not reachable without breaking observable lex_number error
>   behavior. [P5] unit tests cover cursor, classifiers, all
>   scan_number modes, velocity suffix, pitch_to_midi. Full suites
>   green; snapshot byte-identical; wasm green. Commit hashes
>   backfilled at Phase 9.
>
> - **Phase 4 (Pratt OpInfo table) — SHIPPED 2026-07-31.** One
>   `OPERATORS[]` table + `find_op()` in `parser.hpp` backs
>   `get_precedence` / `is_infix_operator` / `parse_infix` /
>   `parse_binary` (associativity from the table; the `^`-specific
>   branch is gone). Deleted `BinOp`, `binop_function_name`,
>   `Node::BinaryOpData` (variant arm + `as_binop`),
>   `NodeType::BinaryOp` (+ name case), the post-parser-dead codegen
>   arm (codegen.cpp ~2503), and the ast_hash branch; migrated
>   test_ast_arena.cpp off the family. [P4] data-driven test iterates
>   `OPERATORS[]` (desugar name + associativity nesting per row);
>   precedence + `2^3^2` regressions locked. Commit hashes backfilled
>   at Phase 9.
>
> - **Phase 3 (frozen builtin scope) — SHIPPED 2026-07-31.** Vendored
>   frozen 1.2.0 (`third_party/frozen/` + `THIRD_PARTY.md`);
>   `BUILTIN_FUNCTIONS` / `BUILTIN_ALIASES` / `BUILTIN_VARIABLES` are
>   `constexpr frozen::unordered_map`. Process-shared `builtin_scope()`
>   built once; `SymbolTable` construction does zero builtin inserts.
>   **Design divergence from the spec above:** SymbolIds are per-compile
>   interner-sequential, so the shared scope cannot be chained as a
>   `scopes_[0]` map keyed on SymbolId — it is keyed by name and consulted
>   as a lookup *fallback* (functions, then aliases; host extensions
>   consulted live so post-first-compile registrations still resolve),
>   with the caller's per-compile id patched onto the returned copy.
>   The insert-into-scope-0 assertion is subsumed by the type system
>   (the shared scope is `const`; `define()` only ever writes per-compile
>   scopes). E150 top-level-reassignment and closure-shadowing semantics
>   preserved and locked by [P3] tests (incl. two-table sharing + a
>   first-use thread race). Commit hashes backfilled at Phase 9.
>
> - **Phase 2 (CompileOptions + grouped CompileResult + debug-JSON gate)
>   — SHIPPED 2026-07-31.** `compile(source, CompileOptions)` replaces
>   the 6-positional-arg form; `CompileResult` is grouped into
>   `program` / `requests` / `artifacts` (+ top-level `success` /
>   `diagnostics`); `StateInitData::ast_json` is gated behind
>   `emit_debug_json` (default false; wasm passes true). All in-tree
>   callers migrated (tools, wasm, ~40 test files). [P2] tests cover
>   the gate + grouped population. Commit hashes backfilled at Phase 9.
>
> - **Phase 1 (dead-code + catalogue) — SHIPPED 2026-07-31.** Deleted
>   `TokenType::MiniString`, the `MiniLexer`/`lex_mini` bool overloads
>   (+ `mode_from_bools`), and the entirely-dead `codegen/literals.hpp`
>   (`make_push_const`/`make_mtof`). Authored
>   `docs/parallelism-blocker-catalogue.md` (PB-001..PB-004 + PB-F5).
>   `BinOp`/`BinaryOpData` deferred to Phase 4 per the conservative rule
>   (dead consumers survive in `codegen.cpp:2502`, `ast_hash.cpp`,
>   `test_ast_arena.cpp`). Commit hashes backfilled at Phase 9.
>
> Filed 2026-05-26 as the
> hardening + parallelism-prep follow-up to
> [`docs/audits/parser-codegen-interop_audit_2026-05-25.md`](audits/parser-codegen-interop_audit_2026-05-25.md).
> Sibling PRD: [`docs/prd-parser-codegen-correctness.md`](prd-parser-codegen-correctness.md)
> (in flight) takes the audit's six critical correctness findings (F1,
> F2, F3, F7, F8, F12, F14). Sibling PRD:
> [`docs/prd-codegen-sprawl-cleanup.md`](prd-codegen-sprawl-cleanup.md)
> (drafted alongside this one) takes the audit's codegen-monolith
> findings (F4, F9, F10, viz/param families). **This PRD** takes the
> remaining audit findings — front-end hardening + the explicit
> parallelism-prep audit — and lands every cleanup short of introducing
> per-import parallelism itself (audit F5 / PRD-3) so that the future
> parallelism PRD can land with a small, well-isolated diff.
>
> Phases land independently. Phase 1 (dead-code sweep + parallelism
> blocker catalogue) is the gating prerequisite; Phases 2–8 are
> independent of each other once Phase 1 ships; Phase 9 is the
> finalization gate (re-run the parallelism audit, assert zero
> remaining shared mutable state below `CompileContext`).

# PRD: Parser/Codegen Hardening + Parallelism Prep

## Executive Summary

The 2026-05-25 parser/codegen interop audit identified 15 findings
across the akkado front-end (75 KLOC). Six critical correctness
findings are handled by `prd-parser-codegen-correctness.md`. The
audit's codegen-monolith findings (F4 visit() Call branch, F9
pattern-transform clones, F10 StateInitData duplication, viz/param
sprawl) are handled by `prd-codegen-sprawl-cleanup.md`. **This PRD
handles the remaining hardening work and explicitly prepares the
front-end for the future per-import parallelism PRD (audit F5 / PRD-3)
without introducing parallelism itself.**

Concretely, this PRD:

1. Eliminates **every remaining shared mutable state hazard** below
   `CompileContext`. After this PRD lands, the compiler is safe to run
   concurrently across imports — only the orchestration in
   `akkado.cpp` (audit F5) blocks the parallel front-end, and that
   single change becomes a self-contained future PRD.
2. Replaces the 478-LOC `shape_index` re-lex+re-parse pipeline with
   a thin formatter over the analyzer's `output_arena_` + `SymbolTable`
   (audit F13 / PRD-2). Editor-cursor latency drops from "full
   front-end run" to "format what's already cached".
3. Unifies the 4 Pratt operator switches into a single `OpInfo[]`
   table, deletes the post-parser-dead `BinOp`/`BinaryOpData` path
   (audit F6 / PRD-10).
4. Extracts shared lexer primitives (`CursorBase`, `scan_number`,
   `parse_pitch_to_midi`, etc.) into `lex_primitives.hpp`, removing
   ~250–300 LOC of mechanical duplication between `lexer.cpp` and
   `mini_lexer.cpp` (audit F11 / PRD-8).
5. Rolls the 6-argument `compile()` into `CompileOptions`, reorganises
   the bloated `CompileResult` into grouped sub-structs, and gates
   `serialize_mini_ast_json` behind `emit_debug_json` (default `false`
   for headless CLI) (audit §3.5 / PRD-11).
6. Promotes the builtin scope to a single process-shared
   `frozen::map` consulted as the `SymbolTable`'s root parent scope,
   eliminating the 600+ per-compile builtin inserts and providing
   free thread-safety (audit §3.5 / PRD-12). **Adds the `frozen`
   header-only library as the project's first third-party dep.**
7. Collapses the scattered "is this a const / pattern / literal?"
   recognizers into a single `expr_kinds.hpp` module; parameterises
   the three near-mirrors of `reorder_named_arguments` (430 LOC → ~150
   LOC) into one helper + two predicates (audit F15 / PRD-13).
8. Moves AST ghost fields (`MatchArmData::guard_node`,
   `ArgumentData::spread_source`, `RecordLitData::spread_source`,
   `DestructureField::default_node`, `HoleData::field_name`,
   `ClosureParamData::annotated_type`) into a uniform
   `Node::extra_children[]` slot so generic traversals work without
   per-field special-casing (audit §3.3 / PRD-14).
9. Dead-code sweep: `TokenType::MiniString` (declared, never
   produced), `MiniLexer` `bool`-overload ctor, `codegen/literals.hpp`
   dead helpers, post-parser-dead `BinOp`/`BinaryOpData` path
   (audit §3.1, §3.2 / PRD-15).

**What this PRD does NOT do:**

- Introduce per-import front-end parallelism (audit F5 / PRD-3 — that
  is the next PRD's job; this PRD prepares everything possible to make
  that PRD a single-screen change).
- Refactor the codegen monoliths (audit F4/F9/F10/viz/param families —
  those land via `prd-codegen-sprawl-cleanup.md`).
- Touch any of the six critical correctness findings (handled by
  `prd-parser-codegen-correctness.md`).

**Key Design Decisions** (locked — see §11 for sourcing):

- **Two-PRD split.** This PRD covers front-end hardening + the
  explicit parallelism-prep audit; `prd-codegen-sprawl-cleanup.md`
  covers the codegen-monolith refactors. Each ships independently;
  reviewers and contributors can split work across the two without
  coordination.
- **"Audit + fix everything in scope" for parallelism blockers.**
  Phase 1 enumerates every remaining shared mutable state hazard,
  every non-const global, every static mutex / singleton; Phases 2–8
  fix all of them as part of their natural scope. Phase 9 re-runs the
  audit and asserts the catalogue is empty before this PRD closes.
- **Full `CompileOptions` rollup + `CompileResult` re-grouping.** The
  6-arg `compile()` becomes `compile(source, opts)`; the
  `CompileResult` struct's 20+ fields are re-grouped into named
  sub-structs (`HostResources`, `RuntimeRequests`, `DebugArtifacts`).
  Existing callers (15+ across CLI, web/wasm, tests) migrate
  one-time; afterwards `compile(src, {.sample_registry=...})` is the
  ergonomic shape.
- **`frozen` header-only library adopted as the project's first
  third-party dep.** Owns `BUILTIN_FUNCTIONS` / `BUILTIN_ALIASES` /
  `BUILTIN_VARIABLES` as compile-time `frozen::unordered_map`
  instances; SymbolTable's root scope chains to a process-shared
  builtin scope built from those maps once at process init.
  Vendored under a new `third_party/` directory.
- **`shape_index` full collapse to ~80 LOC.** Drop the entire
  re-lex+re-parse pipeline; new implementation is a pure formatter
  over `CompileResult::ast` + `CompileResult::symbols` (both already
  populated by `compile()` per the records-system-unification PRD).
- **Ghost fields → `Node::extra_children[]`.** A small inline vector
  (`absl::InlinedVector`-style, hand-rolled — no new deps) on `Node`
  for "auxiliary children outside the linked list". Each ghost field
  becomes an indexed entry with a tiny enum tag. Generic AST
  traversal walks `first_child`/`next_sibling` THEN `extra_children`
  with no per-field knowledge.
- **No backwards-compatibility shims.** `compile()` migrates with a
  one-PR breaking change to its 15+ in-tree callers; there are no
  external API consumers. Field-renames in `CompileResult` are
  similarly direct — the records-system-unification PRD already
  established the precedent.

---

## 1. Problem Statement / Current State

### 1.1 Shared mutable state hazards (the parallelism-blocker catalogue)

After `prd-parser-codegen-correctness.md` Phase 4 ships (eliminates
`voicing_registry` + `registry_mutex` per audit F14), the remaining
known shared mutable state below `compile()` is:

| Hazard | Site | Severity for per-import parallelism |
|---|---|---|
| `SymbolTable` ctor inserts 600+ builtins on every construction | `symbol_table.cpp:236-264` | **High** — every per-import analyzer pays this cost and the inserts target the same hash table. Per-import threading needs per-thread `SymbolTable` instances, multiplying the cost. Fixed by Phase 3 (frozen builtin scope shared, scopes chain to it). |
| `node_types_` is dual-role (cache AND inter-handler channel) | `codegen.hpp:1217` | **Medium** — only matters once per-statement codegen parallelism is attempted. Documented but not split here; per-import parallelism (PRD-3 scope) doesn't trip this. |
| `apply_lambda` mutates `node_types_` per iteration | `codegen_arrays.cpp:76` (audit §3.4) | **Medium** — same scope as above; per-import is unaffected. |
| Statics in `codegen_patterns.cpp:33, 2052, 2071` | grep flags 3 sites | **None** — all const data / pure functions, no mutable state. Documented in catalogue for completeness. |
| Any `static std::mutex` / `static std::unordered_map` reachable from codegen | grep verified zero after correctness Phase 4 | **None expected** — Phase 1 verifies. |

Phase 1's deliverable is the **complete** catalogue in
`docs/parallelism-blocker-catalogue.md` (checked-in). Each remaining
entry is then either fixed in a later phase of this PRD or marked
"Out of scope — PRD-3" with a precise reason.

### 1.2 `shape_index` re-implements the front-end (audit F13)

Site: `shape_index.cpp:424-435` calls `lex(source, "<shape-index>")`
then `parse(tokens, …)` — a complete re-lex + re-parse — driven from
`web/wasm/nkido_wasm.cpp:1380` on every editor cursor move. 478 LOC of
duplicate machinery to compute the same Record/Pattern/Array shape
info that `collect_definitions` already produces and stores in the
`SymbolTable` (and `CompileResult` already exposes via
`CompileResult::ast` + `CompileResult::symbols`).

Why it bites: quadratic web-IDE latency. Every keystroke triggers
(a) the debounced compile cycle, and (b) a shape-index re-lex+re-parse,
both touching the same source.

### 1.3 Pratt fragmentation + dead `BinOp` path (audit F6, F10 P10 portion)

Four parser-side switches enumerate the same operator set:

| File:line | What it switches on |
|---|---|
| `parser.cpp:184-203` | `Parser::get_precedence` — precedence lookup |
| `parser.cpp:205-226` | `Parser::is_infix_operator` — table membership |
| `parser.cpp:702-726` | `Parser::parse_infix` — dispatch |
| `parser.cpp:1431-1451` | `Parser::parse_binary` — also duplicates `binop_function_name` at `ast.hpp:160-169` |

`BinOp` enum (`ast.hpp:148-155`) and `BinaryOpData` variant arm are
post-parser-dead: the parser desugars to `Call(IdentifierData{"add"})`
directly at `parser.cpp:1468-1471`. The BinaryOp node-kind switch arm
and `binop_function_name` constexpr fn are referenced nowhere
downstream.

(F7 / `^` right-assoc was withdrawn during correctness PRD Phase 0 —
verified `2^3^2 == 512`. This PRD does not re-touch the topic; the
regression test ships in the correctness PRD's Phase 2.)

### 1.4 Lexer near-clone (audit F11, F8 already in correctness PRD)

`lexer.cpp` (607 LOC) vs `mini_lexer.cpp` (904 LOC). Concrete
duplication (per audit §F11):

| Concern | `lexer.cpp` | `mini_lexer.cpp` |
|---|---|---|
| `is_at_end` / `peek` / `peek_next` / `advance` / `match` | 59-84 | 54-84 |
| `is_digit` / `is_alpha` / `is_whitespace` | 86-102 | 86-106 |
| `make_token` overloads (3 variants) | 104-137 | 108-138 |
| Numeric scan (`lex_number`) | 317-383 | 426-518 (3 near-clones) |
| Velocity-suffix scanner | n/a | 488-504, 671-686, 759-775 (3× same file) |
| Pitch-MIDI semitone table | 540 (identical comment) | 228 (identical comment) |

F8 (multi-line line tracking) is fixed by the correctness PRD's Phase
2; the `lex_primitives.hpp` extract is the natural follow-up that
collapses the rest of the duplication. ~250–300 LOC removable.

### 1.5 `compile()` API + `CompileResult` bloat (audit §3.5)

Current signature:

```cpp
CompileResult compile(std::string_view source,
                     std::string_view filename = "<input>",
                     SampleRegistry* sample_registry = nullptr,
                     const FileResolver* resolver = nullptr,
                     bool lint_strict = false,
                     bool bypass_master = false);
```

6 positional args mixing config and dependencies. `compile_file` has
a 4-arg parallel signature without `bypass_master`. Test callers and
host integrations alike pass `nullptr, nullptr, false, false` cargo
cult — verified across 50+ in-tree call sites.

`CompileResult` has 20+ fields spanning bytecode + diagnostics + host
resource requests + UI declarations + debug artifacts:

```cpp
struct CompileResult {
    bool success;
    std::vector<std::uint8_t> bytecode;
    std::uint32_t main_instruction_count;
    std::vector<cedar::BlockEntry> block_table;
    std::vector<SourceLocation> source_locations;
    std::vector<Diagnostic> diagnostics;
    std::vector<StateInitData> state_inits;
    std::vector<std::string> required_samples;
    std::vector<RequiredSample> required_samples_extended;
    std::vector<ScalarSampleMapping> scalar_sample_mappings;
    std::vector<RequiredSoundFont> required_soundfonts;
    std::vector<RequiredMidiSource> required_midi_sources;
    std::vector<RequiredMidiCcRoute> required_midi_cc_routes;
    std::vector<std::string> required_input_sources;
    std::vector<ParamDecl> param_decls;
    std::vector<VisualizationDecl> viz_decls;
    std::vector<BuiltinVarOverride> builtin_var_overrides;
    std::vector<RequiredWavetable> required_wavetables;
    std::vector<UriRequest> required_uris;
    std::optional<SymbolTable> symbols;
    std::shared_ptr<Ast> ast;
};
```

Three natural groupings: bytecode/instruction stream
(`bytecode`/`main_instruction_count`/`block_table`/`source_locations`/`state_inits`),
host resource requests (the 8 `required_*` vectors +
`scalar_sample_mappings`), and post-compile-tooling artifacts
(`param_decls`/`viz_decls`/`builtin_var_overrides`/`symbols`/`ast`).
The current flat shape forces every consumer to know the field set;
the grouped shape lets each consumer take only what it cares about.

Plus: `serialize_mini_ast_json` runs unconditionally at
`codegen_patterns.cpp:1379` for every pattern in every compile — even
`nkido render` which never reads it. A debug-JSON gate cuts ~5-15%
off headless compile time on pattern-heavy programs.

### 1.6 Builtin re-registration (audit §3.5 / PRD-12)

`SymbolTable::SymbolTable()` runs `register_builtins()` from
`symbol_table.cpp:5-9`, which `register_builtins()` inserts 600+
builtin function symbols from `BUILTIN_FUNCTIONS` + `BUILTIN_ALIASES`
+ `BUILTIN_VARIABLES`. Two costs:

- **Per-compile time.** ~0.2–0.5 ms inserting symbols (not the
  dominant compile cost, but free to skip).
- **Per-thread cost in any future per-import parallel front-end.**
  Each per-import analyzer would build its own SymbolTable, paying
  the insert cost N times instead of zero.

`BUILTIN_FUNCTIONS` is build-time-known (every entry literal in
`builtins.hpp`); a `frozen::unordered_map` gives O(1) lookup with no
runtime allocation. Chaining a single process-shared root scope built
from that map as `scopes_[0]` skips the per-compile insertion phase.

### 1.7 Const/pattern recognizer duplication + reorder_named_arguments (audit F15)

**MIDI→Hz duplication:**

- `const_eval.cpp:41`: `double hz = 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);`
- `codegen_functions.cpp:41`: same line verbatim.

**Const-value recognizer:**

- `ConstEvaluator::eval` (`const_eval.cpp:31-87`) handles 11 node
  kinds.
- `resolve_const_value` (`codegen_functions.cpp:32-74`) handles 5 of
  them with no shared code.

**Pattern-producer recognizer:**

- `SemanticAnalyzer::is_pattern_producing_expr` (`analyzer.cpp:11-40`)
  lists `chord, seq`.
- `shape_index.cpp:139-168` (`is_pattern_producer`, `rhs_is_pattern`)
  lists `seq, timeline, sample, chord` — **already disagrees**.
- `analyzer.cpp:571-582` AND `analyzer.cpp:732-741` both hardcode
  `call_name == "chord" || call_name == "seq"` for binding
  `PatternInfo`. Two more in-analyzer copies.

**`reorder_named_arguments`:** `analyzer.cpp:2680-2822` (142 LOC,
BuiltinInfo overload), `analyzer.cpp:2823-2964` (142 LOC, UserFunction
overload), `codegen.cpp:3230-3375` (146 LOC, spread variant after
correctness Phase 1a) — **three near-mirrors totaling 430 LOC**.

### 1.8 Ghost-field children in AST (audit §3.3)

Six "auxiliary child" fields are stored inside `Node::data` variant
arms rather than in the child linked list. Every traversal that wants
to be complete must special-case each:

| Ghost field | Defined in | What it points at |
|---|---|---|
| `MatchArmData::guard_node` | `ast.hpp:294` | Guard expression (`NULL_NODE` if none) |
| `ArgumentData::spread_source` | `ast.hpp:187` | `..expr` source — node not added as child |
| `RecordLitData::spread_source` | `ast.hpp:329` | `{..expr, …}` source record |
| `DestructureField::default_node` | `ast.hpp:28` | `{a = default}` default expr |
| `HoleData::field_name` | (per audit §3.3) | Dotted hole field name as data, not child Identifier |
| `ClosureParamData::annotated_type` | `ast.hpp:200` (enum, not node ref) | Type annotation enum — not a node ref but follows the same "extra data not in tree" pattern |

`clone_subtree` (`analyzer.cpp:1262-1321`) and the substitute path
(`analyzer.cpp:1508-1568`) collectively contain ~80 LOC of pure
ghost-field bookkeeping. Every future consumer of the AST (pattern
debug, shape index, LSP) must replicate that knowledge.

### 1.9 Dead code (audit §3.1, §3.2)

- `TokenType::MiniString` declared at `token.hpp:90,159` but never
  produced (only `test_lexer.cpp:839` mentions it as an unreachable
  switch arm).
- `MiniLexer` `bool`-overload ctor (`mini_lexer.hpp:38-39, cpp:31-33`)
  has no production callers.
- `codegen/literals.hpp::make_push_const` and `make_mtof` — `grep`
  returns no users.
- `BinOp` enum + `BinaryOpData` variant arm + `binop_function_name`
  constexpr fn — post-parser-dead (parser desugars to Call directly;
  see §1.3).

---

## 2. Goals and Non-Goals

### Goals

1. **Zero shared mutable state below `CompileContext` at PRD close.**
   Phase 9 re-runs the catalogue from Phase 1 and asserts empty.
   After this PRD, the only remaining concurrency hazard for
   per-import parallelism is the pre-lex source concatenation in
   `akkado.cpp:90-137` (audit F5).
2. **`shape_index` is a thin formatter, not a re-implementation.**
   478 LOC → ~80 LOC. Editor-cursor latency drops from full
   front-end to "format what compile() already produced".
3. **`compile()` takes one `CompileOptions` arg, returns a grouped
   `CompileResult`.** Every in-tree caller migrates one-time.
4. **Builtin scope is process-shared, immutable, free to consult
   concurrently.** `frozen::map`-backed; chained as `scopes_[0]`
   parent in every per-compile `SymbolTable`.
5. **Pratt operator dispatch goes through a single `OpInfo[]`
   table.** Adding a new operator becomes a one-line addition; all
   four switches collapse to table walks.
6. **`lex_primitives.hpp` exists; `lexer.cpp` + `mini_lexer.cpp`
   compose it.** ~250–300 LOC of mechanical duplication removed.
7. **`expr_kinds.hpp` is the single source of truth** for
   `is_const_evaluable` / `is_pattern_producer` / `is_literal_value`
   / `midi_to_hz`. Three `reorder_named_arguments` mirrors collapse
   to one helper.
8. **`Node::extra_children[]` exists; every ghost field migrates to
   it.** `clone_subtree` and substitute paths lose ~80 LOC of
   per-field special-casing.
9. **All audit-flagged dead code is gone.** `grep` for each removed
   symbol returns zero hits in `src/`+`include/`.
10. **`debug-JSON gate exists and defaults `false`.** CLI / headless
    compile times improve by ~5–15% on pattern-heavy programs.

### Non-Goals

- **Per-import front-end parallelism** (audit F5 / PRD-3) — explicit
  future PRD. This PRD prepares everything possible to make that PRD
  a self-contained orchestration change.
- **Codegen monolith refactors** (audit F4 / F9 / F10 / viz/param
  families) — covered by `prd-codegen-sprawl-cleanup.md`.
- **Critical correctness findings** (audit F1, F2, F3, F7, F8, F12,
  F14) — covered by `prd-parser-codegen-correctness.md`.
- **Per-statement parallel codegen** (audit Wave 4 / per-§3.4
  `node_types_` split) — documented as Phase 1 catalogue entry +
  marked "Out of scope — future PRD" with rationale.
- **Backwards-compatibility shims for the `compile()` API change.**
  In-tree migration is one PR; there are no external API consumers.
- **A new test framework / harness.** Existing Catch2 + the snapshot
  harness from correctness PRD Phase 0 cover this PRD's tests too.

---

## 3. Architecture / Technical Design

### 3.1 Parallelism-blocker catalogue file format

New file: `docs/parallelism-blocker-catalogue.md`. Checked in by Phase
1, updated by every later phase to flip entries from "open" to
"resolved <phase>". Phase 9 asserts no "open" entries remain (other
than the F5 / PRD-3 handover entry).

Template:

```markdown
# Parallelism Blocker Catalogue

Last updated: <YYYY-MM-DD>
Maintained by: prd-parser-codegen-hardening.md (Phase 1 + 9)

## Open

| ID | Site | Hazard | Severity | Resolution path |
|----|------|--------|----------|-----------------|
| PB-001 | <file:line> | <one-line description> | High/Med/Low | <phase or PRD ref> |
| …      |             |                       |             |                  |

## Resolved during this PRD

| ID | Resolved in | Commit | Notes |
|----|-------------|--------|-------|
| …  | Phase N     | <hash> |       |

## Out of scope — future PRD-3 (per-import parallelism)

| ID | Site | Note |
|----|------|------|
| PB-F5 | akkado.cpp:90-137 | Source concatenation pre-lex — the orchestration step that PRD-3 replaces with per-import fan-out |
```

### 3.2 `CompileOptions` + grouped `CompileResult`

New struct in `akkado/include/akkado/akkado.hpp`:

```cpp
namespace akkado {

/// All optional inputs to a compile, grouped into one struct.
struct CompileOptions {
    std::string_view filename = "<input>";
    SampleRegistry* sample_registry = nullptr;
    const FileResolver* resolver = nullptr;
    bool lint_strict = false;
    bool bypass_master = false;
    bool emit_debug_json = false;   // NEW: gates serialize_mini_ast_json
    CompileContext* ctx = nullptr;  // Defined by correctness PRD Phase 4
};

CompileResult compile(std::string_view source,
                     const CompileOptions& opts = {});

CompileResult compile_file(const std::string& path,
                          const CompileOptions& opts = {});

} // namespace akkado
```

`CompileResult` re-grouped into named sub-structs (carries the
existing fields, no functional change):

```cpp
namespace akkado {

/// Cedar bytecode + instruction-level metadata.
struct CompiledProgram {
    std::vector<std::uint8_t> bytecode;
    std::uint32_t main_instruction_count = 0;
    std::vector<cedar::BlockEntry> block_table;
    std::vector<SourceLocation> source_locations;
    std::vector<StateInitData> state_inits;
};

/// Per-call requests the host must satisfy before resuming the audio thread.
struct RuntimeRequests {
    std::vector<std::string> required_samples;
    std::vector<RequiredSample> required_samples_extended;
    std::vector<ScalarSampleMapping> scalar_sample_mappings;
    std::vector<RequiredSoundFont> required_soundfonts;
    std::vector<RequiredMidiSource> required_midi_sources;
    std::vector<RequiredMidiCcRoute> required_midi_cc_routes;
    std::vector<std::string> required_input_sources;
    std::vector<RequiredWavetable> required_wavetables;
    std::vector<UriRequest> required_uris;
};

/// UI-bearing declarations and post-compile tooling outputs.
struct CompileArtifacts {
    std::vector<ParamDecl> param_decls;
    std::vector<VisualizationDecl> viz_decls;
    std::vector<BuiltinVarOverride> builtin_var_overrides;
    std::optional<SymbolTable> symbols;
    std::shared_ptr<Ast> ast;
};

struct CompileResult {
    bool success = false;
    CompiledProgram program;
    RuntimeRequests requests;
    CompileArtifacts artifacts;
    std::vector<Diagnostic> diagnostics;
};

} // namespace akkado
```

Caller migration is mechanical: `result.bytecode` →
`result.program.bytecode`, `result.required_samples` →
`result.requests.required_samples`, etc. The full grep map ships in
the Phase 2 PR.

### 3.3 Frozen builtin scope

New dep: vendored `third_party/frozen/` (Apache 2.0 header-only,
~2000 LOC). Picked because it's the standard choice for compile-time
constant maps in modern C++ and the API is `std::unordered_map`-like.

`BUILTIN_FUNCTIONS` / `BUILTIN_ALIASES` / `BUILTIN_VARIABLES` in
`builtins.hpp` become `frozen::unordered_map<frozen::string,
BuiltinInfo>` instances (still inline-defined; no codegen step). The
type signature change is mechanical; lookups via
`BUILTIN_FUNCTIONS.find(name)` keep their existing shape.

Builtin scope as `SymbolTable` root parent:

```cpp
// New in symbol_table.hpp
const SymbolScope& builtin_scope();   // process-singleton, returns const ref

// SymbolTable ctor changes from registering 600+ symbols to chaining
// to the singleton:
SymbolTable::SymbolTable() {
    scopes_.push_back(/* scope_id = 0 */ const_cast<SymbolScope*>(&builtin_scope()));
    scopes_.push_back(/* user scope */ {});  // current scope idx = 1
}
```

The builtin scope is built once in `builtin_scope()`'s first call
(thread-safe via C++11 static-init guarantee). Subsequent
`SymbolTable` instances pay zero per-construction cost for builtins.

**Hazard:** the builtin scope is `const`-accessed via `const SymbolScope*`
held by every `SymbolTable`. Mutations must be impossible. The
existing `Scope::lookup` is `const`-callable; `Scope::insert` would
need to be off-limits for `scopes_[0]`. Phase 3 adds a debug-build
assertion in `SymbolTable::insert` that the target scope index is
never 0.

### 3.4 Pratt `OpInfo[]` table

```cpp
// New in parser.hpp (or a new operators.hpp included by parser)
enum class OpAssoc { Left, Right };

struct OpInfo {
    TokenType    token;
    Precedence   precedence;
    OpAssoc      associativity;
    const char*  builtin_name;   // "add", "sub", "mul", "div", "pow", … (Identifier name in desugared Call)
};

constexpr std::array<OpInfo, /*N*/> OPERATORS = {{
    {TokenType::Plus,         Precedence::Additive,       OpAssoc::Left,  "add"},
    {TokenType::Minus,        Precedence::Additive,       OpAssoc::Left,  "sub"},
    {TokenType::Star,         Precedence::Multiplicative, OpAssoc::Left,  "mul"},
    {TokenType::Slash,        Precedence::Multiplicative, OpAssoc::Left,  "div"},
    {TokenType::Caret,        Precedence::Power,          OpAssoc::Right, "pow"},
    // … pipe / comparison / logical / etc.
}};

constexpr const OpInfo* find_op(TokenType t) {
    for (const auto& op : OPERATORS) if (op.token == t) return &op;
    return nullptr;
}
```

All four parser switches become table walks:

- `get_precedence(t)` → `find_op(t)->precedence` (or `Lowest` if null).
- `is_infix_operator(t)` → `find_op(t) != nullptr`.
- `parse_infix(t)` → look up `OpInfo`, dispatch to a single
  parameterised `parse_binary_op(*info)`.
- `parse_binary` → consult `OpInfo::associativity` directly instead of
  the existing `if (op.type == TokenType::Caret)` branch.

`BinOp`, `BinaryOpData`, `binop_function_name`, the `BinaryOp` `NodeType`
case switch arm are deleted (post-parser-dead per §1.3).

### 3.5 `lex_primitives.hpp` extract

```cpp
// New: akkado/include/akkado/lex_primitives.hpp
namespace akkado::lex {

/// Source cursor shared by Lexer and MiniLexer.
class CursorBase {
public:
    CursorBase(std::string_view source, int line, int column);

    bool is_at_end() const;
    char peek() const;
    char peek_next() const;
    char advance();
    bool match(char expected);

    int  line()    const { return line_; }
    int  column()  const { return column_; }
    int  current() const { return current_; }
    int  start()   const { return start_; }
    void mark_start() { start_ = current_; }

protected:
    std::string_view source_;
    int current_ = 0;
    int start_   = 0;
    int line_    = 1;
    int column_  = 1;
};

bool is_digit(char c);
bool is_alpha(char c);
bool is_alpha_numeric(char c);
bool is_whitespace(char c);

/// Scan a numeric literal (int or float, with optional exponent).
/// Returns the parsed value + whether it's an integer.
struct NumericScanResult {
    double value;
    bool   is_integer;
    bool   ok;
};
NumericScanResult scan_number(CursorBase& cur);

/// Scan an optional `:n` velocity suffix; returns 1.0f if absent.
float scan_velocity_suffix(CursorBase& cur);

/// Parse a pitch token (e.g. "c4", "f#3", "bb-1") to MIDI note number.
/// Returns nullopt if the token doesn't parse.
std::optional<std::uint8_t> parse_pitch_to_midi(std::string_view token);

} // namespace akkado::lex
```

`Lexer` and `MiniLexer` both inherit from `CursorBase` (or compose it
— TBD by reviewer in PR; either shape works, inheritance is the
shorter diff). All character-classifier free functions are deleted
from `lexer.cpp` / `mini_lexer.cpp` and re-imported.

Phase 5 lands AFTER the correctness PRD's Phase 2 (F8 mini-lexer line
tracking fix) so the `advance()` semantics in `CursorBase` are
already correct.

### 3.6 `Node::extra_children[]` slot

```cpp
// In ast.hpp: add to Node struct
struct Node {
    NodeType type;
    SourceLocation location;
    NodeIndex first_child = NULL_NODE;
    NodeIndex next_sibling = NULL_NODE;

    /// Auxiliary children outside the linked list (guards, spread
    /// sources, defaults). Visited by every generic traversal AFTER
    /// the first_child/next_sibling chain. Order is fixed per parent
    /// NodeType (see kExtraChildKinds[NodeType]).
    std::vector<NodeIndex> extra_children;

    std::variant</* … same arms minus the ghost-field references … */> data;
};

/// Returns the extra-child slot names for a node type (debugging /
/// pattern-debug serialization). Aligns with the indices used in
/// extra_children.
std::span<const char* const> extra_child_kinds(NodeType t);
```

Migration of each ghost-field:

| Ghost field today | After |
|---|---|
| `MatchArmData::guard_node` | `node.extra_children[0]` for MatchArm nodes |
| `ArgumentData::spread_source` | `node.extra_children[0]` for Argument-with-spread |
| `RecordLitData::spread_source` | `node.extra_children[0]` for RecordLit-with-spread |
| `DestructureField::default_node` | `node.extra_children[i]` for Destructure-with-defaults |
| `HoleData::field_name` (string in data) | Stays as data (not a node ref; out of scope) |
| `ClosureParamData::annotated_type` | Stays as data (not a node ref) |

The two non-node-ref fields stay in `Data` — `extra_children[]` is
specifically for AST node references. The audit conflated "ghost data"
with "ghost child"; this PRD migrates only the latter.

Generic traversal becomes:

```cpp
template <typename Visitor>
void visit_all_children(const AstArena& arena, NodeIndex n, Visitor v) {
    // Linked-list children
    for (NodeIndex c = arena[n].first_child; c != NULL_NODE; c = arena[c].next_sibling) {
        v(c);
    }
    // Extra children
    for (NodeIndex c : arena[n].extra_children) {
        v(c);
    }
}
```

`clone_subtree` walks both. `substitute` walks both. No per-Data
special-casing remains.

### 3.7 `expr_kinds.hpp`

```cpp
// New: akkado/include/akkado/expr_kinds.hpp
namespace akkado::expr_kinds {

/// True if the expression can be evaluated to a TypedValue at compile time.
/// Single source of truth for ConstEvaluator + codegen_functions.
bool is_const_evaluable(const Ast& ast, NodeIndex node);

/// True if the expression produces a pattern stream (chord, seq,
/// timeline, sample, pat). Replaces 4 disagreeing in-tree copies.
bool is_pattern_producer(const Ast& ast, NodeIndex node);

/// True if the expression is a literal value (Number, Bool, String,
/// Pitch). No side effects, no lookup needed.
bool is_literal_value(const Ast& ast, NodeIndex node);

/// MIDI note number to Hz. Single source of truth.
constexpr double midi_to_hz(double midi) {
    return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
}

} // namespace akkado::expr_kinds
```

Single `reorder_named_arguments` helper takes a "param-info source"
abstraction:

```cpp
struct NamedArgSchema {
    std::span<const std::string_view> param_names;
    int positional_count;   // how many of param_names are mandatory positional
    bool variadic_tail;     // does the function take ...rest at the end
};

/// Reorder the arg chain to match schema slot order. Returns the
/// per-slot NodeIndex vector. NULL_NODE entries mean "use default".
std::vector<NodeIndex> reorder_named_arguments(
    const Ast& ast,
    NodeIndex call_node,
    const NamedArgSchema& schema,
    DiagnosticsCollector& diags);
```

The two analyzer overloads + the codegen variant build the schema from
their respective `BuiltinInfo` / `UserFunction` / spread metadata, then
call the helper.

### 3.8 Debug-JSON gate

`CompileOptions::emit_debug_json` (`false` by default). Threads through
`CodeGenerator` ctor (already takes a `CompileContext*` per correctness
PRD Phase 4 — add the bool there or carry on a separate field).
`serialize_mini_ast_json` (`codegen_patterns.cpp:1379`) is wrapped:

```cpp
if (emit_debug_json_) {
    // existing serialize call
}
```

CLI tools (`nkido`, `akkado`) leave the option at default
(false). Web/wasm explicitly sets `true` (it consumes the JSON for
the pattern debug panel).

---

## 4. Per-Phase Implementation Detail

### Phase 1 — Dead-code sweep + parallelism-blocker catalogue

**Scope.**

1. Delete every audit-flagged dead symbol (§1.9). One commit per
   symbol family for clean review:
   - `TokenType::MiniString` (token.hpp:90, 159; unreachable arm in
     test_lexer.cpp:839).
   - `MiniLexer` `bool`-overload ctor (mini_lexer.hpp:38-39,
     mini_lexer.cpp:31-33).
   - `codegen/literals.hpp::make_push_const` + `make_mtof`.
   - `BinOp` enum / `BinaryOpData` variant arm / `binop_function_name`
     / `BinaryOp` `NodeType::BinaryOp` case arm. **Conditional:** only
     after grep confirms zero downstream consumers; if any survive,
     Phase 4 (Pratt) deletes them as part of its scope.

2. Author and check in
   `docs/parallelism-blocker-catalogue.md`. Mechanism:

   ```bash
   # Exhaustive scans documented in the PRD as queries to re-run:
   grep -rn 'static std::mutex\|static std::unordered_map\|static std::map\|static std::vector\|static std::atomic' akkado/src/ akkado/include/
   grep -rn 'static_assert\|once_flag\|call_once\|thread_local' akkado/src/ akkado/include/
   grep -rn 'std::lock_guard\|std::scoped_lock\|std::unique_lock' akkado/src/
   ```

   Every non-trivial match becomes a `PB-NNN` catalogue entry with
   site / hazard / severity / resolution-path. Trivial matches (const
   data, pure-function statics) are listed once at the bottom under
   "Verified safe" so re-audits don't re-investigate them.

3. **Trivial fixes shipped in this phase.** Anything in the catalogue
   that's a one-line change AND doesn't belong in a later phase. E.g.
   if a stray `static std::vector` is found that should obviously be
   per-call local, fix it here. Anything bigger (frozen builtin
   scope, voicing — already done by correctness PRD, etc.) is
   deferred to its later phase.

**Files touched.**

| File | Change |
|---|---|
| `akkado/include/akkado/token.hpp:90, 159` | Delete `TokenType::MiniString`. |
| `akkado/include/akkado/mini_lexer.hpp:38-39` | Delete `bool`-overload ctor. |
| `akkado/src/mini_lexer.cpp:31-33` | Delete ctor impl. |
| `akkado/include/akkado/codegen/literals.hpp` | Delete `make_push_const`, `make_mtof`. |
| `akkado/include/akkado/ast.hpp:148-167` | Delete `BinOp` enum + `binop_function_name` (only if grep zero, else Phase 4). |
| `akkado/tests/test_lexer.cpp:839` | Remove unreachable arm. |
| `docs/parallelism-blocker-catalogue.md` (NEW) | Author + check in. |

**Exit criteria.**

- `grep -rn 'TokenType::MiniString\|make_push_const\|make_mtof\|binop_function_name' akkado/`
  returns zero hits.
- `docs/parallelism-blocker-catalogue.md` exists with at least the
  "Verified safe" section populated. Every "Open" entry has a
  `resolution path` field pointing to a phase of this PRD or to a
  named follow-up PRD.
- All tests green (no behavior change expected from dead-code
  removal).
- **Docs updated per §12 protocol** — PRD status block marks Phase 1
  SHIPPED with commit hash; audit doc marks F15 RESOLVED.

---

### Phase 2 — `CompileOptions` + `CompileResult` re-grouping + debug-JSON gate

**Scope.** Roll the 6-arg `compile()` into `CompileOptions`. Group
`CompileResult` fields into named sub-structs. Add
`emit_debug_json` field. Migrate all in-tree callers.

**Files touched.**

| File | Change |
|---|---|
| `akkado/include/akkado/akkado.hpp:47-93, 105-120` | Define `CompileOptions`, `CompiledProgram`, `RuntimeRequests`, `CompileArtifacts`, new `CompileResult` shape, new `compile()` / `compile_file()` signatures. |
| `akkado/src/akkado.cpp:29, 263` | Rewrite `compile()` body to read options struct. |
| `akkado/src/codegen.cpp` | Thread `emit_debug_json` into CodeGenerator. |
| `akkado/include/akkado/codegen.hpp` | Add `emit_debug_json_` bool member; ctor parameter. |
| `akkado/src/codegen_patterns.cpp:1379` | Gate `serialize_mini_ast_json` behind the flag. |
| `web/wasm/nkido_wasm.cpp:589` | Migrate to new API; set `emit_debug_json = true`. |
| `tools/nkido/main.cpp:274` | Migrate; leave `emit_debug_json = false`. |
| `tools/nkido/bytecode_loader.cpp:63` | Migrate. |
| `tools/akkado/main.cpp:106` | Migrate; leave `emit_debug_json = false`. |
| `akkado/tests/test_*.cpp` (15+ files) | Mechanical migration; `compile(src)` → `compile(src, {})` mostly. |
| `akkado/tests/test_codegen.cpp` (NEW test) | Assert `emit_debug_json = false` produces no JSON in `compile_result_to_debug_string` (or whatever the existing accessor is). |

**Exit criteria.**

- `grep -rn 'akkado::compile(.*nullptr,' --include="*.cpp"` returns
  zero hits (no caller passes the 6-arg positional form).
- `CompileResult` has 4 top-level fields: `success`, `program`,
  `requests`, `artifacts`, `diagnostics`.
- Headless CLI compile time on a pattern-heavy fixture (e.g.
  `akkado/stdlib/event_transforms.ak`) drops measurably (~5–15%) —
  recorded in PR description.
- All existing tests green; snapshot harness reports byte-identical
  bytecode.
- **Docs updated per §12 protocol** — Phase 2 SHIPPED; audit doc
  marks the §3.5 "no incremental cache + debug-JSON cost" item with
  PARTIAL: debug-JSON gate shipped; incremental cache stays open.

---

### Phase 3 — Frozen builtin scope (PRD-12)

**Scope.** Vendor `frozen`; convert `BUILTIN_FUNCTIONS` /
`BUILTIN_ALIASES` / `BUILTIN_VARIABLES` to `frozen::unordered_map`;
build a process-shared `builtin_scope()` singleton; chain as
`scopes_[0]` in every `SymbolTable`.

**Files touched.**

| File | Change |
|---|---|
| `third_party/frozen/` (NEW) | Vendored release of `frozen` (Apache 2.0; bring in just the headers we need). Add a `THIRD_PARTY.md` listing the version + license. |
| `CMakeLists.txt` / `akkado/CMakeLists.txt` | Add `target_include_directories(... third_party/frozen/include)` to akkado target. |
| `akkado/include/akkado/builtins.hpp:268, 1664, 1717` | Convert the three maps from `std::unordered_map` initializer-list to `frozen::make_unordered_map(...)`. |
| `akkado/include/akkado/symbol_table.hpp` | Add `const SymbolScope& builtin_scope()` free fn declaration. |
| `akkado/src/symbol_table.cpp:5-9, 236-264` | `SymbolTable` ctor chains `scopes_[0] = &builtin_scope()` instead of running `register_builtins()`. `register_builtins()` becomes the impl of `builtin_scope()` (called once, function-local-static). |
| `akkado/src/symbol_table.cpp` (insert) | Debug assertion: `assert(target_scope_idx != 0 && "cannot mutate builtin scope")`. |
| `akkado/tests/test_symbol_table.cpp` | New test: `SymbolTable A; SymbolTable B;` — assert both `lookup("osc")` succeed without any builtin re-registration (verifiable by counting how many inserts happen during each construction). |
| `docs/parallelism-blocker-catalogue.md` | Flip `PB-builtins-reinsertion` from open to resolved-Phase-3. |

**Edge case — `BUILTIN_ALIASES` lookup order.** Today the analyzer
consults `BUILTIN_ALIASES` after `BUILTIN_FUNCTIONS`. Phase 3
preserves the lookup order — the builtin scope's `lookup` consults
functions first, aliases second, variables third. Documented in the
PR.

**Exit criteria.**

- `grep -n 'register_builtins\b' akkado/` returns one definition site
  + zero call sites from `SymbolTable::SymbolTable()`.
- New test passes: two `SymbolTable` instances chain to the same
  builtin scope; neither pays per-construction insert cost.
- `BUILTIN_FUNCTIONS` / `BUILTIN_ALIASES` / `BUILTIN_VARIABLES` are
  `frozen::unordered_map`-typed.
- `third_party/frozen/` present + `THIRD_PARTY.md` lists version +
  license.
- All tests green; snapshot harness byte-identical.
- **Docs updated per §12 protocol** — Phase 3 SHIPPED; audit doc
  marks §3.5 "600+ builtin re-inserts" RESOLVED. Catalogue updated.

---

### Phase 4 — Pratt `OpInfo[]` table unification (PRD-10 minus F7)

**Scope.** Replace four parser switches with one `OpInfo[]` table.
Delete `BinOp` / `BinaryOpData` / `binop_function_name` / `BinaryOp`
NodeType case arm (if not already deleted in Phase 1).

**Files touched.**

| File | Change |
|---|---|
| `akkado/include/akkado/parser.hpp` (or new `operators.hpp`) | Define `OpInfo`, `OpAssoc`, `OPERATORS[]`, `find_op()`. |
| `akkado/src/parser.cpp:184-203` | `get_precedence` → table lookup. |
| `akkado/src/parser.cpp:205-226` | `is_infix_operator` → table membership. |
| `akkado/src/parser.cpp:702-726` | `parse_infix` → `parse_binary_op(*find_op(t))`. |
| `akkado/src/parser.cpp:1431-1487` | `parse_binary` → consult `OpAssoc` from the table; drop the `if (op.type == TokenType::Caret)` branch. |
| `akkado/include/akkado/ast.hpp:148-167` | Delete `BinOp` + `binop_function_name` (if Phase 1 didn't). |
| `akkado/include/akkado/ast.hpp` (Data variants) | Delete `BinaryOpData` variant arm. |
| `akkado/src/analyzer.cpp` / `codegen*.cpp` | Delete any `case NodeType::BinaryOp:` arms (post-parser-dead). |
| `akkado/tests/test_parser.cpp` | New test: every entry in `OPERATORS[]` parses + desugars to the named Call (data-driven, parameterised over the table). |

**Exit criteria.**

- `grep -n 'BinOp\b\|BinaryOpData\|binop_function_name' akkado/`
  returns zero hits in `src/`+`include/`.
- The four parser switches no longer enumerate operators by hand.
- `2 + 3 * 4` still parses as `add(2, mul(3, 4))`; `2 ^ 3 ^ 2` still
  evaluates to 512 (regression of correctness PRD's F7 test).
- All tests green; snapshot harness byte-identical.
- **Docs updated per §12 protocol** — Phase 4 SHIPPED; audit doc
  marks F6 (Pratt portion) RESOLVED + PRD-10 (Pratt-table portion)
  shipped.

---

### Phase 5 — `lex_primitives.hpp` extract (PRD-8 minus F8)

**Scope.** New `lex_primitives.hpp` with `CursorBase` + classifiers +
`scan_number` + `scan_velocity_suffix` + `parse_pitch_to_midi`.
Refactor `lexer.cpp` and `mini_lexer.cpp` to compose it.

**Files touched.**

| File | Change |
|---|---|
| `akkado/include/akkado/lex_primitives.hpp` (NEW) | Per §3.5 spec. |
| `akkado/src/lex_primitives.cpp` (NEW) | Impl of non-trivial helpers (`scan_number`, `scan_velocity_suffix`, `parse_pitch_to_midi`). |
| `akkado/include/akkado/lexer.hpp` | `Lexer` inherits from / composes `CursorBase`. |
| `akkado/src/lexer.cpp:59-102, 317-383, 540` | Delete the duplicates. Reach into `lex::scan_number`, `lex::parse_pitch_to_midi`, classifiers. |
| `akkado/include/akkado/mini_lexer.hpp` | Same. |
| `akkado/src/mini_lexer.cpp:54-106, 426-518, 488-504, 671-686, 759-775, 228` | Delete the duplicates AND the 3 in-file velocity-suffix near-clones. |
| `akkado/tests/test_lex_primitives.cpp` (NEW) | Unit tests for `scan_number`, `scan_velocity_suffix`, `parse_pitch_to_midi`. |

**Pre-implementation grep map.** Before opening the PR, run and check
in:

```bash
grep -n 'is_digit\|is_alpha\|is_whitespace\|peek\|peek_next\|advance\|match' \
    akkado/src/lexer.cpp akkado/src/mini_lexer.cpp \
    > docs/phase5-lexer-primitive-callers.txt
```

Used as the implementer's call-site map; deleted post-merge.

**Exit criteria.**

- `lexer.cpp` line count reduced by ≥150; `mini_lexer.cpp` by ≥200
  (recorded in PR description). Combined removal target ≥250 LOC.
- `lex_primitives.hpp` unit tests pass.
- All existing lexer/mini-lexer tests pass byte-identical token
  streams.
- F8 (correctness Phase 2) regression tests for multi-line patterns
  still pass — confirms the cursor's `advance()` semantics are
  preserved.
- **Docs updated per §12 protocol** — Phase 5 SHIPPED; audit doc
  marks F11 RESOLVED + PRD-8 (lex_primitives portion) shipped.

---

### Phase 6 — `Node::extra_children[]` ghost-field migration (PRD-14)

**Scope.** Add `extra_children` slot to `Node`. Migrate the 4 node-ref
ghost fields. Refactor `clone_subtree` + substitute paths to use the
generic walker.

**Files touched.**

| File | Change |
|---|---|
| `akkado/include/akkado/ast.hpp:28, 187, 294, 329` | Remove node-ref ghost fields from `DestructureField` / `ArgumentData` / `MatchArmData` / `RecordLitData`. |
| `akkado/include/akkado/ast.hpp` (Node) | Add `std::vector<NodeIndex> extra_children;` member; add `extra_child_kinds(NodeType)` helper. |
| `akkado/src/parser.cpp` | At every site that previously set a ghost field, push to `extra_children` instead (5-10 sites). |
| `akkado/src/analyzer.cpp:1262-1321, 1508-1568` | `clone_subtree` and substitute path: replace per-Data special-cases with the generic walker. |
| `akkado/src/pattern_debug.cpp` | Update consumer if it traverses ghost fields. |
| `akkado/src/codegen.cpp` / `codegen_patterns.cpp` | Update any reader that accessed `match_arm.guard_node`, `arg.spread_source`, etc., to read from `extra_children` instead. |
| `akkado/include/akkado/codegen.hpp` | Update any helper signatures that took the ghost fields directly. |
| `akkado/tests/test_ast.cpp` (NEW) | Round-trip test: build a MatchArm with guard, clone the subtree, assert the clone's `extra_children[0]` matches the original's guard. |

**Exit criteria.**

- `grep -n 'guard_node\|spread_source\|default_node' akkado/include/akkado/ast.hpp`
  returns zero hits in the Data structs.
- `clone_subtree` and substitute reduce by ≥60 LOC combined
  (recorded in PR description).
- Generic AST traversal walker (used by pattern_debug, by Phase 7,
  by Phase 8) handles all ghost fields uniformly.
- All tests green; snapshot harness byte-identical.
- **Docs updated per §12 protocol** — Phase 6 SHIPPED; audit doc
  marks §3.3 ghost-field item RESOLVED + PRD-14 shipped.

---

### Phase 7 — `expr_kinds.hpp` dedup + `reorder_named_arguments` consolidation (PRD-13)

**Scope.** New `expr_kinds.hpp` with `is_const_evaluable` /
`is_pattern_producer` / `is_literal_value` / `midi_to_hz`. Single
`reorder_named_arguments(schema)` helper replacing 3 mirrors.

**Files touched.**

| File | Change |
|---|---|
| `akkado/include/akkado/expr_kinds.hpp` (NEW) | Per §3.7 spec. |
| `akkado/src/expr_kinds.cpp` (NEW) | Impls (mostly delegating to ConstEvaluator + the existing recognizers). |
| `akkado/src/const_eval.cpp:41` | Delete inline `midi_to_hz`; use `expr_kinds::midi_to_hz`. |
| `akkado/src/codegen_functions.cpp:32-74` | Delete `resolve_const_value`; use `expr_kinds::is_const_evaluable` + the unified `ConstEvaluator` path. |
| `akkado/src/codegen_functions.cpp:41` | Delete inline `midi_to_hz`. |
| `akkado/src/analyzer.cpp:11-40` | `is_pattern_producing_expr` becomes a thin call to `expr_kinds::is_pattern_producer`. Also unify the spelling (chord/seq/timeline/sample/pat — audit shape_index disagrees today). |
| `akkado/src/analyzer.cpp:571-582, 732-741` | Replace the two `call_name == "chord" || call_name == "seq"` hardcodings with `expr_kinds::is_pattern_producer(ast, node)`. |
| `akkado/src/shape_index.cpp:139-168` | Replace `is_pattern_producer` / `rhs_is_pattern` with the canonical helper (note: this file is replaced wholesale in Phase 8). |
| `akkado/include/akkado/named_args.hpp` (NEW) | `NamedArgSchema` + `reorder_named_arguments` declaration. |
| `akkado/src/named_args.cpp` (NEW) | Single canonical impl. |
| `akkado/src/analyzer.cpp:2680-2822, 2823-2964` | Two analyzer overloads → schema-building + call to the canonical helper. |
| `akkado/src/codegen.cpp:3230-3375` | Spread variant (post-correctness-Phase-1a) → schema-building + call to the canonical helper. |
| `akkado/tests/test_expr_kinds.cpp` (NEW) | Unit tests per predicate. |
| `akkado/tests/test_named_args.cpp` (NEW) | Unit tests for `reorder_named_arguments` against all three schema variants. |

**Exit criteria.**

- `grep -n '440.0 \* std::pow(2.0, (midi - 69.0)' akkado/src/`
  returns one hit (in `expr_kinds.cpp` / `.hpp`).
- `reorder_named_arguments` exists in exactly one location; the three
  near-mirrors are gone (combined LOC drops from ~430 to ~150;
  recorded in PR description).
- `is_pattern_producer` returns identical results across analyzer +
  shape_index + codegen (the existing disagreement is RESOLVED in
  favor of one canonical list — confirmed by a round-trip test that
  parses a fixture and asserts the predicate matches the
  analyzer's PatternInfo binding).
- All tests green; snapshot harness byte-identical.
- **Docs updated per §12 protocol** — Phase 7 SHIPPED; audit doc
  marks F15 RESOLVED + PRD-13 shipped.

---

### Phase 8 — `shape_index` over shared AST (PRD-2)

**Scope.** Delete `shape_index.cpp`'s 478 LOC re-lex+re-parse
pipeline. Replace with a thin formatter over
`CompileResult::artifacts.ast` + `CompileResult::artifacts.symbols`
(both already populated). Update the WASM caller.

**Approach.** The web caller in `web/wasm/nkido_wasm.cpp:1380`
already calls `compile()` for the main compile cycle. Instead of
running shape_index against raw source, pass the
`CompileArtifacts` from the last successful compile (or trigger a
fresh compile, which is what the editor is already doing on a
debounce). The shape-index API becomes:

```cpp
namespace akkado {

/// Format the shape index (records, patterns, arrays) from a
/// successful compile's artifacts. Returns JSON for the web IDE
/// shape-index panel and F1 lookup.
std::string serialize_shape_index(const CompileArtifacts& artifacts);

} // namespace akkado
```

The new impl walks `artifacts.ast` once (using the generic walker
from Phase 6 — no ghost-field special cases) and emits JSON entries
from `artifacts.symbols` + per-node shape inferred via the predicates
from Phase 7 (`is_pattern_producer`, etc.). No lexer, no parser, no
analyzer. ~80 LOC.

**Files touched.**

| File | Change |
|---|---|
| `akkado/include/akkado/shape_index.hpp` | Rewrite API: single `serialize_shape_index(const CompileArtifacts&)` fn. |
| `akkado/src/shape_index.cpp` | Wholesale rewrite: 478 → ~80 LOC. Drop `lex()`/`parse()` calls. |
| `web/wasm/nkido_wasm.cpp:1380` | Migrate to `serialize_shape_index(last_compile_result.artifacts)`. Remove the standalone "shape index re-compile" trigger; reuse the main compile. |
| `akkado/tests/test_shape_index.cpp` | Update existing tests to drive via `compile()` + `serialize_shape_index`. |

**Dependencies.** Hard depends on:

- Correctness PRD Phase 1a + 1b — for read-only AST. (Both shipped
  2026-05-25.)
- This PRD Phase 6 (ghost-fields) — so the generic walker is
  available.
- This PRD Phase 7 (expr_kinds) — for `is_pattern_producer`
  consistency.

**Exit criteria.**

- `akkado/src/shape_index.cpp` ≤ 100 LOC.
- `grep -n 'lex\|parse' akkado/src/shape_index.cpp` returns zero
  hits (no lexer/parser call from shape_index).
- Web IDE editor-cursor latency (measured by tracing the WASM
  `update_shape_index` callback in a representative session)
  drops measurably; recorded in PR description.
- Existing shape-index tests pass byte-identical JSON.
- **Docs updated per §12 protocol** — Phase 8 SHIPPED; audit doc
  marks F13 RESOLVED + PRD-2 shipped.

---

### Phase 9 — Parallelism handover

**Scope.** Re-run the Phase 1 audit. Assert zero "Open" entries in
`docs/parallelism-blocker-catalogue.md` (other than the F5 / PRD-3
handover entry). Document the precondition list for the future
per-import parallelism PRD.

**Files touched.**

| File | Change |
|---|---|
| `docs/parallelism-blocker-catalogue.md` | All open entries either flipped to Resolved (with phase + commit) or moved to "Out of scope — PRD-3" with rationale. Add a "Precondition list for PRD-3" section enumerating what was done here so PRD-3 author can confirm the foundation. |
| `docs/prd-parser-codegen-hardening.md` (this file) | Final status flip to DONE. |
| `docs/audits/parser-codegen-interop_audit_2026-05-25.md` | Status header flipped to reflect all hardening findings RESOLVED. |

**Exit criteria.**

- `docs/parallelism-blocker-catalogue.md` contains exactly one open
  entry: PB-F5 (source concatenation pre-lex), tagged "Out of scope
  — PRD-3".
- Re-running the Phase 1 grep commands surfaces zero new hazards
  beyond what's catalogued.
- This PRD's status block flipped to `DONE — All 9 phases shipped`.
- Audit doc reflects shipped status for F6 (Pratt portion), F11, F13,
  F15, §3.3 ghost-fields, §3.5 builtin reinsertion + debug-JSON
  gate.

---

## 5. Phase Dependencies and Order

```
Phase 1 (dead-code + catalogue) ─┐
                                 ├──> Phase 2 (CompileOptions)
                                 ├──> Phase 3 (frozen builtin)
                                 ├──> Phase 4 (Pratt table)
                                 ├──> Phase 5 (lex_primitives)
                                 ├──> Phase 6 (ghost-fields)
                                 │                │
                                 │                ├──> Phase 7 (expr_kinds)
                                 │                │            │
                                 │                │            v
                                 │                └──> Phase 8 (shape_index)
                                 │                              │
                                 v                              v
                                 Phase 9 (parallelism handover — gates close)
```

- **Phase 1** is the gating prerequisite — clears noise + scopes the
  rest via the catalogue.
- **Phases 2–5 are independent** of each other once Phase 1 ships.
  Can be done in any order, parallelizable across contributors.
- **Phase 6** must precede Phase 8 (shape_index needs the generic
  walker).
- **Phase 7** must precede Phase 8 (shape_index needs the canonical
  `is_pattern_producer`).
- **Phase 8** depends on Phases 6 + 7, and also on correctness PRD
  Phase 1a + 1b (shipped 2026-05-25).
- **Phase 9 is the close-out gate** — all preceding phases must be
  shipped before Phase 9 runs.

Estimated total effort: **6–9 weeks single-engineer**, **3–4 weeks**
if Phases 2/3/4/5 parallelize across 2 contributors.

---

## 6. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| `cedar/` | **No change** | Cedar VM untouched. |
| `compile()` signature | **Modified (breaking)** | 6 positional args → `(source, opts)`. In-tree callers migrate one-time. No external API consumers. |
| `CompileResult` shape | **Modified (breaking)** | 20+ flat fields → 4 grouped sub-structs. Caller-side field renames are mechanical (`r.bytecode` → `r.program.bytecode`). |
| `CompileOptions` | **New** | Single struct for all optional compile inputs incl. `emit_debug_json`. |
| `CompiledProgram` / `RuntimeRequests` / `CompileArtifacts` | **New** | Sub-structs of `CompileResult`. |
| `SymbolTable` ctor | **Modified** | Chains to process-shared `builtin_scope()` instead of running 600+ inserts. |
| `BUILTIN_FUNCTIONS` / `BUILTIN_ALIASES` / `BUILTIN_VARIABLES` | **Modified (type)** | `std::unordered_map` → `frozen::unordered_map`. Lookup API unchanged. |
| `third_party/frozen/` | **New** | Vendored Apache 2.0 header-only lib. Project's first 3rd-party dep. |
| Pratt switches (4) | **Removed** | Replaced by `OPERATORS[]` table + `find_op()` helper. |
| `OpInfo` / `OPERATORS[]` | **New** | Single source of truth for parser operator dispatch. |
| `BinOp` / `BinaryOpData` / `binop_function_name` / `BinaryOp` NodeType arm | **Removed** | Post-parser-dead per §1.3. |
| `lex_primitives.hpp` | **New** | `CursorBase` + scanners + classifiers shared by both lexers. |
| `Lexer` / `MiniLexer` cursors | **Modified** | Inherit from / compose `CursorBase`. |
| `lex_number` etc. duplicates in `lexer.cpp` / `mini_lexer.cpp` | **Removed** | ~250 LOC of mechanical duplication gone. |
| `Node::extra_children[]` | **New** | Inline vector of `NodeIndex` for auxiliary children. |
| `MatchArmData::guard_node` / `ArgumentData::spread_source` / `RecordLitData::spread_source` / `DestructureField::default_node` | **Modified** | Migrated to `extra_children`. |
| `HoleData::field_name` / `ClosureParamData::annotated_type` | **No change** | Not node refs; out of `extra_children` scope. |
| `clone_subtree` / substitute paths | **Modified** | Use generic walker instead of per-field special-cases. |
| `expr_kinds.hpp` | **New** | `is_const_evaluable` / `is_pattern_producer` / `is_literal_value` / `midi_to_hz` single source of truth. |
| `named_args.hpp` | **New** | Canonical `reorder_named_arguments(schema)`. |
| `analyzer.cpp` reorder overloads (2) + `codegen.cpp` reorder variant | **Removed** | Replaced by schema-building + helper call. |
| `resolve_const_value` (`codegen_functions.cpp:32-74`) | **Removed** | Subsumed by `ConstEvaluator` + `expr_kinds::is_const_evaluable`. |
| `shape_index.cpp` re-lex+re-parse pipeline | **Removed** | 478 LOC → ~80 LOC formatter over `CompileArtifacts`. |
| `serialize_mini_ast_json` | **Modified (gated)** | Runs only when `emit_debug_json = true`. CLI tools default to false. |
| `TokenType::MiniString` / `MiniLexer` bool ctor / dead `codegen/literals.hpp` helpers | **Removed** | Dead per audit §3.1/§3.2. |
| `docs/parallelism-blocker-catalogue.md` | **New** | Living document maintained by this PRD. Phase 9 asserts only the PRD-3 handover entry remains open. |

---

## 7. File-Level Changes Summary

### New files

| Path | Purpose | Introduced in phase |
|---|---|---|
| `docs/parallelism-blocker-catalogue.md` | Living catalogue of shared mutable state hazards | 1 |
| `akkado/include/akkado/lex_primitives.hpp` | Shared lexer primitives | 5 |
| `akkado/src/lex_primitives.cpp` | Non-trivial helper impls | 5 |
| `akkado/include/akkado/expr_kinds.hpp` | Expression-kind predicates + MIDI→Hz | 7 |
| `akkado/src/expr_kinds.cpp` | Impl | 7 |
| `akkado/include/akkado/named_args.hpp` | `reorder_named_arguments(schema)` | 7 |
| `akkado/src/named_args.cpp` | Impl | 7 |
| `akkado/include/akkado/operators.hpp` (or extend `parser.hpp`) | `OpInfo` + `OPERATORS[]` + `find_op()` | 4 |
| `third_party/frozen/` | Vendored frozen lib | 3 |
| `third_party/THIRD_PARTY.md` | Lists vendored deps + licenses | 3 |
| `akkado/tests/test_lex_primitives.cpp` | Unit tests | 5 |
| `akkado/tests/test_expr_kinds.cpp` | Unit tests | 7 |
| `akkado/tests/test_named_args.cpp` | Unit tests | 7 |
| `akkado/tests/test_ast.cpp` | `extra_children` clone round-trip | 6 |

### Modified files (alphabetical)

| Path | Phases | Change |
|---|---|---|
| `CMakeLists.txt` / `akkado/CMakeLists.txt` | 3, 5, 7 | Add `third_party/frozen` include; add new sources to akkado target |
| `akkado/include/akkado/akkado.hpp:47-93, 105-120` | 2 | `CompileOptions` + grouped `CompileResult` + new signatures |
| `akkado/include/akkado/ast.hpp` | 1, 4, 6 | Delete `BinOp` / `BinaryOpData` (Phase 4); add `extra_children` (Phase 6); strip 4 ghost-field members |
| `akkado/include/akkado/builtins.hpp:268, 1664, 1717` | 3 | `frozen::unordered_map` |
| `akkado/include/akkado/codegen.hpp` | 2 | Add `emit_debug_json_` member |
| `akkado/include/akkado/codegen/literals.hpp` | 1 | Delete `make_push_const`, `make_mtof` |
| `akkado/include/akkado/lexer.hpp` | 5 | Inherit from / compose `CursorBase` |
| `akkado/include/akkado/mini_lexer.hpp:38-39` | 1, 5 | Delete `bool` ctor (Phase 1); compose `CursorBase` (Phase 5) |
| `akkado/include/akkado/parser.hpp` | 4 | Declare `OpInfo` table (or include `operators.hpp`) |
| `akkado/include/akkado/shape_index.hpp` | 8 | New single-fn API: `serialize_shape_index(const CompileArtifacts&)` |
| `akkado/include/akkado/symbol_table.hpp` | 3 | Declare `builtin_scope()` free fn |
| `akkado/include/akkado/token.hpp:90, 159` | 1 | Delete `TokenType::MiniString` |
| `akkado/src/akkado.cpp:29, 263` | 2 | Read options struct; thread `emit_debug_json` |
| `akkado/src/analyzer.cpp:11-40, 571-582, 732-741, 1262-1321, 1508-1568, 2680-2964` | 6, 7 | `expr_kinds::is_pattern_producer` (Phase 7); generic walker for clone_subtree / substitute (Phase 6); schema-based reorder (Phase 7) |
| `akkado/src/codegen.cpp:3230-3375` | 7 | Schema-based reorder (replaces spread variant from correctness Phase 1a) |
| `akkado/src/codegen_functions.cpp:32-74, 41` | 7 | Delete `resolve_const_value` + inline `midi_to_hz` |
| `akkado/src/codegen_patterns.cpp:1379` | 2 | Gate `serialize_mini_ast_json` |
| `akkado/src/const_eval.cpp:41` | 7 | Delete inline `midi_to_hz` |
| `akkado/src/lexer.cpp:59-102, 317-383, 540` | 5 | Delete duplicates; use `lex::*` |
| `akkado/src/mini_lexer.cpp:31-33, 54-106, 228, 426-518, 488-504, 671-686, 759-775` | 1, 5 | Delete dead ctor (Phase 1); delete duplicates (Phase 5) |
| `akkado/src/parser.cpp:184-203, 205-226, 702-726, 1431-1487` | 4 | Switches → table walks |
| `akkado/src/shape_index.cpp` | 8 | Wholesale rewrite (478 → ~80 LOC) |
| `akkado/src/symbol_table.cpp:5-9, 236-264` | 3 | Chain to `builtin_scope()`; delete per-construction registration |
| `web/wasm/nkido_wasm.cpp:589, 1380` | 2, 8 | Migrate to `CompileOptions`; drive shape_index from `CompileArtifacts` |
| `tools/nkido/main.cpp:274` | 2 | Migrate to `CompileOptions` |
| `tools/nkido/bytecode_loader.cpp:63` | 2 | Migrate |
| `tools/akkado/main.cpp:106` | 2 | Migrate |
| `akkado/tests/test_*.cpp` (~15+ files) | 2, 6, 7, 8 | Mechanical migrations |
| `akkado/tests/test_lexer.cpp:839` | 1 | Remove unreachable `MiniString` arm |
| `docs/audits/parser-codegen-interop_audit_2026-05-25.md` | 9 | Final RESOLVED tags per §12 |

### Files explicitly NOT changed

| Path | Reason |
|---|---|
| `cedar/**` | Cedar VM untouched |
| `akkado/src/voicing.cpp` | Owned by correctness PRD Phase 4 (F14) |
| `akkado/src/mini_lexer.cpp` line-tracking | Owned by correctness PRD Phase 2 (F8) |
| `akkado/src/codegen.cpp` Call branch monolith | Owned by `prd-codegen-sprawl-cleanup.md` (F4) |
| `akkado/src/codegen_patterns.cpp` pattern-transform handlers | Owned by `prd-codegen-sprawl-cleanup.md` (F9) |
| `akkado/src/codegen_viz.cpp` / `codegen_params.cpp` | Owned by `prd-codegen-sprawl-cleanup.md` (PRD-7 portion) |

---

## 8. Edge Cases

### Phase 1 (dead-code + catalogue)

- **`BinOp` removal pre-empts Phase 4.** If grep finds zero downstream
  consumers in Phase 1, delete here. If any surface, defer to Phase 4
  to avoid a partial cleanup.
- **Catalogue grows during later phases.** Phases 2–8 may surface
  hazards not visible to the Phase 1 grep (e.g. a `static` introduced
  by an unrelated WIP branch). Each later phase's exit criteria
  include "catalogue updated if new hazards surfaced".

### Phase 2 (`CompileOptions` + grouped `CompileResult`)

- **Out-of-order positional args.** No backwards-compat shim — every
  caller migrates. If a test passes `nullptr, nullptr, true` today
  meaning `(sample_registry, resolver, lint_strict)`, the new form
  is `{.lint_strict = true}`. Designated initializers are mandatory
  in caller migrations (C++20).
- **`CompileResult` deep copies.** Existing field access is by
  reference; grouped access stays by reference. No new copies.
- **`emit_debug_json = true` parity.** When the web caller turns it
  on, the produced JSON must be byte-identical to today's
  unconditional output. Regression test in Phase 2 PR.

### Phase 3 (frozen builtin scope)

- **Concurrent first-call to `builtin_scope()`.** C++11 static-init
  guarantees one-shot construction. Tested via a multi-threaded
  `TEST_CASE` that races two threads on first SymbolTable
  construction.
- **Builtin scope held by reference.** `SymbolTable` holds `const
  SymbolScope*` to the singleton. Singleton lives forever (function-
  local-static); pointer stays valid for the process lifetime.
- **Insert into builtin scope.** Debug-build assertion forbids it.
  Release builds: the singleton is a `const SymbolScope`, the const-
  cast in `SymbolTable::scopes_` is constrained to lookups only.
- **Alias resolution order.** Documented: `BUILTIN_FUNCTIONS` first,
  `BUILTIN_ALIASES` second, `BUILTIN_VARIABLES` third (matches
  today).

### Phase 4 (Pratt `OpInfo[]`)

- **Operators with multiple precedences.** E.g. unary minus vs binary
  minus. `OPERATORS[]` is for infix only; unary stays in its own
  path. No change.
- **Pipe operator `|>`.** Already in the table set, treated as Left
  associativity. Tested.
- **`as` pipe binding.** Treated as a separate parser production
  (not infix). No change.

### Phase 5 (`lex_primitives` extract)

- **Velocity-suffix differences between full lexer / mini-lexer.**
  Three near-clones in `mini_lexer.cpp` (488/671/759) might be
  intentional (different contexts). Phase 5 unification must
  preserve every observable behavior — verified by lexer snapshot
  tests.
- **Pitch-MIDI parse failure.** `parse_pitch_to_midi` returns
  `nullopt` on failure (today's lexers fall back to "Identifier"
  in those cases). Preserved.

### Phase 6 (ghost-fields)

- **`ArgumentData::spread_source` reorder interaction.** Phase 7
  (named-arg reorder) reads `spread_source`. After Phase 6 the read
  is `arena[arg_node].extra_children[0]`. Phase 7 PR must order
  itself after Phase 6 or land both together.
- **Clone semantics.** Generic walker clones every extra-child by
  default. Verify it doesn't accidentally deep-clone a node already
  cloned by the linked-list walk — extra-children are *distinct*
  nodes from first_child/next_sibling.
- **Empty `extra_children`.** Vast majority of nodes have empty
  `extra_children`. Inline vector with capacity 0 is cheap; no
  perf concern.

### Phase 7 (`expr_kinds` + reorder)

- **`is_pattern_producer` disagreement.** Today shape_index lists
  `seq, timeline, sample, chord`; analyzer lists `chord, seq`.
  Canonical list per Phase 7: `chord, seq, timeline, sample, pat`.
  Phase 7 PR records the rationale + adjusts any tests that depended
  on the narrower list.
- **`reorder_named_arguments` spread case.** Codegen-side variant
  consumes the `pre_resolved_values_` side-table (per correctness
  Phase 1a) — the schema-based helper must accept a `slot_resolver`
  callback for "slot N is pre-resolved" cases.
- **`ConstEvaluator` invocation from codegen.** `is_const_evaluable`
  is a *cheap* predicate (no side effects). Actually evaluating the
  value still calls `ConstEvaluator::eval`. Both surface as separate
  `expr_kinds::*` entry points.

### Phase 8 (`shape_index` over shared AST)

- **Compile failure case.** If `compile()` returns `success = false`,
  `artifacts.ast` may be partial. `serialize_shape_index` must
  tolerate partial AST gracefully (today's re-lex+re-parse path
  also tolerates parse failures). Tested by feeding broken source +
  asserting the function returns the partial-shape JSON without
  crashing.
- **WASM caller no longer needs separate shape-index trigger.**
  Today the web IDE has a separate "update shape index" trigger on
  cursor move. After Phase 8 the shape index is derived from the
  last compile result; the cursor-move trigger becomes a re-render
  (no new compile). Verify the web IDE behavior matches expectations
  in a WASM smoke test.
- **Mini-AST traversal.** `serialize_shape_index` traverses the
  mini-AST sub-arenas (per correctness Phase 1b) for pattern shape
  info. The generic walker from Phase 6 needs to descend into
  `MiniLiteralData::mini_arena` — add a sub-arena hook.

### Phase 9 (parallelism handover)

- **Newly-introduced hazards.** If a phase between 1 and 8 introduces
  a NEW concurrency hazard (e.g. an internal cache), it must show
  up in the catalogue. Phase 9's re-audit catches it.
- **PRD-3 handover entry.** The single remaining open entry
  (PB-F5) must include a precise hand-off note: "what state is
  shared, what state is per-import, where the merge point is, what
  the source-map invariants are post-merge".

---

## 9. Testing Strategy

Every phase ships at least one **precise regression test** that
exercises its behavior. Plus the cross-cutting invariants asserted by
the correctness PRD's Phase 0 snapshot harness continue to hold for
every phase here.

### Phase 1 tests

- Build still works after each dead-symbol delete (CI smoke).
- `docs/parallelism-blocker-catalogue.md` exists and parses as
  markdown.

### Phase 2 tests

- `test_codegen.cpp [P2]`: compile a fixture with
  `emit_debug_json = false`; assert `CompileResult::artifacts` is
  populated but no pattern-debug JSON is emitted by
  `codegen_patterns.cpp:1379`.
- `test_codegen.cpp [P2]`: compile with `emit_debug_json = true`;
  assert JSON output is byte-identical to today's unconditional
  output (snapshot diff against a fixture's pre-Phase-2 JSON).
- `test_codegen.cpp [P2]`: existing snapshot harness diff is
  byte-identical.
- Microbench: report headless-CLI compile time before/after on
  `akkado/stdlib/event_transforms.ak` (or another pattern-heavy
  fixture); record in PR description.

### Phase 3 tests

- `test_symbol_table.cpp [P3]`: `SymbolTable A; SymbolTable B;` —
  count builtin re-registrations via a hook; assert zero on second
  construction.
- `test_symbol_table.cpp [P3]`: builtin scope is shared — `A.lookup("osc")`
  and `B.lookup("osc")` return references to the same `Symbol`.
- `test_symbol_table.cpp [P3]`: insert into scope 0 (builtin) fires
  the debug assertion (death-test pattern).
- `test_symbol_table.cpp [P3]`: multi-thread `SymbolTable`
  construction race — two threads, each constructs a SymbolTable
  while the other does too; assert no data race, both succeed.
- Existing tests green; snapshot harness byte-identical.

### Phase 4 tests

- `test_parser.cpp [P4]`: data-driven — iterate `OPERATORS[]`, parse
  `a OP b` for each entry, assert desugared `Call(name, a, b)` matches
  the `builtin_name` field.
- `test_parser.cpp [P4]`: `2 + 3 * 4` parses as `add(2, mul(3, 4))`
  (precedence regression).
- `test_parser.cpp [P4]`: `2 ^ 3 ^ 2 == 512` (locks correctness PRD
  Phase 2 regression).
- `test_parser.cpp [P4]`: every previously-passing parser test
  still passes — snapshot harness byte-identical.
- Build fails if any source file references `BinOp` /
  `binop_function_name` / `BinaryOpData` (mechanical check via
  compilation).

### Phase 5 tests

- `test_lex_primitives.cpp [P5]`: `scan_number("3.14")` returns
  `{3.14, false, true}`. `scan_number("42")` returns
  `{42.0, true, true}`. `scan_number("abc")` returns
  `{0.0, false, false}`.
- `test_lex_primitives.cpp [P5]`: `parse_pitch_to_midi("c4") == 60`,
  `"a4" == 69`, `"f#3" == 54`, `"bb-1" == 22`, `"foo" == nullopt`.
- `test_lex_primitives.cpp [P5]`: `scan_velocity_suffix(":0.8")`
  consumes the suffix and returns 0.8.
- Existing lexer/mini-lexer tests pass byte-identical token streams.

### Phase 6 tests

- `test_ast.cpp [P6]`: build a MatchArm with guard via parser; assert
  `node.extra_children` contains the guard NodeIndex.
- `test_ast.cpp [P6]`: clone_subtree round-trip — clone the MatchArm,
  assert clone's `extra_children[0]` is a fresh node with the same
  shape as the original guard.
- `test_ast.cpp [P6]`: substitute path — substitute an identifier
  inside a guard expression; assert the substituted clone reflects
  the change.
- Existing tests green; snapshot harness byte-identical.

### Phase 7 tests

- `test_expr_kinds.cpp [P7]`: `is_const_evaluable` matches today's
  `ConstEvaluator::can_eval` for the 11 node kinds.
- `test_expr_kinds.cpp [P7]`: `is_pattern_producer` returns true for
  `chord`, `seq`, `timeline`, `sample`, `pat` and false for
  everything else (data-driven over a fixture set).
- `test_expr_kinds.cpp [P7]`: `midi_to_hz(69) == 440.0` (single
  source of truth — no other site computes this).
- `test_named_args.cpp [P7]`: reorder schema with 3 named args in
  random order produces the canonical slot order. Reorder with
  defaults fills `NULL_NODE` for missing slots. Reorder with spread
  honours the `slot_resolver` callback.
- Existing analyzer + codegen tests green; snapshot harness
  byte-identical.

### Phase 8 tests

- `test_shape_index.cpp [P8]`: feed a fixture through `compile()` +
  `serialize_shape_index(result.artifacts)`; assert JSON output
  byte-identical to today's pre-Phase-8 output (snapshot diff).
- `test_shape_index.cpp [P8]`: feed broken source (parse error);
  assert `serialize_shape_index` returns partial-shape JSON without
  crashing.
- `test_shape_index.cpp [P8]`: `grep -n 'lex\|parse' akkado/src/shape_index.cpp`
  returns zero hits — enforced as a one-shot test that scans the
  source file.
- Web/WASM smoke test (Phase 8 PR includes a manual checklist for
  the WASM build): editor-cursor latency observable improvement.

### Phase 9 tests

- One-shot test: parse `docs/parallelism-blocker-catalogue.md` and
  assert the "Open" section contains exactly one entry tagged
  "PB-F5" / "Out of scope — PRD-3".

### Cross-cutting invariants (asserted by correctness PRD harness)

All correctness PRD invariants continue to hold:

- Snapshot harness reports byte-identical bytecode for every fixture
  after each phase (unless the phase intentionally changes bytecode,
  which none of these do).
- `assert(instructions_.size() == source_locations_.size())` in
  `generate()` epilogue still holds.
- `assert(input_ast_hash_ == hash_arena(ast_->arena))` in
  `generate()` epilogue still holds.

### Build + run commands

```bash
# Configure debug
cmake --preset debug

# Build
cmake --build build --target akkado_tests

# Run all akkado tests
./build/akkado/tests/akkado_tests

# Run per-phase tagged tests
./build/akkado/tests/akkado_tests "[P1]"   # Phase 1 (dead-code sweep)
./build/akkado/tests/akkado_tests "[P2]"   # CompileOptions / debug-JSON
./build/akkado/tests/akkado_tests "[P3]"   # frozen builtin scope
./build/akkado/tests/akkado_tests "[P4]"   # Pratt OpInfo
./build/akkado/tests/akkado_tests "[P5]"   # lex_primitives
./build/akkado/tests/akkado_tests "[P6]"   # extra_children
./build/akkado/tests/akkado_tests "[P7]"   # expr_kinds + reorder
./build/akkado/tests/akkado_tests "[P8]"   # shape_index
```

---

## 10. Cross-Reference: Audit Findings → Phase Map

| Finding | Phase | Notes |
|---|---|---|
| F4 (visit() Call monolith) | **Out of scope** | Owned by `prd-codegen-sprawl-cleanup.md` |
| F5 (pre-lex source concatenation) | **Out of scope** | Future PRD-3; this PRD's Phase 9 documents the precondition |
| F6 (Pratt fragmentation) | **Phase 4** | Pratt table unification; codegen `special_handlers` portion owned by codegen-sprawl PRD |
| F9 (pattern-transform clones) | **Out of scope** | Owned by `prd-codegen-sprawl-cleanup.md` |
| F10 (StateInitData duplication) | **Out of scope** | Owned by `prd-codegen-sprawl-cleanup.md` |
| F11 (mini-lexer near-clone) | **Phase 5** | `lex_primitives.hpp` extract |
| F13 (shape_index full pipeline) | **Phase 8** | 478 → ~80 LOC |
| F15 (const/pattern recognizer dup) | **Phase 7** | `expr_kinds.hpp` + reorder helper |
| §3.1 dead code (`MiniString`, etc.) | **Phase 1** | Dead-code sweep |
| §3.2 dead `BinOp` path | **Phase 1 or Phase 4** | Phase 1 if grep clean; else Phase 4 |
| §3.3 ghost-field children | **Phase 6** | `extra_children[]` migration |
| §3.5 builtin re-registration | **Phase 3** | `frozen::map` + scope chaining |
| §3.5 unconditional debug JSON | **Phase 2** | `emit_debug_json` gate |
| §3.5 `compile()` 6-arg sprawl | **Phase 2** | `CompileOptions` rollup |
| §4 parallelism prep | **Phases 1 + 9** | Catalogue + handover |

---

## 11. Sourcing for Key Design Decisions

| Decision | Where set |
|---|---|
| Two-PRD split (hardening + codegen sprawl) | Round 1 Q1 |
| All 8 hardening PRDs in scope | Round 1 Q2/Q3/Q4 (multi-select) |
| Frontend hardening PRD written first | Round 2 Q1 |
| "Audit + fix everything in scope" for parallelism | Round 2 Q2 |
| Full CompileOptions rollup + CompileResult re-grouping | Round 2 Q3 |
| shape_index full collapse (not incremental) | Round 2 Q4 |
| Filename: `prd-parser-codegen-hardening.md` | Round 3 Q1 |
| Adopt `frozen` header-only library | Round 3 Q3 |
| Ghost-fields → uniform `extra_children[]` slot | Round 3 Q4 |
| Phase ordering (1 first, 2-5 parallel, 6-8 sequential, 9 closes) | This PRD draft (derived from dependency analysis) |
| Non-node-ref ghost fields (HoleData::field_name, ClosureParamData::annotated_type) stay in Data | This PRD draft (refinement: `extra_children` is for node refs only) |
| `BinOp` deletion timing (Phase 1 if grep clean, else Phase 4) | This PRD draft (conservative: only delete in Phase 1 if no downstream consumers) |

---

## 12. Per-Phase Documentation Maintenance Protocol

Mirrors the correctness PRD's §11 protocol. Each phase PR must
include doc updates in the same commit (or follow-up commit in the
same PR). Three files are updated per phase: **this PRD**, the
**source audit doc**, and the **parallelism-blocker catalogue**.

### 12.1 PRD status-block edit template

Open the status block at the top of this file. Replace the
`NOT STARTED — 9 phases` line with a progress tally and append a
per-phase bullet:

```markdown
> **Status: IN PROGRESS — Phase 1 SHIPPED, 8 phases remaining.**
> Filed 2026-05-26 …
>
> - **Phase 1 (dead-code + catalogue) — SHIPPED.** Commit `<hash>`,
>   `<YYYY-MM-DD>`. See §4 Phase 1.
```

After the final phase:

```markdown
> **Status: DONE — All 9 phases shipped.** Filed 2026-05-26; closed
> <YYYY-MM-DD>. All audit hardening findings RESOLVED.
>
> - Phase 1 … `<hash>` `<date>`
> - Phase 2 … `<hash>` `<date>`
> - Phase 3 … `<hash>` `<date>`
> - Phase 4 … `<hash>` `<date>`
> - Phase 5 … `<hash>` `<date>`
> - Phase 6 … `<hash>` `<date>`
> - Phase 7 … `<hash>` `<date>`
> - Phase 8 … `<hash>` `<date>`
> - Phase 9 … `<hash>` `<date>`
```

### 12.2 Audit-doc edit template

Open `docs/audits/parser-codegen-interop_audit_2026-05-25.md`. For
each finding RESOLVED by a phase, two edits:

**Edit 1 — finding header in §2.** Append a resolved tag with commit
backlink:

```markdown
### F13. `shape_index` reimplements the front-end on every keystroke — *High*

> **RESOLVED <YYYY-MM-DD>** by Phase 8 of `prd-parser-codegen-hardening.md`,
> commit `<hash>`. shape_index now formats `CompileArtifacts` directly;
> the 478-LOC re-lex+re-parse pipeline is gone.
```

**Edit 2 — PRD-shortlist row in §5.** Mark the relevant PRD shipped
(or partially shipped if this PRD covers only part of it):

```markdown
### PRD-2 — `shape_index` shares the analyzer's AST  *(High)*

> **SHIPPED via `prd-parser-codegen-hardening.md` Phase 8**, commit
> `<hash>`, <YYYY-MM-DD>.
```

For phases that touch part of a larger audit PRD (e.g. Phase 4 ships
PRD-10's Pratt table unification but not the F7 fix, which was
withdrawn), use "partially shipped" language:

```markdown
### PRD-10 — Pratt operator table unification + `^` fix  *(High-recall, small)*

> **PARTIALLY SHIPPED.** Pratt table unification shipped via
> `prd-parser-codegen-hardening.md` Phase 4, commit `<hash>`,
> <YYYY-MM-DD>. F7 (`^` fix) was withdrawn during correctness PRD
> Phase 0 verification.
```

### 12.3 Parallelism-blocker catalogue maintenance

Every phase PR that resolves a catalogue entry flips it from the
"Open" section to the "Resolved" section, with the phase number +
commit hash. Phase 9 asserts the catalogue's "Open" section contains
exactly the PB-F5 / PRD-3 handover entry; no other open entries
allowed.

### 12.4 Finding ↔ Phase ↔ Audit shortlist map

| Finding / Source | Phase | Audit shortlist row |
|---|---|---|
| F6 (Pratt portion) | 4 | PRD-10 (Pratt portion shipped; F7 was withdrawn during correctness PRD Phase 0) |
| F11 (lex_primitives) | 5 | PRD-8 (lex_primitives portion shipped; F8 already in correctness PRD) |
| F13 (shape_index) | 8 | PRD-2 |
| F15 (const/pattern recognizers) | 7 | PRD-13 |
| §3.1 dead code | 1 | PRD-15 |
| §3.2 dead BinOp path | 1 or 4 | PRD-15 |
| §3.3 ghost fields | 6 | PRD-14 |
| §3.5 builtin re-inserts | 3 | PRD-12 |
| §3.5 debug JSON unconditional | 2 | PRD-11 (debug-JSON gate portion) |
| §3.5 6-arg compile() | 2 | PRD-11 (CompileOptions portion) |
| §4 parallelism prep | 1 + 9 | (new: this PRD's contribution) |

### 12.5 What NOT to edit

- Do **not** delete content from the audit doc — append RESOLVED tags
  in place.
- Do **not** flip the audit's executive-summary tally except at the
  very end (Phase 9) — and even then, only update the hardening-side
  tally, not the codegen-sprawl one (that's owned by the other PRD).
- Do **not** touch correctness PRD's status block or audit-doc edits.

---

## 13. Open Questions

None at filing. Section reserved for deferrals identified during
implementation:

- **[OPEN]** TBD by Phase 5 reviewer: should `CursorBase` be inherited
  or composed by `Lexer`/`MiniLexer`? Inheritance is the shorter
  diff; composition is the more orthogonal design. Either works; the
  PR picks one and documents the rationale.
- **[OPEN]** TBD by Phase 6 reviewer: should `extra_children[]` use
  `std::vector<NodeIndex>` (heap, 24 bytes overhead) or a small-buffer
  optimisation like `absl::InlinedVector<NodeIndex, 2>` (stack for
  ≤2, heap otherwise)? Vast majority of nodes have 0 extra children;
  most ghost-field-bearing nodes have exactly 1. Vector is simplest;
  SBO is a 30-LOC custom impl if profiling justifies. Default to
  vector for v1.

---

## 14. Follow-ups Unblocked by This PRD

Tracked in `docs/audits/parser-codegen-interop_audit_2026-05-25.md`
and `docs/parallelism-blocker-catalogue.md`:

- **Future PRD-3 (per-import front-end parallelism)** — Phase 9 of
  this PRD ships the precondition list. PRD-3's diff becomes the
  orchestration-only change in `akkado.cpp:90-137` + per-import
  fan-out: lex+parse each `ResolvedModule` on its own thread into
  per-module token/AST/diagnostic vectors; merge step re-indexes
  `NodeIndex` and concatenates `SourceMap` regions; AST cache keyed
  by `(import_path, file_mtime, content_hash)` for unchanged
  imports. Foundation entirely from this PRD: read-only AST
  (correctness PRD), per-compile state (correctness PRD), frozen
  builtin scope (Phase 3), thread-safe per-compile interner
  (correctness PRD Phase 5).
- **Future per-statement parallel codegen** — out of scope here;
  catalogued by Phase 1 + 9. Requires splitting `node_types_`'s
  dual role (cache vs channel) and fixing `apply_lambda` 's
  per-iteration thrash. Documented as a separate future PRD.
- **`prd-codegen-sprawl-cleanup.md`** — drafted alongside this PRD;
  covers F4 / F9 / F10 / viz + param families. Independent of this
  PRD; can land in parallel.
- **Future LSP** — `shape_index` (Phase 8) is the prototype for an
  LSP that consumes `CompileArtifacts`. LSP work becomes a thin
  layer over the same API.
