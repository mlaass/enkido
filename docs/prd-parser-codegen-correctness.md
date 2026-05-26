> **Status: DONE — All 5 phases shipped (F7 withdrawn during Phase 0).**
> Filed 2026-05-25; closed 2026-05-26. All 5 in-scope critical
> correctness findings (F1 / F2 / F8 / F12 / F14) from
> [`docs/audits/parser-codegen-interop_audit_2026-05-25.md`](audits/parser-codegen-interop_audit_2026-05-25.md)
> are resolved; F7 is marked withdrawn (Phase 0 verified `^` is already
> right-associative on master). Phases landed independently; the
> audit's complexity-sink findings remain deferred to separate PRDs.
>
> - **Phase 0 (snapshot harness) — SHIPPED 2026-05-25** (commit
>   `b203e2e`). Per-fixture bytecode-disassembly snapshot test at
>   `akkado/tests/test_bytecode_snapshot.cpp`, fixtures under
>   `akkado/tests/fixtures/`, baselines under `akkado/tests/snapshots/`.
>   Regen with `NKIDO_UPDATE_SNAPSHOTS=1 ./build/akkado/tests/akkado_tests "[snapshot]"`.
>   The bytecode-disassembly formatter was extracted into a
>   `nkido_bytecode_dump` static library so tests can link it without
>   pulling in the CLI. See §8 *Snapshot harness*.
> - **Phase 1b (mini-notation parse-at-parse-time + F3 tail) — SHIPPED
>   2026-05-25** (commit `ed8703a`). The four
>   `codegen_patterns.cpp` `const_cast<AstArena&>` re-parses are gone;
>   `grep -rn 'const_cast<AstArena' akkado/src/` now returns zero code
>   hits. `Node::data` gained a `MiniLiteralData` variant arm that
>   carries `mode_marker` + a `shared_ptr<AstArena>` sub-arena +
>   `mini_root` + pre-collected diagnostics; `Parser::parse_mini_literal`
>   parses into the sub-arena at parse time and stops stitching mini
>   nodes as main-arena children. Codegen's prefix-form path
>   (`handle_mini_literal`, `handle_pattern_reference`,
>   `handle_timeline_literal`, `compile_pattern_for_transform`'s
>   MiniLiteral case) reads `as_mini_literal().mini_arena`. The
>   call-form path (`chord("…")`, `timeline("…")`, transform with a
>   string-literal pattern arg) parses into a per-call codegen scratch
>   arena owned by `CodeGenerator::codegen_mini_arenas_`. `SequenceCompiler`
>   and `PatternEvaluator` hold the arena via a settable pointer (added
>   `set_arena(const AstArena&)`) so a single instance can compile a
>   sub-arena leaf while transform recursion walks the main arena.
>   `compile_pattern_for_transform` gained an `out_arena` outparam so
>   callers know which arena `out_pattern_node` lives in; the existing
>   `ast_->arena[pattern_node]` reads in transform handlers now go
>   through `(*pattern_arena)[pattern_node]`. F3 tail: the mini-parser's
>   `parse_sample_atom` now calls `parse_chord_symbol(sample.name)`
>   opportunistically and caches the result onto `MiniAtomData`'s
>   `chord_root` / `chord_quality` / `chord_intervals` / `chord_root_midi`
>   fields; `PatternEvaluator::eval_atom`'s chord-mode branch reads the
>   cached fields (empty `chord_root` ⇒ Rest, matching the legacy
>   parse-failure path) and the `akkado/chord_parser.hpp` include is
>   removed from `pattern_eval.cpp`. **Caveat / scope note:** the PRD's
>   strict "`grep parse_mini( akkado/src/` only hits parser-stage files"
>   criterion is not met for the Call-form / StringLit-pattern paths,
>   because those legitimately receive an arbitrary string-literal at
>   codegen time (the parser never sees them as a MiniLiteral). They
>   now parse into a codegen-owned scratch arena instead of mutating
>   `ast_->arena`, which fulfils F1's headline goal even though the
>   strict parse-once-store-once tally still has codegen-time parses.
>   Five `[F1b]` + three `[F3]` regression tests in
>   `akkado/tests/test_codegen.cpp` assert arena hash unchanged across
>   `generate()` for the chord prefix / chord call / timeline call /
>   transform-with-chord shapes, that the parser populates
>   `MiniLiteralData::mini_arena`, that Sample-kind atoms cache chord
>   fields for chord-shaped names and leave them empty for non-chord
>   names, and that `pattern_eval.cpp` no longer references
>   `parse_chord_symbol(`. Full akkado suite remains green (1072 cases,
>   141 958 assertions); snapshot harness reports byte-identical
>   bytecode across every fixture. See §4 Phase 1b.
> - **Phase 1a (drop `NodeType::PreResolved` + read-only spread-arg
>   reorder) — SHIPPED 2026-05-25** (commit `deda0c6`).
>   `NodeType::PreResolved` removed entirely; spread-resolved
>   `TypedValue`s now live in a side-table keyed by `(call_node,
>   slot_index)`. The two `const_cast<AstArena&>` sites in
>   `akkado/src/codegen.cpp` (spread expansion + named-arg reorder) are
>   gone. `reorder_spread_named_args` operates on the local
>   `ExpandedArg` vector and gap-fills with `is_underscore` entries; the
>   per-arg loop consumes a unified `CallSlot` vector (AstNode /
>   Resolved / Underscore) so spread and non-spread calls share one
>   iteration shape. Two downstream re-walks of `n.first_child`
>   (chord-expansion at codegen.cpp:1656, channel-type validation at
>   :1612) are gated on `!did_spread_swap` — pre-Phase-1a those walks
>   saw the synthesized PreResolved chain (which never triggered
>   multi-buffer fan-out or Identifier checks), post-Phase-1a they
>   would see the original spread Argument whose `first_child` is
>   `NULL_NODE`, so skipping preserves byte-identical behaviour. New
>   helper `akkado/include/akkado/ast_hash.hpp` provides
>   `arena_structural_hash()`; production-side assertion in
>   `generate()` is deferred to Phase 1b (the four
>   `codegen_patterns.cpp` const_casts still mutate). Two `[F1a]`
>   regression tests in `akkado/tests/test_codegen.cpp` assert
>   `arena.size()` and structural hash are unchanged across
>   `generate()` for the `04_spread_args.ak` + `05_named_args.ak`
>   shapes. See §4 Phase 1a.
> - **Phase 5 (F12 StringInterner + Token shape change) — SHIPPED
>   2026-05-26** (commit `<commit>`). New
>   `akkado::StringInterner` (`akkado/include/akkado/string_interner.hpp`,
>   `akkado/src/string_interner.cpp`) defines `SymbolId` (u32 sequential
>   id) + `NULL_SYMBOL = 0` + an open-keyed dedup map. The interner
>   owns its string storage (deviation from the PRD's
>   view-into-source design — required so `CompileResult.symbols->lookup()`
>   keeps working after `compile()` returns); the source-buffer
>   lifetime contract was relaxed accordingly. The interner is held by
>   `CompileContext::interner` (Phase 4's struct gained the field
>   alongside its existing `voicing_registry`).
>   `Token::TokenValue` variant: the `std::string` arm split into
>   `SymbolId` (for `Identifier` tokens, interned at lex time via
>   `Lexer::interner_`) and `StringLitData { std::string }` (for
>   `String`, `Directive`, `Error` tokens). `Token::as_string()` was
>   removed; new accessors `as_identifier() -> SymbolId` and
>   `as_string_lit() -> const std::string&`. Mirror change in
>   `MiniToken`: ctor now takes a `StringInterner&` for symmetry; the
>   variant itself didn't need a new arm because no MiniToken atom is
>   identifier-shaped.
>   `IdentifierData::name` changed from `std::string` to `SymbolId`;
>   `Node::as_identifier()` now returns `SymbolId`. Consumers across
>   the analyzer/codegen/const_eval/shape_index/pattern_eval surface
>   (~150 touch points) resolve to text via
>   `ctx_->interner->view(SymbolId)` where they need a string, or pass
>   the `SymbolId` straight into `SymbolTable::lookup(SymbolId)` for
>   the no-rehash fast path.
>   `SymbolTable`: internal `scopes_` map switched from FNV-hash key
>   to `SymbolId` key. `Symbol.name_hash` renamed to `Symbol.name_id`
>   (type `SymbolId`). `lookup(uint32_t)` overload removed; new
>   `lookup(SymbolId)` is the primary path. Default `SymbolTable()`
>   leaves the table empty (no builtins, no interner) for unit-test
>   compatibility; new ctor `SymbolTable(StringInterner&)` and method
>   `register_builtins(StringInterner&)` pre-seed builtins through
>   the interner. `fnv1a_hash()` calls inside `symbol_table.cpp` are
>   gone (the header declaration stays for Cedar runtime interop).
>   `SemanticAnalyzer` gained a ctor taking `StringInterner&`; the
>   analyzer's `analyze()` re-seeds builtins after its
>   `symbols_ = SymbolTable{}` reset so the interner stays attached
>   across multiple calls. `Lexer` / `Parser` / `lex()` / `parse()`
>   ctors+free fns take a `StringInterner&`. `CompileResult` gained
>   `owned_ctx` (`shared_ptr<CompileContext>`) so a `compile()` call
>   that constructs its own context keeps it alive for downstream
>   `result.symbols->lookup()` queries.
>   `shape_index_json` got its own local `StringInterner` (PRD-2
>   shared-AST work is the long-term replacement; for now shape_index
>   re-lexes/re-parses with a self-owned interner). `ConstEvaluator`
>   ctor took a `StringInterner&`; `extract_closure_info` helper in
>   `codegen/helpers.hpp` similarly. Tests: 7 `[F12]` regression
>   tests across `test_lexer.cpp` and `test_symbol_table.cpp` lock
>   the structural invariants (id dedup, NULL_SYMBOL semantics,
>   variant shape, lifetime). Plus a per-thread test helper
>   `test_lex_helper.hpp` wraps the two-arg `lex(...)` / `parse(...)`
>   shapes so test_lexer / test_parser / test_pattern_scalar /
>   test_mini_notation / test_analyzer keep compiling unchanged.
>   Perf bench (`/usr/bin/time -v` on a non-trivial 650-line input
>   `akkado/stdlib/scale_quantize.ak`, 3 trials each before/after):
>   user-time `0.34-0.35s → 0.32-0.33s` (~3-6% faster); minor page
>   faults `27521 → 27484` (-37, ~0.1%); Max RSS unchanged at
>   ~76 MB. Smaller `akkado/stdlib/event_transforms.ak` (77 lines):
>   user-time `0.17-0.18s → 0.16-0.17s`; page faults `13841 → 13806`.
>   The structural-correctness check (TokenValue no longer holds
>   std::string for identifier tokens; SymbolTable map keyed on
>   SymbolId; zero `fnv1a_hash` calls in symbol_table.cpp) is the
>   real exit criterion per PRD §4 Phase 5; the bench is
>   informational. Full akkado suite green (1088 cases / 142128
>   assertions); snapshot harness byte-identical. See §4 Phase 5.
> - **Phase 4 (F14 `voicing_registry` per-compile isolation) — SHIPPED
>   2026-05-26** (commit `7ef8a49`). New
>   `akkado::CompileContext` (`akkado/include/akkado/compile_context.hpp`,
>   `akkado/src/compile_context.cpp`) owns a `unique_ptr<voicing::VoicingRegistry>`
>   and gets passed to `compile()` / `compile_file()` as a final
>   defaulted `CompileContext* ctx = nullptr` argument. When the caller
>   doesn't supply one, `compile()` constructs a stack-local
>   `CompileContext` so the existing API stays additive. The new
>   `voicing::VoicingRegistry` class replaces the deleted process-global
>   `voicing_registry()` + `registry_mutex()` pair in `voicing.cpp` — no
>   compiler mutex remains, and each compile gets its own
>   `VoicingRegistry` pre-seeded with the four built-ins
>   (`close`/`open`/`drop2`/`drop3`). The free `voicing::lookup_voicing`
>   / `voicing::register_voicing` declarations were deleted from
>   `voicing.hpp` (no in-tree callers). `CodeGenerator` gained an
>   `explicit CodeGenerator(CompileContext&)` ctor (default ctor
>   deleted) plus a non-owning `CompileContext* ctx_` member; the four
>   `voicing::lookup_voicing` / `voicing::register_voicing` call sites
>   in `codegen_patterns.cpp` (`:4667`, `:4669`, `:4929`, `:5030` —
>   line numbers drifted slightly post-Phase-3 from the PRD's
>   originally-listed 4538/4540/4803/4904) now route through
>   `ctx_->voicing_registry->{lookup,define}`. The static
>   `apply_voicing` helper gained a `const voicing::VoicingRegistry&`
>   parameter so its three callers in `handle_anchor_call` /
>   `handle_mode_call` / `handle_voicing_call` can hand it the ctx's
>   registry. Three `[F14]` regression tests in `test_codegen.cpp`
>   cover: (a) fresh-ctx isolation — voicing defined in compile A is
>   not visible to a fresh ctx for compile B (E141 fires); (b)
>   shared-ctx persistence — same ctx across A+B keeps the voicing
>   visible (live-coding workflow); (c) built-ins resolve in every
>   fresh ctx. Full akkado suite green; snapshot harness
>   byte-identical. See §4 Phase 4.
> - **Phase 3 (F2 source-location emit consolidation) — SHIPPED
>   2026-05-26** (commit `<commit>`). Six free-function emit helpers
>   (`codegen::emit_push_const`, `codegen::emit_zero`,
>   `codegen::emit_midi_to_freq`, `codegen::finalize_array_result`, plus
>   the file-local statics `emit_binary_op` in `codegen_arrays.cpp` and
>   `emit_pattern_with_state`/`emit_instruction_helper` in
>   `codegen_patterns.cpp`) are gone or rewired to route every emission
>   through `CodeGenerator::emit()` — the single push site that touches
>   both `instructions_` and `source_locations_` atomically (and honours
>   the FOREACH_EVENT subprogram body detour). The four scope-creep
>   discoveries beyond the PRD's named files: `emit_midi_to_freq`
>   (1 site, also F2-buggy), the static `emit_binary_op` (13 sites, same
>   bug), the static `emit_pattern_with_state` + `emit_instruction_helper`
>   thunk (8 sites, function-pointer-driven so we dropped the
>   `instructions/emit_fn` params and route through `gen.emit()`), and
>   `handle_fast_call/handle_slow_call`'s mid-stream EVENT_RATE_SCALE
>   insert (needed a paired `loc_stream()` insert to preserve parity at
>   the non-tail position). Three method helpers landed:
>   `CodeGenerator::emit_push_const(float)`,
>   `CodeGenerator::emit_zero()`, `CodeGenerator::emit_midi_to_freq(float)`,
>   `CodeGenerator::finalize_array_result(node, buffers)`; the static
>   `finalize_result` duplicate in `codegen_arrays.cpp` was deleted and
>   its 14 callers point at the method instead. The 7 manual
>   `source_locations_.push_back(...)` compensations (4 in `codegen.cpp`
>   / `codegen_functions.cpp` pushing `n.location`; 3 in
>   `codegen_patterns.cpp` pushing `current_source_loc_`) are gone —
>   `emit()` writes the location once via `current_source_loc_`, which
>   `visit()` sets to `n.location` at the top of every node visit. A new
>   `loc_stream()` accessor parallels `emit_stream()` for the one
>   non-tail insert case. `emit()` moved from `private:` to `public:` so
>   the remaining file-local static helpers (`emit_binary_op`,
>   `emit_pattern_with_state`) can call it directly without per-helper
>   friend declarations. Debug `assert()` in `generate()`'s epilogue
>   enforces `instructions_.size() == source_locations_.size()` (main
>   stream) and per-subprogram body parity — silent regression coverage
>   across every codegen test in debug builds. Four `[F2]` regression
>   tests in `akkado/tests/test_codegen.cpp` assert the parity invariant
>   independently against `CompileResult.bytecode` /
>   `CompileResult.source_locations` so it's checked in `NDEBUG` builds
>   too: a single-fixture parity check, a sweep across every
>   `akkado/tests/fixtures/*.ak`, a multi-`PUSH_CONST` shape (NumberLit +
>   array-const path), and the EVENT_RATE_SCALE mid-stream-insert path
>   from `slow()`. Full akkado suite: 1079 cases / 142080 assertions, all
>   green. Snapshot harness byte-identical (snapshot disasm carries
>   opcode/buffer/immediate info only — it would not catch source-loc
>   drift on its own, hence the parity assert and the explicit `[F2]`
>   tests). See §4 Phase 3.
> - **Phase 2 (F8 mini-lexer line tracking + F7 lock-in tests) — SHIPPED
>   2026-05-26** (commit `<commit>`). MiniLexer now tracks `line_` across
>   `\n` (incl. `\r\n`) and snapshots `start_line_` / `start_column_` at
>   token start in `lex_token()`. `current_location()` reports the
>   token-start line/column; line 1 still adds `base_location_.column`
>   so single-line callers see byte-identical output (since `column_`
>   bumps once per non-`\n` char and `start_column_ - 1 == start_` on
>   line 1). Line 2+ reports the pattern-relative column (we don't know
>   the source-file indentation of continuation lines). The PRD's strict
>   "diagnostic on c reports line 2" exit criterion lands via six `[F8]`
>   regression tests in `akkado/tests/test_mini_notation.cpp` covering
>   default base_location, non-trivial base_location, `\r\n` endings,
>   trailing `\n`, and an error-token path through `make_error_token →
>   current_location()`. F7 ships **regression tests only** — Phase 0
>   already verified `^` is right-assoc. Four `[F7]` AST-structure tests
>   in `akkado/tests/test_parser.cpp` (right-nested pow tower, `-2 ^ 2`
>   via lexer's negative-number fusion, `x ^ -1`) plus two `[F7]`
>   const-eval tests in `akkado/tests/test_const_eval.cpp` (`2^3^2 ==
>   512`, `2^2^2^2 == 65536`). A clarifying multi-line comment was added
>   above the no-op `static_cast<Precedence>(static_cast<int>(p))` at
>   `parser.cpp:1459` explaining the intentional Pratt mechanism, with a
>   backreference to §1.3. The stale Phase-2-prediction comment in
>   `akkado/tests/fixtures/06_power_op.ak` was rewritten to reflect the
>   Phase 0 finding; the `.disasm` snapshot is byte-identical. Full
>   akkado suite green; snapshot harness reports byte-identical bytecode
>   across every fixture. See §4 Phase 2.
> - **F7 (right-assoc `^`) — WITHDRAWN 2026-05-25.** Phase 0's
>   `06_power_op.ak` fixture compiled `2^3^2 * 100` and snapshotted POW
>   instructions emitted in right-assoc order — outer `POW(2, POW(3,2))`
>   = 512, not `POW(POW(2,3), 2)` = 64. The PRD/audit's Pratt analysis
>   at §1.3 was flawed: the left-assoc branch passes `p+1` while the
>   right-assoc branch passes `p`, and `parse_precedence(p)` happily
>   accepts another `^` (since `p ≥ p`), giving real right-assoc. No
>   code change needed. Phase 2 (above) shipped only a regression test
>   (`2^3^2 == 512` + AST structure) to lock current behavior. See §1.3,
>   §4 Phase 2, §11.3.
>
> **Per-phase documentation protocol (mandatory).** On completion of
> each phase, the implementing PR must also (a) update this PRD's
> status block to mark the phase as `SHIPPED` with the commit hash and
> date, and (b) update the source audit doc to mark the corresponding
> finding (F1a/F1b/F2/F7/F8/F12/F14) as resolved with a backlink to
> the commit. See §11 for the exact edit template.

# PRD: Parser/Codegen Correctness Bundle

## Executive Summary

The parser/codegen interop audit (2026-05-25) surfaced **6 critical
findings**. Phase 0 verification reversed one (**F7** — `^`
right-associativity — is already correct on master; see §1.3) leaving
**5 critical findings** that produce wrong outputs today or break
architectural invariants the rest of the codebase relies on. This PRD
addresses all five in a coordinated rollout. Each finding becomes one
phase, shipped as an independent PR after Phase 1a unblocks the rest.
The audit's complexity-sink findings (codegen sprawl, dispatcher
fragmentation, pattern-transform boilerplate) are explicitly **out of
scope** here and covered by separate PRDs.

