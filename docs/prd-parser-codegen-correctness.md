> **Status: NOT STARTED** — Filed 2026-05-25 as the correctness follow-up
> to [`docs/audits/parser-codegen-interop_audit_2026-05-25.md`](audits/parser-codegen-interop_audit_2026-05-25.md).
> Phases land independently after Phase 1a; the audit's complexity-sink
> findings are deferred to separate PRDs.
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
findings** that produce wrong outputs today or break architectural
invariants the rest of the codebase relies on. This PRD addresses all
six in a coordinated rollout. Each finding becomes one phase, shipped as
an independent PR after Phase 1a unblocks the rest. The audit's
complexity-sink findings (codegen sprawl, dispatcher fragmentation,
pattern-transform boilerplate) are explicitly **out of scope** here and
covered by separate PRDs.

The six findings:

| # | Finding | Severity | Phase |
|---|---|---|---|
| F1 | Codegen mutates the post-parse AST (5 sites) | Critical | 1a + 1b |
| F2 | Source-location vector silently desynchronises | Critical | 3 |
| F7 | Right-associative `^` parses left-associative | Critical | 2 |
| F8 | Mini-lexer never bumps `line_` across `\n` | Critical | 2 |
| F14 | `voicing_registry` leaks state across compiles | Critical | 4 |
| F12 | Lexers don't intern strings (16× rehash per compile) | Critical | 5 |

**Key Design Decisions** (locked — see §10 for sourcing):

- **Phase 1a first; everything else parallelizes after.** The codegen
  AST-mutation refactor (F1a — `PreResolved`) unblocks `shape_index` and
  future parallel pass work. Phases 2–5 are independent and may land in
  any order once Phase 1a is in.
- **Per-pattern mini-notation sub-arena owned by `MiniLiteralData`.**
  Each `MiniLiteralData` carries a `std::unique_ptr<AstArena>` populated
  at parse time. The 4 `const_cast<AstArena&>` codegen-time re-parses
  are deleted. Sub-arena destroyed with the literal; thread-local during
  future parallel mini-parse.
- **Codegen emit helpers become `CodeGenerator&` methods.** The free
  `codegen::emit_push_const(buffers_, stream_, val)` shape is deleted;
  the method `cg.emit_push_const(val)` routes through `emit()` which
  unconditionally writes both `instructions_` AND `source_locations_`.
  The bug becomes structurally impossible to reintroduce.
- **`^` becomes truly right-associative.** `2^3^2 == 512`. The audit
  confirms this was always the intent (per the comment at
  `parser.cpp:1455`). Stdlib + tests swept to confirm no callers rely
  on the broken left-assoc behavior.
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
  2. After codegen, the input `Ast::arena` byte-hash is unchanged from
     pre-codegen (asserts no codegen-side mutation crept back in).

---

## 1. Problem Statement / Current State Inventory

### 1.1 F1 — Codegen mutates the post-parse AST

| Site | File:line | What it does |
|---|---|---|
| PreResolved alloc | `codegen.cpp:1017` | `arena.alloc(NodeType::PreResolved, ea.loc)` inside `expand_call_arguments` writes a synthetic node into the analyzer's `output_arena_` during spread expansion. Value already lives in side table `pre_resolved_values_` (`codegen.hpp:1194`). |
| Chord re-parse | `codegen_patterns.cpp:1778` | `parse_mini(chord_str, const_cast<AstArena&>(ast_->arena), …)` parses chord string into the same arena codegen is reading from. |
| Generic pattern arg re-parse | `codegen_patterns.cpp:2133` | Same `const_cast` pattern for any string-literal pattern argument. |
| Chord-in-transform re-parse | `codegen_patterns.cpp:2183` | Same pattern, inside pattern transforms. |
| Timeline curve re-parse | `codegen_patterns.cpp:2983` | Same pattern, for `timeline(t"…")` curve strings. |

Why it bites:

- `shape_index` cannot share the main compile's AST because it might be
  mid-mutation — instead it re-lexes + re-parses on every editor cursor
  move (`shape_index.cpp:424-435`, 478 LOC of duplicate pipeline).
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

