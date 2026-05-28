> **Status: SHIPPED** — Landed in commit `0de384c` on 2026-05-17. The parser
> accepts `@field` / `%field` whenever the field name is immediately adjacent
> to the hole (`akkado/src/parser.cpp:708`); whitespace defeats the shorthand
> so `@ as e` still parses as a pipe binding. `@method()` produces E108 with
> a hint pointing to the dotted form. The opt-in `W201` lint is wired through
> `akkado::compile()` and the `--strict` CLI flag. Test coverage: 11 sections
> under tag `[hole-shorthand]` in `akkado/tests/test_parser.cpp:1700` covering
> AST equivalence, aliases, keyword-as-field, whitespace defeat, chained
> access, E108, and W201 strict/default behaviour. The full in-repo corpus
> (16 patches + 4 docs) was migrated to the canonical dotless form and is
> W201-clean under `--strict`. Editor autocomplete in the web IDE now triggers
> on bare adjacent `@` / `%` (`web/src/lib/editor/akkado-completions.ts`).
> Minor doc gap: §10's "update `prd-records-and-field-access.md` §2.3 to
> cross-reference this PRD" was not done — that PRD is itself marked DONE
> and the user-facing callout in `web/static/docs/reference/language/records.md`
> already explains both forms compile to the same code.
>
> As of 2026-05-15, `@` is the canonical hole token in nkido docs and
> examples; `%` continues to parse as a legacy alias. This PRD's spec covers
> both, and new prose/examples should prefer `@`.

# PRD: Dotless Hole Field Shorthand (`@field`, with `%field` legacy alias)

## Executive Summary

Today, accessing a field of a pattern event (or any record) on a hole requires
the dotted form `@.freq` (or the legacy `%.vel`). In practice — especially
when typing — the dot is awkward: users naturally reach for `@freq`. This PRD
specifies a dotless shorthand so that **anywhere `@.field` is legal, `@field`
is also legal** (and likewise for the legacy `%`).

The dotted form keeps working unchanged. A new opt-in warning (`W201`) flags
dotted usages for users who want to migrate. Existing patches, docs, and
tests are rewritten to the dotless form to make it the canonical syntax in
the corpus.

**Key design decisions** (resolved during the question rounds):

- Both `%field` and `@field` are accepted — the two holes remain
  interchangeable.
- Backward-compatible: `@.field` / `%.field` continue to compile; an opt-in
  `--strict` lint mode emits `W201` suggesting the dotless form.
- **Adjacency-gated** at the parser: the field name must start in the column
  immediately after the hole. Whitespace breaks the shorthand, which lets
  `@ as e` keep working as a pipe binding while `@as` becomes the field `as`.
- **Field access only** — `@method()` is a parse error with a hint; method
  calls still require `@.method()`.
- **Keywords are allowed as field names** when adjacent: `@as`, `@if`,
  `@match` all read as field access on the hole.
- **Chained field access works**: `@foo.bar` parses as `(@foo).bar`.
- No lexer changes — implemented entirely as parser lookahead.

---

## 1. Current State

### 1.1 Hole tokens

Two tokens act as holes today (`akkado/include/akkado/token.hpp:77`):

| Char | Token             | Notes                                            |
|------|-------------------|--------------------------------------------------|
| `%`  | `TokenType::Hole` | Original hole, used in piping examples.          |
| `@`  | `TokenType::At`   | Added later, identical semantics to `%`.         |

Both are parsed by the same `parse_hole()` routine
(`akkado/src/parser.cpp:652`). The choice between `%` and `@` is purely
stylistic — every existing test and patch uses one or the other based on
authorial preference.

### 1.2 Existing dotted field syntax

`parse_hole()` already detects `.field`:

```cpp
NodeIndex Parser::parse_hole() {
    Token tok = advance();                  // consume '%' or '@'
    NodeIndex node = make_node(NodeType::Hole, tok);

    if (check(TokenType::Dot)) {
        // ... method-call disambiguation: %.method() restores position
        // ... otherwise emit HoleData{field_name}
    } else {
        arena_[node].data = Node::HoleData{std::nullopt};
    }
    return node;
}
```