One adjacent **High**-severity finding (**F3** — mini-notation re-parsed
up to 5×) shares its mechanism with F1: 4 of F3's 5 re-parse sites are
the same `const_cast<AstArena&>` re-parses Phase 1b removes. The 5th
site (`pattern_eval.cpp:206`, chord-symbol re-parse at evaluation time)
is folded into Phase 1b's scope so the audit's "parse-once-store-once"
headline lands in one phase rather than leaving a one-day tail.

The findings:

| # | Finding | Severity | Phase |
|---|---|---|---|
| F1 | Codegen mutates the post-parse AST (5 sites) | Critical | 1a + 1b |
| F2 | Source-location vector silently desynchronises | Critical | 3 |
| F7 | Right-associative `^` parses left-associative | ~~Critical~~ **WITHDRAWN** (Phase 0 verified `2^3^2 → 512` already; regression test only in Phase 2) | 2 (test only) |
| F8 | Mini-lexer never bumps `line_` across `\n` | ~~Critical~~ **RESOLVED** (Phase 2, 2026-05-26) | 2 |
| F14 | `voicing_registry` leaks state across compiles | Critical | 4 |
| F12 | Lexers don't intern strings (16× rehash per compile) | Critical | 5 |
| F3 | Mini-notation re-parsed up to 5× per string | High (tail) | 1b |

**Key Design Decisions** (locked — see §10 for sourcing):

- **Phase 0 first (snapshot harness), then Phase 1a; rest parallelizes.**
  Phase 0 lands a per-fixture bytecode-disassembly snapshot test so every
  subsequent phase's "byte-identical bytecode" exit criterion is
  mechanically checked. Phase 1a (the codegen AST-mutation refactor)
  unblocks `shape_index` and future parallel pass work. Phases 2–5 are
  independent and may land in any order once Phase 1a is in.
- **`PreResolved` node kind removed entirely; side-table keyed by
  `(call_node, arg_position)`.** No synthetic AST nodes are minted at
  any stage. `expand_call_arguments` returns a flat `ExpandedArg` list;
  the Call branch in `visit()` zips it against `pre_resolved_values_`
  (a `std::unordered_map<std::pair<NodeIndex,int>, TypedValue>`). No
  auxiliary arena is needed. `reorder_spread_named_args` is rewritten to
  produce an `ArgInfo` vector + a separate "slot lookup" path instead of
  mutating `arena[call_node].first_child` in place.
- **New `MiniLiteralData` variant arm on `Node::data`.** Today
  `parser.cpp:1759` sets `arena_[node].data = Node::StringData{mode_marker}`
  on a `NodeType::MiniLiteral` node and adds the parsed mini-AST as a
  child via `arena_.add_child(...)`. Phase 1b introduces a dedicated
  `MiniLiteralData { std::string mode_marker; std::unique_ptr<AstArena>
  mini_arena; NodeIndex mini_root; std::vector<Diagnostic> mini_diagnostics; }`
  and stops stitching mini nodes as main-arena children. The 4
  `const_cast<AstArena&>` codegen-time re-parses are deleted. Sub-arena
  destroyed with the literal; thread-local during future parallel
  mini-parse.
- **Sample-atom chord caching at parse time (F3 tail).** Phase 1b also
  closes F3's 5th re-parse site (`pattern_eval.cpp:206`). When the
  mini-parser builds a `Sample`-kind atom, it opportunistically calls
  `parse_chord_symbol(sample.name)` and, on success, writes the result
  into the atom's existing `chord_root` / `chord_quality` /
  `chord_intervals` / `chord_root_midi` fields. `PatternEvaluator`'s
  chord-mode branch reads those fields directly instead of re-parsing
  the string. Failure path is unchanged (empty chord fields → treated
  as Rest). After Phase 1b, `parse_chord_symbol` is invoked at parse
  time only and `MiniAtomData`'s chord fields become the single source
  of truth, satisfying the audit's "parse-once-store-once" goal.
- **Codegen emit helpers become `CodeGenerator&` methods.** The free
  `codegen::emit_push_const(buffers_, stream_, val)` shape is deleted;
  the method `cg.emit_push_const(val)` routes through `emit()` which
  unconditionally writes both `instructions_` AND `source_locations_`.
  The bug becomes structurally impossible to reintroduce.
- **`^` is already right-associative.** Phase 0 empirically confirmed
  `2^3^2 == 512` (commit `b203e2e`; snapshot `06_power_op.disasm` shows
  outer `POW(2, POW(3,2))`). The audit's claim that the no-op cast at
  `parser.cpp:1455` produces left-assoc was wrong: the right-assoc
  branch passes `p` while the left-assoc branch passes `p+1`, and
  `parse_precedence(p)` accepts another `^` since `p ≥ p`. Phase 2
  ships only a regression test to lock the behavior; no code change.
- **Minimal `CompileContext` introduced now.** Holds `VoicingRegistry`
  and `StringInterner` only. `SampleRegistry` / `FileResolver` /
  `lint_strict` / `bypass_master` migration is **deferred to a future
  PRD-11** (`compile()` options consolidation). Public API: optional
  parameter on `compile()`, defaults to a fresh per-call context.
- **String interning at lex time, view-into-source.** Per-compile
  `StringInterner` owned by `CompileContext`. Interned strings are
  `string_view`s into the source buffer plus a stable `SymbolId(u32)`.
  No string copies anywhere. `TokenValue::string` arm is **replaced** by
  `SymbolId` for all identifier-like uses; string literals keep their
  own variant arm.
- **Per-bug regression tests + cross-cutting invariants.** Each phase
  ships a precise regression test (e.g. `2^3^2 == 512` for F7,
  multi-line mini-pattern positions for F8). Plus two cross-cutting
  invariants asserted in `generate()`:
  1. `instructions_.size() == source_locations_.size()`
  2. After codegen, a **structural hash** of `Ast::arena` is unchanged
     from pre-codegen (asserts no codegen-side mutation crept back in).
     The hash is computed by an explicit post-order traversal that
     reads each `Node::data` variant arm's named fields and feeds them
     into FNV-1a — *not* a naive `memcpy` (the AST contains
     `std::string` and `std::variant` members whose padding is
     unspecified).