### 1.3 F7 — Right-associative `^` is silently broken

Site: `parser.cpp:1455-1463`:

```cpp
// For right-associative (^), use lower precedence
Precedence next_prec = get_precedence(op.type);
if (op.type == TokenType::Caret) {
    // Power is right-associative
    next_prec = static_cast<Precedence>(static_cast<int>(next_prec));   // ← no-op
} else {
    // Left-associative: increment to bind tighter on right
    next_prec = static_cast<Precedence>(static_cast<int>(next_prec) + 1);
}
```

The "right-associative" branch is a no-op cast; the only thing that
differs from the left-assoc branch is the absent `+ 1`. Pratt
right-assoc semantics require `parse_precedence(next_prec)` to recurse
at the **same** precedence as the operator — which would be correct **if
the left-assoc branch added 1** — but `parse_precedence` itself returns
when it sees an operator of precedence `< min`, and both branches end up
with the same minimum, so `^` always binds left.

Observable: `2 ^ 3 ^ 2` parses as `(2^3)^2 = 64` instead of the
mathematical / Python / Haskell `2^(3^2) = 512`. No test catches it.

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

Mutated by codegen's `addVoicings()` handler (17 call sites in
`codegen_patterns.cpp`). Two problems:

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

---

## 2. Goals and Non-Goals

### Goals

1. **Eliminate every codegen-side write to `Ast::arena`.** After this
   PRD, `Ast::arena` is `const` in every post-analyzer signature.
2. **Make `instructions_.size() == source_locations_.size()`
   structurally true.** A single `emit()` path; helpers route through
   it. The invariant is `assert()`-ed at the end of `generate()`.
3. **`2^3^2 == 512`.** Right-associative `^` behaves correctly.
4. **Multi-line mini-notation patterns report correct line/column** in
   every diagnostic.
5. **No cross-compile state in compiler globals.** `voicing_registry`
   moves into a per-compile context. Compiler globals reduce to
   read-only metadata (`BUILTIN_FUNCTIONS`, `CHORD_INTERVALS`, etc).
6. **Per-compile string interner.** Every identifier-bearing token
   carries a `SymbolId(u32)`, not a `std::string`. Lookup is O(1) with
   no rehash. Identifier copies through the parser/analyzer chain go
   away.
7. **Every fixed bug has a precise regression test** + the two
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

Phase 1a removes the one mutating call site in `expand_call_arguments`
(`codegen.cpp:1010-1025`). Replacement: index the `pre_resolved_values_`
side table by `(parent_arg_node_index, position_within_call)` — or, more
robustly, allocate a stable sentinel `NodeIndex` from a **separate
small arena** owned by `CodeGenerator`, used only for these synthetic
nodes. The `NodeType::PreResolved` kind is then removed entirely; the
generic `visit()` Call branch's "is this a PreResolved arg?" check
becomes a lookup in `pre_resolved_values_`.

Phase 1b removes the 4 `const_cast<AstArena&>` sites by parsing mini-
notation strings at parse time. `MiniLiteralData` gains a sub-arena:

```cpp
struct MiniLiteralData {
    std::string raw;                              // existing
    NodeIndex   mode_marker;                      // existing
    std::unique_ptr<AstArena> mini_arena;         // ← new
    NodeIndex   mini_root = NULL_NODE;            // ← new (index into mini_arena)
    std::vector<Diagnostic> mini_diagnostics;     // ← new (parser pre-collected)
};
```

The parser at `parser.cpp:1744` already calls `parse_mini(...)` and
stitches into the main arena. Phase 1b changes it to:

1. Construct a fresh `AstArena` for the literal.
2. Parse into that sub-arena, capturing root index + diagnostics.
3. Store both on `MiniLiteralData`.
4. **Stop stitching mini nodes as children of the `MiniLiteral` main-
   arena node** — they live in the sub-arena instead.

Codegen reads `MiniLiteralData::mini_arena` + `mini_root` instead of
re-parsing. The chord/transform/timeline call sites use the same
sub-arena handle on `MiniLiteralData::mini_arena`.

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
`addVoicing(name, dict)` / `lookupVoicing(name)` become methods. The 17
call sites in `codegen_patterns.cpp` route through
`ctx_->voicing_registry->…`.

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