The field name is stored in `HoleData::field_name` (`ast.hpp:328`), and
codegen resolves it against the pipe LHS's pattern event record
(`codegen.cpp:2598`, `:2631`). Aliases (`f` → `freq`, `t` → `trig`, …) are
normalized via `pattern_field_aliases()` in
`akkado/src/typed_value.cpp:30-49`.

### 1.3 Pain point

Users typing `@.freq` reach across the dot every time. Patches end up
visually noisy:

```akkado
n"c4 e4 g4" |> sine(@.freq) * @.vel * ar(@.trig, 0.01, 0.1)
```

vs. the proposed:

```akkado
n"c4 e4 g4" |> sine(@freq) * @vel * ar(@trig, 0.01, 0.1)
```

The corpus is small (6 patches, 3 doc files, a handful of test strings —
see §5.4) so the corpus migration is mechanical.

### 1.4 Comparison table

| Form                 | Today  | After this PRD                      |
|----------------------|--------|-------------------------------------|
| `@.freq`             | ✓      | ✓ (deprecated under `--strict`)     |
| `@freq`              | ✗      | ✓ (canonical)                       |
| `% .freq`            | ✗      | ✗ (whitespace between `%` and `.`)  |
| `@ freq`             | ✗      | ✗ (whitespace; bare hole + `freq`)  |
| `@.method()`         | ✓      | ✓                                   |
| `@method()`          | ✗      | parse error with hint               |
| `@foo.bar`           | ✗      | ✓ — `(@foo).bar`                    |
| `@as`                | parses as bare `@` + `as` clause | field access on `as` |
| `@ as e` (pipe bind) | ✓      | ✓ (whitespace defeats shorthand)    |

---

## 2. Goals and Non-Goals

### 2.1 Goals

1. Accept `@field` and `%field` anywhere `@.field` / `%.field` is accepted
   today, with identical semantics.
2. Keep the dotted form compiling without diagnostics by default.
3. Provide an opt-in lint warning (`W201`) that flags dotted usages.
4. Migrate the in-repo corpus (patches, docs, golden tests) to the
   dotless form so users meet it as the canonical syntax everywhere.
5. Surface the dotless form in error messages (E136 "Unknown field …") and
   in editor autocomplete.

### 2.2 Non-Goals

- **Removing the dotted form.** Deferred to a future PRD once the dotless
  form has soaked.
- **Method-call shorthand.** `@method()` does **not** become shorthand for
  `@.method()`. Method calls keep the dot.
- **Lexer changes.** No new token type — adjacency is enforced by the parser.
- **Underscore placeholder interaction.** `_` is unrelated; this PRD only
  touches `parse_hole`.
- **Mini-notation `@`.** `@2` inside `n"c4@2"` is the weight modifier
  (mini-lexer, separate codepath) — unaffected.

---

## 3. Target Syntax

### 3.1 Basic pattern event access

```akkado
// Today
n"c4 e4 g4" |> sine(@.freq) * @.vel |> out(@, @)

// After (canonical dotless form)
n"c4 e4 g4" |> sine(@freq) * @vel |> out(@, @)

// Legacy `%` alias accepts the same shorthand
n"c4 e4 g4" |> sine(%freq) * %vel |> out(%, %)
```

### 3.2 Aliases work the same

```akkado
n"c4" |> sine(@f) * @v * ar(@t, 0.01, 0.1)
//                       │     │       │
//                       │     │       └─ @t → trig
//                       │     └─ @v → vel
//                       └─ @f → freq
```

### 3.3 User records on holes via `as`

```akkado
fn make_voice(freq) -> {sig: saw(freq), env: ar(1, 0.01, 0.3)}
make_voice(440) as v |> lp(v.sig, 1000) * v.env |> out(@, @)
```

`v.sig` / `v.env` are unaffected (they use a regular identifier, not a
hole). Only hole-rooted access gets the dotless option.

### 3.4 Pipe binding (`as`) still works with bare holes

