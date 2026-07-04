# Akkado Core Language Grammar (EBNF)

Reference grammar for the Akkado surface language, extracted from the
implementation on 2026-07-04. Source of truth:

- `akkado/src/lexer.cpp` + `akkado/include/akkado/token.hpp` (lexical)
- `akkado/src/parser.cpp` + `akkado/include/akkado/parser.hpp` (syntax, precedence)
- `akkado/src/codegen.cpp` (directive names, contextual builtins)

Companion files: [mini-notation-grammar.md](mini-notation-grammar.md) (the
sub-language inside pattern strings) and [chord-grammar.md](chord-grammar.md)
(chord symbols).

**Notation**: `=` defines, `;` terminates, `"x"` terminal, `[x]` optional,
`{x}` zero-or-more, `(x|y)` grouping/alternation, `? … ?` prose constraint.
`(* … *)` comments.

---

## 1. Lexical grammar

Whitespace (space, tab, CR, LF) separates tokens and is otherwise
insignificant — **there is no statement terminator**; statement boundaries
are inferred (see §5). Comments run `//` to end of line. There are no block
comments.

```ebnf
letter        = "a"…"z" | "A"…"Z" | "_" ;
digit         = "0"…"9" ;

identifier    = letter { letter | digit } ;
                (* excluding keywords; a lone "_" lexes as UNDERSCORE *)

keyword       = "true" | "false" | "match" | "fn" | "as" | "const" | "import" ;

number        = [ "-" ] int_or_frac [ exponent ] ;
int_or_frac   = digit { digit } [ "." digit { digit } ]
              | "." digit { digit } ;                (* leading-dot: .5, .001 *)
exponent      = ( "e" | "E" ) [ "+" | "-" ] digit { digit } ;
                (* exponent only lexes when a digit provably follows *)

string        = '"' { str_char } '"'
              | "`" { str_char } "`"
              | "'" { str_char } "'" ;               (* only when not a pitch_lit *)
str_char      = ? any char except the delimiter ? | escape ;
                (* newlines allowed inside strings (multi-line mini-notation) *)
escape        = "\" ( "n" | "t" | "r" | "\" | '"' | "'" | "`" ) ;

pitch_lit     = "'" note_letter [ "#" | "b" ] digit [ digit ] "'" ;
note_letter   = "a"…"g" | "A"…"G" ;                  (* 'c4' → MIDI 60; clamped 0–127 *)

pattern_prefix = "t" | "v" | "n" | "s" | "c" ;
                (* lexes as a distinct token ONLY when the letter is a
                   single-char identifier immediately followed by '"' or '`';
                   the pattern string itself is the next string token *)

directive_tok = "$" letter { letter | digit } ;      (* $polyphony, $soundfont_alias *)
```

Operator and punctuation tokens:

```text
|>   pipe                >>   alias for |>          <>   diamond (out/bus sugar)
+  - *  /  ^             arithmetic (^ = power)
== != <  >  <= >=        comparison
&& ||                    logic          (single & or | is a lex error)
=    assignment          ->   fn/closure arrow      as   pipe binding (keyword)
.    field/method        ..   spread / range        ...  rest-param prefix
( ) [ ] { } , :          delimiters
@  %                     hole (pipe input; % is legacy alias of @)
!    logical not (prefix), also != start
#    annotation prefix (#inline)
_    placeholder / discard
$    directive prefix
```

Lexer quirks (load-bearing):

- **`-` folds into a numeric literal** when immediately followed by a digit:
  `a -5` is two expressions (`a`, `-5`), while `a - 5` is subtraction.
  There is **no unary minus** operator: `-x` and `-.5` are parse errors
  (write `0 - x` / `-0.5`).
- `t" v" n" s" c"` prefixes only trigger when the letter directly touches the
  quote: `t "x"` is identifier `t` then string `"x"`.
- Reserved-but-unused tokens: `;` (Semicolon), `?` (Question), `~` (Tilde)
  are lexed but **no core-grammar production consumes them** — using them
  outside a pattern string is a parse error. `MiniString` in `TokenType` is
  dead (never emitted). `?`/`~`/`_`/`@`/`!` get their pattern meanings only
  inside mini-notation strings, which are lexed separately.