Plus a debug-build invariant: the input `Ast::arena` byte-hash is
recorded before codegen begins and re-checked at the end; assert that
it hasn't changed.

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

### Phase 1a — `PreResolved` to side table (F1 part 1)

**Scope.** Remove `NodeType::PreResolved`. Move `pre_resolved_values_`
keyed by a stable identifier that doesn't require minting a real AST
node.

**Approach.** `CodeGenerator` gains a small per-compile auxiliary
arena (`AstArena synthetic_arena_`) used only for synthetic nodes that
codegen needs to mint. `expand_call_arguments` allocates synthetic
`Argument`-shaped placeholder indices into `synthetic_arena_`, never
into `ast_->arena`. The `pre_resolved_values_` map keys on the
`(synthetic_arena_, synthetic_node)` pair. The Call branch in `visit()`
checks for `NodeType::PreResolved` (now removed) — replaced with "if
arg's first child resolves to a `pre_resolved_values_` entry, use the
cached `TypedValue`".

**Files touched.**

| File | Change |
|---|---|
| `akkado/include/akkado/ast.hpp` | Remove `NodeType::PreResolved` and `PreResolvedData` variant arm. |
| `akkado/include/akkado/codegen.hpp` | Add `synthetic_arena_` member; change `pre_resolved_values_` key shape. |
| `akkado/src/codegen.cpp:1010-1025` | Replace `arena.alloc(NodeType::PreResolved, …)` with synthetic-arena alloc. |
| `akkado/src/codegen.cpp` (Call branch) | Replace PreResolved switch arm with side-table lookup before generic emit. |
| `akkado/tests/test_codegen.cpp` | Add Phase 1a regression test (see §7). |

**Exit criteria.**

- `grep -rn 'PreResolved' akkado/` returns no hits in `src/` or
  `include/` (tests + comments may reference it as historical).
- Debug `assert(ast_->arena.size() == initial_size)` at end of
  `generate()` passes for every fixture.
- All existing codegen tests pass byte-identical bytecode.
- **Docs updated per §11 protocol** — PRD status block reflects
  `Phase 1a: SHIPPED <commit> <date>`; audit doc marks F1 (part 1)
  resolved with backlink to the same commit.

---

### Phase 1b — Mini-notation parse-at-parse-time (F1 part 2)

**Scope.** Parse mini-notation strings into per-pattern sub-arenas at
parse time. Delete the 4 `const_cast<AstArena&>` codegen-time re-parses.

**Approach.** `MiniLiteralData` gains `std::unique_ptr<AstArena>
mini_arena`, `NodeIndex mini_root`, and `std::vector<Diagnostic>
mini_diagnostics`. `Parser::parse_mini_literal` (`parser.cpp:1705-1762`)
constructs the sub-arena, parses into it, captures diagnostics, and
stops stitching mini nodes as main-arena children. The 4 codegen sites
read `mini_arena` instead of re-parsing.

**Files touched.**

| File | Change |
|---|---|
| `akkado/include/akkado/ast.hpp` | Add 3 fields to `MiniLiteralData`. |
| `akkado/src/parser.cpp:1705-1762` | Switch to sub-arena parsing; no main-arena child stitching. |
| `akkado/src/codegen_patterns.cpp:1778` | Replace `parse_mini(..., const_cast<AstArena&>)` with read of `mini_literal_data.mini_arena`. |
| `akkado/src/codegen_patterns.cpp:2133` | Same. |
| `akkado/src/codegen_patterns.cpp:2183` | Same. |
| `akkado/src/codegen_patterns.cpp:2983` | Same. |
| `akkado/src/akkado.cpp` | Merge `MiniLiteralData::mini_diagnostics` into per-pass diagnostics with `SourceMap::adjust_all`. |
| `akkado/tests/test_codegen.cpp` | Phase 1b regression test (no codegen-time arena growth). |

**Exit criteria.**

- `grep -rn 'const_cast.*AstArena' akkado/src/` returns zero hits.
- Every codegen function signature taking `Ast&` is rewritten to
  `const Ast&`.