Whitespace defeats the shorthand, so `@ as e` keeps its today-meaning:

```akkado
// Bare hole + pipe binding — still works.
n"c4 e4" as e |> sine(e.freq) |> @ as raw |> raw * 0.5 + reverb(raw)
//                                          └─ space between @ and `as` → bare hole, then binding

// Field shorthand named `as` (rare but possible).
some_record_with_as_field |> @as          // accesses field `as`
```

The disambiguation rule is purely positional: column of the next token
must equal column of the hole + 1.

### 3.5 Chained field access

```akkado
// Hypothetical nested record on a pattern event.
n"c4" as e |> sine(e.osc.freq)   // works today

// Dotless equivalent via hole — chains naturally.
n"c4" |> sine(@osc.freq)
//                       └────┴─ (@osc).freq
```

The dotless syntax only replaces the **first** dot. Subsequent `.field`
chains are parsed by the existing field-access postfix machinery.

### 3.6 Method calls (unchanged)

```akkado
saw(440) |> @.lp(800) |> out(@, @)        // ✓ today, ✓ after
saw(440) |> @.lp(800).hp(2000) |> out(@)  // ✓ today, ✓ after
saw(440) |> @lp(800)                       // ✗ parse error E108
```

`@lp(800)` triggers a new parse error:

```
E108 — Method calls on holes require a dot: write `@.lp(800)` instead of `@lp(800)`.
```

### 3.7 Bare holes are unaffected

```akkado
out(@, @)                  // bare hole — no change
saw(440) |> lp(@, 1000)    // bare hole — no change
```

After consuming the hole token, the parser only consumes a field name when
the next token is an adjacent Identifier **or** keyword. Anything else
(comma, `)`, operator, EOF, whitespace before identifier) leaves the hole
bare.

---

## 4. Architecture

### 4.1 Parser change (the entire feature)

The change lives in `parse_hole()` in `akkado/src/parser.cpp`. Pseudocode:

