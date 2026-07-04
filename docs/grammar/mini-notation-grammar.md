# Akkado Mini-Notation Grammar (EBNF)

Reference grammar for the pattern sub-language inside Akkado pattern
strings, extracted from the implementation on 2026-07-04. Source of truth:

- `akkado/src/mini_lexer.cpp` + `akkado/include/akkado/mini_token.hpp` (lexical)
- `akkado/src/mini_parser.cpp` (syntax)

Companions: [akkado-grammar.md](akkado-grammar.md) (host language),
[chord-grammar.md](chord-grammar.md) (chord symbols used by atoms here).

**Notation**: `=` defines, `;` terminates, `"x"` terminal, `[x]` optional,
`{x}` zero-or-more, `(x|y)` grouping/alternation, `? … ?` prose constraint.

---

## 1. Parse modes

The same structural grammar runs in five modes; the mode changes which
**leaf atoms** are legal and how a few characters lex. Mode is selected by
the host-language literal prefix (or by the builtin re-parsing a plain
string):

| Mode | Prefix | Leaf atoms |
|---|---|---|
| Note (default) | `n"…"`, plain-string pattern arguments to builtins | pitch, bare MIDI number, chord symbol, sample name |
| Sample | `s"…"` | sample name only (pitch/chord detection disabled) |
| Chord | `c"…"`, `chord("…")` | chord symbols (standard lex path; sample-shaped atoms also resolve as chords) |
| Value | `v"…"` | signed numeric literals only (E163 otherwise) |
| Curve | `t"…"`, `timeline("…")` | level / ramp / smooth atoms |

All modes share: rests, elongation, grouping, sequencing, polymeter,
euclidean rhythms, modifiers, and random choice.

---

## 2. Structural grammar (all modes)

```ebnf
pattern    = { choice } ;
             (* top level: one element per cycle (per-cycle alternation).
                <…> is an explicit synonym; [ … ] packs into one cycle. *)

choice     = element { "|" element } ;      (* random choice each cycle *)

element    = atom [ euclid ] { modifier } ;

atom       = leaf                            (* mode-dependent, see §3 *)
           | rest
           | elongate
           | group
           | sequence
           | polymeter ;

group      = "[" { choice } "]"                            (* subdivision *)
           | "[" choice "," choice { "," choice } "]" ;    (* polyrhythm *)
             (* a comma anywhere in the bracket promotes it to polyrhythm:
                all comma-separated lanes play simultaneously *)

sequence   = "<" { choice } ">" ;            (* per-cycle alternation *)

polymeter  = "{" { choice } "}" [ "%" number ] ;
             (* %n sets the step count; default = number of children *)

euclid     = "(" number "," number [ "," number ] ")" ;
             (* atom(hits, steps [, rotation]) *)

modifier   = "*" number        (* speed up: per-element duration ÷ n *)
           | "/" number        (* slow down: per-element duration × n *)
           | "@" number        (* weight *)
           | "!" [ number ]    (* repeat; default 2 *)
           | "?" [ number ] ;  (* chance; default 0.5 *)
             (* modifiers chain left-to-right, each wrapping the result;
                *, /, @ REQUIRE the number *)

rest       = "~" ;             (* silent step — NOT in curve mode (§3.5) *)
elongate   = "_" ;             (* extends the previous step (Tidal `_`) *)
```

`*N` / `/N` are **per-element duration modifiers** — one uniform mechanism
at any nesting depth (no separate top-level vs inner behavior).

---

## 3. Leaf atoms (lexical, per mode)

Common building blocks:

```ebnf
digit         = "0"…"9" ;
decimal       = digit { digit } [ "." digit { digit } ]
              | "." digit { digit } ;
velocity_sfx  = ":" decimal ;                    (* clamped to 0..1 *)
record_suffix = "{" key ":" signed_dec { "," key ":" signed_dec } "}" ;
key           = ( letter ) { letter | digit | "_" } ;
signed_dec    = [ "-" | "+" ] decimal ;
                (* record_suffix must IMMEDIATELY follow the atom (no space
                   before "{"); if the content is not exactly
                   `ident : number , …` the lexer rewinds and the "{" lexes
                   as a polymeter brace instead *)
```

### 3.1 Pitch (Note mode; also Chord mode's fallback path)

```ebnf
pitch      = note_letter { pitch_mod } [ octave ] [ velocity_sfx ] [ record_suffix ] ;
note_letter = "a"…"g" | "A"…"G" ;
pitch_mod  = "#"            (* sharp, +1 semitone; stackable *)
           | "b"            (* flat, −1 semitone; stackable *)
           | "x"            (* double sharp, +2 *)
           | "^" | "+"      (* microtonal step up,   micro_offset +1 *)
           | "v" | "\"      (* microtonal step down, micro_offset −1 *)
           | "d" ;          (* micro down alias — ONLY when the remaining
                               modifier chain reaches an octave digit
                               (disambiguates from samples "bd", "sd") *)
octave     = digit [ digit ] ;    (* default 4; MIDI clamped 0..127 *)
```

