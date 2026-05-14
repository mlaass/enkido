> **Status: NOT STARTED** — Removal of the `pat` keyword/builtin and the `p"…"` auto-detect literal in favor of typed pattern prefixes.

# Remove `pat` Builtin PRD — Typed Pattern Literals Only

## Executive Summary

Akkado patterns can already be written as **typed string-prefix literals** —
`v"…"` (value), `n"…"` (note), `s"…"` (sample), `c"…"` (chord), `t"…"`
(timeline) — each with unambiguous parse semantics fixed at the literal site
(see `docs/prd-patterns-as-scalar-values.md` and
`web/static/docs/reference/pattern/literals.md`).

The legacy `pat` form predates those prefixes. It exists as:
- a reserved keyword used in a call form `pat("c4 e4")` / `pat("…", closure)`,
- the `p"…"` string-prefix literal,

both lexed to a single `TokenType::Pat` that drives `MiniParseMode::Auto` —
the per-atom **auto-detection** mode that guesses whether each atom is a note,
a sample, or a number. Auto-detection is exactly the ambiguity the typed
prefixes were introduced to eliminate. There is no longer a reason to keep
`pat` around.

This PRD specifies the **complete removal** of `pat`, `p"…"`, the
`MiniParseMode::Auto` mode, and the analogous `value(…)` / `note(…)` *call
forms*, plus the rewrite of every doc, tutorial, example, and test that uses
them.

**Key design decisions:**
- Remove **both** the `pat` keyword/call form **and** the `p"…"` prefix
  literal. Auto-detect is gone entirely.
- `MiniParseMode::Auto` is **deleted** — the enum value, its lexer/parser
  branches, and the auto-detect atom-classification logic. `mode` becomes a
  required argument on the mini lexer/parser.
- The `pat("…", closure)` per-event callback form has **no direct
  successor** — that role belongs to `poly(n, fn (e) -> …)` consuming a typed
  literal.
- The `value(…)` and `note(…)` **call forms** are also removed (only the
  literal prefixes `v"…"` / `n"…"` remain). `scalar(…)` **stays** — it is a
  Pattern→Signal cast, not a literal.
- `pat` and `p` stop being reserved — they become ordinary identifiers.
  Removal is **silent**: old code fails with a generic "unknown
  builtin" / parse error, not a special deprecation diagnostic.
- Internal hot-swap state-ID path segments that happen to be named `"pat"`
  are **left unchanged** — renaming them would shift semantic-ID hashes and
  re-allocate state for existing patches.
- `seq()` is **out of scope** — a non-goal for this PRD.

---

## 1. Current State

### 1.1 Lexing

`pat` is a reserved keyword (`akkado/src/lexer.cpp:20`,
`{"pat", TokenType::Pat}`). The `p"…"` / `p\`…\`` prefix is recognized in
`lex_identifier` (`akkado/src/lexer.cpp:424-430`) and also produces
`TokenType::Pat`. The enum value lives at `akkado/include/akkado/token.hpp:35`
with a `to_string` arm at `token.hpp:111`.

### 1.2 Parsing

`Parser::parse_mini_literal` (`akkado/src/parser.cpp:1266-1347`) maps
`TokenType::Pat` → `MiniParseMode::Auto` with `mode_marker = "pat"`. `pat` is
the **only** token that:
- maps to `MiniParseMode::Auto`,
- is allowed the function-call-with-closure form (`is_pat_call`,
  `has_parens`, the `parse_closure()` branch at `parser.cpp:1336-1345`).

### 1.3 Auto-detect mode

`MiniParseMode::Auto` (`akkado/include/akkado/mini_token.hpp:26`) is the
default argument for the mini lexer (`mini_lexer.hpp:35,101,120`) and mini
parser (`mini_parser.hpp:113`). Its per-atom classification logic lives in
`mini_lexer.cpp:13-28` and `mini_parser.cpp:641-642`. `pat` / `p"…"` are the
only surface syntax that selects it; the typed prefixes select
`Value`/`Note`/`Sample`/`Chord`/`Curve` explicitly.