```cpp
NodeIndex Parser::parse_hole() {
    Token hole_tok = advance();                       // consume '%' or '@'
    NodeIndex node = make_node(NodeType::Hole, hole_tok);
    const std::uint32_t adjacent_col = hole_tok.column + 1;

    // (a) Dotless shorthand: adjacent identifier or keyword acts as field name.
    if (current().column == adjacent_col &&
        is_identifier_or_keyword(current().type)) {

        // Disambiguate: @field( is a method-call attempt — emit E108.
        if (peek_next().type == TokenType::LParen) {
            error("E108", "Method calls on holes require a dot: write `"
                  + hole_lexeme(hole_tok) + "." + std::string(current().lexeme)
                  + "(...)` instead.");
            // Recover by consuming the identifier and continuing as field
            // access (so cascade errors stay localized).
        }

        Token field_tok = advance();
        arena_[node].data = Node::HoleData{std::string(field_tok.lexeme)};
        return node;
    }

    // (b) Existing dotted form: @.field, %.field, including method-call
    //     restore-position trick. On a real field match, emit W201 if --strict.
    if (check(TokenType::Dot)) {
        // ... existing logic, unchanged ...
        // After producing HoleData{field_name}:
        if (lint_strict_) {
            warn("W201",
                 "Dotted hole-field access is deprecated; write `"
                 + hole_lexeme(hole_tok) + std::string(field_tok.lexeme)
                 + "` instead.",
                 hole_tok.location);
        }
        return node;
    }

    // (c) Bare hole.
    arena_[node].data = Node::HoleData{std::nullopt};
    return node;
}
```

Where:

- `is_identifier_or_keyword(t)` returns `true` for `TokenType::Identifier`
  and any reserved keyword token (`As`, `If`, `Match`, `Fn`, `Let`, …).
- `peek_next()` looks one token ahead **without** advancing.
- `lint_strict_` is a parser flag wired from the existing
  `CompilerOptions::strict` / `--strict` CLI flag.

### 4.2 Adjacency check

Tokens already carry `column` and `line` in their `SourceLocation`
(`akkado/include/akkado/source_location.hpp`). The adjacency rule:

```
next_token.line == hole_tok.line && next_token.column == hole_tok.column + 1
```

The `+ 1` is the width of `@` / `%` (both single-byte). No multi-byte hole
characters exist.

### 4.3 Subsequent dots chain normally

After `parse_hole()` returns a `Hole{field: "foo"}` node, the Pratt parser's
postfix loop continues. If it sees `.`, the existing
`parse_field_access(left)` runs against the hole node — producing
`FieldAccess(Hole{field: "foo"}, "bar")`. The analyzer already handles
field access on arbitrary expressions, so no extra wiring is needed.

### 4.4 Method-call disambiguation

Two forms must remain distinct:

| Source         | Parse                                            |
|----------------|--------------------------------------------------|
| `@.method()`   | Bare hole, postfix method-call (existing path).  |
| `@method()`    | Parse error E108. Recover by treating `method` as field; codegen will likely flag the trailing `(...)` as an attempt to call a non-callable. |
| `@field()`     | Same as `@method()` — there is no semantic distinction in the parser; we can't tell field-from-method without analyzer knowledge. The error message is identical. |

Rationale: enforcing the dot for *any* `()` after a hole keeps the parser
rule purely syntactic (no analyzer feedback) and the error message simple.
Users who genuinely want to call a closure stored in an event field still
have a workaround: `(@callback)()` or `@.callback()`.

### 4.5 W201 — opt-in deprecation warning

| Property      | Value                                               |
|---------------|-----------------------------------------------------|
| Code          | `W201`                                              |
| Severity      | Warning                                             |
| Default       | Off                                                 |
| Enabled by    | `CompilerOptions::strict == true` (existing flag)   |
| CLI           | `akkado --strict file.akk`                      |
| Web IDE       | Tied to a future "Lint" toggle in settings (out of scope here; for now the web IDE compiles without `strict`). |
| Message       | `Dotted hole-field access is deprecated; write '<hole><field>' instead.` |
| Location      | The hole token's source location.                   |

Picked `W201` because the next-unused warning slot above `W200`.

### 4.6 Compiler options plumbing

`akkado::CompilerOptions` already has a `strict: bool` field
(`akkado/include/akkado/compiler.hpp`). Thread it into `Parser` via the
existing constructor argument — no new option needed.

---

## 5. Impact Assessment

### 5.1 Component-level

| Component                                | Status      | Notes |
|------------------------------------------|-------------|-------|
| `lexer.cpp`                              | **Stays**   | `@`, `%`, identifiers, keywords all tokenize as today. |
| `token.hpp`                              | **Stays**   | No new token types. |
| `ast.hpp` (`HoleData`)                   | **Stays**   | `HoleData{optional<string>}` already models field. |
| `parser.cpp::parse_hole()`               | **Modified** | Add adjacency-gated shorthand branch; emit W201 in dotted branch under strict. |
| `parser.cpp::parse_primary()`            | **Stays**   | Still dispatches to `parse_hole()` on `Hole`/`At`. |
| `analyzer.cpp` (field validation)        | **Stays**   | The shorthand produces the same `HoleData{field_name}` node — analyzer sees no difference. |
| `codegen.cpp` (E136 unknown field msg)   | **Modified** | Update suggestion text to show dotless form. |
| `akkado/src/diagnostics.cpp`             | **Modified** | Register `W201`, `E108`. |
| `web/src/lib/editor/akkado-completions.ts` | **Modified** | Trigger field-name completions on adjacent `@` / `%` typing, not just `@.` / `%.`. |
| `web/src/lib/editor/akkado-shape-index.ts` | **Modified** | Comment update; trigger registration. |
| Web pretty-printer / formatter (if any)  | **N/A**     | None exists today. |

### 5.2 Patches (canonical examples shipped with the web IDE)

| File                                          | Change                               |
|-----------------------------------------------|--------------------------------------|
| `web/static/patches/effects-chain.akk`        | `@.freq` → `@freq`                   |
| `web/static/patches/microtonal-raga.akk`      | `@.freq`, `@.trig` → `@freq`, `@trig` |
| `web/static/patches/poly-chords.akk`          | `@.freq` etc. → dotless              |
| `web/static/patches/rock-groove.akk`          | `@.freq`, `@.trig` → dotless         |
| `web/static/patches/visualizations.akk`       | `@.freq` → `@freq`                   |
| `web/static/patches/wavetable-scan.akk`       | `@.freq` → `@freq`                   |

### 5.3 Documentation

| File                                                          | Change                       |
|---------------------------------------------------------------|------------------------------|
| `web/static/docs/reference/language/conditionals.md`          | Two `%.field` examples → dotless. |
| `web/static/docs/reference/language/records.md`               | Eight `%.field` / `@.field` examples → dotless. Add a callout: "Both `@field` and `@.field` are accepted; the dotless form is canonical." |
| `web/static/docs/reference/mini-notation/chords.md`           | One `@.freq` example → `@freq`. |
| `docs/Akkado A Rhythmic & DSP Language Specification.md`      | Update hole syntax section (see §2.3 of `prd-records-and-field-access.md` for the canonical reference). Add the dotless rule. |
| `docs/mini-notation-reference.md`                             | No change (mini-notation `@` weight modifier is distinct). |

### 5.4 Tests

| File                                          | Change                              |
|-----------------------------------------------|-------------------------------------|
| `akkado/tests/test_parser.cpp`                | Add new dotless cases. Keep at least one dotted case per syntactic shape to lock in back-compat. |
| `akkado/tests/test_codegen.cpp`               | Same — add dotless coverage for every existing `%.field` / `@.field` test. Keep one or two dotted regression tests. |
| `akkado/tests/test_diagnostics.cpp` (or wherever W-codes are tested) | New tests for E108 and W201. |

---

## 6. File-Level Changes

### 6.1 Files to modify

| File                                              | Change |
|---------------------------------------------------|--------|
| `akkado/src/parser.cpp`                           | In `parse_hole()` (~line 652): add the adjacency-gated dotless branch (§4.1 (a)). Add W201 emission in the dotted branch when `lint_strict_` is set. Add E108 for `@ident(`. |
| `akkado/include/akkado/parser.hpp`                | Add `bool lint_strict_` member; constructor takes it from `CompilerOptions`. Helper `is_identifier_or_keyword(TokenType)`. |
| `akkado/src/diagnostics.cpp`                      | Register `W201` (severity Warning) and `E108` (severity Error). |
| `akkado/include/akkado/diagnostics.hpp`           | Update the diagnostic-codes comment block to include W201 and E108. |
| `akkado/src/codegen.cpp` (~line 2598, ~line 2631) | E136 message: change `Available: …` suffix to show example as `@freq` instead of `%.freq`. |
| `web/src/lib/editor/akkado-completions.ts`        | Add trigger registration for bare adjacent `@` / `%`. Reuse the existing hole-completion provider used for `@.` / `%.`. |
| `web/src/lib/editor/akkado-shape-index.ts`        | Update header comment to mention `@field` / `%field`. |
| `tools/akkado/main.cpp`                       | Wire the `--strict` flag through to `CompilerOptions::strict` if not already done. |

### 6.2 Files to migrate (mechanical rewrite, dotted → dotless)

See tables in §5.2 and §5.3 — same set.

### 6.3 Files explicitly NOT changing

| File                                                            | Why |
|-----------------------------------------------------------------|-----|
| `akkado/src/lexer.cpp`                                          | No new token types; adjacency is positional. |
| `akkado/src/mini_lexer.cpp`, `akkado/src/mini_parser.cpp`       | Mini-notation `@` is the weight modifier, separate codepath. |
| `akkado/include/akkado/ast.hpp` (`HoleData`)                    | Already correctly shaped. |
| `akkado/src/analyzer.cpp`                                       | The shorthand produces the same AST node — no semantic difference. |
| `akkado/src/typed_value.cpp` (`pattern_field_aliases()`)        | Aliases are field-name-level; unaffected by the dot. |

---

## 7. Implementation Phases

### Phase 1 — Parser & diagnostics

**Goal:** `@field` / `%field` compile to the same bytecode as `@.field` /
`%.field`; `@method()` errors with E108; `W201` is registered (off by
default).

1. Update `parse_hole()` per §4.1.
2. Register `W201` and `E108` in `diagnostics.cpp`.
3. Add the `lint_strict_` member on `Parser`, wired from `CompilerOptions`.
4. Add parser tests (`test_parser.cpp`): `@freq`, `%freq`, `@osc.freq`
   (chain), `@as` (keyword as field), `@ as e` (whitespace preserves
   binding), `@method()` → E108.

**Verification:**

```bash
cmake --build build --target akkado_tests
./build/akkado/tests/akkado_tests "[parser][hole-shorthand]"
```

### Phase 2 — Codegen test coverage & error messages

**Goal:** Confirm every existing `%.field` / `@.field` test passes when
mechanically rewritten dotless. Update E136 message.

1. Copy each `%.field` / `@.field` test case in `test_codegen.cpp` to a
   dotless sibling. Keep at least one dotted case per shape for regression.
2. Update the E136 emission in `codegen.cpp` to show `@freq` style hints
   (build the example from the canonical alias of the closest match).
3. Verify bytecode equivalence: `compile("...@freq...").bytecode ==
   compile("...@.freq...").bytecode`.

**Verification:**

```bash
./build/akkado/tests/akkado_tests "[codegen][hole-field]"
```

### Phase 3 — Corpus migration

**Goal:** Every patch and doc in the repo uses the dotless form.

1. Run the mechanical rewrite over the 6 patches and 3 doc files listed in
   §5.2/§5.3.
2. Update `prd-records-and-field-access.md` §2.3 to mention the dotless
   form as canonical (cross-reference this PRD).
3. Add a short callout to `web/static/docs/reference/language/records.md`:
   "Both `@field` and `@.field` access the same field; `@field` is the
   canonical form."
4. Manual spot-check in the web IDE: load each patch, confirm it still
   plays.

**Verification:**

```bash
grep -rEn "@\.\w+|%\.\w+" web/static/patches/ web/static/docs/
# Should return only intentional examples (the records.md callout).
```

### Phase 4 — Editor autocomplete

**Goal:** Typing `@` (no dot) in the web IDE shows pattern field
suggestions.

1. In `akkado-completions.ts`, extend the trigger logic that fires on `@.`
   / `%.` to also fire on bare adjacent `@` / `%` when the cursor is at a
   position where a hole field would be valid (i.e., not at the start of a
   line in a context where bare `@` is the whole expression).
2. Re-run `bun run check` and load the IDE; verify completion popup behaves
   for both `@` and `@.`.

**Verification:**

```bash
cd web && bun run check
# Manual: type `@` in a pattern context, confirm field suggestions appear.
```

### Phase 5 — Opt-in lint warning (W201)

**Goal:** `--strict` mode flags dotted hole-field usages.

1. Confirm the `--strict` flag is plumbed through `akkado` to the
   parser.
2. Wire the `lint_strict_` branch in `parse_hole()` to emit W201.
3. Add a test that compiles a dotted snippet under `strict=true` and
   asserts W201 is in the diagnostics.

**Verification:**

```bash
./build/bin/akkado --strict examples/old-style.akk 2>&1 | grep W201
```

---

## 8. Edge Cases

### 8.1 Bare hole still works in all contexts

```akkado
out(@, @)
saw(440) |> lp(@, 1000)
n"c4" |> @                  // bare hole, equivalent to @.freq
```

The shorthand only kicks in when the next token is an **adjacent**
Identifier or keyword. Bare-hole usage is unaffected.

### 8.2 Whitespace defeats the shorthand

```akkado
@freq          // field access on freq
@ freq         // bare hole, then `freq` — usually a parse error
@\nfreq        // bare hole, then `freq` on next line — same
% freq         // same as `@ freq` (no shorthand)
```

The adjacency rule is `line == hole.line && column == hole.column + 1`.

### 8.3 Keywords as field names

```akkado
some_event |> @as     // field `as` (rare, but legal record key)
some_event |> @if     // field `if`
some_event |> @match  // field `match`
```

These work because the parser checks for `is_identifier_or_keyword(...)`
when adjacent. Without adjacency, the keyword token retains its keyword
meaning:

```akkado
n"c4" as e |> sine(e.freq)   // `as` here is the pipe-binding keyword
```

### 8.4 `@ as e` keeps working as bare-hole pipe binding

```akkado
saw(440) |> @ as raw |> raw + reverb(raw)
//          └─ space before `as` → bare hole, then pipe binding `as raw`
```

Adjacency is the only disambiguator — no special-casing of `as`.

### 8.5 Chained access

```akkado
@osc.freq             // (@osc).freq
@osc.freq.lp(800)     // ((@osc).freq).lp(800) — chained method call
@event.osc.freq       // ((@event).osc).freq — works if `osc` is a nested record on the event
```

The dotless rule only consumes the **first** field name. Subsequent dots
chain through the existing field-access postfix.

### 8.6 Method call attempts

```akkado
@.method()    // ✓ method call (existing behavior)
@method()     // ✗ E108 — Method calls on holes require a dot
@field()      // ✗ E108 — parser can't tell field-with-call apart from method-call here
```

If a user genuinely wants to call a callable stored in a field, they can
write `(@field)()` or `@.field()`. Documented in the E108 message.

### 8.7 Underscore is not an identifier

```akkado
@_       // bare hole + `_`. `_` is `TokenType::Underscore`, not Identifier or keyword, so no shorthand.
```

The shorthand explicitly only triggers on `Identifier` or keyword tokens.
`_` remains a placeholder per `prd-underscore-placeholder.md`.

### 8.8 Number after hole

```akkado
@1       // bare hole + `1` (number) — same parse error today.
@0.5     // bare hole + `0.5`
```

Numbers aren't identifiers or keywords, so the shorthand doesn't apply.
Same behavior as today.

### 8.9 Dotless hole as full expression

```akkado
fn double_freq(e) -> e.freq * 2
n"c4" |> double_freq(@)         // bare hole, no field — works
n"c4" |> double_freq(@freq)     // works — field access shorthand
```

When the next token is `)`, `,`, `|>`, an operator, EOF, or whitespace,
the hole stays bare.

### 8.10 Mixed dotted and dotless in the same expression

```akkado
n"c4" |> sine(@freq) * @.vel    // both forms; both compile
```

Mixed usage is legal; W201 (under `--strict`) fires once per dotted site.

### 8.11 Pre-existing E136 message change

```
// Old:
E136 — Unknown field 'fre' on pattern. Available: freq, vel, trig, ...