---

## 1. Problem Statement / Current State Inventory

### 1.1 F1 — Codegen mutates the post-parse AST

| Site | File:line | What it does |
|---|---|---|
| PreResolved alloc | `codegen.cpp:1008,1018` | `expand_call_arguments` takes `AstArena& arena = const_cast<AstArena&>(ast_->arena);` then calls `arena.alloc(NodeType::PreResolved, ea.loc)` to write a synthetic node into the analyzer's `output_arena_` during spread expansion. Value already lives in side table `pre_resolved_values_` (`codegen.hpp:1194`). |
| Spread arg reordering | `codegen.cpp:3233` | `reorder_spread_named_args` takes `AstArena& arena = const_cast<AstArena&>(ast_->arena);` and rewrites `arena[call_node].first_child` + sibling chains in-place to put named args in slot order. |
| Chord re-parse | `codegen_patterns.cpp:1778` | `parse_mini(chord_str, const_cast<AstArena&>(ast_->arena), …)` parses chord string into the same arena codegen is reading from. |
| Generic pattern arg re-parse | `codegen_patterns.cpp:2133` | Same `const_cast` pattern for any string-literal pattern argument. |
| Chord-in-transform re-parse | `codegen_patterns.cpp:2183` | Same pattern, inside pattern transforms. |
| Timeline curve re-parse | `codegen_patterns.cpp:2983` | Same pattern, for `timeline(t"…")` curve strings. |

Total: **6** `const_cast<AstArena&>` sites in `akkado/src/` today. Phase 1a addresses the two `codegen.cpp` sites (spread expansion + spread reorder); Phase 1b addresses the four `codegen_patterns.cpp` re-parses.

Why it bites:

- `shape_index` cannot share the main compile's AST because it might be
  mid-mutation — instead it re-lexes + re-parses on every editor cursor
  move (`shape_index.cpp:429-435` does the re-lex/parse;
  `shape_index.cpp` is 478 LOC total of duplicate pipeline glue around
  it).
- Pattern evaluators cannot run in parallel across patterns because the
  arena they read could be growing.
- The future LSP / autocomplete cannot hand the analyzer's AST to
  multiple inspection clients without copying.
- Arena indices stored in side tables become invalid mid-codegen if the
  arena resizes (`std::vector` reallocation).

### 1.2 F2 — Source-location vector silently desynchronises

`CodeGenerator` maintains two parallel vectors:

- `instructions_` — `std::vector<cedar::Instruction>`
- `source_locations_` — `std::vector<SourceLocation>`

The single push site that keeps them in sync is `CodeGenerator::emit()`
at `codegen.cpp:2272`. But the free-function helpers
`codegen::emit_push_const` and `codegen::emit_zero` (`codegen/helpers.hpp:31`)
push directly to the instruction vector without touching
`source_locations_`. Callers must manually compensate. Audit found 12
sites that **don't** compensate; only 3 do:

| File | Lines that miss the compensation |
|---|---|
| `codegen_patterns.cpp` | 3158, 3181, 4262, 4338, 4424, 4464, 4499, 5096, 5156, 5263 |
| `codegen_higher_order.cpp` | 699 |
| `codegen.cpp` | 320, 901, 923 |
| `codegen_arrays.cpp` | (sites inside) |

Each missed compensation shifts the parallel arrays by one entry **from
that point forward**. Click-to-source in the web IDE and `--trace` in
`nkido-cli` will misattribute every subsequent instruction. No existing
test asserts vector-length parity, so the bug is silent.

### 1.3 F7 — Right-associative `^` — **WITHDRAWN 2026-05-25**

**Status:** withdrawn during Phase 0 verification (commit `b203e2e`).
`^` is already right-associative on master. Phase 2 ships only a
regression test (`2^3^2 == 512`) to lock the behavior. No parser code
change.

The site is `parser.cpp:1455-1463`:

```cpp
// For right-associative (^), use lower precedence
Precedence next_prec = get_precedence(op.type);
if (op.type == TokenType::Caret) {
    // Power is right-associative
    next_prec = static_cast<Precedence>(static_cast<int>(next_prec));   // ← no-op cast, intentional
} else {
    // Left-associative: increment to bind tighter on right
    next_prec = static_cast<Precedence>(static_cast<int>(next_prec) + 1);
}
```

The audit (and an earlier draft of this PRD) read the no-op cast as
"effectively identical to the left-assoc branch" and concluded `^`
binds left. That reading was wrong. The two branches **do** differ:
the right-assoc branch passes `p`, the left-assoc branch passes
`p + 1`. Inside `parse_precedence(min)`, the next-operator loop
condition is `prec(op) >= min`. For an upcoming `^` at precedence `p`
called with `min = p` (right-assoc), `p >= p` holds, so the recursive
call binds the next `^` — giving `a ^ (b ^ c)`. Called with `min = p+1`
(left-assoc), `p >= p+1` fails, so the recursive call returns just `b`
and the outer loop binds the next `^` — giving `(a ^ b) ^ c`. Standard
Pratt; the code is correct.

Observable: `2 ^ 3 ^ 2` evaluates to `2^(3^2) = 512` — matches Python,
Haskell, mathematical convention. Verified by Phase 0's
`06_power_op.ak` snapshot:

```
PUSH_CONST  buf[0] = 2.000
PUSH_CONST  buf[1] = 3.000
PUSH_CONST  buf[2] = 2.000
POW         buf[3] <- buf[1], buf[2]   // 3^2 = 9
POW         buf[4] <- buf[0], buf[3]   // 2^9 = 512
```

The audit doc and the audit's PRD-shortlist row for F7 should be
updated to mark the finding withdrawn rather than resolved (no fix
shipped; the premise was wrong).

### 1.4 F8 — Mini-lexer multi-line source positions are broken

Site: `mini_lexer.cpp:73-77`:

```cpp
char MiniLexer::advance() {
    char c = pattern_[current_++];
    column_++;   // ← never bumps line_, never resets column_ on '\n'
    return c;
}
```

And `mini_lexer.cpp:146-153`:

```cpp
SourceLocation MiniLexer::current_location() const {
    return {
        .line = base_location_.line,                       // ← frozen
        .column = base_location_.column + start_,          // ← wrong for \n
        .offset = base_location_.offset + start_,
        .length = current_ - start_
    };
}
```

`lexer.cpp:393` deliberately allows multi-line mini-notation strings.
Any diagnostic emitted from inside such a pattern reports the wrong
line, and once `start_` exceeds the line break, `column` is a meaningless
offset.

### 1.5 F14 — `voicing_registry` leaks state across compiles

Site: `voicing.cpp:13-30`:

```cpp
std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, VoicingDict>& voicing_registry() {
    static std::unordered_map<std::string, VoicingDict> reg = [&]() {
        // … seed with close/open/drop2/drop3 …
    }();
    return reg;
}
```