### 1.4 Builtins & codegen

| Location | Role |
|----------|------|
| `akkado/include/akkado/builtins.hpp:1086` | `pat` builtin entry (signature help only) |
| `akkado/include/akkado/builtins.hpp` (near :1093-1100) | `value`, `note` builtin entries |
| `akkado/src/codegen.cpp:1011-1013` | dispatch table: `value`→`handle_value_call`, `note`→`handle_note_call`, `scalar`→`handle_scalar_call` |
| `akkado/src/codegen_patterns.cpp:1935-1953` | `compile_typed_pattern_call`, `handle_value_call`, `handle_note_call` |
| `akkado/src/codegen_patterns.cpp:1272-1283` | `pat` MiniLiteral codegen: `closure_node` pickup, `call_counters_["pat"]`, `push_path("pat#N")` |
| `akkado/src/codegen_patterns.cpp:1396-1434` | closure-form handling (`(t,v,p)` callback) |
| `akkado/src/codegen_patterns.cpp:2057-2070` | `is_pattern_call` — lists `pat`, `value`, `note` |
| `akkado/src/codegen_patterns.cpp:4158` | `push_path("pat")` (transport path) |
| `akkado/src/analyzer.cpp:1778-1790` | pattern-call recognition — lists `pat`, `value`, `note` |
| `akkado/src/shape_index.cpp:152-153` | pattern-producer recognition — lists `pat`, `value`, `note` |
| `akkado/include/akkado/codegen.hpp:444-445` | doc comments referencing `value("…")` / `note("…")` |

### 1.5 Surface usage

- `pat(` / `p"` appears **~480 times** across `docs/` and
  `web/static/docs/`.
- `pat(` / `p"` appears **~435 times** across `akkado/tests/` (20+ files,
  including `test_lexer.cpp:734` which asserts `lex("pat")`).
- The closure call form `pat("…", (t,v,p) -> …)` is used heavily in
  `web/static/docs/tutorials/05-testing-progression.md`.

### 1.6 Limitation

Auto-detect re-introduces the ambiguity typed prefixes solve. `pat("60")` —
is `60` a raw value or MIDI note 60? `pat("bd c4")` mixes a sample and a note
under one guess-per-atom heuristic. Two spellings (`pat(...)` keyword and
`p"…"` literal) for one concept, plus a third and fourth (`value(...)` /
`note(...)` call forms duplicating `v"…"` / `n"…"`), is redundant surface
area that every reader, every doc, and the autocomplete engine must carry.

---

## 2. Target Syntax

### 2.1 Pattern literals (after removal)

```akkado
n"c4 e4 g4"        // note pattern  (was pat("c4 e4 g4"))
v"<220 440 880>"   // value pattern (was pat("<220 440 880>"))
s"bd ~ sd ~"       // sample pattern
c"Am C G Em"       // chord pattern
t"0 1 0.5"         // timeline curve
```

There is no auto-detect spelling. The author picks the type at the literal.

### 2.2 The closure callback → `poly()`

The `pat("…", closure)` per-event callback form is replaced by a typed
literal piped into `poly()`:

```akkado
// Before
pat("c4 e4 g4 c5", (t, v, p) ->
  osc("sin", p) * ar(t, 0.01, 0.3)
)

// After
n"c4 e4 g4 c5" |> poly(1, fn (e) ->
  osc("sin", e.freq) * ar(e.trig, 0.01, 0.3)
)
```

The closure parameters `(t, v, p)` (trigger / velocity / pitch signals) map
to the event-record fields `e.trig` / `e.vel` / `e.freq`. Voice count `1`
preserves the original monophonic behavior; raise it for genuine polyphony.

### 2.3 `value` / `note` calls → prefixes

```akkado
value("<220 440>")   →   v"<220 440>"
note("c4 e4 g4")     →   n"c4 e4 g4"
scalar(n"c4 e4")     →   scalar(n"c4 e4")   // unchanged — scalar() stays
```

### 2.4 Freed identifiers

After removal `pat` and `p` are ordinary identifiers:

