# Akkado Chord Symbol Grammar (EBNF)

Reference grammar for chord symbols, extracted from the implementation on
2026-07-04. Source of truth:

- `akkado/src/chord_parser.cpp` (`parse_chord_symbol`, `root_name_to_midi`)
- `akkado/include/akkado/music_theory.hpp` (`CHORD_INTERVALS` — canonical table)
- `akkado/src/mini_lexer.cpp` (`try_lex_chord_symbol` — in-pattern detection)

Companions: [akkado-grammar.md](akkado-grammar.md),
[mini-notation-grammar.md](mini-notation-grammar.md).

---

## 1. Grammar

```ebnf
chord_symbol = root quality ;

root         = root_letter { accidental } ;
root_letter  = "A"…"G" | "a"…"g" ;
               (* parse_chord_symbol() is case-insensitive and normalizes to
                  uppercase. Inside mini-notation patterns, chord DETECTION
                  only triggers on uppercase A–G — lowercase goes to
                  pitch/sample lexing first. *)
accidental   = "#" | "b" ;      (* stackable; each shifts ±1 semitone *)

quality      = { quality_char } ;
quality_char = letter | digit | "^" | "-" | "+" ;
               (* the whole remaining suffix is the quality; it is looked up
                  verbatim in CHORD_INTERVALS (§2). Unknown quality does NOT
                  fail: it coerces to a major triad {0,4,7} and the quality
                  string resets to "" (coerce-don't-fail). *)
```

Root MIDI defaults to octave 4 (`(octave+1)*12 + semitone + accidentals`,
clamped 0–127). `expand_chord` adds each interval to the root at the chosen
octave, dropping notes outside MIDI range.

Mini-notation extras (pattern context only — not part of `parse_chord_symbol`):

```ebnf
chord_atom = chord_symbol [ ":" velocity ] [ record_suffix ] ;
             (* Am:0.5 — velocity clamped 0..1; record_suffix per
                mini-notation-grammar.md §3 *)
```

A single-digit quality is ambiguous with a pitch octave; the tie-break
(digits `5 6 7 9` → chord, `0–4, 8` → pitch, any digit after an accidental
→ pitch) lives in mini-notation-grammar.md §4.

---

## 2. Canonical quality table (`CHORD_INTERVALS`)

Semitone intervals from the root. This table is the single source of truth
shared by `parse_chord_symbol()` and the mini-notation chord lexer.

| Quality | Intervals | Chord |
|---|---|---|
| *(empty)*, `M`, `maj` | 0 4 7 | Major triad |
| `m`, `min`, `-` | 0 3 7 | Minor triad |
| `dim`, `o` | 0 3 6 | Diminished |
| `aug`, `+` | 0 4 8 | Augmented |
| `sus`, `sus4` | 0 5 7 | Suspended 4th |
| `sus2` | 0 2 7 | Suspended 2nd |
| `7`, `dom7` | 0 4 7 10 | Dominant 7th |
| `M7`, `maj7`, `^`, `^7` | 0 4 7 11 | Major 7th |
| `m7`, `min7`, `-7` | 0 3 7 10 | Minor 7th |
| `dim7`, `o7` | 0 3 6 9 | Diminished 7th |
| `m7b5`, `0` | 0 3 6 10 | Half-diminished 7th |
| `aug7`, `+7` | 0 4 8 10 | Augmented 7th |
| `mM7`, `m^7`, `minmaj7` | 0 3 7 11 | Minor-major 7th |
| `6` | 0 4 7 9 | Major 6th |
| `m6`, `min6` | 0 3 7 9 | Minor 6th |
| `9` | 0 4 7 10 14 | Dominant 9th |
| `M9`, `maj9` | 0 4 7 11 14 | Major 9th |
| `m9`, `min9` | 0 3 7 10 14 | Minor 9th |
| `add9` | 0 4 7 14 | Add 9 |
| `add2` | 0 2 4 7 | Add 2 |
| `11` | 0 4 7 10 14 17 | Dominant 11th |
| `m11` | 0 3 7 10 14 17 | Minor 11th |
| `13` | 0 4 7 10 14 21 | Dominant 13th |
| `5` | 0 7 | Power chord |

---

## 3. Where chord symbols appear

- `c"…"` / `chord("…")` patterns (Chord mode).
- Uppercase atoms in Note-mode patterns (`"Am F C G"` inside `pat()` / `n"…"`).
- Sample-shaped atoms in Chord mode resolve opportunistically at parse time.
- The voicing system (`akkado/include/akkado/voicing.hpp`) consumes the
  parsed `(root_midi, intervals, quality)` and may override qualities via
  voicing dictionaries.

**Removed syntax**: apostrophe chord literals in the core language (`C4'`,
`Am7'`, slash-bass `F#m7_4'`) were removed 2026-05-21 by
`docs/prd-pattern-event-arrays.md`. Today `'Am7'` lexes as a plain string
and `'C4'` as a pitch literal; older PRDs/proposals may still show the dead
form.