---

## 2. Operator precedence (Pratt levels, low → high)

| Level | Operators | Assoc | Notes |
|---|---|---|---|
| Pipe | `\|>` (`>>`), `<>`, `as` | left | loosest; `as` binding lives here |
| Or | `\|\|` | left | desugars to `bor` |
| And | `&&` | left | desugars to `band` |
| Equality | `==` `!=` | left | `eq` / `neq` |
| Comparison | `<` `>` `<=` `>=` | left | `lt` `gt` `lte` `gte` |
| Addition | `+` `-` | left | `add` / `sub` |
| Multiplication | `*` `/` | left | `mul` / `div` |
| Power | `^` | **right** | `pow`; `a^b^c` = `a^(b^c)` |
| Unary | `!` (prefix) | — | `bnot(x)` |
| Method/Postfix | `.field` `.m(…)` `[i]` | left | chainable |
| Call/Primary | `f(…)`, literals | — | |

**Pipe RHS precedence**: the right-hand side of `|>` is parsed at *Addition*
level. So `a |> b + c` ≡ `a |> (b + c)`, but `a |> b == c` ≡ `(a |> b) == c` —
comparison/logic operators after a pipe attach to the whole pipe, not its RHS.

---

## 3. Syntactic grammar

### 3.1 Program & declarations

```ebnf
program       = { import_decl } { statement } ;
                (* imports must precede all other code — E501 otherwise *)

import_decl   = "import" string [ "as" identifier ] ;

statement     = directive
              | fn_def
              | const_decl
              | assignment
              | destructure_assignment
              | field_assignment
              | expression ;

directive     = directive_tok [ "(" [ expression { "," expression } ] ")" ] ;
                (* recognized: $polyphony(1..32 int literal),
                   $soundfont_alias("name", "path"); unknown → W150 warning *)

fn_def        = [ "#" "inline" ] [ "const" ] "fn" identifier
                "(" [ param_list ] ")" "->" ( block | expression ) ;
                (* "#inline" (E249/E246 on misuse) is the only annotation *)

const_decl    = "const" identifier "=" expression ;

assignment    = identifier "=" expression ;

destructure_assignment = destructure_pattern "=" expression ;

field_assignment = postfix_expr "=" expression ;
                (* only when postfix_expr ends in a .field access; statement
                   only — as a pipe RHS it is rejected with E205 *)
```

### 3.2 Parameters & destructuring

```ebnf
param_list    = param { "," param } ;
param         = rest_param | destructure_param | simple_param ;
rest_param    = "..." identifier ;               (* must be last; no default,
                                                    no annotation (E104) *)
destructure_param = destructure_pattern ;        (* fn defs AND closures;
                                                    no annotation (E104) *)
simple_param  = identifier [ ":" type_name ] [ "=" expression ] ;
                (* a required param may not follow a defaulted one *)

type_name     = "Signal" | "Number" | "Pattern" | "Record"
              | "Array" | "String" | "Function" | "Stream" ;
                (* contextual identifiers, not keywords; unknown name → E185 *)

destructure_pattern = "{" destructure_field { "," destructure_field } "}" ;
destructure_field   = identifier [ "=" expression ] ;
                (* duplicate field → E188. Defaults allowed only in
                   statement-level and fn-param positions; rejected in
                   `as {…}` bindings and match-arm patterns *)
```

### 3.3 Expressions

