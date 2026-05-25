# Parser / Codegen Interop & Redundancy Audit

**Date:** 2026-05-25
**Scope:** full akkado front-end pipeline — lexer, parser, AST, analyzer, const-eval, pattern-eval, shape-index, codegen, orchestration
**Depth:** exhaustive forensic
**Method:** five parallel agent investigators (lexer / parser / AST-handover / codegen-sprawl / cross-pass), each reading their stage in full; this document deduplicates and ranks across all five reports
**LOC in scope:** 75,783 (akkado/ tree)

---

## Executive summary

The akkado compiler is structurally clean for a 75 KLOC code-base — one mutex, one process-global mutable map, no static state in lex/parse/analyze/codegen instance fields. But the *interfaces between stages* have rotted: every consumer of the AST has its own ad-hoc walker, every dispatcher table is repeated 3–7×, and the codegen layer has been growing by accretion (110 commits to `codegen.cpp` alone) without a coherent extension model. Two correctness bugs and one architectural pretense fall out of this:

1. **Codegen mutates the AST in place** (`PreResolved` node injection + mini-notation re-parse via `const_cast`), silently breaking the immutability assumption that downstream readers like `shape_index` rely on, and foreclosing every form of pass-parallelism the rest of the architecture is otherwise ready for.
2. **Source-location wiring silently desynchronises**: ~12 helper-emission sites push to `instructions_` without pushing to the parallel `source_locations_` vector, corrupting click-to-source for every instruction emitted afterwards.
3. **Lexers don't intern strings at all**, despite the architecture overview's headline claim of FNV-1a string interning — identifiers are copied 4-5 times and rehashed 16+ times per compile.

The dominant simplification opportunity is the codegen `visit()` Call branch (**1,180 lines** in a single switch arm) plus its 7-way dispatcher fragmentation. The dominant parallelisation opportunity is splitting front-end work per import file — imports are *already resolved as a DAG* and then immediately collapsed into a single byte stream before tokenisation, throwing away the parallelism the import graph proves safe.

---

## 1. Scope and methodology

### Files reviewed (75,783 LOC total)

```
akkado/include/akkado/
  ast.hpp (572)                    parser.hpp                     typed_value.hpp (380)
  lexer.hpp                        mini_parser.hpp                analyzer.hpp
  mini_lexer.hpp (138)             chord_parser.hpp               codegen.hpp (1423)
  token.hpp (209)                  builtins.hpp (1723)            const_eval.hpp
  mini_token.hpp (204)             diagnostics.hpp                pattern_eval.hpp
  symbol_table.hpp (273)           file_resolver.hpp              shape_index.hpp
  source_map.hpp                   import_scanner.hpp             pattern_debug.hpp
  codegen/{codegen,helpers,arrays,literals,fm_detection,options}.hpp
  music_theory.hpp                 tuning.hpp                     voicing.hpp

akkado/src/
  lexer.cpp (607)                  parser.cpp (2218)              analyzer.cpp (2954)
  mini_lexer.cpp (904)             mini_parser.cpp (651)          const_eval.cpp (608)
  chord_parser.cpp                 pattern_eval.cpp (781)         shape_index.cpp (478)
  symbol_table.cpp                 source_map.cpp                 codegen.cpp (3898)
  codegen_patterns.cpp (5953)      codegen_functions.cpp (3061)   codegen_arrays.cpp (1722)
  codegen_higher_order.cpp (830)   codegen_stereo.cpp (707)       codegen_state.cpp (563)
  codegen_control_flow.cpp (377)   codegen_viz.cpp (417)          codegen_params.cpp (416)
  codegen/options.cpp              akkado.cpp                     pattern_debug.cpp
  import_scanner.cpp               file_resolver.cpp              voicing.cpp (305)
  diagnostics.cpp                  tuning.cpp                     typed_value.cpp
```

### Method

Five parallel forensic investigators were dispatched. Each was given a self-contained brief, file list with offsets for the largest files, and a structured output format. Reports were merged here with deduplication; identical findings flagged by two agents are listed once with both source citations.

### How to read this doc