- **Uppercase letters require an explicit octave** to be a pitch (`A4` is a
  pitch, bare `A` / `Am` fall through to chord detection — see §4).
- A pitch must be followed by a delimiter: end, whitespace, one of
  `* / @ ! ? %` `[ ] < > ( ) { }` `,` `|` `:`.

### 3.2 Bare MIDI number (Note mode)

```ebnf
midi_atom  = decimal [ velocity_sfx ] [ record_suffix ] ;
             (* 0..127 required; fractional values round to nearest
                semitone; `n"60"` ≡ `n"c4"` *)
```

### 3.3 Sample (Note & Sample modes)

```ebnf
sample     = samp_start { letter | digit | "#" } [ ":" digit { digit } ]
             [ record_suffix ] ;
samp_start = letter ;         (* any letter that didn't win pitch/chord
                                 detection; "_" excluded (elongate) *)
             (* ":" n selects the sample variant, e.g. bd:2 *)
```

In Chord mode, sample-shaped atoms are opportunistically re-parsed as chord
symbols (cached at parse time); non-chord-shaped names stay samples.

### 3.4 Value atom (Value mode)

```ebnf
value_atom = [ "+" | "-" ] decimal [ exponent ] ;
exponent   = ( "e" | "E" ) [ "+" | "-" ] digit { digit } ;
             (* raw scalar, no mtof; letter-leading atoms → error E163 *)
```

### 3.5 Curve atoms (Curve mode, `t"…"`)

```ebnf
curve_atom  = [ "~" ] curve_level | curve_ramp ;
curve_level = "_"    (* 0.00 *)
            | "."    (* 0.25 — unless followed by a digit (then: number) *)
            | "-"    (* 0.50 *)
            | "^"    (* 0.75 *)
            | "'" ;  (* 1.00 *)
curve_ramp  = "/" | "\" ;
              (* "/" followed by a digit is the slow modifier instead *)
              (* "~" is the smooth prefix and must precede a level;
                 curve mode therefore has NO rest atom *)
```

### 3.6 Chord symbol (Note & Chord modes)

Uppercase `A`–`G` + optional accidental + quality; full grammar and the
canonical quality table live in [chord-grammar.md](chord-grammar.md).
Supports `velocity_sfx` and `record_suffix` like pitches.

---

## 4. Lexer disambiguation rules

Reproduce these exactly when re-implementing:

1. **Uppercase A–G: chord first, then pitch.** `Am`, `C7`, `Fmaj7`, bare
   `G` → chord; `A4`, `C5` → pitch. Tie-break when the "quality" is a
   single digit: with an accidental (`Bb5`, `F#4`) → pitch; without,
   digits `0 1 2 3 4 8` → pitch octave, digits `5 6 7 9` → chord quality
   (`C7` dominant 7th, `G5` power chord).
2. **Lowercase a–g** → pitch if the whole token matches the pitch shape
   (`looks_like_pitch`: modifier chain, optional octave, then a valid
   delimiter); otherwise sample (`bd`, `sd`, `cp`…).
3. **`d` as micro-flat** only counts as a modifier when the remaining
   chain provably reaches an octave digit; otherwise the token is a sample.
4. **After a value-taking modifier** (`* / @ ! ? %`) — and anywhere inside
   euclid `( … )` — digits lex as plain `number`, never as value/MIDI
   atoms. So `v"1 2"*3` reads `3` as the speed factor.
5. **Chord tokens require a delimiter** after them (same set as pitches);
   otherwise chord detection is abandoned.
6. **Record suffix vs polymeter**: `{` right after an atom tries the
   record-suffix shape first and rewinds on any mismatch (see §3).
7. Whitespace (incl. newlines) separates elements; there is no comment
   syntax inside patterns.

---

## 5. Grammar-adjacent semantics (for reference)

- Top level and `<…>` alternate one element per cycle; `[…]` subdivides one
  cycle; `[a, b]` plays lanes simultaneously; `{a b c}%n` advances n steps
  per cycle with LCM wraparound; `a | b` picks randomly each cycle.
- `!` repeats the element; `@` weights its duration share; `?` drops it
  probabilistically.
- Pitch atoms carry MIDI + micro-offset + velocity + property record into
  pattern events (fields `@freq`, `@vel`, `@trig`, `@gate`, …).
- AST node types: `MiniPattern`, `MiniAtom`, `MiniGroup`, `MiniSequence`,
  `MiniPolyrhythm`, `MiniPolymeter`, `MiniChoice`, `MiniEuclidean`,
  `MiniModified` (`akkado/include/akkado/ast.hpp`).
