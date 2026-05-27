# PRD: Contextual Type-Annotation Keywords

> **Status: DRAFT** — filed 2026-05-27 alongside a rename fix for
> `web/static/patches/arpeggio-demo.akk` and `stepper-demo.akk`, which
> both broke because they used `arr` as a `fn` parameter name. Rename
> unblocks the patches; this PRD addresses the underlying trap.

## Executive Summary

Commit `1b7b317` ("param-type annotations Phase 2") added the
abbreviated type-annotation keywords `num`, `rec`, `arr`, `str`, `sig`,
`evs` to the global keyword table (`akkado/src/lexer.cpp:24-30`). The
lexer now emits a dedicated token type for each (e.g. `arr` →
`TokenType::Array`) regardless of surrounding context. The parameter-
list parser at `akkado/src/parser.cpp:1287` demands
`TokenType::Identifier` for parameter names; any user-written
`fn f(arr, …)`, `fn g(num, …)`, etc. now fails with `P001 "Expected
parameter name"`. The grammar only consumes the type token after `:`
in `parse_optional_annotation`
(`akkado/src/parser.cpp:1221-1240`) — so the reservation is **broader
than the feature requires**.

Two demo patches collided immediately. The stdlib was swept
preemptively in `1b7b317` itself (`multiband3fx`/`glide` parameter
`sig`→`s`), but every future user is one short, common name (`arr`,
`num`, `rec`, `str`) away from the same dead-end.

This conflicts with the live-coding-coerces philosophy
(memory: `feedback_livecoding_coerce_dont_fail`): common collection /
numeric / record names are exactly what a live coder reaches for, and
the language should not punish them with a hard error when the type
system has no annotation in scope to compete with.

**Goal**: make `num`, `rec`, `arr`, `str`, `sig`, `evs` *contextual*
keywords — type tokens only in annotation position; plain identifiers
everywhere else. `fn` stays globally reserved (it's a declaration
keyword, not a type keyword in the same sense).

## Motivation

- **Live-coding ergonomics**: `arr`, `num`, `rec`, `str` are natural
  parameter and binding names. Reserving them globally trades a tiny
  parser convenience for a footgun that recurs every time a new user
  writes `fn f(arr, …)`.
- **Pattern coherence**: Akkado leans toward warnings over errors
  (`feedback_livecoding_coerce_dont_fail`); this is one of the few
  remaining E-class errors that triggers for purely cosmetic reasons.
- **Migration churn**: every breaking patch costs a rename in user
  files we may not own (community examples, GitHub gists, the docs
  site, snippets in blog posts).

## Non-goals

- **`fn` stays reserved** globally. It signals a declaration and the
  parser branches on it at many top-level sites; making it contextual
  is a different (much larger) change.
- **No change to annotation semantics.** `(xs: arr)` still binds a
  parameter named `xs` with type `arr`. Behavior at annotation sites
  is identical before and after.
- **No new type tokens.** Scope is exactly the six keywords listed.

## Approach options

Pick one during implementation planning; both are sketched here.

### Option A — Parser fallback (recommended, lower risk)

Lexer continues to emit `TokenType::Number` / `Record` / `Array` /
`String` / `Signal` / `Evs` for the bare words. Anywhere the parser
calls the moral equivalent of `expect(Identifier)`, add a single
fallback that also accepts a type-keyword token and treats its lexeme
as the identifier name.

Concretely:

- Introduce a helper `consume_identifier_or_type_keyword()` returning
  the lexeme + source location.
- Audit call sites: parameter lists, `let`/`=` LHS, record-field
  shorthand, `as` bindings, `fn` body identifier references. The PRD
  must enumerate these during implementation.
- Annotation position (after `:`) keeps its existing strict path:
  `parse_optional_annotation` already runs **only** after `match(Colon)`
  succeeds, so accepting type tokens as identifiers in non-annotation
  spots does not affect annotation parsing.

Cost: small, surgical. Risk: the audit must be exhaustive — any
missed `expect(Identifier)` site continues to hard-error.

### Option B — Context-aware lexing

Lexer tracks whether it is in "annotation mode" (the next token follows
`:` in a binding/param/return-type slot) and only emits type tokens
there; elsewhere the same lexemes become `Identifier`. Cleaner
semantics, but requires lexer state and more careful interaction with
multi-line parsing and recovery.

Defer unless Option A turns out to need a third or more identifier
sites and the audit becomes unwieldy.

## Acceptance criteria

1. The following programs compile cleanly (currently fail):
   ```akkado
   fn step (arr, trig) -> arr[counter(trig)]
   fn add  (num, x)    -> num + x
   fn name (str)       -> str
   rec_local = {a: 1}            // identifier `rec_local` is fine today; add
   evs       = pat("c4 d4")      // simple binding to `evs`
   sig       = osc("sin", 440)   // binding to `sig`
   ```
2. Annotated forms still work:
   ```akkado
   fn step (xs: arr, trig: sig) -> xs[counter(trig)]
   fn add  (x:  num, y:   num) -> x + y
   ```
3. `fn` remains a hard keyword:
   ```akkado
   fn = 5     // error, unchanged
   ```
4. Existing akkado_tests pass unchanged.
5. New parser tests cover each of the six keywords as:
   - a `fn` parameter name (untyped)
   - a top-level `let` binding LHS
   - an identifier reference in an expression body
6. The two patches edited in 2026-05-27 (`arpeggio-demo.akk`,
   `stepper-demo.akk`) can revert their `xs` renames back to `arr`
   without breaking. (Whether we actually revert them is taste; the
   point is that we *could*.)

## Test plan

- Add `akkado/tests/test_parser_contextual_keywords.cpp` (or extend
  the existing parser-test file) with the cases above.
- Add a smoke test that compiles every `.akk` in
  `web/static/patches/` and asserts no `P001`. (Currently we'd
  need to grep manually; an end-to-end "all bundled patches compile"
  test is independently worth having.)
- Re-run the full test suite (`./build/cedar/tests/cedar_tests` +
  `./build/akkado/tests/akkado_tests`).

## Migration

None. Purely additive — previously-rejected programs start working;
no previously-accepted program changes meaning, because the lexer
output at annotation sites is unchanged.

## Rollout

Single PR. No CHANGELOG entry beyond the standard
`Fixed: parameter names `arr` / `num` / `rec` / `str` / `sig` / `evs`
are now accepted outside annotation position` line.

## Open questions

- **OQ-1**: Does the record literal short-hand `{arr, num}` (positional
  punning) collide with anything once `arr` and `num` parse as
  identifiers? Audit during implementation.
- **OQ-2**: Should the `cycle` / `co` / `beat(n)` / `param()` /
  `toggle()` builtin namespace be allowed to be shadowed by user
  bindings of the same name today? If yes, this PRD changes nothing
  for those; if no, file a separate PRD for builtin-shadowing rules
  rather than expanding scope here.