Findings are ranked by **severity for the stated goal** ("reduce complexity across parser+codegen, clean AST handover, identify parallelisation"). Each finding lists:
- **File:line** citations
- **Mechanism** (what's wrong)
- **Why it bites** (concrete consequence)
- **Fix sketch** (one-paragraph direction)
- **Severity** (Critical / High / Medium / Low)

The PRD shortlist at the end groups findings into actionable refactors with rough effort tags.

---

## 2. Top findings (ranked)

### F1. Codegen mutates the post-parse AST — blocks every form of parallelism — *Critical*

**Sites:**
- `codegen.cpp:1017` — `expand_call_arguments` allocates synthetic `NodeType::PreResolved` nodes directly into the analyzer's `output_arena_` during spread expansion.
- `codegen_patterns.cpp:1778, 2132, 2182, 2982` — four sites re-parse mini-notation strings into the same arena via `const_cast<AstArena&>(ast_->arena)`. Triggered by `chord()`, generic `StringLit` pattern args, chord-inside-transform, and `timeline()` curve strings.

**Mechanism.** After parsing+analysis the AST is conceptually frozen, but codegen splices new nodes into the arena mid-traversal. Both sites are workarounds: `PreResolved` is a side-channel for spread expansion that already has a side-table (`pre_resolved_values_` at `codegen.hpp:1194`); the const_cast'd mini-parse is because the parser stored only the raw string and codegen needs the parsed mini-AST.

**Why it bites.**
- `shape_index` (`shape_index.cpp`) cannot share the main compile's AST because it might be mid-mutation — instead it does a **full re-lex + re-parse on every editor cursor move** (`shape_index.cpp:429-435`), 478 LOC of duplicate pipeline.
- `PatternEvaluator` cannot run in parallel across patterns because the arena it reads could be growing.
- The future LSP cannot hand the AST to multiple inspection clients without copying.
- Arena indices stored in side-tables become invalid mid-codegen if the arena resizes.

**Fix sketch.**
- Move `PreResolved` payload entirely into the existing `pre_resolved_values_` side-table; remove the node-kind. Spread expansion looks up by parent node index + arg index instead of allocating a node.
- Parse all mini-notation strings into per-pattern sub-arenas during the parser pass and store the sub-arena handle on the `MiniLiteralData`; codegen's chord/timeline/transform handlers consume the pre-parsed sub-arena instead of re-parsing.
- Mark `Ast::arena` `const` in every post-analyzer signature.

**Severity: Critical.** Single biggest architectural blocker in the codebase.

---

### F2. Source-location vector silently desynchronises from instruction vector — *Critical*

**Sites:**
- `codegen.hpp:1139` declares `current_source_loc_`. `codegen.cpp:215` mutates it in every `visit()` entry. `codegen.cpp:2272` (the `emit()` body) is the only site that pushes onto `source_locations_`.
- Helpers `codegen::emit_push_const` (`codegen/helpers.hpp:31`) and `codegen::emit_zero` push to the instruction stream directly without touching `source_locations_`.
- ~12 callers of those helpers do not manually correct: `codegen_patterns.cpp:3158, 3181, 4262, 4338, 4424, 4464, 4499, 5096, 5156, 5263`; `codegen_higher_order.cpp:699`; `codegen.cpp:320, 901, 923`; sites inside `codegen_arrays.cpp`. Only `codegen_patterns.cpp:5103, 5163, 5270` manually push to `source_locations_`.

**Why it bites.** Each missing push silently shifts the parallel arrays by one entry from that point forward, mis-mapping every subsequent instruction to a different source location. Click-to-source highlighting (`web/`) and `--trace` output (`tools/nkido-cli`) will point at the wrong line for any program that triggers a desynced helper. This is data corruption that no test catches today because no test asserts vector lengths or the round-trip "instruction[N] ↔ source_locations[N]" invariant.

**Fix sketch.** Make `emit()` the sole instruction-push path. Either move the helpers into `CodeGenerator&` methods that go through `emit()`, or have them take a `SourceLocation` arg and forbid the source-location-less form. Add a test that asserts `instructions_.size() == source_locations_.size()` after every `generate()`.

**Severity: Critical** (latent correctness bug + observability erosion).

---

### F3. Mini-notation re-parsed up to 5× per string, each at a different stage — *High*

**Sites:**
- `parser.cpp:1744` — initial parse during `parse_mini_literal`. Correct.
- `codegen_patterns.cpp:1778, 2132, 2182, 2982` — four codegen-time re-parses (see F1).
- `pattern_eval.cpp:206` — `PatternEvaluator` re-parses chord symbols at evaluation time, bypassing the data already in `MiniAtomData::chord_root/chord_quality/chord_intervals` set by `parse_chord_atom` at `mini_parser.cpp:332-352`.

**Why it bites.** Three sources of truth for chord parsing alone (mini_lexer → chord_parser → pattern_eval), each with subtly different rules — `mini_lexer.cpp:621-632` carries an octave-vs-quality heuristic the standalone parser doesn't know about. A single failure cascades into different diagnostics or silent fallbacks depending on which layer caught it.

**Fix sketch.** Parse-once-store-once: every `MiniLiteralData` carries a parsed sub-arena pointer; chord-symbol fields on `MiniAtomData` are authoritative; downstream consumers read fields, never re-parse strings.

**Severity: High.** Direct prerequisite to F1.

---

### F4. `visit()` Call branch is a 1,180-line single switch arm — *High*

**Site:** `codegen.cpp:966-2143` — `case NodeType::Call:` inside the giant `visit()` function (which is itself **2,059 lines**, `codegen.cpp:203-2261`).

**Inside the Call branch.** A 100-entry `special_handlers` static map (`codegen.cpp:1068`); five inlined "default-fill PUSH_CONST" blocks (`:1335`, `:1761`, `:1862`, `:1989` + the BUILTIN_VARIABLES path at `:616`); spread expansion (calls into `expand_call_arguments` which itself mutates the AST per F1); chord-expansion branch; stereo-native branch (~130 lines, `:1700-1830`); SAMPLE_PLAY scalar branch; FM-detection retrofit; ADSR/delay rate-field special-casing; generic emission tail.

Other monoliths in codegen.cpp:
- `emit_bus_epilogue` — 234 lines (`:2739`).
- `handle_field_access` — 202 lines (`:3488`).
- `handle_record_literal` — 113 lines (`:3375`).
- `reorder_spread_named_args` — 146 lines (`:3230`), 90% mirror of analyzer's 142-line equivalent.

**Fix sketch.** Three new files, ~250 lines remain in codegen.cpp:
- `codegen_visit_dispatch.cpp` — outer switch + literal handlers.
- `codegen_call_dispatch.cpp` — the Call branch, split into 5 sub-helpers (default-fill / chord-expand / stereo-native / scalar-SAMPLE_PLAY / generic), with `special_handlers` extracted to a generated table driven by `BuiltinInfo` annotations.
- `codegen_bus.cpp` — `handle_bus_call`, `handle_mixer_call`, `inline_mixer_closure`, `scan_closure_for_sinks`, `emit_bus_epilogue` (~620 lines).
- `codegen_records.cpp` — record + field + pipe-binding + destructure (~750 lines).

**Severity: High.** Highest-payoff codegen refactor; the Call branch is where every new feature gets added and every regression hides.

---

### F5. Front-end concatenates all sources before lex — forecloses per-import parallelism — *High*

**Sites:**
- `akkado.cpp:90-137` — `combined_source` built by string-appending stdlib + embedded stdlib + every resolved import + user source, with a `SourceMap::Region` recorded per chunk.
- `akkado.cpp:140` — `lex(combined_source, …)` operates on the fused buffer; the per-file boundary is gone before tokenisation.

**Why it bites.** Imports are *already resolved as a dependency DAG* by `import_scanner.cpp:155-206` (with cycle detection). The architecture proves at resolution time which modules are independent, then immediately throws away the parallelism. Each import is then re-read from disk on every compile (no source cache, no AST cache), and re-lexed/re-parsed serially.

**Fix sketch.**
- Stop concatenating. Lex+parse each `ResolvedModule` on its own thread into a per-module `vector<Token>` + per-module `AstArena` + per-module diagnostics vector.
- Join phase: re-index `NodeIndex` by adding base offsets (the arena already uses indices, not pointers — see `CLAUDE.md` arena description), concatenate diagnostics, merge `SourceMap` regions, hand the unified arena to the analyzer.
- Build an AST cache keyed by `(import_path, file_mtime, content_hash)` so unchanged imports skip the whole front-end on subsequent compiles. The web live-coding loop recompiles every keystroke; this is the single biggest latency win.

**Severity: High.** Practical, well-scoped, large payoff.

---

### F6. Dispatcher fragmentation — 7 parallel tables over the same name space — *High*

**The 7 tables:**
1. `BUILTIN_FUNCTIONS` — 231 entries, `builtins.hpp:268`.
2. `BUILTIN_ALIASES` — 38 entries, `builtins.hpp:1664`.
3. `BUILTIN_VARIABLES` — 3 entries, `builtins.hpp:1717`.
4. `special_handlers` — 100 entries, `codegen.cpp:1068`.
5. `visit()` outer switch — 26 NodeType cases, `codegen.cpp:230`.
6. `visit()` BinaryOp switch, `codegen.cpp:2167`, mirrored by `binary_name_to_opcode` at `codegen_arrays.cpp:1042`.
7. FM-detection trio (`is_audio_rate_producer`, `is_upgradeable_oscillator`, `upgrade_for_fm` in `codegen/fm_detection.hpp:14-66`) — three separate switches enumerating the oscillator family.

**Plus parser-side Pratt redundancy:**
- `Parser::get_precedence` (`parser.cpp:184-203`)
- `Parser::is_infix_operator` (`parser.cpp:205-226`)
- `Parser::parse_infix` (`parser.cpp:702-726`)
- `Parser::parse_binary` (`parser.cpp:1431-1451`) — also duplicates `binop_function_name` at `ast.hpp:160-169`

Four switches enumerating the same operator set.

**Why it bites.** Adding a new operator or builtin requires touching 3-4 places; nothing keeps them in sync. The `special_handlers` table contains ~40 names that also exist in `BUILTIN_FUNCTIONS` with no enforced consistency. `BUILTIN_ALIASES` is not consulted before `special_handlers` lookup, so alias dispatch is path-dependent.

**Fix sketch.**
- Parser: single `OpInfo[]` table with `{token, precedence, associativity, builtin_name}`; all four switches become table walks.
- Codegen: extend `BuiltinInfo` with an optional `codegen_handler` member-fn-ptr field; eliminate `special_handlers`. Alias resolution lives in `lookup_builtin` and is consulted *before* the handler dispatch.
- Generate the FM-family enumerations from a single `OscillatorKind` table at build time (mirror what Cedar already does for `opcode_metadata.hpp`).

**Severity: High.**

---

### F7. Pratt right-associativity for `^` is silently broken — *High*

**Site:** `parser.cpp:1457-1463` — the "right-associative power" code has both branches setting `next_prec` to the same value (`static_cast<Precedence>(static_cast<int>(next_prec))` is a no-op). Power is parsed left-associatively despite the comment claiming otherwise.

**Why it bites.** `2 ^ 3 ^ 2` parses as `(2^3)^2 = 64` instead of the mathematical `2^(3^2) = 512`. Trivially testable, no test catches it.

**Fix sketch.** Either commit to left-assoc (delete the comment + branch) or fix the right-assoc by recursing at `next_prec` (no `+1`).

**Severity: High** (correctness bug, low-effort fix, but ships wrong math today).

---

### F8. Mini-lexer multi-line source positions are broken — *High*

**Site:** `mini_lexer.cpp:73-77` (`advance` only bumps `column_`, never `line_`); `mini_lexer.cpp:146-153` (`current_location` reuses `base_location_.line` unconditionally).

**Why it bites.** `lexer.cpp:393` explicitly allows multi-line mini-notation strings ("Allow multi-line strings for mini-notation"). A diagnostic emitted from inside such a pattern reports the wrong line. The `column = base_location_.column + start_` formula at `mini_lexer.cpp:149` also corrupts column tracking once `start_` exceeds the line's actual end.

**Fix sketch.** Mirror `lexer.cpp`'s `update_location` discipline — bump `line_` and reset `column_` on every `\n`, never accumulate `start_` into `column`.

**Severity: High** (diagnostic quality today, will worsen as multi-line patterns become idiomatic).

---

### F9. Pattern transform handlers — ~1,500 lines of boilerplate clones — *High*

**Sites:** `codegen_patterns.cpp` — `handle_bank_call` (161 lines, `:3583`), `handle_variant_call` (213, `:3744`), `handle_transport_call` (193, `:3957`), `handle_tune_call` (71, `:4150`), plus `palindrome`/`compress`/`zoom`/`segment`/`iter`/`iterBack`/`anchor`/`mode`/`voicing` (30-50 each, `:4221-4908`).

**Shared shape (every one of the above):**
```
get_pattern_arg → compile_pattern_for_transform → push_path → allocate
value/velocity/trigger triple → emit SEQPAT_QUERY → emit SEQPAT_STEP →
push StateInitData.
```

Compare e.g. `codegen_patterns.cpp:3636-3660` vs `:3795-3820` vs `:3974-4000` — same 30-line block thrice, differing only in the inner mutator.

**Fix sketch.** A `PatternTransformEmitter` helper taking `(transform_name, StateInitPayload, BuiltinInfo*)` and a per-transform lambda for the inner work. Net: shrinks ~1500 lines to ~600 and makes adding a new transform a 10-line job. Each accreted "we forgot to copy field X across all handlers" follow-up commit (`b485c3f`, `c56e65f`, `c3802b6`, `2f710ef`) becomes impossible.

**Severity: High.**

---

### F10. `StateInitData` constructed field-by-field at 19 sites — *High*

**Sites:** 19 `state_inits_.push_back` calls total; 14 in `codegen_patterns.cpp` alone. Representative duplication: `codegen_patterns.cpp:1369-1380` (SequenceProgram) is byte-for-byte repeated at `:1719, :1869, :2867, :3685, :3900, :4118`. EventTransform (`codegen_higher_order.cpp:787-793`), RateScale, Reorder, Fanout, Timeline all replicate the same boilerplate per `StateInitData::Type`.

**Why it bites.** Every commit that adds a field to `StateInitData` must touch all 19 sites; missing one silently breaks one transform family. The Phase-2/3/4 event-transform follow-up commits are this pattern in practice.

**Fix sketch.** `StateInitBuilder` with one factory method per `Type`: `StateInitBuilder::sequence_program(state_id).cycle_length(c).sequences(s).publish(*this)`. The single existing centraliser — `emit_extended_params_init` (`codegen.cpp:35-73`) — is bug-free precedent.

**Severity: High.**

---

### F11. Mini-lexer is a near-clone of main lexer — *High*

**Files:** `lexer.cpp` (607) vs `mini_lexer.cpp` (904). Concrete duplication (file:line on each side):

| Concern | `lexer.cpp` | `mini_lexer.cpp` |
|---|---|---|
| `is_at_end` / `peek` / `peek_next` / `advance` / `match` | 59-84 | 54-84 |
| `is_digit` / `is_alpha` / `is_whitespace` | 86-102 | 86-106 |
| `make_token` overloads (3 variants) | 104-137 | 108-138 |
| Numeric scan (`lex_number`) | 317-383 | 426-518 (3 near-clones for value/note/plain) |
| Velocity-suffix scanner | n/a | 488-504, 671-686, 759-775 (3× in same file) |
| `chain_reaches_digit` lambda | n/a | 174-187 and 715-728 (identical lambdas) |

Pitch-MIDI semitone table identical in both (`lexer.cpp:540`, `mini_lexer.cpp:228`), down to the same comment. Token enums share ~25% of values (`Comma`, `Colon`, brackets, `Star`, `Slash`, `At`, `Bang`, `Question`, `Number`, `Error`, `Eof` etc).

**Fix sketch.** Extract a `lex_primitives.hpp` with `CursorBase` (source ptr + start_/current_/column_/line_, peek/advance/match), shared `scan_number`, shared `scan_velocity_suffix`, shared `parse_pitch_to_midi`, shared char classifiers. Either lexer composes it. **Estimated removal: 250-300 LOC** of mechanical duplication.

**Severity: High** (also where the F8 fix lands naturally).

---

### F12. Lexers don't intern strings — identifiers re-hashed 16+ times per compile — *High*

**Mechanism.** `grep -rn 'Symbol\|symbol_table' akkado/src/lexer.cpp akkado/src/mini_lexer.cpp` returns nothing. The lexers produce `std::string` payloads (`lexer.cpp:464`). Each identifier then:
1. allocates in `Token.value` as `std::string` (`lexer.cpp:464`)
2. is also stored as `lexeme` view (`lexer.cpp:113`)
3. is copied into AST `IdentifierData{std::string}` at parser.cpp:513 and 14 other sites
4. is copied again into `Symbol.name` (`symbol_table.cpp:32`)
5. is FNV-hashed on every `lookup` (`symbol_table.cpp:119` and 15+ sites)

Hot identifiers like `freq`, `gate`, `vel` get rehashed dozens of times per compile. The architecture overview in `CLAUDE.md` claims "String interning with FNV-1a hashing for fast identifier comparison" — the actual lexer ignores it.

**Why it bites.** Cache locality (every identifier reference dereferences a pointer to a heap `std::string`); concurrency (any future per-statement parallel parser needs concurrent interning — designing it now is cheap, retrofitting later is hard).

**Fix sketch.** Promote `SymbolTable`'s interner to a `StringInterner` owned by the top-level `CompileContext`; lexers receive a reference and emit `Symbol` handles (`uint32 id + interned string_view`) instead of `std::string`. AST nodes carry the handle. Hash computed once at intern time.

**Severity: High** (the architectural pretense is broken; downstream work compounds).

---

### F13. `shape_index` reimplements the front-end on every keystroke — *High*

**Site:** `shape_index.cpp:424-435` calls `lex(source, "<shape-index>")` then `parse(tokens, …)` — a complete re-lex + re-parse — driven from `web/wasm/nkido_wasm.cpp:1380` on every editor cursor move. 478 LOC of duplicate machinery to compute the same Record/Pattern/Array shape info that `collect_definitions` already produces and stores in the SymbolTable.

**Why it bites.** Quadratic web-IDE latency: every keystroke triggers (a) the debounced compile cycle, and (b) a shape-index re-lex+re-parse, both touching the same source.

**Fix sketch.** Once F1 is in (no AST mutation by codegen), `shape_index` reads the analyzer's `output_arena_` + `SymbolTable` directly. The 478 LOC collapses to perhaps 80 LOC of "format the data the analyzer already has". Editor latency drops to one parse pass.

**Severity: High.**

---

### F14. `voicing_registry` is process-global and leaks state across compiles — *High*

**Site:** `voicing.cpp:15-30` — `voicing_registry()` is a function-local `static std::unordered_map` guarded by a `static std::mutex registry_mutex()`. Mutated by codegen's `addVoicings()` handler.

**Why it bites.**
- Re-defining a voicing with the same name in source A persists into the next compile of source B — cross-compile state leak that survives full `CodeGenerator` reset.
- Only mutex in the compiler; defeats lock-free per-compile parallelism unless removed.

**Fix sketch.** Move the registry into a per-compile `CompileContext` carried through `CodeGenerator`. Drop `registry_mutex()`. Single-customer (called only from `codegen_patterns.cpp`, 17 sites all in one file), so the blast radius is contained.

**Severity: High** (correctness bug + only concurrency obstacle in compiler globals).

---

### F15. Multiple "is this a const / pattern / literal?" recognizers — *High*

**The MIDI→Hz dup:**
- `const_eval.cpp:41`: `double hz = 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);`
- `codegen_functions.cpp:41`: same line verbatim.

**Const-value recognizer:**
- `ConstEvaluator::eval` (`const_eval.cpp:31-87`) handles 11 node kinds.
- `resolve_const_value` (`codegen_functions.cpp:32-74`) handles 5 of them with no shared code. Exists because codegen's "is this arg a compile-time constant?" check must not have side effects.

**Pattern-producer recognizer:**
- `SemanticAnalyzer::is_pattern_producing_expr` (`analyzer.cpp:11-40`) lists `chord, seq`.
- `shape_index.cpp:139-168` (`is_pattern_producer`, `rhs_is_pattern`) lists `seq, timeline, sample, chord` — **already disagree**.
- `analyzer.cpp:571-582` AND `analyzer.cpp:732-741` both hardcode `call_name == "chord" || call_name == "seq"` for binding `PatternInfo`. Two more copies inside the analyzer alone.

**`reorder_named_arguments`:** `analyzer.cpp:2680-2822` (142 lines, BuiltinInfo overload), `analyzer.cpp:2823-2964` (142 lines, UserFunction overload), `codegen.cpp:3230-3375` (146 lines, spread variant) — **three near-mirrors totaling 430 lines**.

**Fix sketch.** One pure `expr_kinds.hpp` module with `is_const_evaluable`, `is_pattern_producer`, `is_literal_value`, sharing a single `NodeType` switch. ConstEvaluator becomes the single MIDI→Hz site, called by both layers. `reorder_named_arguments` becomes one parameterised helper + two predicates.

**Severity: High.**

---

## 3. By-stage notes

### 3.1 Lexer / token layer

In addition to F11 (lexer near-clone) and F12 (no interning):

- **Token-shape redundancy.** `Token` (`token.hpp:181-207`) and `MiniToken` (`mini_token.hpp:161-202`) are structurally isomorphic. Could become `BasicToken<EnumT, ValueT>`. Parallel 60-case `token_type_name` switches in `token.hpp:100-164` and `mini_token.hpp:80-113`, both with silent-default branches. (*Medium*)
- **`MiniLexer::lex_token` 5-flag state machine.** `mini_lexer.cpp:242-424` — `curve_mode_`, `value_mode_`, `note_mode_`, `sample_only_`, `last_was_modifier_`, `paren_depth_` all gate which scan path runs. The "snapshot-then-clear `last_was_modifier_`" comment at `:252-255` and "force numeric inside `(`" at `:254` are accretion markers. Highest-churn file in the lexer layer (17 commits). (*High* — but the F11 extract is the natural moment to address this.)
- **Dead code.** `TokenType::MiniString` declared at `token.hpp:90,159` but never produced (only test_lexer.cpp:839 mentions). `MiniLexer` `bool`-overload ctor (`mini_lexer.hpp:38-39, cpp:31-33`) has no production callers. `codegen/literals.hpp::make_push_const` and `make_mtof` are dead (`grep` returns no users). (*Low*)

### 3.2 Parser

In addition to F6 (Pratt redundancy) and F7 (`^` right-assoc bug):

- **AST builder repetition.** 40 `make_node` call sites + 18 raw `arena_.alloc` sites in parser.cpp. Each follows `node = make_node(type, tok); arena_[node].data = …Data{…};` — no wrapping helper. `parse_binary` (`:1431-1487`) wraps each operand in `Argument` with `nullopt` 5 times, exactly duplicated in `parse_unary_not` (`:856-873`) and the diamond synth (`:1528-1548`). A `wrap_positional_arg(NodeIndex)` helper erases ~30 lines. (*High*)
- **Inconsistent error recovery.** Three styles coexist — helper-routed (`error`/`error_at`/`error_with_code` set `panic_mode_`), direct `diagnostics_.push_back` (parser.cpp:276, 482, 2187 — bypasses panic mode), warnings. `MiniParser::error_at` (`mini_parser.cpp:75-83`) has no panic mode at all → a bad 4-line pattern can emit 20 cascading errors. `chord_parser.cpp` returns `nullopt` silently, no diagnostic ever reaches the user. (*Medium*)
- **5 hand-rolled save/restore lookahead sites.** `parse_grouping` (`parser.cpp:986-1115`, 130 lines of manual paren-depth scanning to disambiguate `(expr)` vs `(params) -> body`), statement-level destructure detection (`:390-433`), `loop_form_ahead` (`:907-927`), `parse_hole` dotted-vs-dotless (`:819-844`), `parse_argument` named-vs-positional (`:1677-1692`), `parse_closure_body` record-vs-block (`:1391-1404`). Each implements paren-depth tracking differently. (*Medium-High* — `parse_grouping`'s 130-line block is the worst, fixable with a leading keyword or distinct production.)
- **Dead AST kinds.** `BinOp` / `BinaryOpData` exist but the parser desugars to `Call(IdentifierData{"add"})` directly (`parser.cpp:1468-1471`); the BinaryOp node-kind and `binop_function_name` switch (`ast.hpp:160-169`) are post-parser-dead. (*Medium*)

### 3.3 AST handover

In addition to F1 (codegen AST mutation) and F13 (shape_index re-pipeline):

- **Ghost fields stored in `data` rather than child list.** `MatchArmData::guard_node`, `ArgumentData::spread_source`, `RecordLitData::spread_source`, `DestructureField::default_node`, `HoleData::field_name`, `ClosureParamData::annotated_type`. Each forces every traversal (especially `clone_subtree` at `analyzer.cpp:1262-1321` and the substitute path at `:1508-1568`) to special-case. ~80 lines of pure ghost-field bookkeeping. (*High* — refactor to a uniform `Node::extra_children[]` slot.)
- **40 NodeType kinds × 25-arm `std::variant`.** One alternative — `PreResolved` — exists *only* so codegen can splice (see F1). `MiniAtomData` (`ast.hpp:227-248`) has 11 fields including chord-only fields wasted on every Pitch/Sample/Rest atom. (*Medium*)
- **Dispatch count by file** (every consumer reinventing AST traversal):
  | File | Switches over NodeType |
  |---|---|
  | `codegen.cpp` | 29 |
  | `codegen_patterns.cpp` | 28 |
  | `pattern_eval.cpp` | 18 |
  | `analyzer.cpp` | 10 + ~80 inline `n.type ==` |
  | `const_eval.cpp` | 11 |
  | `shape_index.cpp` | 7 + ad-hoc chains |
- **Min 4 full AST traversals per compile** (collect_definitions, rewrite_pipes, resolve_and_validate, codegen visit); PatternEvaluator + ConstEvaluator are sub-tree, invoked many times from codegen. (*Medium*)
- **11 side tables** in `CodeGenerator` alone (`codegen.hpp:1189-1408`): `node_types_`, `pre_resolved_values_`, `param_literals_`, `param_string_defaults_`, `param_function_refs_`, `param_multi_buffer_sources_`, `stereo_outputs_`, `stereo_buffer_pairs_`, `polyphonic_pattern_nodes_`, plus `node_map_` and `foreach_record_param_` in analyzer. (*Medium* — bookkeeping bloat, but better than baking the slots into the AST.)
- **`pattern_eval` and `codegen_patterns` walk the same Mini\* subtree with different outputs.** `pattern_eval.cpp:146-176` `eval_pattern` vs `codegen_patterns.cpp:763-826` `SequenceCompiler::compile` — identical traversal shape, no shared walker. Could merge to a single visitor producing both event stream and bytecode. (*Medium*)

### 3.4 Codegen sprawl

In addition to F4 (visit() Call branch), F9 (transform boilerplate), F10 (StateInitData):

- **File organisation has no rule.** Two parallel layouts:  `akkado/include/akkado/codegen/` (subdir, six small `inline` helper headers) AND `akkado/include/akkado/codegen.hpp` (1423-line monolith) + `akkado/src/codegen_*.cpp` (flat). Headers in `codegen/` are included only by some sources (e.g. `codegen_state.cpp` includes none of them); `codegen/literals.hpp` is entirely dead. Either fold everything in `codegen/` back into `codegen.hpp` or move all sources under `src/codegen/`. (*High* — affects every contributor's mental model.)
- **Slot/input parsing repetition.** 126 sites with `cedar::Instruction X{}` declarations; 606 bare `0xFFFF` sentinels across the 10 cpps; 48 manual `inputs[0] = 0xFFFF` inits; 177 `buffers_.allocate()` sites each followed by ~4 lines of `if (out == BUFFER_UNUSED) { error("E101", "Buffer pool exhausted", …); … }`. The `"Buffer pool exhausted"` string literal appears 167 times. A `set_unused_inputs()` helper at `helpers.hpp:156` is called *once*. The `Instruction::make_unary/make_binary` factories on `cedar::Instruction` are used only for COPY (~25 sites). (*High* — write a thin `InstructionBuilder` that defaults all 5 inputs to `0xFFFF`, takes named setters, and routes through a single `emit()` that handles buffer-allocation failure once.)
- **`codegen_patterns.cpp` is not a megapass** — it's `SequenceCompiler` (1199 lines, `:77-1275`) + `compile_pattern_for_transform` (573 lines, `:2096`) + a pile of 30-200-line handlers. Natural split: extract `SequenceCompiler` to `pattern_compiler.cpp`, group transforms into `codegen_pattern_transforms.cpp`, group I/O builtins (midi, soundfont, smooch, wt_load, samples) into `codegen_pattern_io.cpp`. (*High*)
- **Codegen overlaps analyzer work.** `fn_call_counts_` pre-pass at `codegen.cpp:1078-1080` re-walks AST counting call sites — analyzer work. E160 polyphonic-pattern enforcement (`codegen.cpp:1432-1446`) checks types the analyzer's `BuiltinInfo.param_types` already encode. Defensive `lookup_builtin` calls at codegen because the analyzer's validation isn't expressed in the type system. (*Medium*)
- **`codegen_viz.cpp` (417 LOC) collapsible to ~100.** 5 handlers (pianoroll/oscilloscope/waveform/spectrum/waterfall), all the same shape: validate signal arg → visit → name → options → push_path → state_id → push VisualizationDecl → emit PROBE. Add `BuiltinInfo.kind = Visualization` + a single emitter. Same for `codegen_params.cpp` (416 → ~120) — 4 handlers param/button/toggle/select. (*High*)
- **Stereo handling cluster.** `codegen_stereo.cpp:88-572` is 11 hand-written handlers because stereo channels-as-adjacency is a side-channel — the second buffer index isn't in any AST node. `handle_pingpong` (`:572`, ~130 lines) duplicates ExtendedParams-init instead of going through `emit_extended_params_init`, called out in `CLAUDE.md`. (*Medium*)
- **`apply_lambda` cache-thrashes per iteration.** `codegen_arrays.cpp:76-230` — `auto saved_node_types = std::move(node_types_); node_types_.clear();` runs every map() iteration. For map over a 32-element array with a 3-instruction lambda, that's 32× the inner visit cost + 32 cache flushes. Significant codegen-time perf debt AND a parallelisation blocker. (*Medium*)
- **`emit_event_transform` = 385 lines** (`codegen_higher_order.cpp:436-820`) — closure compilation + event-bank scratch allocation + write-mask construction + transform-owned SequenceState all in one function. Split into 3-4 stages. (*Medium*)
- **Churn concentration.** 110 commits on codegen.cpp + 83 on codegen_patterns.cpp + 41 on codegen_functions.cpp = **82% of all codegen commits** in just 3 files. These are the hot zones for any refactor. (*N/A — informational*)

### 3.5 Cross-pass + orchestration

In addition to F5 (concatenation) and F14 (voicing registry):

- **SymbolTable re-registers 600+ builtins on every compile.** `symbol_table.cpp:236-264` is run from the `SymbolTable()` ctor (line 5-9) on every analyzer construction. Could be replaced by a process-shared frozen-hash builtin scope chained as parent of the per-compile scope; or a perfect-hash table (`frozen::map`) given the set is closed and build-time-known. (*Medium*)
- **`serialize_mini_ast_json` runs unconditionally** at `codegen_patterns.cpp:1379` for every pattern in every compile — even `nkido-cli` and `akkado-cli --check` which never read it. Gate behind `CompilerOptions::emit_debug_json = false` (default) for headless; WASM build sets true. (*Medium* — trivial fix, real CPU savings.)
- **No incremental cache.** Every compile re-reads imports from disk, re-lexes stdlib + embedded stdlib + every import + user source, re-parses, re-builds symbol table, runs analyzer 3-pass, runs full codegen. In live-coding workflow (compile every ~200ms during keystroke debounce) this is heavy. (*Medium-High* — addressed naturally by F5.)
- **`compile()` public surface is 6 positional args** mixing config and dependencies — `source, filename, sample_registry, file_resolver, lint_strict, bypass_master`. Roll into one `CompileOptions` with optional fields. (*Low*)
- **No partial-compile entry points.** `lex()` and `parse()` are declared in their headers but there is no public "give me the AST" API for tooling; `shape_index` and any future LSP must drive the full pipeline. (*Low*)

---

## 4. Parallelisation landscape

### What blocks parallelism today

1. **Codegen mutates the AST** (F1) — `PreResolved` injection + mini-notation re-parse via `const_cast`. Any reader running concurrently with codegen sees torn arena state.
2. **`node_types_` is a side-channel** (`codegen.hpp:1189`) — used both as cached evaluation result AND as inter-handler data channel. Splitting these two roles is a prerequisite to per-statement parallel codegen.
3. **`voicing_registry` is process-global** (F14).
4. **Sources concatenated pre-lex** (F5) — destroys per-import independence.
5. **`apply_lambda` mutates `node_types_` in-place per iteration** — see §3.4. Breaks any "map() in parallel" scheme.

### Opportunities, ranked by feasibility

1. **Per-import front-end fan-out (F5)** — lex + parse + mini-parse each `ResolvedModule` on its own thread, merge arenas at join. Front-end latency drops to `max(per-file)` from `sum(per-file)`. Prerequisite: stop concatenating; teach `SourceMap` about per-region offsets. **High value, medium effort.**
2. **Per-pattern parallel mini-parse + evaluate.** Mini-literals are pre-tokenized by the main lexer; their string content is opaque to the main grammar. After F1 (parse-once-store-once), all mini-strings in a file can `parse_mini` in parallel, and `PatternEvaluator::evaluate_pattern` (pure function over frozen AST) can fan out per pattern. **High value, low risk** once F1 is done.
3. **Frozen builtin scope as parent of per-compile SymbolTable.** Build a `static const SymbolScope kBuiltinScope` once at process init; chain `scopes_[0] = &kBuiltinScope`. Saves ~600 inserts per compile, free thread-safety. **Medium value, low effort.**
4. **Per-function-body codegen.** L2 BLOCK_CALL shared-block compilation (`get_or_compile_shared_block`, `codegen_functions.cpp:1008`) is *already* speculative + rollbackable via `BufferAllocator::reset_to`. Compiling each `fn` in a fresh `CodeGenerator` instance and splicing the result parallelises naturally for fns that don't reference outer state. Aligns with existing architecture. **Medium value, medium effort.**
5. **Background-thread pattern-debug JSON.** Once F1 hoists `serialize_mini_ast_json` out of the codegen hot path, it can run on a worker thread that joins before the result returns. Or simpler: gate behind `emit_debug_json` and skip entirely for CLI. **Medium value, trivial effort.**
6. **Per-statement parallel codegen at Program level.** `case NodeType::Program` in `visit()` iterates top-level statements. Parallelisable if (a) each gets a private `BufferAllocator` range, (b) `node_types_` is per-thread, (c) state_id paths seed per-statement, (d) merge concatenates instructions in source order. Lower payoff (top-level typically <12 statements) and high refactor cost. **Lower value, high effort.**

### What stays sequential

Analyzer: symbol table is global per program; pipe rewriting transforms a single arena. Splitting requires per-module sub-analysis with cross-module name resolution at the join — invasive redesign of `SymbolTable` into a chained-scope structure. Not blocking the architecture today.

---

## 5. Follow-up PRD shortlist

Ranked by ROI (impact ÷ effort). Each is a self-contained refactor; most can be done in parallel.

### PRD-1 — Codegen no longer mutates the AST  *(Critical; blocks 3+ other wins)*
**Scope:** Eliminate the two AST-mutation sites: move `PreResolved` payload into the existing `pre_resolved_values_` side-table and remove the node kind; parse every mini-notation string into a per-pattern sub-arena at parse time and store the handle on `MiniLiteralData`, removing all 4 codegen-time re-parses. Mark `Ast::arena` `const` post-analyzer.
**Files touched:** `ast.hpp`, `parser.cpp`, `mini_parser.cpp`, `codegen.cpp:1017`, `codegen_patterns.cpp:1778, 2132, 2182, 2982`.
**Effort:** Medium (1-2 weeks).
**Unlocks:** PRD-2 (shape_index), PRD-3 (parallel mini-eval), per-pattern parallel pattern_eval.

### PRD-2 — `shape_index` shares the analyzer's AST  *(High)*
**Scope:** Once PRD-1 lands, delete the re-lex+re-parse pipeline in `shape_index.cpp:424-435`. Replace with a thin formatter over `SymbolTable` + `output_arena_`. Web IDE keystroke latency drops to one parse pass.
**Files touched:** `shape_index.cpp` (478 LOC → est. 80 LOC), `nkido_wasm.cpp:1380`.
**Effort:** Small (3-5 days). Blocked by PRD-1.

### PRD-3 — Per-import front-end parallelism  *(High)*
**Scope:** Stop concatenating sources in `akkado.cpp:90-137`. Lex+parse each `ResolvedModule` on its own thread into per-module token/AST/diagnostic vectors. Build a merge step that re-indexes `NodeIndex` and concatenates `SourceMap` regions. Add a content-hash-keyed AST cache for unchanged imports.
**Files touched:** `akkado.cpp`, `import_scanner.cpp`, `source_map.cpp`, `parser.cpp` (re-indexing helper).
**Effort:** Medium-Large (2-3 weeks).
**Unlocks:** Largest single live-coding latency win; foundation for any future LSP.

### PRD-4 — Codegen file split + visit() Call-branch refactor  *(High)*
**Scope:** Split `codegen.cpp` (3,898 LOC) into 5 files per §F4. Extract the 100-entry `special_handlers` table; replace with a `codegen_handler` member-fn-ptr on `BuiltinInfo`. Split the Call branch into named sub-helpers (default-fill, chord-expand, stereo-native, scalar-SAMPLE_PLAY, generic). Decide one rule for `akkado/include/akkado/codegen/` vs the flat `codegen.hpp` and apply uniformly.
**Files touched:** `codegen.hpp`, `codegen.cpp` + 3 new files, `builtins.hpp` (handler field).
**Effort:** Medium-Large (2-3 weeks).
**Unlocks:** PRD-5, PRD-6; cuts contributor friction for every codegen feature thereafter.

### PRD-5 — `StateInitBuilder` + `InstructionBuilder` + source-loc fix  *(High; bundles 3 problems)*
**Scope:** Bundle three correctness/complexity wins.
- `InstructionBuilder` with named setters, default-`0xFFFF` inputs, single failure path → eliminates 606 bare sentinels + 177 manual buffer-alloc-failure blocks.
- `StateInitBuilder` factory per `Type` → eliminates the 19 push-back duplications + the "forgot to copy field X" bug class.
- Fix the source-location desync (F2): make `emit()` the only push path; helpers either go through it or take a `SourceLocation` arg; add `assert(instructions_.size() == source_locations_.size())` to every codegen test.
**Files touched:** `codegen.hpp`, `codegen.cpp`, `codegen/helpers.hpp`, every `codegen_*.cpp`.
**Effort:** Medium (1-2 weeks).

### PRD-6 — Pattern-transform handler consolidation  *(High; ~900 LOC reduction)*
**Scope:** Replace the 8+ near-clone pattern-transform handlers with a `PatternTransformEmitter` helper taking `(transform_name, payload_mutator)`. Adding a new transform becomes a 10-line job.
**Files touched:** `codegen_patterns.cpp:3583-4908` (est. 1500 → 600 LOC).
**Effort:** Medium (1-2 weeks). Best after PRD-5.

### PRD-7 — Data-driven viz + param handler families  *(Medium; ~600 LOC reduction)*
**Scope:** Extend `BuiltinInfo` with `kind = Visualization | Param | …` and target-specific metadata; collapse `codegen_viz.cpp` (417 → ~100) and `codegen_params.cpp` (416 → ~120) to single emitters.
**Files touched:** `builtins.hpp`, `codegen_viz.cpp`, `codegen_params.cpp`.
**Effort:** Small-Medium (1 week).

### PRD-8 — Shared lexer primitives  *(High but bundle-only)*
**Scope:** Extract `lex_primitives.hpp` (`CursorBase`, `scan_number`, `scan_velocity_suffix`, `parse_pitch_to_midi`, classifiers). Lexers compose it. Fixes F8 multi-line source-position bug as a side effect. Estimated 250-300 LOC removal.
**Files touched:** New `lex_primitives.hpp`, `lexer.cpp`, `mini_lexer.cpp`, `token.hpp`/`mini_token.hpp` (optional template-based merge).
**Effort:** Medium (1-2 weeks).

### PRD-9 — Real string interning at lex time  *(High; foundational)*
**Scope:** Promote `StringInterner` to a top-level `CompileContext` owned object; lexers emit `Symbol` handles instead of `std::string` payloads; AST `IdentifierData` carries the handle. Hash computed once at intern time.
**Files touched:** `symbol_table.hpp`, `token.hpp`/`mini_token.hpp`, `lexer.cpp`, `mini_lexer.cpp`, `parser.cpp` (15+ sites), `ast.hpp`, `analyzer.cpp`.
**Effort:** Medium-Large (2-3 weeks). Best paired with PRD-8.

### PRD-10 — Pratt operator table unification + `^` fix  *(High-recall, small)*
**Scope:** Single `OpInfo[]` table over the 4 Pratt switches. Fix right-assoc `^` (F7). Drop the dead `BinOp`/`BinaryOpData`/`binop_function_name` path.
**Files touched:** `parser.hpp`, `parser.cpp:184-203, 205-226, 702-726, 1431-1487`, `ast.hpp:160-169`.
**Effort:** Small (3-5 days). One correctness bug fixed.

### PRD-11 — `CompileOptions` + voicing-registry-per-compile + debug-JSON gate  *(Medium; bundles 3 cleanups)*
**Scope:** Roll the 6-arg `compile()` into `CompileOptions`. Move `voicing_registry()` (`voicing.cpp:15-30`) into the options object, eliminating the process-global mutex and cross-compile state leak. Add `emit_debug_json` field defaulting false; CLI/headless stop paying `serialize_mini_ast_json` cost.
**Files touched:** `akkado.hpp`, `akkado.cpp`, `voicing.cpp`/`.hpp`, `codegen_patterns.cpp:1379`, CLI/WASM entry points.
**Effort:** Small (3-5 days).

### PRD-12 — Frozen builtin scope as parent of SymbolTable  *(Medium)*
**Scope:** Stop running 600+ builtin inserts in every `SymbolTable()` construction. Use `frozen::map` for `BUILTIN_FUNCTIONS` lookup at the build-time-known set; chain a single process-shared scope as `scopes_[0]`.
**Files touched:** `symbol_table.hpp`/`.cpp`, `builtins.hpp` (frozen-map adoption).
**Effort:** Small (3-5 days).

### PRD-13 — Eliminate const-eval + pattern-recognizer duplication  *(Medium)*
**Scope:** One `expr_kinds.hpp` module with `is_const_evaluable`, `is_pattern_producer`, `is_literal_value`. ConstEvaluator becomes the single MIDI→Hz site (called by codegen_functions). `reorder_named_arguments` parameterised into one helper.
**Files touched:** New `expr_kinds.hpp`, `const_eval.cpp`, `codegen_functions.cpp`, `analyzer.cpp` (2 sites + 2 inline checks), `shape_index.cpp`.
**Effort:** Small (3-5 days).

### PRD-14 — AST handover hardening  *(Medium; foundational for future LSP)*
**Scope:** Move ghost-field children (`MatchArmData::guard_node`, spreads, destructure defaults) into a uniform `Node::extra_children[]` slot. Eliminates 80+ lines of special-cased bookkeeping in `clone_subtree` / substitute paths.
**Files touched:** `ast.hpp`, `analyzer.cpp:1262-1321, 1508-1568`, every consumer that traverses (codegen, pattern_debug).
**Effort:** Medium (1-2 weeks).

### PRD-15 — Dead-code sweep  *(Low; one-PR cleanup)*
**Scope:** Remove `TokenType::MiniString` (declared, never produced); `MiniLexer` `bool`-overload ctor; `codegen/literals.hpp::make_push_const` + `make_mtof`; the post-parser dead `BinOp`/`BinaryOpData` path (covered by PRD-10).
**Effort:** Small (1 day).

---

## 6. Suggested execution order

```
Wave 1 (parallel): PRD-1, PRD-5, PRD-10, PRD-11, PRD-15
Wave 2 (parallel): PRD-2, PRD-4, PRD-12, PRD-13
Wave 3 (parallel): PRD-3, PRD-6, PRD-7, PRD-9
Wave 4: PRD-8, PRD-14
```

Wave 1 is correctness-and-foundations. Wave 2 picks up the unblocked downstream wins. Wave 3 is the parallelism + larger restructures. Wave 4 is the deepest refactors. The whole programme is roughly 8-12 weeks of focused work with one engineer or ~5 weeks split across two.

---

## Appendix A — finding-to-PRD map

| Finding | PRD |
|---|---|
| F1 — Codegen mutates AST | PRD-1 |
| F2 — Source-location desync | PRD-5 |
| F3 — Mini-notation re-parsed 5× | PRD-1 |
| F4 — visit() Call branch monolith | PRD-4 |
| F5 — Pre-lex source concatenation | PRD-3 |
| F6 — Dispatcher fragmentation | PRD-4 + PRD-10 |
| F7 — Right-assoc `^` broken | PRD-10 |
| F8 — Mini-lexer multi-line source positions | PRD-8 |
| F9 — Pattern transform boilerplate | PRD-6 |
| F10 — StateInitData manual construction | PRD-5 |
| F11 — Mini-lexer near-clone | PRD-8 |
| F12 — No string interning | PRD-9 |
| F13 — shape_index full re-pipeline | PRD-2 |
| F14 — voicing_registry leak | PRD-11 |
| F15 — Const/pattern recognizer dup | PRD-13 |

## Appendix B — agent reports

Five forensic agents produced ~6,000 words of source-cited findings each. Their full reports are preserved in the audit task transcripts. This document is the synthesised dedup.