```akkado
pat = n"c4 e4"     // legal — `pat` is just a variable now
p = osc("sin", 440) // legal
p"foo"              // parse error: identifier `p` followed by string literal
```

---

## 3. Goals and Non-Goals

### 3.1 Goals

1. Remove the `pat` keyword, the `pat(…)` call form, and the `p"…"` /
   `p\`…\`` prefix literal.
2. Delete `TokenType::Pat` and `MiniParseMode::Auto` and all code reachable
   only through them (auto-detect atom classification, closure-on-pattern
   parsing, the `is_pat_call` paren branch).
3. Remove the `value(…)` and `note(…)` call forms and their codegen
   (`compile_typed_pattern_call`, `handle_value_call`, `handle_note_call`).
4. Rewrite every `pat` / `p"…"` / `value(…)` / `note(…)` occurrence in
   `docs/`, `web/static/docs/`, examples, and `akkado/tests/` to typed
   prefixes, classifying each by atom content.
5. Drop `pat` / `value` / `note` from the generated builtins JSON, editor
   autocomplete, CodeMirror syntax highlighting, and the docs F1 lookup
   index.
6. Keep all functionality reachable: typed prefixes cover every former `pat`
   use; `poly()` covers the closure form.

### 3.2 Non-Goals

- **`seq()`** — left entirely untouched. Any redundancy between `seq()` and
  typed literals is a future PRD.