- All existing mini-notation tests pass byte-identical sequence output.
- **Docs updated per §11 protocol** — PRD status block reflects
  `Phase 1b: SHIPPED <commit> <date>`; audit doc marks F1 (part 2)
  resolved and updates the F1 row to "resolved" overall.

---

### Phase 2 — `^` right-assoc + mini-lexer line tracking (F7 + F8)

Bundled because both are tiny, both are parser-layer, and both ship
their regression test as a single PR.

**F7 fix.** `parser.cpp:1455-1463` — the right-assoc branch recurses at
`current_prec` (one *below* the operator's binding precedence, allowing
the same operator to bind again on the right):

```cpp
// Right-associative: recurse at same precedence (Pratt convention)
Precedence next_prec = (op.type == TokenType::Caret)
    ? get_precedence(op.type)
    : static_cast<Precedence>(static_cast<int>(get_precedence(op.type)) + 1);
NodeIndex right = parse_precedence(next_prec);
```

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
| `akkado/src/parser.cpp:1455-1463` | Right-assoc fix. |
| `akkado/src/mini_lexer.cpp:73-77, 146-153` | Line tracking. |
| `akkado/include/akkado/mini_lexer.hpp` | Add `line_`, `column_offset_` members. |
| `akkado/tests/test_parser.cpp` | `2^3^2 == 512` + tower assoc tests. |
| `akkado/tests/test_mini_notation.cpp` | Multi-line pattern diagnostic line/column tests. |
| Sweep | `grep -nE '\^.*\^' akkado/stdlib/ akkado/tests/ web/static/docs/` to confirm no caller relied on broken left-assoc. |

**Exit criteria.**

- `2 ^ 3 ^ 2` evaluates to 512 in const_eval, codegen, and any test
  using power.
- Multi-line mini-pattern `"c4 d4\ne4 f4"` reports line 2 for `e4`'s
  diagnostic.
- Stdlib sweep finds zero callers needing left-assoc `^`.
- **Docs updated per §11 protocol** — PRD status block reflects
  `Phase 2: SHIPPED <commit> <date>`; audit doc marks F7 + F8 resolved.

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
| `akkado/src/voicing.cpp:13-30` | Delete `voicing_registry()` + `registry_mutex()`. Move logic into `VoicingRegistry`. |
| `akkado/include/akkado/akkado.hpp:105` | Add optional `CompileContext* ctx = nullptr` parameter. |
| `akkado/src/akkado.cpp` | Construct default ctx if null; pass to `CodeGenerator`. |
| `akkado/include/akkado/codegen.hpp` | Hold `CompileContext*` member; receive in ctor. |
| `akkado/src/codegen_patterns.cpp` | 17 call sites: `addVoicing(...)` → `ctx_->voicing_registry->define(...)`; `lookupVoicing(...)` → `ctx_->voicing_registry->lookup(...)`. |
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
- Microbench: identifier-heavy compile (~50KB stdlib) shows measurable
  reduction in `std::string` allocations (visible via heap profiler /
  ASan stats).
- **Docs updated per §11 protocol** — PRD status block reflects
  `Phase 5: SHIPPED <commit> <date>`; audit doc marks F12 resolved;
  audit doc's overall "all 6 critical correctness findings" tally
  flipped to "all resolved".

---

## 5. Phase Dependencies and Order

```
Phase 1a (PreResolved)  ───┐
                           ├──> Phase 1b (mini-AST)
                           │
                           ├──> Phase 2 (^ + mini-lexer)        [parallel]
                           ├──> Phase 3 (source-loc emit)       [parallel]
                           ├──> Phase 4 (CompileContext+voicing) [parallel]
                           │                       │
                           │                       v
                           └──>             Phase 5 (interner)
```

- Phase 1a ships first; nothing else depends on it strictly but it
  proves out the "no codegen mutation" pattern.
- Phase 1b depends on Phase 1a being merged (both touch the codegen
  invariant; want to fix one mutation site cleanly before the next).
- Phases 2, 3, 4 are independent of each other and of Phase 1b. Can
  ship in parallel by different contributors.