Mutated by `voicing::register_voicing` (`voicing.cpp:300`), called from
codegen's `handle_add_voicings_call` handler at `codegen_patterns.cpp:4904`.
Read by `voicing::lookup_voicing` (`voicing.cpp:292`), called from three
sites in `codegen_patterns.cpp`: `:4538` (resolves the user-named dict
in `apply_voicing`), `:4540` (built-in `"close"` fallback when no dict
named or named-dict lookup returned null), and `:4803` (membership
check in `handle_voicing_call`, gates the E141 "dictionary not
registered" diagnostic). Phase 4 must route all four registry touches
through the new `VoicingRegistry`. The rest of the `voicing::*`
namespace in `codegen_patterns.cpp` (`parse_anchor`, `parse_mode`,
`voice_chords`, plus the `Mode` / `ChordSpec` / `VoicingDict` types) is
pure functions / types and needs no rewire. The audit's "17 sites all
in one file" line at `audit:313` conflated all `voicing::` namespace
mentions with registry touches; the real registry surface is 4 call
sites. Two problems:

1. **Cross-compile state leak.** A user-defined voicing in source A
   persists into the next compile of source B. Survives full
   `CodeGenerator` reset.
2. **Only mutex in the compiler.** Defeats lock-free per-compile
   parallelism, which the future per-import-parallel front-end (audit
   F5 / PRD-3) needs.

### 1.6 F12 — Lexers don't intern strings

`grep -rn 'Symbol\|symbol_table' akkado/src/lexer.cpp akkado/src/mini_lexer.cpp`
returns nothing. The lexers produce `std::string` payloads
(`lexer.cpp:464`). Each identifier:

1. allocates in `Token.value` as `std::string` (`lexer.cpp:464`)
2. is also stored as `lexeme` view (`lexer.cpp:113`)
3. is copied into AST `IdentifierData{std::string}` (`parser.cpp:513`
   and 14+ other sites)
4. is copied again into `Symbol.name` (`symbol_table.cpp:32`)
5. is FNV-hashed on every `lookup` (`symbol_table.cpp:119` and 15+
   sites)

Hot identifiers like `freq`, `gate`, `vel` get rehashed dozens of times
per compile. The architecture overview in `CLAUDE.md` claims "String
interning with FNV-1a hashing for fast identifier comparison" — the
actual lexer ignores it.

### 1.7 F3 — Mini-notation re-parsed up to 5× per string (tail folded into Phase 1b)

The audit (§F3) enumerates 6 invocation sites for mini-notation /
chord-symbol parsing (1 correct + 5 redundant). Phase 1b kills all 5
redundant ones:

| # | Site | What it does |
|---|---|---|
| 1 | `parser.cpp:1744` | Initial parse during `parse_mini_literal`. **Correct.** |
| 2 | `codegen_patterns.cpp:1778` | Codegen-time chord re-parse via `const_cast`. Killed by Phase 1b sub-arena. |
| 3 | `codegen_patterns.cpp:2133` | Codegen-time generic pattern-arg re-parse via `const_cast`. Killed by Phase 1b. |
| 4 | `codegen_patterns.cpp:2183` | Codegen-time chord-in-transform re-parse via `const_cast`. Killed by Phase 1b. |
| 5 | `codegen_patterns.cpp:2983` | Codegen-time timeline-curve re-parse via `const_cast`. Killed by Phase 1b. |
| 6 | `pattern_eval.cpp:206` | `PatternEvaluator` re-parses chord symbols at evaluation time, bypassing `MiniAtomData`'s already-allocated chord fields. **Killed by Phase 1b sample-atom caching.** |

Mechanism at site 6: when `chord_mode_` is on and the atom is
`MiniAtomKind::Sample`, `evaluate_atom` calls
`parse_chord_symbol(atom_data.sample_name)` to convert a sample-shaped
token (e.g. `"Am7"` lexed as a sample) into a chord. The `MiniAtomData`
struct *already* has `chord_root` / `chord_quality` / `chord_intervals`
/ `chord_root_midi` fields populated by the mini-parser's
`parse_chord_atom` path (`mini_parser.cpp:332-352`) — but only for
chord-token atoms, not sample-token atoms.

Why it bites: a single failure cascades into different diagnostics
depending on which layer caught it (audit F3 mechanism). After Phase
1b's Sample-atom caching, the mini-parser is the single source of
truth for chord parsing; pattern_eval and codegen both read fields.

Phase 1b scope addition: the mini-parser's `parse_sample_atom`
(`mini_parser.cpp:309-330`) attempts `parse_chord_symbol(sample.name)`
opportunistically and writes the result (or empty fallback) into the
new atom's chord fields. `PatternEvaluator::evaluate_atom`
(`pattern_eval.cpp:197-224`) drops the `parse_chord_symbol(...)` call
and reads from `atom_data.chord_root` / `chord_quality` /
`chord_intervals` / `chord_root_midi` instead, treating empty
`chord_root` as the parse-failed case.

---

## 2. Goals and Non-Goals

### Goals

1. **Eliminate every codegen-side write to `Ast::arena`.** After this
   PRD, `Ast::arena` is `const` in every post-analyzer signature.
2. **Make `instructions_.size() == source_locations_.size()`
   structurally true.** A single `emit()` path; helpers route through
   it. The invariant is `assert()`-ed at the end of `generate()`.
3. **`2^3^2 == 512` stays true.** Right-associative `^` already
   behaves correctly on master (verified in Phase 0); Phase 2 ships a
   regression test so it can't silently regress.
4. **Multi-line mini-notation patterns report correct line/column** in
   every diagnostic.
5. **No cross-compile state in compiler globals.** `voicing_registry`
   moves into a per-compile context. Compiler globals reduce to
   read-only metadata (`BUILTIN_FUNCTIONS`, `CHORD_INTERVALS`, etc).
6. **Per-compile string interner.** Every identifier-bearing token
   carries a `SymbolId(u32)`, not a `std::string`. Lookup is O(1) with
   no rehash. Identifier copies through the parser/analyzer chain go
   away.
7. **Parse-once-store-once for chord and mini-notation strings.**
   `parse_chord_symbol` is invoked at parse time only;
   `MiniAtomData`'s chord fields are authoritative for every downstream
   consumer (codegen, pattern_eval). `parse_mini` is invoked from
   `Parser::parse_mini_literal` only; the sub-arena handle on
   `MiniLiteralData` is authoritative for every downstream consumer.
   After this PRD, `grep -n 'parse_chord_symbol\|parse_mini(' akkado/src/`
   shows zero hits outside parser-stage code.
8. **Every fixed bug has a precise regression test** + the two
   cross-cutting invariants are asserted in production code.

### Non-Goals

- **Codegen file split** (audit F4 / PRD-4) — separate PRD.
- **Pattern-transform handler consolidation** (audit F9 / PRD-6) —
  separate PRD.
- **`StateInitBuilder` / `InstructionBuilder`** (audit F10 + dispatcher
  cleanup) — separate PRD.
- **`shape_index` consuming shared AST** (audit F13 / PRD-2) — separate
  PRD; unblocked by Phase 1.
- **Per-import front-end parallelism** (audit F5 / PRD-3) — separate
  PRD; unblocked by Phase 4 (no compiler mutex) and Phase 5 (concurrent-
  ready interner API).
- **Full `CompileOptions` migration** (audit F11) — separate PRD;
  `CompileContext` introduced here holds only `VoicingRegistry` +
  `StringInterner`. Sample/file/lint args stay on `compile()`.
- **`Token::TokenValue::string` arm for string literals** stays. Only
  the identifier-like uses migrate to `SymbolId`.
- **Backward compatibility shims.** The `compile()` API gains an
  optional `CompileContext*` parameter (default `nullptr` → fresh
  context). Old callers compile unchanged. `Token::as_string()` is
  rewritten to require an interner reference — this is a breaking
  change for in-tree consumers but there are no external API consumers.

---

## 3. Architecture / Technical Design

### 3.1 `CompileContext`

New struct in a new header `akkado/include/akkado/compile_context.hpp`:

```cpp
namespace akkado {

class StringInterner;    // forward decl, defined per-section 3.3
class VoicingRegistry;   // forward decl, defined per-section 3.4

struct CompileContext {
    std::unique_ptr<StringInterner>   interner;        // owned
    std::unique_ptr<VoicingRegistry>  voicing_registry; // owned

    CompileContext();   // creates fresh interner + registry
    ~CompileContext();
    CompileContext(const CompileContext&) = delete;
    CompileContext& operator=(const CompileContext&) = delete;
    CompileContext(CompileContext&&) noexcept = default;
    CompileContext& operator=(CompileContext&&) noexcept = default;
};

} // namespace akkado
```

Public `compile()` API change (additive, defaulted):

```cpp
CompileResult compile(std::string_view source,
                     std::string_view filename = "<input>",
                     SampleRegistry* sample_registry = nullptr,
                     const FileResolver* resolver = nullptr,
                     bool lint_strict = false,
                     bool bypass_master = false,
                     CompileContext* ctx = nullptr);   // ← new, optional
```

Behavior when `ctx == nullptr`: `compile()` constructs a fresh
`CompileContext` on the stack and uses it for the call. Behavior when
`ctx != nullptr`: caller-owned, can be reused across compiles
(voicings/interned strings persist intentionally).

### 3.2 `Ast::arena` immutability

Phase 1a removes the two mutating call sites in `codegen.cpp`:

1. **`expand_call_arguments` at `codegen.cpp:1008,1018`.** Drop the
   `const_cast<AstArena&>(ast_->arena)` and stop allocating
   `NodeType::PreResolved` nodes entirely. Replacement: the function
   returns its existing `ExpandedArg` list directly to the caller; the
   `pre_resolved_values_` side table is rekeyed from `NodeIndex` to
   `std::pair<NodeIndex, int>` where the pair is `(call_node, arg_position)`.
   The Call branch in `visit()` looks up `(node, i)` in
   `pre_resolved_values_` before falling through to the generic
   per-child visit. **`NodeType::PreResolved` is removed from
   `NodeType`.** No synthetic AST nodes are minted anywhere.
2. **`reorder_spread_named_args` at `codegen.cpp:3233`.** Today it
   `const_cast`s the arena and rewrites `arena[call_node].first_child`
   and sibling chains in place to put named args in slot order.
   Replacement: it constructs the slot-ordered `ArgInfo` vector as a
   local and the Call visit path iterates that vector instead of
   re-walking `arena[call_node].first_child`. The arena is read-only.

Phase 1b removes the 4 `const_cast<AstArena&>` sites in
`codegen_patterns.cpp` by parsing mini-notation strings into a per-
literal sub-arena at parse time. Today the parser sets
`arena_[node].data = Node::StringData{mode_marker}` on the MiniLiteral
node (`parser.cpp:1759`) and stitches the parsed mini-AST as a child
via `arena_.add_child(node, pattern_ast)` (`parser.cpp:1754`) — there is
**no `MiniLiteralData` struct today**, and the raw pattern string is
never stored on the node.

Phase 1b introduces a new variant arm:

```cpp
// Added to Node::data variant in ast.hpp
struct MiniLiteralData {
    std::string mode_marker;                      // replaces StringData usage
    std::unique_ptr<AstArena> mini_arena;         // owns the parsed sub-AST
    NodeIndex   mini_root = NULL_NODE;            // index into mini_arena
    std::vector<Diagnostic> mini_diagnostics;     // pre-collected by parser
};
```

The parser at `parser.cpp:1705-1762` changes to:

1. Construct a fresh `AstArena` for the literal.
2. Parse `pattern_str` into that sub-arena, capturing root index +
   diagnostics.
3. Set `arena_[node].data = Node::MiniLiteralData{...}` (replaces the
   existing `StringData` assignment at line 1759).
4. **Stop calling `arena_.add_child(node, pattern_ast)`** — mini nodes
   live in the sub-arena, not as main-arena children.

Diagnostics buffered in `mini_diagnostics` are merged into the main
parser's diagnostic list at the end of `Parser::parse()` (no UX change).

Codegen reads `MiniLiteralData::mini_arena` + `mini_root` instead of
re-parsing. The chord / transform / timeline call sites all read the
sub-arena handle.

After this PRD, every codegen function signature that takes an `Ast&`
becomes `const Ast&`.

### 3.3 `StringInterner`

New class in `akkado/include/akkado/string_interner.hpp`:

```cpp
namespace akkado {

using SymbolId = std::uint32_t;
constexpr SymbolId NULL_SYMBOL = 0;

class StringInterner {
public:
    StringInterner();

    /// Intern a string-view. View MUST outlive the returned SymbolId
    /// (caller responsibility). Returns NULL_SYMBOL only for empty input.
    SymbolId intern(std::string_view sv);

    /// O(1) resolve back to the originally-interned view.
    std::string_view view(SymbolId id) const;

    /// O(1) cached FNV-1a hash (set at intern time).
    std::uint32_t hash(SymbolId id) const;

    std::size_t size() const noexcept;

private:
    // open-addressing hash table; entries store {hash, view, id}.
    // view is `string_view` into caller-owned source; no copies.
};

} // namespace akkado
```

**Lifetime contract.** Interned views are non-owning — they reference
the input source buffer. `CompileContext` is destroyed before the source
buffer; if a caller wants cross-compile interning, the caller must own
the source strings for the interner's lifetime.

**Concurrency.** v1 is single-threaded (no mutex). The API is shaped so
a future `intern()` can become lock-free (open-addressing CAS) without
changing call-site code. This unblocks future per-import parallel
lexing without re-renovating the interner.

### 3.4 `VoicingRegistry`

Existing globals in `voicing.cpp:13-30` migrate to:

```cpp
class VoicingRegistry {
public:
    VoicingRegistry();   // pre-seeds close/open/drop2/drop3

    void define(std::string_view name, VoicingDict dict);
    const VoicingDict* lookup(std::string_view name) const;

private:
    std::unordered_map<std::string, VoicingDict> reg_;
};
```

No mutex. Owner is `CompileContext`. The free functions
`voicing::register_voicing(name, dict)` / `voicing::lookup_voicing(name)`
(declared in `voicing.hpp:72,77`, implemented at `voicing.cpp:292,300`)
become methods on `VoicingRegistry`. The four registry call sites in
`codegen_patterns.cpp` route through `ctx_->voicing_registry->…`:

- `:4538` — `voicing::lookup_voicing(compiler.voicing_dict_name())` in
  `apply_voicing`
- `:4540` — `voicing::lookup_voicing("close")` (built-in fallback) in
  `apply_voicing`
- `:4803` — `voicing::lookup_voicing(*name_str)` (membership check) in
  `handle_voicing_call`
- `:4904` — `voicing::register_voicing(*name_str, std::move(dict))` in
  `handle_add_voicings_call`

The free-function symbols are kept as thin wrappers around a
process-default registry only if any out-of-tree caller needs them —
`grep` confirms no in-tree caller does, so they are deleted in Phase 4.
The other `voicing::*` references in `codegen_patterns.cpp`
(`parse_anchor`, `parse_mode`, `voice_chords`, plus `Mode` / `ChordSpec`
/ `VoicingDict` types) are pure / type-only and need no rewire.

### 3.5 Codegen emit consolidation

`CodeGenerator` gains member methods:

```cpp
class CodeGenerator {
public:
    // …
    std::uint16_t emit_push_const(float value);  // was free fn
    std::uint16_t emit_zero();                   // was free fn
    void emit(cedar::Instruction inst);          // existing, unchanged

private:
    SourceLocation current_source_loc_;
    // …
};
```

The free functions `codegen::emit_push_const` /
`codegen::emit_zero` in `codegen/helpers.hpp` are **deleted**. All ~25
call sites (per audit count) are mechanically rewritten to use the
methods. The methods route through `emit()`, which is the **only** push
site for both `instructions_` and `source_locations_`. The non-pushing
helpers (e.g. `set_unused_inputs`) stay free functions.

Invariant assertion added to `CodeGenerator::generate()` epilogue:

```cpp
assert(instructions_.size() == source_locations_.size() &&
       "F2: source_locations_ vector desync");
```

Plus a debug-build invariant: a **structural hash** of the input
`Ast::arena` is recorded before codegen begins and re-checked at the
end; assert that it hasn't changed. The hash function (in a new helper
`akkado/include/akkado/ast_hash.hpp`) does a post-order traversal that
reads each `Node::data` variant arm's named fields (string `.value`,
`.name`, integer atoms, etc.) into FNV-1a — explicitly *not*
`memcpy`-style, since the AST contains `std::string` and `std::variant`
members whose padding bytes and heap pointers are unspecified.

### 3.6 Token shape change

`TokenValue` variant currently:

```cpp
using TokenValue = std::variant<std::monostate, NumericValue, std::string, PitchValue>;
```

After Phase 5:

```cpp
using TokenValue = std::variant<std::monostate, NumericValue, SymbolId, StringLitData, PitchValue>;
```

- `SymbolId` replaces `std::string` for identifier-like tokens
  (`Identifier`, contextual keywords, builtin names).
- `StringLitData` is a new struct wrapping `std::string` for string
  literals where the string content is the value (e.g. mini-notation
  raw payload). The variant carries a distinct type so identifier-vs-
  literal-string can never be confused.
- All consumers update: `Token::as_string()` is removed; new
  `Token::as_identifier(const StringInterner&)` and
  `Token::as_string_lit()` are added.

`MiniToken` follows the same shape change in the same phase.

---

## 4. Per-Phase Implementation Detail

### Phase 1a — Remove `NodeType::PreResolved` + reorder via local vector (F1 part 1)

**Scope.** Remove `NodeType::PreResolved` entirely. Make codegen's two
`codegen.cpp` `const_cast<AstArena&>` sites read-only.

**Approach.** No synthetic arena is introduced. Two changes:

1. **`expand_call_arguments` (`codegen.cpp:1000-1025`):** drop the
   `const_cast<AstArena&>(ast_->arena)`; return the existing
   `ExpandedArg` vector directly. Rekey `pre_resolved_values_` from
   `std::unordered_map<NodeIndex, TypedValue>` to
   `std::unordered_map<std::pair<NodeIndex, int>, TypedValue>` (or
   equivalent), where the pair is `(call_node, arg_position)`. The
   Call branch in `visit()` checks `pre_resolved_values_.find({call,
   i})` before falling through to the generic per-child visit. The
   `NodeType::PreResolved` enum value is deleted from `NodeType`.
2. **`reorder_spread_named_args` (`codegen.cpp:3230-3360`):** drop the
   `const_cast<AstArena&>`. Today this function rewrites
   `arena[call_node].first_child` and sibling chains in place to put
   named args in slot order. Replace the in-place rewrite by leaving
   the function's local `std::vector<ArgInfo> args` as the canonical
   slot order (already constructed and ordered) and exposing it to the
   Call visit path via a per-call `std::optional<std::vector<NodeIndex>>
   reordered_arg_chain_` member on `CodeGenerator`. The Call branch
   prefers the reordered chain when present, else walks
   `arena[call_node].first_child` as today.

**Files touched.**

| File | Change |
|---|---|
| `akkado/include/akkado/ast.hpp` | Remove `NodeType::PreResolved` enum value (no `PreResolvedData` variant arm exists today — nothing to remove there). |
| `akkado/include/akkado/codegen.hpp` | Change `pre_resolved_values_` key from `NodeIndex` to `std::pair<NodeIndex,int>`; add `reordered_arg_chain_` per-call helper. |
| `akkado/src/codegen.cpp:1000-1025` | Drop `const_cast`; remove `arena.alloc(NodeType::PreResolved, …)`; return ExpandedArg list to caller; record `pre_resolved_values_[{call,i}] = …`. |
| `akkado/src/codegen.cpp:3230-3360` | Drop `const_cast`; replace in-place arena rewrite with read-only ArgInfo vector consumed by the Call visit path. |
| `akkado/src/codegen.cpp` (Call branch) | Look up `pre_resolved_values_[{call,i}]` per arg before generic visit; honor `reordered_arg_chain_` when set. |
| `akkado/include/akkado/ast_hash.hpp` (NEW) | Helper `arena_structural_hash(const AstArena&)` used by the debug invariant. |
| `akkado/tests/test_codegen.cpp` | Add Phase 1a regression test (see §7). |