// New:
E136 — Unknown field 'fre' on pattern. Available: freq, vel, trig, ...
       hint: did you mean `@freq`?
```

The suggestion picks the closest available alias via simple edit-distance
(already used by other akkado error messages).

---

## 9. Testing Strategy

### 9.1 Parser tests (`akkado/tests/test_parser.cpp`)

```cpp
SECTION("dotless hole field — basic") {
    auto ast = parse_ok("n\"c4\" |> osc(\"sin\", @freq)");
    // Walk AST, find the Hole node, assert HoleData::field_name == "freq".
}

SECTION("dotless equivalence with dotted") {
    auto a = parse_ok("n\"c4\" |> %.freq");
    auto b = parse_ok("n\"c4\" |> %freq");
    REQUIRE(ast_equal_ignoring_locations(a, b));
}

SECTION("whitespace defeats shorthand") {
    auto ast = parse_ok("n\"c4\" |> @ as e |> osc(\"sin\", e.freq)");
    // Assert the `@` parses as bare hole; `as e` parses as pipe binding.
}

SECTION("keyword as field name when adjacent") {
    auto ast = parse_ok("some_record |> @as");
    // Field name should be "as".
}

SECTION("E108 on @method(") {
    auto err = parse_err("saw(440) |> @lp(800)");
    REQUIRE(has_code(err, "E108"));
}