- Phase 5 depends on Phase 4 (`CompileContext` must exist to hold the
  interner). Otherwise independent of Phases 1a/1b/2/3.

Estimated total effort: **6–9 weeks single-engineer**, **3–4 weeks**
if Phases 2/3/4 parallelize across 2 contributors.

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
| `MiniLiteralData` | **Modified** | Gains 3 fields (sub-arena, root, diagnostics). |
| `pre_resolved_values_` key shape | **Modified** | Now keyed by synthetic arena index, not main arena. |
| `NodeType::PreResolved` | **Removed** | Side-table replaces it. |
| `codegen::emit_push_const`, `emit_zero` (free fns) | **Removed** | Replaced by `CodeGenerator` methods. |
| `voicing_registry()` global, `registry_mutex()` global | **Removed** | Moved into `VoicingRegistry` class owned by `CompileContext`. |
| `TokenValue::std::string` arm | **Removed** | Replaced by `SymbolId` (identifier-like) and `StringLitData` (literals). |
| `Token::as_string` | **Removed** | Replaced by `as_identifier(interner)` + `as_string_lit()`. |
| `IdentifierData::name` field type | **Modified** | `std::string` → `SymbolId`. |
| `SymbolTable` lookup API | **Modified** | Keyed by `SymbolId`; FNV recomputation gone. |
| `parser.cpp:1455-1463` `^` precedence | **Modified** | Right-assoc fix. |
| `mini_lexer.cpp` line tracking | **Modified** | Bumps `line_` on `\n`. |
| `CompileContext` | **New** | New header + impl. |
| `StringInterner` | **New** | New header + impl. |
| `VoicingRegistry` class | **New** | Wraps the existing free fns. |

---

## 7. Edge Cases

### Phase 1a (`PreResolved` removal)

- **Synthetic arena lifetime.** `CodeGenerator::synthetic_arena_` must
  survive every `visit()` call within one `generate()`. Cleared at
  start of each `generate()` (matches existing `node_types_` reset
  pattern).
- **Concurrent reads.** No reader of `Ast::arena` runs during
  `generate()` today; the assertion is a future-proofing.
- **Spread expansion of zero args.** Spread `..[]` produces zero
  `ExpandedArg` entries — the existing `expanded_opt` is empty and
  no synthetic nodes are allocated. No change.

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

### Phase 2 tests

- `test_parser.cpp [F7]`: `2 ^ 3 ^ 2` → 512 (via const-eval).
- `test_parser.cpp [F7]`: tower `2 ^ 2 ^ 2 ^ 2` → 65536.
- `test_parser.cpp [F7]`: `-2 ^ 2` → 4 (unary-tighter, documented).
- `test_parser.cpp [F7]`: `x ^ -1` parses without error.
- `test_mini_notation.cpp [F8]`: multi-line pattern `"a b\nc d"` —
  diagnostic on `c` reports line 2.
- `test_mini_notation.cpp [F8]`: pattern with trailing `\n` followed by
  unterminated content reports correct line.