```ebnf
expression    = pipe_expr ;

pipe_expr     = logic_or { pipe_tail } ;
pipe_tail     = "as" ( identifier | destructure_pattern )   (* pipe binding *)
              | ( "|>" | ">>" ) additive                    (* see §2 note *)
              | "<>" [ "(" expression ")" ] ;               (* out/bus sugar *)

logic_or      = logic_and { "||" logic_and } ;
logic_and     = equality { "&&" equality } ;
equality      = comparison { ( "==" | "!=" ) comparison } ;
comparison    = additive { ( "<" | ">" | "<=" | ">=" ) additive } ;
additive      = multiplicative { ( "+" | "-" ) multiplicative } ;
multiplicative = power { ( "*" | "/" ) power } ;
power         = unary [ "^" power ] ;                       (* right-assoc *)
unary         = "!" unary | postfix_expr ;

postfix_expr  = primary { postfix_op } ;
postfix_op    = "." identifier [ "(" [ argument_list ] ")" ]  (* field / method *)
              | "[" expression "]" ;
                (* indexing only after: Identifier, Call, MethodCall, Index,
                   FieldAccess, ArrayLit, StringLit — never after match/block,
                   so a following [array] literal isn't swallowed *)

primary       = number | string | pitch_lit | "true" | "false"
              | mini_literal
              | hole
              | loop_expr
              | call
              | identifier
              | match_expr
              | array_lit
              | record_lit
              | closure
              | "(" expression ")"
              | "_" ;                          (* partial-application slot *)

call          = identifier "(" [ argument_list ] ")" ;
                (* the "(" MUST be on the same source line as the identifier;
                   a next-line "(" starts a new parenthesized statement *)

loop_expr     = "loop" "(" expression ")" block ;
                (* contextual keyword: only the shape `loop ( … ) {` is a
                   loop; plain `loop(x)` is an ordinary call *)

argument_list = argument { "," argument } ;
argument      = ".." expression                (* spread *)
              | identifier ":" expression      (* named argument *)
              | expression ;                   (* positional *)

array_lit     = "[" [ array_element { "," array_element } ] "]" ;
array_element = ".." expression | expression ; (* spreads allowed anywhere *)

record_lit    = "{" [ ".." expression [ "," ] ]
                    [ record_field { "," record_field } ] "}" ;
record_field  = identifier ":" expression
              | identifier ;                   (* shorthand {x} ≡ {x: x} *)
                (* at most one spread, and only in leading position;
                   duplicate field names are an error *)

closure       = "(" [ param_list ] ")" "->" closure_body ;
closure_body  = block | record_lit | expression ;
                (* `{` body is a record literal iff it is `{}`, `{..…}`,
                   or opens with `identifier :` — otherwise a block *)

block         = "{" { statement } "}" ;
                (* value of a block = its final expression *)

hole          = ( "@" | "%" ) [ hole_field ] ;
hole_field    = ? identifier or keyword immediately adjacent (no space) ?
              | "." identifier ;
                (* dotless `@freq` is canonical; dotted `@.freq` is deprecated
                   (W201 under --strict). `@.m(…)` is a method call on the
                   hole; `@m(…)` without dot is E108 *)

mini_literal  = pattern_prefix string ;
                (* t"…" v"…" n"…" s"…" c"…" — content parsed by the
                   mini-notation grammar, see mini-notation-grammar.md *)
```

### 3.4 Match expressions

```ebnf
match_expr    = "match" "(" expression ")" "{" { match_arm [ "," ] } "}"
              | "match"                    "{" { guard_arm [ "," ] } "}" ;

match_arm     = arm_pattern [ "&&" guard ] ":" arm_body ;   (* scrutinee form *)
guard_arm     = ( guard | "_" ) ":" arm_body ;              (* guard-only form *)

arm_pattern   = string
              | signed_number [ ".." signed_number ]        (* value / range *)
              | "true" | "false"
              | destructure_pattern                         (* no defaults *)
              | "_" ;                                       (* wildcard *)
signed_number = [ "-" ] number ;

guard         = ? expression parsed at Or precedence — i.e. any expression
                 not containing top-level "|>" / "<>" / "as" ? ;

arm_body      = block | expression ;
```

---

## 4. Parser-level desugarings

The parser canonicalizes; later stages never see the sugar.

| Surface | AST produced |
|---|---|
| `a + b` etc. | `Call(add, a, b)` — add/sub/mul/div/pow/bor/band/eq/neq/lt/gt/lte/gte |
| `!x` | `Call(bnot, x)` |
| `e <>` | `Pipe(e, Call(out, Hole))` — node-for-node identical to `e \|> out(@)` |
| `e <>(N)` | `Pipe(e, Call(bus, N, Hole))` |
| `{x}` | `{x: x}` |
| `e as {a, b}` | `PipeBinding` with synthetic temp `__destr_N` + field bindings |
| `@field` / `@.field` | identical `Hole{field}` node |

---

## 5. Context-sensitive rules (not expressible in EBNF)

These are the disambiguation decisions the parser makes with lookahead;
any re-implementation must reproduce them.

1. **Newline-sensitive call**: `identifier (` is a call only if `(` is on the
   same line (prevents a trailing identifier from swallowing the next
   statement — the grammar has no terminators).
2. **`loop` form**: `loop` + same-line `(` + balanced `)` followed by `{`.
3. **Closure vs grouping**: at `(`, scan ahead for
   `[params] ) ->` (params may include defaults with balanced-bracket
   expressions, `...rest`, `{…}` destructures). Only then is it a closure.
4. **Statement-level `{`**: destructure-assignment iff first token after `{`
   is an identifier *not* followed by `:` **and** the balanced `}` is
   followed by `=`. Otherwise the `{` opens a record-literal expression
   statement. A bare block is **never** an expression — `{` in expression
   position always means record literal.
5. **Closure `{` body**: record literal iff `{}`, `{..`, or `{ ident :`
   (§3.3). **Asymmetry**: `fn` bodies and match-arm bodies do *not* apply
   this rule — `fn f() -> {a: 1}` parses `{…}` as a block (and then fails);
   wrap in parens.
6. **Hole-field adjacency**: `@field` requires zero whitespace between `@`
   and the word; keyword tokens (`fn`, `as`, …) are accepted as field names.
   Pattern-prefix letters followed by a quote are not (they'd eat the quote).
7. **Named argument**: `identifier :` inside an argument list, with rollback
   if no `:` follows.
8. **Diamond + `(`**: after `<>`, any following `(` — even on the next
   line — is consumed as the bus index of `<>(N)`. A parenthesized
   statement directly after a bare `<>` will be mis-eaten (same-line rule
   from (1) is *not* applied here).
9. **Import guard**: `import` is only special before the first non-import
   statement (E501 after).

---

## 6. Reserved & contextual identifiers

- **Non-bindable** (parse error to assign/define): `state`, `get`, `set`,
  `_` (as assignment target or fn name).
- **Contextual keywords** (plain identifiers elsewhere): `loop`,
  the eight `type_name`s, annotation name `inline`.
- **Shadowable but special-cased by codegen**: `len`, `map`, `notes`,
  `freqs`, builtin function names generally.
- `%` is a legacy alias of `@` — parses identically; new code uses `@`.

---

## 7. Beyond the parser (codegen-level surface syntax)

Not part of this grammar but part of the language surface:

- **Plain strings re-parsed as mini-notation**: builtins that take a
  pattern argument also accept a plain string, re-parsed at codegen time in
  note mode; `chord("…")` and `timeline("…")` re-parse in chord/curve mode
  (`codegen_patterns.cpp`). The typed prefixes (`n"…"` etc.) do the same at
  parse time. There are no `pat()`/`seq()`/`note()` functions despite older
  docs.
- **Record-as-options**: builtins may take a trailing record literal
  validated against an `OptionSchema` (see `docs/…/record-as-options.md`) —
  grammatically it is just a `record_lit` argument.
- **Removed syntax**: the apostrophe `ChordLit` form (`C4'`, `Am7'`,
  slash-bass `F#m7_4'`) was removed 2026-05-21 by
  `docs/prd-pattern-event-arrays.md`. Today `'C4'` lexes as a pitch literal,
  `'Am7'` as a plain string, and a trailing `'` opens a string. Chords enter
  via `c"…"`, `chord(…)`, and uppercase atoms in note-mode patterns (see
  chord-grammar.md); older PRDs/proposals may still show the dead form.