SECTION("chained @foo.bar") {
    auto ast = parse_ok("event |> @osc.freq");
    // Expect FieldAccess { Hole{field:"osc"}, "freq" }.
}

SECTION("bare hole still works") {
    parse_ok("out(@, @)");
    parse_ok("saw(440) |> lp(@, 1000)");
}
```

### 9.2 Codegen tests (`akkado/tests/test_codegen.cpp`)

For every existing `[hole-field]` test that uses `%.foo` or `@.foo`, add a
dotless sibling and assert identical bytecode:

```cpp
SECTION("dotless bytecode equivalence") {
    auto dotted = akkado::compile("n\"c4 e4 g4\" |> osc(\"sin\", @.freq) |> out(@, @)");
    auto dotless = akkado::compile("n\"c4 e4 g4\" |> osc(\"sin\", @freq) |> out(@, @)");
    REQUIRE(dotted.success);
    REQUIRE(dotless.success);
    REQUIRE(dotted.bytecode == dotless.bytecode);
}
```

Aliases:

```cpp
for (auto alias : {"f", "n", "pitch", "frequency"}) {
    auto r = akkado::compile("n\"c4\" |> osc(\"sin\", @" + alias + ") |> out(@, @)");
    REQUIRE(r.success);
}
```

### 9.3 Diagnostic tests

```cpp
SECTION("W201 fires under --strict") {
    CompilerOptions opts; opts.strict = true;
    auto r = akkado::compile("n\"c4\" |> osc(\"sin\", @.freq)", opts);
    REQUIRE(has_warning(r, "W201"));
}