- Sweep `grep -nE '\^.*\^' akkado/stdlib akkado/tests web/static/docs` —
  audit any existing 3+-operand `^` and re-baseline test outputs.

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
| `akkado/include/akkado/ast.hpp` | 1a, 1b, 5 | Remove `PreResolved` node kind/data; add `MiniLiteralData::{mini_arena, mini_root, mini_diagnostics}`; `IdentifierData::name` → `SymbolId` |
| `akkado/include/akkado/codegen.hpp` | 1a, 3, 4 | `synthetic_arena_`; `emit_push_const`/`emit_zero` methods; `CompileContext&` ctor arg; new key shape for `pre_resolved_values_` |
| `akkado/include/akkado/codegen/helpers.hpp` | 3 | Delete `emit_push_const` and `emit_zero` free fns |
| `akkado/include/akkado/lexer.hpp` | 5 | Ctor takes `StringInterner&` |
| `akkado/include/akkado/mini_lexer.hpp` | 2, 5 | Add `line_`/`column_offset_` members; ctor takes interner |
| `akkado/include/akkado/mini_token.hpp` | 5 | `MiniTokenValue` variant: drop `std::string` arm; add `SymbolId`/`StringLitData` |
| `akkado/include/akkado/symbol_table.hpp` | 5 | Keys/lookup on `SymbolId` |
| `akkado/include/akkado/token.hpp` | 5 | Same as `mini_token.hpp`; remove `as_string()`, add `as_identifier(interner)`/`as_string_lit()` |
| `akkado/include/akkado/voicing.hpp` | 4 | `VoicingRegistry` class wrapping the existing fns |
| `akkado/src/akkado.cpp` | 1b, 4, 5 | Construct default ctx if null; merge mini-literal diagnostics; pass interner to lex |
| `akkado/src/analyzer.cpp` | 1b, 5 | Treat input arena as const; resolve `SymbolId` → view for diagnostics |
| `akkado/src/codegen.cpp` | 1a, 3, 4 | Synthetic-arena replacement at line 1017; emit_*-method impls; ctx threading |
| `akkado/src/codegen_patterns.cpp` | 1b, 3, 4 | Replace 4 `const_cast` re-parses with sub-arena reads; replace 10 helper call sites; 17 voicing call sites |
| `akkado/src/codegen_higher_order.cpp` | 3 | Rewrite line 699 helper call |
| `akkado/src/codegen_arrays.cpp` | 3 | Internal helper-call rewrites |
| `akkado/src/lexer.cpp` | 5 | `make_token(type, intern(text))` everywhere |
| `akkado/src/mini_lexer.cpp` | 2, 5 | `advance` bumps `line_`; `current_location` uses local `line_`; interning |
| `akkado/src/parser.cpp` | 1b, 2, 5 | Sub-arena mini-parse; `^` right-assoc fix; 15+ identifier-handling sites use `SymbolId` |
| `akkado/src/shape_index.cpp` | 5 | Interner-aware identifier handling (shape_index AST share is PRD-2, not here) |
| `akkado/src/symbol_table.cpp` | 5 | Lookup on `SymbolId`; remove 16+ FNV rehash sites |
| `akkado/src/voicing.cpp` | 4 | Delete process-globals; impl `VoicingRegistry` |
| `akkado/tests/test_codegen.cpp` | 1a,1b,3,4 | F1a/F1b/F2/F14 regression tests |
| `akkado/tests/test_lexer.cpp` | 5 | F12 interner tests |
| `akkado/tests/test_mini_notation.cpp` | 2 | F8 multi-line position tests |
| `akkado/tests/test_parser.cpp` | 2 | F7 right-assoc tests |
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
| `^` fix to right-associative | Round 3 Q1 |
| CompileContext holds VoicingRegistry + StringInterner only | Round 3 Q2 |
| Intern all identifiers + keywords + builtin names | Round 3 Q3 |
| Invariants asserted in `generate()` + checked by every codegen test | Round 3 Q4 |
| F1 split into two sub-phases (PreResolved first, mini-parse second) | Round 4 Q1 |
| CompileContext arg optional, defaults to fresh context | Round 4 Q2 |
| Token: `std::string` arm replaced by `SymbolId` for identifier-like uses | Round 4 Q3 |

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

For phases that resolve more than one finding (Phase 2 resolves both F7
and F8), add the RESOLVED tag to **both** finding headers. Phase 5
additionally flips the executive-summary tally in the audit doc — find
the sentence "Two correctness bugs and one architectural pretense fall
out of this:" near the top of the executive summary and replace it
with the resolved counterpart, noting all 6 findings are now closed.

### 11.3 Finding ↔ Phase ↔ PRD-shortlist map

For convenience when editing:

| Finding | Phase | Audit PRD-shortlist row to flip |
|---|---|---|
| F1 (codegen AST mutation) | 1a + 1b | PRD-1 |
| F2 (source-loc desync) | 3 | PRD-5 (note: PRD-5 also bundled F10/builder work, **not** in scope here — only mark the F2 portion shipped) |
| F7 (`^` right-assoc) | 2 | PRD-10 (mark the `^` fix shipped; rest of PRD-10 — Pratt table unification — stays open) |
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