**Exit criteria.**

- `grep -rn 'PreResolved' akkado/` returns no hits in `src/` or
  `include/` (tests + comments may reference it as historical).
- `grep -n 'const_cast<AstArena' akkado/src/codegen.cpp` returns zero
  hits (both `codegen.cpp` sites resolved here; the four
  `codegen_patterns.cpp` sites are Phase 1b's scope).
- Debug `assert(arena_structural_hash(ast_->arena) == initial_hash)` at
  end of `generate()` passes for every fixture.
- All existing codegen tests pass byte-identical bytecode (verified via
  the Phase 0 snapshot harness).
- **Docs updated per §11 protocol** — PRD status block reflects
  `Phase 1a: SHIPPED <commit> <date>`; audit doc marks F1 (part 1)
  resolved with backlink to the same commit.

---

### Phase 1b — Mini-notation parse-at-parse-time (F1 part 2 + F3 tail)

**Scope.** Introduce a `MiniLiteralData` variant arm on `Node::data`,
parse mini-notation strings into per-pattern sub-arenas at parse time,
and delete the 4 `const_cast<AstArena&>` codegen-time re-parses in
`codegen_patterns.cpp`. **Additionally** close F3's 5th re-parse site
by caching `parse_chord_symbol` results on Sample-kind `MiniAtomData`
at parse time, and switching `PatternEvaluator`'s chord-mode branch to
read those fields instead of re-parsing.

**Approach.**

Part 1 — `MiniLiteralData` sub-arena. Today `parser.cpp:1759` sets
`arena_[node].data = Node::StringData{mode_marker}` and
`parser.cpp:1754` stitches the parsed mini-AST as a child via
`arena_.add_child(node, pattern_ast)`. Phase 1b adds a dedicated
`MiniLiteralData` arm (carrying `mode_marker`, `mini_arena`,
`mini_root`, `mini_diagnostics`) to the `Node::data` variant.
`Parser::parse_mini_literal` (`parser.cpp:1705-1762`) constructs the
sub-arena, parses into it, captures diagnostics, sets
`arena_[node].data = Node::MiniLiteralData{...}`, and **stops calling
`arena_.add_child(node, pattern_ast)`**. The 4 codegen sites read
`as_mini_literal().mini_arena` instead of re-parsing. All consumers
that previously read `as_string()` on a `MiniLiteral` node migrate to
`as_mini_literal().mode_marker`.

Part 2 — Sample-atom chord caching (F3 tail). Today
`mini_parser.cpp:309-330` (`parse_sample_atom`) initialises Sample-kind
`MiniAtomData` with empty `chord_root` / `chord_quality` /
`chord_intervals` / `chord_root_midi` fields, leaving the chord-symbol
parse to runtime. Phase 1b invokes `parse_chord_symbol(sample.name)`
inside `parse_sample_atom` and writes the result into those four
fields on success; on failure the fields stay empty (current behavior).
`PatternEvaluator::evaluate_atom`'s `MiniAtomKind::Sample` branch at
`pattern_eval.cpp:197-224` is rewritten to read
`atom_data.chord_root` (and siblings) directly when `chord_mode_` is
on, treating empty `chord_root` as the "not a valid chord symbol →
Rest" case. The `#include <akkado/chord_parser.hpp>` in
`pattern_eval.cpp` is removed (no parser dependency from runtime).

Costs of Part 2: every Sample-kind atom pays one `parse_chord_symbol`
call at parse time even when it's not consumed in chord mode. The
function is pure, branches on the first character (`[A-G]`), and
returns `nullopt` cheaply for non-chord-shaped names — measured cost
in the parser is negligible compared to mini-lex overhead. The win is
mechanical: `parse_chord_symbol` exists in exactly one execution path
(parser) instead of two (parser + evaluator), and `MiniAtomData`'s
chord fields are the single source of truth.

**Files touched.**

| File | Change |
|---|---|
| `akkado/include/akkado/ast.hpp` | Add `MiniLiteralData` struct (4 fields) and add it as a variant arm of `Node::data`. Add `as_mini_literal()` accessor mirroring the other `as_*` helpers. |
| `akkado/src/parser.cpp:1705-1762` | Construct sub-arena; parse into it; set `MiniLiteralData` on the node; remove the `add_child` call at line 1754; remove the `StringData` assignment at line 1759. |
| `akkado/src/mini_parser.cpp:309-330` | In `parse_sample_atom`, call `parse_chord_symbol(sample.name)` opportunistically and write `chord_root` / `chord_quality` / `chord_intervals` / `chord_root_midi` on success; leave empty on failure. Add `#include <akkado/chord_parser.hpp>`. |
| `akkado/src/pattern_eval.cpp:197-224` | Rewrite `MiniAtomKind::Sample` chord-mode branch to read cached `chord_root` (and siblings) from `atom_data` instead of calling `parse_chord_symbol`. Empty `chord_root` ⇒ Rest. Remove `#include <akkado/chord_parser.hpp>`. |
| `akkado/src/codegen_patterns.cpp:1778` | Replace `parse_mini(..., const_cast<AstArena&>)` with read of `ast_->arena[node].as_mini_literal().mini_arena`. |
| `akkado/src/codegen_patterns.cpp:2133` | Same. |
| `akkado/src/codegen_patterns.cpp:2183` | Same. |
| `akkado/src/codegen_patterns.cpp:2983` | Same. |
| `akkado/src/codegen.cpp` (MiniLiteral switch arm) | Migrate from `as_string()` to `as_mini_literal().mode_marker` for mode-marker reads. |
| `akkado/src/analyzer.cpp` (MiniLiteral handling at `:240`, `:267`, `:727`, `:2326`) | Migrate any `as_string()` reads on MiniLiteral nodes. |
| `akkado/src/shape_index.cpp` | Migrate MiniLiteral mode-marker read. |
| `akkado/src/akkado.cpp` | Merge `MiniLiteralData::mini_diagnostics` into per-pass diagnostics with `SourceMap::adjust_all`. |
| `akkado/tests/test_codegen.cpp` | Phase 1b regression tests (no codegen-time arena growth + chord-cache parity). |
| `akkado/tests/test_pattern_eval.cpp` (or `test_mini_notation.cpp`) | F3-tail regression: chord-mode pattern with cached vs uncached chord produces identical events. |

**Pre-implementation sweep.** Before opening the Phase 1b PR, run
`grep -rn 'as_string()\|NodeType::MiniLiteral' akkado/src/ akkado/include/`
and audit every `MiniLiteral`-adjacent `as_string()` reader for
migration to `as_mini_literal().mode_marker`. The grep above is the
implementer's call-site map.

**Exit criteria.**

- `grep -rn 'const_cast.*AstArena' akkado/src/codegen_patterns.cpp`
  returns zero hits. (The two `codegen.cpp` sites were resolved in
  Phase 1a; total `const_cast<AstArena` count across `akkado/src/` is
  zero after Phase 1b.)
- Every codegen function signature taking `Ast&` is rewritten to
  `const Ast&`.
- `grep -n 'parse_chord_symbol' akkado/src/` returns hits **only** in
  parser-stage files (`mini_parser.cpp`, `chord_parser.cpp`, and the
  existing `parser.cpp` call sites if any). No hits in
  `pattern_eval.cpp`, `codegen*.cpp`, or `analyzer.cpp`.
- `grep -n 'parse_mini(' akkado/src/` returns hits only in parser-stage
  files. No hits in `codegen*.cpp`.
- All existing mini-notation tests pass byte-identical sequence output
  (verified via Phase 0 snapshot harness).
- Chord-mode pattern test: `pat("C E Am G").chord()` produces identical
  `PatternEvent` chord_data before and after Phase 1b.
- **Docs updated per §11 protocol** — PRD status block reflects
  `Phase 1b: SHIPPED <commit> <date>`; audit doc marks F1 (part 2) and
  F3 (5th site) resolved, updates the F1 row to "resolved" overall, and
  adds a RESOLVED tag to the F3 finding header.

---

### Phase 2 — Mini-lexer line tracking (F8) + F7 regression test only

**Scope reduction (2026-05-25).** F7 withdrew during Phase 0 (`^` is
already right-associative — see §1.3). Phase 2 keeps F7's regression
tests (`2^3^2 == 512`, tower assoc, unary interaction) to lock current
behavior, but ships **no parser code change**. The remaining
behavioral change in this phase is the F8 mini-lexer line-tracking
fix.

**F7 (test-only).** Add Catch2 regression tests asserting
`2^3^2 == 512`, `2^2^2^2 == 65536`, `-2^2 == 4` (unary-tighter), and
`x ^ -1` parses cleanly. Leave `parser.cpp:1455-1463` untouched. Also
add a one-line comment above the no-op cast explaining why it's
intentional (left-assoc passes `p+1`, right-assoc passes `p`,
recursion accepts another `^` since `p ≥ p` — see §1.3).

**F8 fix.** `mini_lexer.cpp:73-77` and `:146-153`:

```cpp
char MiniLexer::advance() {
    char c = pattern_[current_++];
    if (c == '\n') { line_++; column_ = 1; }
    else           { column_++; }
    return c;
}

SourceLocation MiniLexer::current_location() const {
    // line_ now tracked locally; column_ is current; base_location_
    // provides the absolute line offset for the first line only.
    int abs_line = base_location_.line + (line_ - 1);
    int abs_col  = (line_ == 1) ? (base_location_.column + column_offset_) : column_;
    return { .line = abs_line, .column = abs_col,
             .offset = base_location_.offset + start_,
             .length = current_ - start_ };
}
```

New `column_offset_` member captures `start_at_first_line` so column
arithmetic stops accumulating once line 2+ starts. Initialised in
ctor; reset on `\n`.

**Files touched.**

| File | Change |
|---|---|
| `akkado/src/parser.cpp:1455-1463` | **No code change** (F7 withdrawn). Add a one-line comment explaining the no-op cast is intentional, with a back-reference to §1.3. |
| `akkado/src/mini_lexer.cpp:73-77, 146-153` | Line tracking. |
| `akkado/include/akkado/mini_lexer.hpp` | Add `line_`, `column_offset_` members. |
| `akkado/tests/test_parser.cpp` | `2^3^2 == 512` + tower assoc tests (regression — locks current behavior). |
| `akkado/tests/test_mini_notation.cpp` | Multi-line pattern diagnostic line/column tests. |

**Exit criteria.**

- `2 ^ 3 ^ 2` evaluates to 512 in const_eval, codegen, and any test
  using power (regression test, asserts current behavior).
- Multi-line mini-pattern `"c4 d4\ne4 f4"` reports line 2 for `e4`'s
  diagnostic.
- Phase 0 snapshot `06_power_op.disasm` remains byte-identical (no
  parser change → no codegen change).
- **Docs updated per §11 protocol** — PRD status block reflects
  `Phase 2: SHIPPED <commit> <date>`; audit doc marks F8 resolved and
  marks F7 **withdrawn** with backlink to Phase 0 (commit `b203e2e`).

---

### Phase 3 — Source-location emit consolidation (F2)

**Scope.** Promote the two free helper functions to `CodeGenerator&`
methods. Delete free functions. Mechanical rewrite of ~25 call sites.
Add the cross-cutting invariant assertion.

**Files touched.**

| File | Change |
|---|---|
| `akkado/include/akkado/codegen/helpers.hpp` | Delete `emit_push_const` and `emit_zero` free fns. |
| `akkado/include/akkado/codegen.hpp` | Add `emit_push_const` / `emit_zero` member methods. |
| `akkado/src/codegen.cpp` | Implement methods; route through `emit()`. Add invariant assertion in `generate()` epilogue. |
| `akkado/src/codegen.cpp` | Rewrite 3 call sites (320, 901, 923). |
| `akkado/src/codegen_patterns.cpp` | Rewrite 10 call sites (3158, 3181, 4262, 4338, 4424, 4464, 4499, 5096, 5156, 5263) — drop the manual `source_locations_.push_back` at the 3 sites that have it. |
| `akkado/src/codegen_higher_order.cpp:699` | Rewrite. |
| `akkado/src/codegen_arrays.cpp` | Rewrite any internal call sites. |
| `akkado/tests/test_codegen.cpp` | Add `instructions == source_locations` parity check sweep. |

**Exit criteria.**

- `grep -n 'codegen::emit_push_const\|codegen::emit_zero' akkado/src/`
  returns zero hits.
- `assert(instructions_.size() == source_locations_.size())` at end of
  `generate()` does not fire across the full test suite.
- New test sweeping every fixture through `compile()` asserts parity
  passes.
- **Docs updated per §11 protocol** — PRD status block reflects
  `Phase 3: SHIPPED <commit> <date>`; audit doc marks F2 resolved.

---

### Phase 4 — `voicing_registry` into `CompileContext` (F14)

**Scope.** Introduce `CompileContext` (minimum viable: `VoicingRegistry`
only — `StringInterner` empty/no-op until Phase 5). Delete the
process-global registry + mutex. Thread the registry through
`CodeGenerator`.

**Files touched.**

| File | Change |
|---|---|
| `akkado/include/akkado/compile_context.hpp` | New header. |
| `akkado/src/compile_context.cpp` | New impl. |
| `akkado/include/akkado/voicing.hpp` | `VoicingRegistry` class wrapping the existing free fns. |
| `akkado/src/voicing.cpp:13-30,292-303` | Delete `voicing_registry()` + `registry_mutex()` + the free `register_voicing` / `lookup_voicing` wrappers. Move logic into `VoicingRegistry`. |
| `akkado/include/akkado/akkado.hpp:105` | Add optional `CompileContext* ctx = nullptr` parameter. |
| `akkado/src/akkado.cpp` | Construct default ctx if null; pass to `CodeGenerator`. |
| `akkado/include/akkado/codegen.hpp` | Hold `CompileContext*` member; receive in ctor. |
| `akkado/src/codegen_patterns.cpp:4538,4540,4803,4904` | Four registry call sites: `voicing::lookup_voicing(...)` → `ctx_->voicing_registry->lookup(...)` at `:4538` (user dict in `apply_voicing`), `:4540` (built-in `"close"` fallback), `:4803` (membership check in `handle_voicing_call`); `voicing::register_voicing(...)` → `ctx_->voicing_registry->define(...)` at `:4904` (inside `handle_add_voicings_call`). Other `voicing::*` namespace mentions (pure fns + types) are untouched. |
| `akkado/tests/test_codegen.cpp` | Phase 4 regression: compile source A defining voicing `x`, then compile source B *without* defining `x` — assert `x` lookup fails in B. |

**Exit criteria.**

- `grep -n 'static std::mutex\|static std::unordered_map' akkado/src/`
  returns zero hits in compiler code.
- The cross-compile-leak regression test fails on master and passes
  after Phase 4.
- No new lints / warnings introduced.
- **Docs updated per §11 protocol** — PRD status block reflects
  `Phase 4: SHIPPED <commit> <date>`; audit doc marks F14 resolved.

---

### Phase 5 — String interner + Token shape change (F12)

**Scope.** Add `StringInterner` to `CompileContext`. Lex-time interning
for all identifier-like token kinds. Token variant change. Parser /
analyzer / codegen consumers updated.

**Pre-implementation call-site sweep (required).** `IdentifierData::name`
is `std::string` today (`ast.hpp:185`) and surfaces via
`Node::as_identifier() const std::string&` (`ast.hpp:395-397`). Every
consumer of that accessor changes. Before opening the Phase 5 PR, run
and check in the result of:

```bash
grep -rn 'as_identifier\(\)\|IdentifierData\|symbol_table\.\(lookup\|insert\)' \
    akkado/src/ akkado/include/
```

The output is the implementer's authoritative call-site map. Without
this sweep, the implementer discovers the breadth of analyzer
(`analyzer.cpp`, 2954 LOC), codegen (`codegen.cpp` + `codegen_patterns.cpp`,
~10 KLOC combined), `const_eval.cpp`, `pattern_eval.cpp`, and
`shape_index.cpp` consumers only inside the PR diff. The sweep is
checked in as `docs/phase5-identifier-consumers.txt` and updated
in-PR.

**Files touched.**

| File | Change |
|---|---|
| `akkado/include/akkado/string_interner.hpp` | New header. |
| `akkado/src/string_interner.cpp` | New impl. |
| `akkado/include/akkado/compile_context.hpp` | Populate `interner` field. |
| `akkado/include/akkado/token.hpp` | `TokenValue` variant: drop `std::string`; add `SymbolId`, `StringLitData`. Add `as_identifier`, `as_string_lit`. Drop `as_string`. |
| `akkado/include/akkado/mini_token.hpp` | Mirror Token change for `MiniToken`. |
| `akkado/include/akkado/lexer.hpp` | `Lexer` ctor takes `StringInterner&`. |
| `akkado/src/lexer.cpp` | All `make_token(type, std::string(text))` call sites switch to `make_token(type, interner_.intern(text))`. |
| `akkado/include/akkado/mini_lexer.hpp` | Same. |
| `akkado/src/mini_lexer.cpp` | Same. |
| `akkado/include/akkado/ast.hpp` | `IdentifierData` switches to `SymbolId name;` (was `std::string`). |
| `akkado/src/parser.cpp` | All 15+ identifier copy sites switch to `SymbolId` propagation. |
| `akkado/include/akkado/symbol_table.hpp` | Use `SymbolId` instead of `std::string` for lookup key; drop FNV-1a recomputation. |
| `akkado/src/symbol_table.cpp` | 16+ rehash sites collapse to direct id lookup. |
| `akkado/src/analyzer.cpp` | Identifier resolution + diagnostics: resolve `SymbolId` → `string_view` only for diagnostic strings via the interner. |
| `akkado/src/codegen*.cpp` | Identifier references resolve via interner where a string is required. |
| `akkado/src/akkado.cpp` | Pass `ctx->interner` into `lex()`. Hold source ownership until codegen completes. |
| `akkado/src/shape_index.cpp` | Update consumer. |
| `akkado/tests/test_lexer.cpp` | New tests: identical strings produce identical SymbolIds; interner survives lexer's lifetime. |
| `akkado/tests/test_symbol_table.cpp` | Lookup-by-SymbolId tests. |

**Exit criteria.**

- `grep -n 'fnv1a_hash' akkado/src/symbol_table.cpp` returns at most one
  hit (the interner's own internal hash function).
- `Token` no longer holds `std::string` for any identifier-like token
  kind (verified by checking `TokenValue` variant).
- Existing tests pass; new interner tests pass.
- Microbench: identifier-heavy compile (~50 KB stdlib, e.g.
  `akkado/stdlib/event_transforms.ak`) shows a reduction in
  `std::string` allocations attributable to identifier handling. The
  Phase 5 PR description **must** include before/after allocation
  counts (recorded via heap profiler, `mallinfo()`, or ASan stats) so
  the win is documented. No fixed numeric target — the structural
  assertion above (`TokenValue` no longer holds `std::string` for
  identifier tokens) is the real correctness check; the bench is
  informational evidence.
- **Docs updated per §11 protocol** — PRD status block reflects
  `Phase 5: SHIPPED <commit> <date>`; audit doc marks F12 resolved;
  audit doc's overall "all 6 critical correctness findings" tally
  flipped to "all resolved".

---

## 5. Phase Dependencies and Order

```
Phase 0 (snapshot harness) ─┐
                            v
                 Phase 1a (PreResolved + reorder)  ───┐
                                                      ├──> Phase 1b (mini-AST)
                                                      │
                                                      ├──> Phase 2 (^ + mini-lexer)        [parallel]
                                                      ├──> Phase 3 (source-loc emit)       [parallel]
                                                      ├──> Phase 4 (CompileContext+voicing) [parallel]
                                                      │                       │
                                                      │                       v
                                                      └──>             Phase 5 (interner)
```

- **Phase 0** ships a per-fixture bytecode-disassembly snapshot test
  harness (see §8 *Snapshot harness*) so every subsequent phase's
  "byte-identical bytecode" exit criterion is mechanically checked.
  Mandatory prerequisite for Phase 1a.
- Phase 1a ships next; nothing else depends on it strictly but it
  proves out the "no codegen mutation" pattern.
- Phase 1b depends on Phase 1a being merged (both touch the codegen
  invariant; want to fix one mutation site cleanly before the next).
- Phases 2, 3, 4 are independent of each other and of Phase 1b. Can
  ship in parallel by different contributors.
- Phase 5 depends on Phase 4 (`CompileContext` must exist to hold the
  interner). Otherwise independent of Phases 1a/1b/2/3.

Estimated total effort: **6–9 weeks single-engineer** (incl. Phase 0
~0.5 week), **3–4 weeks** if Phases 2/3/4 parallelize across 2
contributors.

---

## 6. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| `cedar/` | **No change** | Cedar VM untouched. |
| `BUILTIN_FUNCTIONS` registry | **No change** | Stays static-init const map. |
| `SampleRegistry` / `FileResolver` | **No change** | Stays on `compile()` arg list. |
| Codegen public bytecode output | **Byte-identical** | Existing tests assert no diff. |
| `compile()` public signature | **Modified** (additive) | New optional `CompileContext*` param. |
| `Ast::arena` | **Modified** | Becomes `const`-correct post-analyzer. |
| `Node::data` variant | **Modified** | Gains `MiniLiteralData` arm (replaces `StringData` usage for `MiniLiteral` nodes). |
| `MiniLiteralData` | **New** | New struct: `mode_marker` (replaces existing StringData usage) + 3 new fields (sub-arena, root, diagnostics). No such struct exists today. |
| `pre_resolved_values_` key shape | **Modified** | Now keyed by `std::pair<NodeIndex,int>` = `(call_node, arg_position)`. No synthetic arena; no synthetic nodes. |
| `NodeType::PreResolved` | **Removed** | Side-table replaces it. |
| `CodeGenerator::reordered_arg_chain_` | **New** | Per-call read-only slot vector replacing `reorder_spread_named_args`'s in-place arena rewrite. |
| `codegen::emit_push_const`, `emit_zero` (free fns) | **Removed** | Replaced by `CodeGenerator` methods. |
| `voicing_registry()` global, `registry_mutex()` global | **Removed** | Moved into `VoicingRegistry` class owned by `CompileContext`. |
| `TokenValue::std::string` arm | **Removed** | Replaced by `SymbolId` (identifier-like) and `StringLitData` (literals). |
| `Token::as_string` | **Removed** | Replaced by `as_identifier(interner)` + `as_string_lit()`. |
| `IdentifierData::name` field type | **Modified** | `std::string` → `SymbolId`. |
| `SymbolTable` lookup API | **Modified** | Keyed by `SymbolId`; FNV recomputation gone. |
| `parser.cpp:1455-1463` `^` precedence | **No change** | F7 withdrawn (Phase 0 verified `^` already right-assoc). Phase 2 adds a clarifying comment only. |
| `mini_lexer.cpp` line tracking | **Modified** | Bumps `line_` on `\n`. |
| `CompileContext` | **New** | New header + impl. |
| `StringInterner` | **New** | New header + impl. |
| `VoicingRegistry` class | **New** | Wraps the existing free fns. |

---

## 7. Edge Cases

### Phase 1a (`PreResolved` removal + reorder via local vector)

- **`pre_resolved_values_` lifetime.** Map cleared at start of each
  `generate()` (matches existing reset patterns). Survives every
  `visit()` call within one `generate()`.
- **`reordered_arg_chain_` lifetime.** Per-call optional, set by
  `reorder_spread_named_args` immediately before the Call branch
  descends, cleared on Call branch exit. Never persists across calls.
- **Concurrent reads.** No reader of `Ast::arena` runs during
  `generate()` today; the assertion is a future-proofing.
- **Spread expansion of zero args.** Spread `..[]` produces zero
  `ExpandedArg` entries — `pre_resolved_values_` gets no new entries.
  Call visit falls through to a zero-arg builtin call. No change.
- **Named-arg reorder with no named args.** Today's fast-exit `if
  (!has_named) return true;` (`codegen.cpp:3306`) stays — no
  `reordered_arg_chain_` is set; Call walks `arena[call_node]
  .first_child` as before.

### Phase 1b (mini-AST sub-arena)

- **Bad mini-notation.** Parse-time errors land in
  `MiniLiteralData::mini_diagnostics` and get merged into the main
  parser's diagnostic list at the end of `Parser::parse()`. UX is
  identical to today (the error reaches the user at the same time);
  internally, the codegen no longer needs to re-emit the diag.
- **Move-only AST.** `MiniLiteralData` becomes move-only because of
  `unique_ptr<AstArena>`. Verify every `AstNode` copy site (clone,
  serialization) handles move correctly; `analyzer.cpp:1262-1321` is
  the main worry. The fix: clone the sub-arena explicitly when cloning
  the `MiniLiteral` node.
- **Empty mini-notation.** `""` produces an empty sub-arena with
  `mini_root == NULL_NODE`. Codegen treats as no events (existing
  behavior).
- **Mini-notation in const expressions.** const_eval doesn't currently
  enter mini-notation; sub-arena is opaque to const_eval. No change.
- **Sample atom name that happens to look like a chord but isn't used
  as one.** `pat("kick snare hh")` lexes as Sample atoms; the parser
  attempts `parse_chord_symbol("kick")` which returns `nullopt` (no
  uppercase root). Chord fields stay empty. Cost: 3 cheap `nullopt`
  returns per such pattern at parse time. No behavioral change in
  non-chord mode.
- **Sample atom name that IS a valid chord but used as a sample.**
  `pat("C E G").sample()` — parser caches chord fields on each atom,
  but `chord_mode_ == false` at evaluation time so pattern_eval reads
  `sample_name` as before. Cached fields are dead weight; harmless.
- **Sample atom name that's a valid chord and used in chord mode.**
  `pat("C E Am G").chord()` — parser cached fields. pattern_eval reads
  `atom_data.chord_root` (`"C"`, `"E"`, `"A"`, `"G"`) directly. Output
  identical to today.
- **Chord lookup failure in chord mode.** `pat("xyz").chord()` —
  parser's `parse_chord_symbol("xyz")` returns `nullopt`, chord fields
  stay empty. pattern_eval sees empty `chord_root` and emits Rest. No
  diagnostic change (same as today's failure path).
- **Tests asserting current chord-mode diagnostic behavior.** Today
  pattern_eval silently emits Rest on chord-symbol parse failure; no
  diagnostic. The migration preserves this (parser's
  `parse_chord_symbol` failure on a Sample atom is non-diagnostic — a
  Sample atom is a valid form even if it isn't also a chord). No
  diagnostic regressions expected.

### Phase 2 (`^` + mini-lexer)

- **`-x ^ 2`.** Unary minus binds tighter than `^` today; this PRD does
  not change unary precedence. `-2^2` continues to parse as `(-2)^2 == 4`
  (matches existing behavior, NOT Python; documented in mini-notation
  reference if not already).
- **`x ^ -1`.** Right operand of `^` is a unary expression — should
  continue parsing as `x ^ (-1)`. Confirm with a test.
- **Mini-pattern with trailing `\n` only.** Final newline must increment
  `line_` even though no character follows. Test case:
  `"a b\nc d\n"` — last line tracked.
- **Mini-pattern with `\r\n`.** Treat as one line bump (consume `\r`
  but only `\n` bumps `line_`). Match `lexer.cpp` behavior.

### Phase 3 (source-loc consolidation)

- **Helper called with `current_source_loc_ == {}`** (initial state) —
  should emit a sentinel location, not crash. Existing `emit()` already
  handles this; the methods inherit the behavior.
- **Helper called from within a nested visit (recursive).** The
  `current_source_loc_` save-and-restore protocol in
  `codegen.cpp:971, 1251, 1739, 1975` continues to apply; the methods
  read the current value at call time.

### Phase 4 (`CompileContext` + voicing)

- **Caller passes the same ctx to two consecutive compiles.** Voicings
  defined in compile 1 ARE visible in compile 2 (this is the intended
  cross-compile-state mechanism for live coding). Tests must
  distinguish "default ctx" (fresh) from "shared ctx" (intentional
  persistence).
- **Built-in voicings.** Pre-seeded by `VoicingRegistry` ctor
  (`close`, `open`, `drop2`, `drop3`). Identical to today's seed.
- **Null ctx in `CodeGenerator` after the change.** Disallowed — the
  ctor takes `CompileContext&` (reference, not pointer). `compile()`
  guarantees non-null by constructing a default if needed.

### Phase 5 (`StringInterner`)

- **Source buffer outlives interner: required.** If a caller hands the
  `CompileContext` to another compile after the original source goes
  away, interned views are dangling. Document loudly in `CompileContext`
  header. The internal compile flow keeps source alive until codegen
  completes, then both die together — the dangerous shape is "caller
  reuses ctx across source buffers". Mitigation: if a caller wants
  cross-compile interning, the caller owns persistent source storage
  (or, as a future option, the interner could copy-on-demand).
- **`SymbolId == 0` reserved as `NULL_SYMBOL`.** Returned for empty-
  string intern attempts. Lookup with `NULL_SYMBOL` returns empty
  string_view. Symbol-table lookup with `NULL_SYMBOL` returns no match.
- **Identifier names colliding with keywords in mini-notation context.**
  Mini-tokens use `SymbolId` from the same per-compile interner as the
  main tokens. Collision is by intent (same identifier text → same id).
- **Builtin-name comparisons in `lookup_builtin`.** Today
  `BUILTIN_FUNCTIONS` keys on `string_view`. After Phase 5, lookups can
  go either way: keep string-view keys but `Token` resolves
  `SymbolId → string_view` before lookup. Or: rebuild
  `BUILTIN_FUNCTIONS` keyed on `SymbolId` per-compile (interner
  populates with all builtin names eagerly). v1 picks the first
  (simpler, no second copy of the table); a future PRD can pre-intern
  builtin names into the static SymbolId space if profiling justifies
  it.

---

## 8. Testing Strategy

Every phase ships at least one **precise regression test** that fails
on master and passes after the phase lands. Plus two cross-cutting
invariants asserted in production.

### Phase 0 — Snapshot harness (prerequisite)

Each "byte-identical bytecode" exit criterion across phases 1a/1b/2/3/4/5
is verified by a snapshot test added before Phase 1a opens. The harness:

1. **Fixture set.** Glob every `.ak` file under `akkado/stdlib/` plus
   any `akkado/tests/fixtures/*.ak` (or add a small fixture dir if
   none exists). Limit to files that successfully compile on master.
2. **Snapshot generator.** A new test target `akkado_bytecode_snapshot`
   that compiles each fixture via `compile()` and writes a
   deterministic disassembly (one instruction per line: opcode name,
   inputs, output, immediates) to `akkado/tests/snapshots/<fixture>.disasm`.
   The disassembly reuses `tools/nkido-cli/bytecode_dump.cpp`'s
   formatter (already exists, per CLAUDE.md) wrapped in a test-only
   entry point.
3. **Diff check.** A second test target `akkado_bytecode_snapshot_diff`
   that re-runs the generator into a tempdir and asserts every file
   matches the checked-in snapshot. Runs in `akkado_tests` by default.
4. **Update flow.** A `--update-snapshots` flag on the test binary
   overwrites the checked-in snapshots; intentional bytecode changes
   are reviewed as snapshot diffs in PR.

Every subsequent phase's exit criterion that says "byte-identical
bytecode" is mechanically interpreted as "`akkado_bytecode_snapshot_diff`
passes with no snapshot update." Phases that *intentionally* change
bytecode (none of phases 1a–5 should) would update snapshots in-PR
with reviewer sign-off.

### Phase 1a tests

- `test_codegen.cpp [F1a]`: compile a fixture that triggers spread
  expansion; assert `ast.arena.size()` before and after `generate()`
  are equal.
- `test_codegen.cpp [F1a]`: `grep`-style test asserting no AST node has
  kind `PreResolved` (compile-time assertion via removed enum value
  also suffices).

### Phase 1b tests

- `test_codegen.cpp [F1b]`: compile a chord-containing pattern; assert
  `ast.arena.size()` unchanged by codegen.
- `test_codegen.cpp [F1b]`: verify mini-notation diagnostic appears in
  parser-stage diagnostic list, not codegen-stage.
- `test_parser.cpp [F1b]`: `MiniLiteralData::mini_arena` populated for
  every parsed mini-literal; root index reachable.
- `test_mini_notation.cpp [F3]`: parse `pat("C E Am G").chord()`;
  assert every Sample-kind atom in the sub-arena has `chord_root` set
  (`"C"`, `"E"`, `"A"`, `"G"`).
- `test_mini_notation.cpp [F3]`: parse `pat("kick snare hh")`; assert
  every Sample-kind atom has empty `chord_root` (failed
  `parse_chord_symbol` left fields empty).
- `test_pattern_eval.cpp [F3]`: evaluate `pat("C E Am G").chord()` and
  assert the emitted `PatternEvent::chord_data` matches today's
  byte-for-byte output. Snapshot the event stream (root, quality,
  intervals, root_midi) and diff against a checked-in expectation.
- `test_pattern_eval.cpp [F3]`: evaluate `pat("xyz").chord()`; assert
  emitted event is `Rest` (parse-failure path).
- `test_pattern_eval.cpp [F3]`: `grep`-style invariant — fail the test
  if `pattern_eval.cpp` contains the token `parse_chord_symbol` (build
  a one-shot check that scans the source file).

### Phase 2 tests

F7 tests are **regression tests** (F7 withdrawn — `^` is already
right-assoc on master). They lock current behavior so future
parser refactors can't silently break it.

- `test_parser.cpp [F7]`: `2 ^ 3 ^ 2` → 512 (via const-eval).
- `test_parser.cpp [F7]`: tower `2 ^ 2 ^ 2 ^ 2` → 65536.
- `test_parser.cpp [F7]`: `-2 ^ 2` → 4 (unary-tighter, documented).
- `test_parser.cpp [F7]`: `x ^ -1` parses without error.
- `test_mini_notation.cpp [F8]`: multi-line pattern `"a b\nc d"` —
  diagnostic on `c` reports line 2.
- `test_mini_notation.cpp [F8]`: pattern with trailing `\n` followed by
  unterminated content reports correct line.
- (Sweep `grep -nE '\^.*\^' akkado/stdlib akkado/tests web/static/docs`
  no longer needed — F7 withdrew, no behavior change to re-baseline.)

### Phase 3 tests

- `test_codegen.cpp [F2]`: assertion in `generate()` epilogue
  (`instructions_.size() == source_locations_.size()`) — runs on every
  existing test invocation, free regression coverage.
- `test_codegen.cpp [F2]`: new fixture sweep — compile every `.ak` file
  in `akkado/stdlib/` and `akkado/tests/fixtures/` (if present); assert
  parity post-compile.
- `test_codegen.cpp [F2]`: emit-helper unit test — call
  `cg.emit_push_const(1.0f)` twice with different `current_source_loc_`;
  assert `source_locations_` has 2 entries matching.

### Phase 4 tests

- `test_codegen.cpp [F14]`: cross-compile leak regression — construct
  source A defining custom voicing `myV`; compile. Construct source B
  using `myV` without defining it; compile **with a default (fresh)
  context**; assert E-class error "unknown voicing `myV`". Compile B
  again with **the same context as A** (shared); assert success.
- `test_codegen.cpp [F14]`: built-in voicings (`close`, `open`,
  `drop2`, `drop3`) resolve in every fresh context.
- `test_codegen.cpp [F14]`: no `static std::mutex` in compiler binaries
  (verified by `grep` against compiled object dump if practical, or by
  source-tree grep).

### Phase 5 tests

- `test_lexer.cpp [F12]`: lex `foo bar foo`; assert tokens at positions
  0 and 2 carry the same `SymbolId`.
- `test_lexer.cpp [F12]`: lex `freq` (a builtin name); assert
  `SymbolId != NULL_SYMBOL`.
- `test_symbol_table.cpp [F12]`: `lookup(symbol_id)` is O(1) — assert
  no rehash by injecting an interner that counts hash() calls.
- `test_lexer.cpp [F12]`: interned view points into the source buffer,
  not into a copy (verifiable via address arithmetic in test).
- `test_lexer.cpp [F12]`: empty-string intern returns `NULL_SYMBOL`.
- Memory-pressure check (informal): an identifier-heavy compile
  (`akkado/stdlib/event_transforms.ak` or similar) shows reduced
  `std::string` allocation count under ASan (if available) or via
  `mtrack`-style instrumentation.

### Cross-cutting invariants (asserted in production)

Added to `CodeGenerator::generate()` epilogue:

```cpp
#ifndef NDEBUG
    assert(instructions_.size() == source_locations_.size() &&
           "F2: source_locations_ desync");
    assert(input_ast_hash_ == hash_arena(ast_->arena) &&
           "F1: codegen mutated input AST");
#endif
```

These fire on every test compile in debug builds, providing continuous
regression coverage with zero per-test boilerplate.

### Build + run commands

```bash
# Configure debug
cmake --preset debug

# Build
cmake --build build --target akkado_tests

# Run all akkado tests
./build/akkado/tests/akkado_tests

# Run per-phase tagged tests
./build/akkado/tests/akkado_tests "[F1a]"
./build/akkado/tests/akkado_tests "[F1b]"
./build/akkado/tests/akkado_tests "[F2]"
./build/akkado/tests/akkado_tests "[F7]"
./build/akkado/tests/akkado_tests "[F8]"
./build/akkado/tests/akkado_tests "[F12]"
./build/akkado/tests/akkado_tests "[F14]"
```

---

## 9. File-Level Changes Summary

### New files

| Path | Purpose |
|---|---|
| `akkado/include/akkado/compile_context.hpp` | `CompileContext` declaration |
| `akkado/src/compile_context.cpp` | `CompileContext` impl |
| `akkado/include/akkado/string_interner.hpp` | `StringInterner` + `SymbolId` declaration |
| `akkado/src/string_interner.cpp` | `StringInterner` impl |

### Modified files (alphabetical)

| Path | Phases | Change |
|---|---|---|
| `akkado/include/akkado/akkado.hpp` | 4 | `compile()` gains optional `CompileContext*` arg |
| `akkado/include/akkado/ast.hpp` | 1a, 1b, 5 | Remove `NodeType::PreResolved` enum value; add new `MiniLiteralData` struct (`mode_marker`, `mini_arena`, `mini_root`, `mini_diagnostics`) + variant arm + `as_mini_literal()` accessor; `IdentifierData::name` → `SymbolId` |
| `akkado/include/akkado/ast_hash.hpp` | 1a | NEW header: `arena_structural_hash(const AstArena&)` for the debug invariant |
| `akkado/include/akkado/codegen.hpp` | 1a, 3, 4 | New `pre_resolved_values_` key `std::pair<NodeIndex,int>`; new `reordered_arg_chain_` member; `emit_push_const`/`emit_zero` methods; `CompileContext&` ctor arg |
| `akkado/include/akkado/codegen/helpers.hpp` | 3 | Delete `emit_push_const` and `emit_zero` free fns |
| `akkado/include/akkado/lexer.hpp` | 5 | Ctor takes `StringInterner&` |
| `akkado/include/akkado/mini_lexer.hpp` | 2, 5 | Add `line_`/`column_offset_` members; ctor takes interner |
| `akkado/include/akkado/mini_token.hpp` | 5 | `MiniTokenValue` variant: drop `std::string` arm; add `SymbolId`/`StringLitData` |
| `akkado/include/akkado/symbol_table.hpp` | 5 | Keys/lookup on `SymbolId` |
| `akkado/include/akkado/token.hpp` | 5 | Same as `mini_token.hpp`; remove `as_string()`, add `as_identifier(interner)`/`as_string_lit()` |
| `akkado/include/akkado/voicing.hpp` | 4 | `VoicingRegistry` class wrapping the existing fns |
| `akkado/src/akkado.cpp` | 1b, 4, 5 | Construct default ctx if null; merge mini-literal diagnostics; pass interner to lex |
| `akkado/src/analyzer.cpp` | 1b, 5 | Treat input arena as const; resolve `SymbolId` → view for diagnostics |
| `akkado/src/codegen.cpp` | 1a, 3, 4 | Drop `const_cast` + remove `PreResolved` alloc at `:1008,1018`; drop `const_cast` + replace in-place reorder at `:3233`; emit_*-method impls; ctx threading |
| `akkado/src/codegen_patterns.cpp` | 1b, 3, 4 | Replace 4 `const_cast` re-parses with sub-arena reads; replace 10 helper call sites; 4 voicing-registry call sites (`:4538`, `:4540`, `:4803`, `:4904`) route through `ctx_->voicing_registry` |
| `akkado/src/codegen_higher_order.cpp` | 3 | Rewrite line 699 helper call |
| `akkado/src/codegen_arrays.cpp` | 3 | Internal helper-call rewrites |
| `akkado/src/lexer.cpp` | 5 | `make_token(type, intern(text))` everywhere |
| `akkado/src/mini_lexer.cpp` | 2, 5 | `advance` bumps `line_`; `current_location` uses local `line_`; interning |
| `akkado/src/mini_parser.cpp` | 1b | `parse_sample_atom` opportunistically calls `parse_chord_symbol(sample.name)` and caches result on `MiniAtomData`'s chord fields (F3 tail) |
| `akkado/src/parser.cpp` | 1b, 2, 5 | Sub-arena mini-parse; Phase 2 adds a one-line clarifying comment above the no-op cast at `:1455` (F7 withdrawn, no code change); 15+ identifier-handling sites use `SymbolId` |
| `akkado/src/pattern_eval.cpp` | 1b | `MiniAtomKind::Sample` chord-mode branch reads cached `chord_root` (and siblings) from `MiniAtomData` instead of calling `parse_chord_symbol`; drop `chord_parser.hpp` include (F3 tail) |
| `akkado/src/shape_index.cpp` | 5 | Interner-aware identifier handling (shape_index AST share is PRD-2, not here) |
| `akkado/src/symbol_table.cpp` | 5 | Lookup on `SymbolId`; remove 16+ FNV rehash sites |
| `akkado/src/voicing.cpp` | 4 | Delete process-globals; impl `VoicingRegistry` |
| `akkado/tests/test_codegen.cpp` | 1a,1b,3,4 | F1a/F1b/F2/F14 regression tests |
| `akkado/tests/test_lexer.cpp` | 5 | F12 interner tests |
| `akkado/tests/test_mini_notation.cpp` | 2, 1b | F8 multi-line position tests; F3 chord-cache parser tests |
| `akkado/tests/test_parser.cpp` | 2 | F7 right-assoc tests |
| `akkado/tests/test_pattern_eval.cpp` | 1b | F3 chord-mode event-stream parity + `parse_chord_symbol` scan invariant (new file if not already present) |
| `akkado/tests/test_symbol_table.cpp` | 5 | F12 lookup-by-id tests |

### Files explicitly NOT changed

| Path | Reason |
|---|---|
| `cedar/**` | Cedar VM untouched |
| `akkado/include/akkado/builtins.hpp` | Builtin table format unchanged (Phase 5 SymbolId lookup uses string-view query, not table-key change) |
| `tools/akkado-cli/**`, `tools/nkido-cli/**`, `web/wasm/**` | Public `compile()` API change is additive; existing callers compile unchanged |

---

## 10. Sourcing for Key Design Decisions

| Decision | Where set |
|---|---|
| All 6 critical findings in scope | Round 1 Q1 |
| One PRD with phases per finding | Round 1 Q2 |
| F1 first, others land alongside | Round 1 Q3 |
| Per-bug tests + cross-cutting invariants | Round 1 Q4 |
| Per-pattern sub-arena owned by MiniLiteralData | Round 2 Q1 |
| Codegen helpers become `CodeGenerator&` methods | Round 2 Q2 |
| Per-compile interner, view-into-source | Round 2 Q3 |
| Introduce minimal CompileContext now | Round 2 Q4 |
| `^` fix to right-associative | Round 3 Q1 — *superseded* by Phase 0 verification (2026-05-25): `^` is already right-assoc, no fix needed |
| CompileContext holds VoicingRegistry + StringInterner only | Round 3 Q2 |
| Intern all identifiers + keywords + builtin names | Round 3 Q3 |
| Invariants asserted in `generate()` + checked by every codegen test | Round 3 Q4 |
| F1 split into two sub-phases (PreResolved first, mini-parse second) | Round 4 Q1 |
| CompileContext arg optional, defaults to fresh context | Round 4 Q2 |
| Token: `std::string` arm replaced by `SymbolId` for identifier-like uses | Round 4 Q3 |
| Phase 0 snapshot harness added as prerequisite | Review pass 2026-05-25 |
| `PreResolved` removed entirely; side-table keyed by `(call_node, arg_position)`; no synthetic arena | Review pass 2026-05-25 |
| Phase 1a also addresses `reorder_spread_named_args` in-place arena mutation | Review pass 2026-05-25 |
| `MiniLiteralData` introduced as a new variant arm on `Node::data` (no such struct exists today) | Review pass 2026-05-25 |
| Voicing free fn names corrected: `register_voicing` / `lookup_voicing` (not `addVoicing` / `lookupVoicing`) | Review pass 2026-05-25 |
| Structural-hash invariant (not byte-hash) | Review pass 2026-05-25 |
| F7 withdrawn: `^` already right-assoc on master; Phase 2 ships regression tests only | Phase 0 verification 2026-05-25 (commit `b203e2e`) |
| Phase 5 perf criterion → "PR must include before/after numbers" | Review pass 2026-05-25 |
| F3's 5th re-parse site (`pattern_eval.cpp:206`) folded into Phase 1b scope (Sample-atom chord caching) so the audit's "parse-once-store-once" goal lands in one phase | Review pass 2026-05-25 (post-filing) |

---

## 11. Per-Phase Documentation Maintenance Protocol

Each phase PR must include doc updates as part of the same commit (or a
follow-up commit in the same PR, never deferred to a later PR). Two
files are updated per phase: **this PRD** and the **source audit doc**.

### 11.1 PRD status-block edit template

Open the status block at the top of this file. Replace the
`NOT STARTED` line with a phase-progress tally and append a per-phase
bullet. Example after Phase 1a ships:

```markdown
> **Status: IN PROGRESS — Phase 1a SHIPPED, 4 phases remaining.**
> Filed 2026-05-25 as the correctness follow-up to
> [`docs/audits/parser-codegen-interop_audit_2026-05-25.md`](audits/parser-codegen-interop_audit_2026-05-25.md).
>
> - **Phase 1a (PreResolved → side table) — SHIPPED.** Commit `<hash>`,
>   `<YYYY-MM-DD>`. `NodeType::PreResolved` removed; codegen no longer
>   mutates `ast_->arena` for spread expansion. See §4 Phase 1a.
```

After the final phase, flip to:

```markdown
> **Status: DONE — All 5 phases shipped.** Filed 2026-05-25; closed
> <YYYY-MM-DD>. All 6 critical correctness findings from
> [`docs/audits/parser-codegen-interop_audit_2026-05-25.md`](audits/parser-codegen-interop_audit_2026-05-25.md)
> resolved.
>
> - Phase 1a … `<hash>` `<date>`
> - Phase 1b … `<hash>` `<date>`
> - Phase 2  … `<hash>` `<date>`
> - Phase 3  … `<hash>` `<date>`
> - Phase 4  … `<hash>` `<date>`
> - Phase 5  … `<hash>` `<date>`
```

### 11.2 Audit-doc edit template

Open `docs/audits/parser-codegen-interop_audit_2026-05-25.md`. Two
edits per finding-resolution:

**Edit 1 — finding header.** Find the `### F<N>. <Title> — *<Severity>*`
heading in §2 and append a resolved tag and commit backlink:

```markdown
### F1. Codegen mutates the post-parse AST — blocks every form of parallelism — *Critical*

> **RESOLVED 2026-MM-DD** by Phase 1a commit `<hash1a>` (PreResolved
> side-table) and Phase 1b commit `<hash1b>` (mini-AST sub-arena). See
> [`docs/prd-parser-codegen-correctness.md`](../prd-parser-codegen-correctness.md).
```

**Edit 2 — PRD shortlist row.** In §5 of the audit, find the
`### PRD-N — <Title>` heading whose scope covers the finding and tag
it as shipped:

```markdown
### PRD-1 — Codegen no longer mutates the AST  *(Critical; blocks 3+ other wins)*

> **SHIPPED via `prd-parser-codegen-correctness.md` Phase 1a + 1b**,
> commits `<hash1a>` + `<hash1b>`, <YYYY-MM-DD>.
```

For phases that touch more than one finding, add the appropriate tag
to **each** finding header. Phase 2 marks F8 as RESOLVED, and marks F7
as **WITHDRAWN** rather than resolved:

```markdown
### F7. Right-associative `^` parses left-associative — *Critical*

> **WITHDRAWN 2026-05-25** during Phase 0 verification (commit `b203e2e`).
> `^` is already right-associative on master: `2^3^2 → 512`. The
> audit's Pratt analysis was flawed — the left-assoc branch passes
> `p+1` while the right-assoc branch passes `p`, and the recursive
> call accepts another `^` (since `p ≥ p`). See
> [`docs/prd-parser-codegen-correctness.md`](../prd-parser-codegen-correctness.md)
> §1.3. No code change shipped; Phase 2 of the PRD adds a regression
> test to lock the behavior.
```

The audit's executive-summary line was updated when this PRD was filed
(2026-05-25) to enumerate all six findings — when F7 is marked
WITHDRAWN, also update the executive-summary tally from "6 critical"
to "5 critical + 1 withdrawn" without rewriting the enumeration.
When Phase 5 ships, append a one-line "5 resolved, 1 withdrawn" status
marker beneath the enumeration.

### 11.3 Finding ↔ Phase ↔ PRD-shortlist map

For convenience when editing:

| Finding | Phase | Audit PRD-shortlist row to flip |
|---|---|---|
| F1 (codegen AST mutation) | 1a + 1b | PRD-1 |
| F2 (source-loc desync) | 3 | PRD-5 (note: PRD-5 also bundled F10/builder work, **not** in scope here — only mark the F2 portion shipped) |
| F3 (mini-notation re-parsed 5×) | 1b | PRD-1 (folded into the same row as F1; mark F3 resolved alongside F1) |
| F7 (`^` right-assoc) | 2 (test-only) | PRD-10 (mark F7 **withdrawn** — Phase 0 verified `^` already right-assoc, no fix needed; Phase 2 ships regression tests only. Rest of PRD-10 — Pratt table unification — stays open.) |
| F8 (mini-lexer multi-line) | 2 | PRD-8 (mark line-tracking shipped; rest of PRD-8 — lex_primitives extract — stays open) |
| F12 (string interning) | 5 | PRD-9 |
| F14 (voicing leak) | 4 | PRD-11 (mark voicing-registry-per-compile shipped; rest of PRD-11 — full CompileOptions — stays open) |

**Important:** several phases address **part of** a larger audit PRD.
When editing the audit's PRD-shortlist rows, prefer "partially shipped"
language over flipping the whole row, so the still-open complexity
work remains visible.

### 11.4 What NOT to edit

- Do **not** edit the audit's executive summary except at the very end
  (Phase 5) to flip the headline tally.
- Do **not** delete content from the audit doc — append RESOLVED tags
  in place. The audit is a historical record; preserve its diagnostic
  value.
- Do **not** edit other audit findings (the complexity-sink section)
  unless a separate PRD ships them.

---

## 12. Open Questions

None. All design decisions resolved across the 4 question rounds; this
section exists as a placeholder for any deferrals identified during
implementation (which should be appended here with timestamps).

---

## 13. Follow-ups Unblocked by This PRD

Tracked in `docs/audits/parser-codegen-interop_audit_2026-05-25.md`:

- **PRD-2 (shape_index over shared AST)** — directly unblocked by Phase
  1a + 1b. `shape_index` can stop re-lex+re-parsing and consume the
  analyzer's `output_arena_` directly.
- **PRD-3 (per-import parallel front-end)** — voicing-registry-mutex
  gone (Phase 4) + concurrent-ready interner API (Phase 5) remove the
  two known concurrency obstacles.
- **PRD-11 (full CompileOptions consolidation)** — `CompileContext`
  scaffold from Phase 4 is the home for the eventual full options
  migration.