SECTION("W201 silent without --strict") {
    auto r = akkado::compile("n\"c4\" |> osc(\"sin\", @.freq)");
    REQUIRE_FALSE(has_warning(r, "W201"));
}

SECTION("E108 hint suggests dotted method call") {
    auto r = akkado::compile("saw(440) |> @lp(800)");
    REQUIRE_FALSE(r.success);
    auto& msg = first_diag_message(r, "E108");
    REQUIRE_THAT(msg, Catch::Matchers::ContainsSubstring("@.lp"));
}
```

### 9.4 Corpus regression

After Phase 3 migration:

```bash
# Every shipped patch must still compile and produce non-silent audio.
for p in web/static/patches/*.akk; do
    ./build/bin/akkado "$n" --check
done
```

A manual pass in the web IDE: load each migrated patch, confirm it plays
identically to before.

### 9.5 Build & run

```bash
cmake --build build --target akkado_tests cedar_tests
./build/akkado/tests/akkado_tests
./build/cedar/tests/cedar_tests

cd web && bun run check && bun run build
```

---

## 10. Done Criteria

- All parser, codegen, and diagnostic tests in §9 pass.
- `compile("...@freq...").bytecode == compile("...@.freq...").bytecode` for
  every existing field and alias.
- All 6 patches and 3 doc files listed in §5.2/§5.3 use the dotless form
  and play correctly in the web IDE.
- `--strict` mode reports W201 for every remaining dotted usage; default
  mode reports zero diagnostics.
- `@method()` produces E108 with a message that mentions `@.method()`.
- `@ as e` still parses as a pipe binding (whitespace preserves the
  existing meaning).
- Web IDE autocomplete shows pattern fields when the user types `@` or
  `%` adjacent to a hole-valid position.
- `docs/prd-records-and-field-access.md` §2.3 is updated to cross-reference
  this PRD and call the dotless form canonical.

---

## 11. Future Work

- **Remove the dotted form.** Once the corpus is fully migrated and
  external users have had a release cycle to adopt, a follow-up PRD can
  promote `W201` to an error and eventually remove the dotted branch
  from `parse_hole()` entirely.
- **Web IDE lint toggle.** A "strict mode" toggle in the settings panel
  would surface W201 in the diagnostics gutter without requiring CLI use.
- **Auto-fixer.** A `akkado --fix` mode (out of scope here) could
  rewrite dotted hole-field accesses to the dotless form automatically.