- **`scalar()`** — stays; it is a cast, not a literal.
- **`timeline` / `t"…"`** — stays; only `pat`/`p"…"` auto-detect is removed.
- **A deprecation warning path** — removal is silent (decision §1, "Rewrite
  all, silent removal").
- **Renaming internal state-ID path segments** — left as-is to preserve
  hot-swap hashes (§6.3).
- **New migration tooling / codemod** — the rewrite is a one-time manual
  pass guided by the classification table (§5.1).

---

## 4. Architecture / Technical Design

### 4.1 What disappears

```
TokenType::Pat            ─┐
  keyword "pat"            │
  prefix  p"…" / p`…`      ├─►  deleted
MiniParseMode::Auto       ─┤
  auto-detect classify     │
parse_mini_literal:        │
  is_pat_call / has_parens │
  closure branch          ─┘

builtins.hpp:  pat, value, note entries          ─►  deleted
codegen:       handle_value_call, handle_note_call,
               compile_typed_pattern_call         ─►  deleted
               pat closure-form codegen           ─►  deleted
is_pattern_call / analyzer / shape_index:
               "pat", "value", "note" entries     ─►  removed from lists
```

### 4.2 `parse_mini_literal` after removal

The function keeps the typed-prefix tokens only:

```
TokenType::Timeline   → MiniParseMode::Curve
TokenType::ValuePat   → MiniParseMode::Value
TokenType::NotePat    → MiniParseMode::Note
TokenType::SamplePat  → MiniParseMode::Sample
TokenType::ChordPat   → MiniParseMode::Chord
```

- The `is_pat_call` / `has_parens` / `parse_closure()` branch is deleted —
  no token reaches the call form anymore.
- `mode_marker` is always non-empty (every remaining token has a tag), so the
  `if (mode != MiniParseMode::Auto)` guard at `parser.cpp:1328` becomes
  unconditional.

### 4.3 Mini lexer / parser signature change

`MiniParseMode::Auto` is removed from `mini_token.hpp` (enum + `to_string`).
Because it was the **default argument**, every constructor/function that
defaulted `mode = MiniParseMode::Auto` must now take `mode` explicitly:

- `mini_lexer.hpp:35,101,120`
- `mini_parser.hpp:113`

Call sites in `mini_lexer.cpp:13-28` and `mini_parser.cpp:641-642` that
branch on `curve_mode` / `sample_only` / etc. to *fall back* to `Auto` are
rewritten — the fallback case no longer exists; an explicit mode is always
threaded through.

### 4.4 Closure form → `poly()` (no codegen successor)

The closure-handling code at `codegen_patterns.cpp:1396-1434` (closure param
extraction, polyphonic-tracking clearing) is **deleted**, not migrated. The
callback capability is already provided by `poly()` consuming a typed
literal; nothing in the `pat` closure path is unique enough to preserve.

---

## 5. Migration Strategy

### 5.1 Classification rule (decision: "Classify each by content")

Every `pat("X")` / `p"X"` / `value("X")` / `note("X")` occurrence is
reclassified by inspecting its atoms:

| Atoms in the pattern        | Rewrite to |
|-----------------------------|------------|
| Note names (`c4`, `e4`) or bare MIDI ints used as pitch | `n"X"` |
| Raw numbers used as values/Hz/params | `v"X"` |
| Sample names (`bd`, `sd`, `hh:2`) | `s"X"` |
| Chord symbols (`Am`, `C7`) | `c"X"` |
| `value("X")` call | `v"X"` |
| `note("X")` call | `n"X"` |
| `pat("X", closure)` | `n"X" |> poly(1, fn (e) -> …)` (or `s"…"` if sample atoms) |

Mixed-type patterns (a former `pat` legitimately mixing notes and samples)
must be split or re-expressed — flag these for the user during
implementation rather than guessing.

### 5.2 Golden / determinism tests (decision: "Regenerate goldens")

`test_sample_emission_golden`, `test_hot_swap_determinism`, and
`test_fuzz_determinism` may shift output when their `pat` inputs become typed
literals. Snapshot changes are **expected**: regenerate the golden files as
part of the migration and review the diff for sanity (the audio/bytecode
should be equivalent, only the parse path differs).

### 5.3 Verification gate

After each phase the full test suites must pass:

```bash
cmake --build build
./build/akkado/tests/akkado_tests
./build/cedar/tests/cedar_tests
cd web && bun run check && bun run build
```

---

## 6. Edge Cases

### 6.1 `p"…"` after removal
`p"foo"` lexes as identifier `p` followed by string literal `"foo"` → generic
parser error (two adjacent primary expressions). Acceptable per the "silent
removal" decision — no special diagnostic.

### 6.2 `pat` / `p` as identifiers
Both become ordinary identifiers. `pat = n"c4"` and `p = osc("sin", 440)`
must compile. Any test that previously asserted `pat` lexes as a keyword
(`test_lexer.cpp:734`) must be rewritten to assert it now lexes as
`Identifier`.

### 6.3 Hot-swap state-ID path segments
`codegen_patterns.cpp:1283` (`push_path("pat#N")`) and `:4158`
(`push_path("pat")`) feed the semantic-ID hash. These literal strings are
**kept unchanged** — the MiniLiteral codegen path that emits them still
exists (it serves the typed prefixes too); renaming would re-allocate
hot-swap state for every existing patch for no functional gain.

### 6.4 Mixed-type former `pat` patterns
`pat("bd c4")` (sample + note in one auto-detected pattern) has no single
typed-prefix equivalent. Implementation must surface these to the user, not
silently pick one prefix.

### 6.5 `compile_typed_pattern_call` becomes dead
Once `value`/`note` call handlers are gone, `compile_typed_pattern_call` has
no callers and is deleted. Confirm no other handler routes through it before
removal.

### 6.6 `builtins_json.hpp:23` comment
The example keyword list in the doc comment names `"pat"` — update so the
generated-JSON documentation does not reference a removed builtin.

---

## 7. Impact Assessment

| Component | Status | Notes |
|-----------|--------|-------|
| Typed prefixes `v/n/s/c"…"` | **Stays** | The replacement; unchanged |
| `t"…"` / `timeline` | **Stays** | Not part of auto-detect |
| `scalar()` | **Stays** | Cast, not a literal |
| `seq()` | **Stays** | Explicit non-goal |
| `poly()` | **Stays** | Absorbs the closure-callback use case |
| `TokenType::Pat` | **Removed** | Enum value + `to_string` arm |
| `MiniParseMode::Auto` | **Removed** | Enum value, branches, default args |
| `pat` keyword + `p"…"` prefix | **Removed** | Lexer keyword table + `lex_identifier` |
| `pat` / `value` / `note` builtin entries | **Removed** | `builtins.hpp` |
| `handle_value_call` / `handle_note_call` / `compile_typed_pattern_call` | **Removed** | `codegen_patterns.cpp` + dispatch table |
| `pat` closure-form codegen | **Removed** | `codegen_patterns.cpp:1396-1434` |
| `parse_mini_literal` closure/paren branch | **Removed** | `parser.cpp` |
| `is_pattern_call` / analyzer / shape_index lists | **Modified** | Drop `pat`/`value`/`note` strings |
| Mini lexer/parser signatures | **Modified** | `mode` becomes required |
| Builtins JSON / autocomplete / CodeMirror highlighting | **Modified** | Drop `pat`/`value`/`note` |
| Docs, tutorials, examples (~480 sites) | **Modified** | Rewrite to typed prefixes |
| `akkado/tests/` (~435 sites, 20+ files) | **Modified** | Rewrite + regenerate goldens |
| Hot-swap state-ID path strings | **Stays** | Kept as `"pat"` to preserve hashes |

---

## 8. File-Level Changes

### 8.1 Compiler core (Phase 1)

| File | Change |
|------|--------|
| `akkado/src/lexer.cpp` | Remove `{"pat", TokenType::Pat}` from keyword table; remove `p"…"`/`p\`…\`` prefix branch (~:424-430) |
| `akkado/include/akkado/token.hpp` | Remove `TokenType::Pat` enum value + `to_string` arm |
| `akkado/src/parser.cpp` | `parse_mini_literal`: remove `TokenType::Pat` case, `is_pat_call`, `has_parens`, closure branch; make `mode_marker` unconditional |
| `akkado/include/akkado/mini_token.hpp` | Remove `MiniParseMode::Auto` enum value + `to_string` arm |
| `akkado/include/akkado/mini_lexer.hpp` | Remove `= MiniParseMode::Auto` defaults (:35,:101,:120); `mode` required |
| `akkado/include/akkado/mini_parser.hpp` | Remove `= MiniParseMode::Auto` default (:113); `mode` required |
| `akkado/src/mini_lexer.cpp` | Remove Auto fallback branches (:13-28); thread explicit mode |
| `akkado/src/mini_parser.cpp` | Remove Auto fallback (:641-642) |
| `akkado/include/akkado/builtins.hpp` | Remove `pat`, `value`, `note` builtin entries |
| `akkado/include/akkado/builtins_json.hpp` | Fix doc-comment keyword example (:23) |
| `akkado/src/codegen.cpp` | Remove `value`/`note` dispatch entries (:1011-1012); keep `scalar` |
| `akkado/src/codegen_patterns.cpp` | Remove `handle_value_call`, `handle_note_call`, `compile_typed_pattern_call`, `pat` closure-form codegen; drop `pat`/`value`/`note` from `is_pattern_call` |
| `akkado/include/akkado/codegen.hpp` | Remove `handle_value_call`/`handle_note_call` decls; fix comments (:444-445) |
| `akkado/src/analyzer.cpp` | Drop `pat`/`value`/`note` from pattern-call recognition (:1778-1790) |
| `akkado/src/shape_index.cpp` | Drop `pat`/`value`/`note` from pattern-producer list (:152-153) |

### 8.2 Editor tooling & web (Phase 2)

| File | Change |
|------|--------|
| Generated builtins JSON (driven by `builtins.hpp`) | Regenerate so `pat`/`value`/`note` drop from autocomplete & signature help |
| CodeMirror Akkado language mode (`web/src/lib/**`) | Remove `pat` from keyword highlighting — grep `web/src` for `pat` keyword list |
| `web/scripts` build:docs output (`src/lib/docs/lookup-index.ts`) | Rebuilt via `bun run build:docs` after Phase 3 doc rewrite |

### 8.3 Docs, examples, tests (Phase 3)

| Area | Change |
|------|--------|
| `docs/**` (~ occurrences across ~30 files incl. `mini-notation-reference.md`, `vision-language-evolution.md`, the spec) | Rewrite `pat`/`p"…"` per §5.1 |
| `web/static/docs/**` (reference, tutorials, concepts) | Rewrite; `literals.md` loses the `p"…"` row and the "legacy `pat(string)`" sentence; `tutorials/05-testing-progression.md` closure examples → `poly()` |
| `akkado/tests/**` (~435 sites, 20+ files) | Rewrite per §5.1; `test_lexer.cpp` `pat`-keyword assertions → identifier assertions; add coverage that `pat`/`p` work as identifiers and `p"…"` errors |
| Golden files (`test_sample_emission_golden`, hot-swap/fuzz determinism) | Regenerate; review diffs (§5.2) |
| `bun run build:docs` | Run after doc rewrite to refresh F1 index |

---

## 9. Implementation Phases

### Phase 1 — Compiler core
**Goal:** `pat`, `p"…"`, `MiniParseMode::Auto`, and the `value`/`note` call
forms no longer exist in the compiler.
- Files: §8.1.
- Dependency: none.
- Verify: `cmake --build build` succeeds; `akkado_tests` compiles. Tests
  still using `pat` will fail here — that is expected and resolved in
  Phase 3. Optionally land Phase 1 + Phase 3 together to keep the suite
  green (see §10).

### Phase 2 — Editor tooling
**Goal:** `pat`/`value`/`note` gone from autocomplete, signature help, and
syntax highlighting.
- Files: §8.2.
- Dependency: Phase 1 (builtins JSON is generated from `builtins.hpp`).
- Verify: `cd web && bun run check && bun run build`; manually confirm
  autocomplete no longer offers `pat`.

### Phase 3 — Docs, examples, tests
**Goal:** zero `pat` / `p"…"` / `value(…)` / `note(…)` occurrences in the
repo; all suites green; golden files regenerated.
- Files: §8.3.
- Dependency: Phase 1 (so rewritten code actually compiles against the new
  grammar).
- Verify: `grep -rn '\bpat(\|p"' docs web/static/docs akkado/tests` returns
  nothing; `akkado_tests` + `cedar_tests` pass; `bun run build` passes.

### Phasing note
Because Phase 1 breaks ~435 test sites, Phases 1 and 3 may be landed as a
single commit/PR to avoid a red suite on `master`. Phase 2 can follow
independently. The phase split is organizational, not a sequence of
separately-shippable states.

---

## 10. Testing / Verification Strategy

### 10.1 New / changed unit tests
- **Lexer:** `pat` lexes as `Identifier` (not a keyword); `p"foo"` lexes as
  `Identifier("p")` + `String("foo")`.
- **Parser:** `p"…"` produces a parse error; no token maps to a removed
  `MiniParseMode::Auto`.
- **Identifier reuse:** `pat = n"c4 e4"` and `p = osc("sin", 440)` compile
  and behave as plain bindings.
- **Removed builtins:** `pat(…)`, `value(…)`, `note(…)` calls error as
  unknown builtins.

### 10.2 Equivalence checks
For a representative sample of rewritten patterns, confirm the typed-prefix
version produces equivalent audio/bytecode to what `pat` produced before
removal (spot-check, since auto-detect → explicit type should be a no-op when
classification is correct).

### 10.3 Golden regeneration
Regenerate `test_sample_emission_golden`, `test_hot_swap_determinism`,
`test_fuzz_determinism` snapshots; review the diff to confirm changes are
limited to the parse-path substitution.

### 10.4 Full-repo gate
```bash
cmake --build build
./build/akkado/tests/akkado_tests
./build/cedar/tests/cedar_tests
cd web && bun run check && bun run build
grep -rn '\bpat(\|[^a-zA-Z]p"' docs web/static/docs akkado/tests   # expect: no matches
```

### 10.5 Manual
- Load a former-`pat` patch in the web IDE, confirm typed-prefix rewrite
  plays identically.
- Confirm F1 help no longer surfaces `pat`; confirm autocomplete no longer
  offers it.
