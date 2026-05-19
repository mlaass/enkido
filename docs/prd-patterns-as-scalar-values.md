> **Status: SHIPPED** — Phases A–G implemented (typed prefixes lex, MiniParseMode threads through mini-parser, `value()`/`note()`/`scalar()` builtins land, Pattern→Signal coerce + E160 reject for polyphonic-non-sample patterns, pattern-arg `bend()`/`aftertouch()`/`dur()`, custom-property accessor coerce coverage, smoke acceptance, and reference + tutorial docs). 539/539 akkado tests pass. §10.3 long-window experiments (`test_op_seqpat_step_value.py`, `test_op_seqpat_prop.py` covering bend / aftertouch / dur) render 320 s each with stable RMS, no >5 s silences, and spectrum-verified per-event values within ±5 Hz. The `m"…"` MIDI-int prefix (§11) remains a deferred follow-up. Subsumes Phase 2.1 §11.1 (custom-property accessor) and §11.2 (standalone `bend`/`aftertouch`/`dur` transforms). Follow-up to `prd-pattern-array-note-extensions-phase-2.md` and the design review `review-patterns-as-first-class-data.md`.

# PRD: Patterns as Scalar Values

## 1. Executive Summary

Today, patterns can only be consumed via field access (`p.freq`, `p.vel`) or via a fixed set of pattern-aware builtins (`osc(…, freq_pat)` works because `osc` is hand-wired). You cannot write `bend(notes, "<0 0.5 -0.5>")` and have the pattern's per-cycle values feed `bend`'s second argument as a scalar — the second arg expects a `Signal`, not a `Pattern`, and there is no implicit conversion.

This PRD makes patterns usable **anywhere a scalar `Signal` is accepted**. Two complementary mechanisms ship together:

1. **Typed mini-notation prefixes** disambiguate parse semantics at the literal site:
   - `v"<0 0.5 -0.5>"` — **value** pattern. Numeric atoms only; produces a stepped scalar.
   - `n"c4 e4 g4"` / `n"60 64 67"` — **note** pattern. Note names and bare MIDI ints both map to Hz via mtof.
   - `s"bd sd hh"` — **sample** pattern.
   - `c"Am C G"` — **chord** pattern.
   The legacy `pat` builtin / `n"…"` form stays as the auto-detect alias (additive, no breakage).

2. **Type-system-level implicit `Pattern → Signal` coercion**: when an expression of type `Pattern` is passed where a `Signal` is expected, the analyzer inserts an implicit `scalar()` cast that unwraps the pattern's primary value buffer. Multi-voice (chord) and sample patterns are rejected at the coerce site with a clear error — the user must pick a field or voice explicitly. An explicit `scalar(p)` builtin is also exposed as sugar for `p.freq` (works on `n`/`m`/`c` patterns).

### Key Design Decisions

- **Four typed prefixes (`v` / `n` / `s` / `c`); `n` stays as auto-detect.** No hard removal — pre-1.0 still, but additive change.
- **`v"…"` keeps `ValueType::Pattern`.** It does not become a different type. `.vel` / `.trig` / `.gate` access still works at the binding site. Coercion only fires at use sites that expect `Signal`.
- **Coerce site lives in the analyzer/visit pass**, not in each builtin handler. When an AST node produces `Pattern` and its consumer expects `Signal`, the visitor wraps the node in an implicit `scalar()` cast before codegen runs. This is the same shape the planned compiler-type-system pass will use for stereo lifting and signal lifting.
- **`scalar(p) ≡ p.freq`** for `n`/`m`/`c` patterns; error on `s` patterns. No voice/field selector — for any other field, write `p.vel` / `p.gate` directly.
- **Mini-notation parser gains a numeric mode** for `v"…"`. In numeric mode, atoms must parse as numeric literals (`-0.5`, `1e3`, `42`); symbolic atoms (`c4`, `bd`, `Am`) are a parse error. Full mini-notation otherwise — brackets, angle, polymeter, euclidean, generators (`run`, `binary`).
- **Auto-coerce only succeeds for monophonic, non-sample patterns.** Multi-voice (chord) or sample patterns in scalar context error at the coerce point with E160; user must select a field/voice explicitly.
- **User-defined functions get default `Signal` coercion** for any `Pattern` arg. Per-param kind annotations (`pitch` / `signal` / `pattern`) are out of scope here and tracked under `prd-compiler-type-system.md`.
- **Phase 2.1 §11.1 / §11.2 ride on this foundation.** This PRD subsumes their remaining surface: once auto-coerce lands, `bend(notes, v"<0 0.5 -0.5>")` "just works" through the standard coerce path; per-event property buffers (`SEQPAT_PROP`) feed the same coerce path when accessed via `as e |> e.cutoff`.

---

## 2. Problem Statement

### 2.1 Current behavior

Patterns are second-class:

- `osc("sin", n"c4 e4 g4")` works (osc-freq is hand-wired to accept patterns).
- `osc("sin", n"c4 e4 g4")` works (auto-detect gives a note pattern).
- `bend(notes, "<0 0.5 -0.5>")` **does not work**. `bend`'s second arg is a scalar slot, and a string literal is not a pattern in scalar context.
- `bend(notes, n"<0 0.5 -0.5>")` **does not work**. `n"…"` produces a `Pattern`, but `bend`'s codegen has no path that consumes `Pattern` as a scalar value.
- `lp(sig, n"<200 800 2000>", 0.7)` **does not work** for the same reason — the cutoff slot expects `Signal`.

The user-facing consequence: every pattern consumer must be hand-wired in codegen. This blocks:

- Per-event modulation pulled from a pattern (filter cutoff, bend depth, aftertouch, custom DSP params).
- The user's reported gap: `bend("<0 0.5 -0.5>")` syntax for time-varying bend depth.
- Symmetric use of patterns as values in arithmetic: `v"<0 0.5>" * 12` does not currently produce a `Signal`.

### 2.2 Mini-notation parse-mode ambiguity

`n"…"` is an auto-detector. It guesses note vs sample vs chord by atom shape. For scalar values the auto-detector is ambiguous — `"<0 0.5 -0.5>"` could mean MIDI 0 / 0.5 / -0.5 (mtof-applied), or could mean raw scalars 0, 0.5, -0.5. There is no way for the user to express their intent.

| Today's behavior | What user wants for `bend()` |
|------------------|------------------------------|
| `n"<0 0.5>"` → atoms parsed as MIDI ints → mtof → ~8.18 Hz, ~13 Hz | Atoms parsed as raw numbers → 0.0, 0.5 |
| No way to say "this string is numeric values, not pitches" | Need a syntactic signal at the literal site |

### 2.3 Phase 2.1 standalone `bend`/`aftertouch`/`dur` transforms (§11.2)

These shipped as part of f8dca4d but only by accepting per-event constants or by mirroring `velocity()`'s shape. The richer use case — passing a *pattern* of values — was deferred because it requires both a parse-mode for raw numerics and a `Pattern → Signal` coerce path on the consumer side. This PRD closes both.

---

## 3. Goals and Non-Goals

### Goals

1. Add four typed mini-notation prefixes — `v"…"`, `n"…"`, `s"…"`, `c"…"` — and corresponding builtin functions `value()`, `note()`, `sample()`, `chord()` (the latter three already exist in some form; `value` is new and `note` may need polymorphism over patterns).
2. Add a numeric-atom parse mode to `MiniLexer` / `MiniParser`, gated by the prefix in use.
3. Implement type-system-level implicit `Pattern → Signal` coercion in the analyzer/visit pass: when a Pattern-typed expression feeds a Signal-expecting position, wrap with an implicit `scalar()` cast before codegen.
4. Implement `scalar(p)` as an explicit builtin that returns `p.freq` for `n`/`m`/`c` patterns; error on `s` patterns.
5. Reject multi-voice / sample patterns in scalar coerce sites with diagnostic E160, naming the disallowed shape and pointing to the explicit field-access path (`.freq`, `poly`, etc.).
6. Subsume Phase 2.1 §11.1 (custom-property accessor `as e |> e.cutoff`) and §11.2 (standalone `bend`/`aftertouch`/`dur` transforms accepting pattern args) — both consume the new coerce path.
7. Keep the legacy `pat` builtin / `n"…"` working as the auto-detect alias. No migration of existing user code is required, but emit a soft compiler warning W170 when `n"…"` parses unambiguously as a single mode (suggesting the typed prefix). Tutorials and reference docs migrate to the typed-prefix style throughout.
8. Per-mode unit tests for the four prefixes; integration tests for the `bend(notes, v"<0 0.5 -0.5>")` user story; coerce-error tests for chord/sample patterns in scalar slots.
9. **Documentation and tutorial coverage** (full scope, see §10.6):
   - New: `web/static/docs/reference/pattern/literals.md` (and parent dir `pattern/`).
   - New: `web/static/docs/tutorials/06-pattern-modulation.md` covering typed prefixes, the Pattern→Signal coerce mental model, the `bend(notes, v"…")` flagship, and the `e.cutoff` custom-property pattern.
   - Update: `web/static/docs/reference/mini-notation/basics.md` — typed-prefix table.
   - Update: `web/static/docs/concepts/signals.md` — Pattern↔Signal section explaining coerce, idempotent `scalar()`, and the operator type rules from §5.3.1.
   - Update: `web/static/docs/tutorials/04-rhythm.md` — short cross-reference to the new modulation tutorial; migrate any legacy pattern examples to typed prefixes.
   - Update: `docs/Akkado A Rhythmic & DSP Language Specification.md` — typed prefixes + coerce rules.
   - Update: `docs/mini-notation-reference.md` — typed-prefix table; numeric atom mode.
   - Regenerate F1 lookup index via `bun run build:docs`.

### Non-Goals

- **User-defined-function param-kind annotations.** `fn f(freq: pitch)` is out of scope; tracked under `prd-compiler-type-system.md`. User fns get default `Signal` coerce for any `Pattern` arg passed.
- **Removing the legacy `pat` builtin / `n"…"` auto-detect.** It stays additive. The W170 soft warning suggests the typed prefix when the parse is unambiguous, but `n"…"` is never an error and never breaks. A future PRD may deprecate it once telemetry shows the typed prefixes have replaced it.
- **Per-builtin slot-kind metadata for pitch vs numeric.** Slot kind is `Signal` everywhere; `n"…"` literals still apply mtof to integer atoms because they always do — that's encoded in the lexer's parse mode, not in slot metadata.
- **Stereo lifting through patterns.** Patterns remain mono. `Pattern → Stereo Signal` is not auto-handled; users wrap with `stereo()` explicitly.
- **Full runtime pattern mutation** (Approach A from the review). This PRD only adds *consumption* of patterns as values — not runtime mutation of pattern event content. That stays as future work in `review-patterns-as-first-class-data.md`.
- **`m"…"` as a separate MIDI-int prefix.** Subsumed by `n"…"`, which detects bare ints vs note names per-atom.
- **New cedar opcodes** for the coerce path. The Pattern's primary value buffer (the `freq` field, populated by `SEQPAT_STEP`) IS the Signal — no new opcode needed for the cast itself. (Phase 2.1 already shipped `SEQPAT_PROP` for custom keys.)

---

## 4. Target Syntax

### 4.1 Typed prefixes — overview

| Prefix | Builtin | Atom semantics | Output `PatternPayload.fields` | Auto-coerce to Signal |
|--------|---------|----------------|-------------------------------|------------------------|
| `v"…"` | `value(str)` | Numeric only: `-0.5`, `42`, `1e3`, `0.125` | `freq` ← raw value, `vel` ← 1.0 default, `trig` / `gate` populated | Yes — the freq buffer holds raw scalars |
| `n"…"` | `note(str)` | Note names (`c4`, `eb5`) AND bare MIDI ints (`60`, `127`); both → mtof → Hz | `freq` ← Hz, `vel` ← per-atom velocity, `trig` / `gate` populated | Yes — freq buffer is the scalar |
| `s"…"` | `sample(str)` | Sample names: `bd`, `sd`, `kick:2`, `loop@bank` | `freq` ← type_id (rarely used), `vel` populated | **No** — error E160 if used in scalar slot |
| `c"…"` | `chord(str)` | Chord symbols: `Am`, `C7`, `F#m7b5_3` | Multi-voice `freq` (1–N voices) | **No** — error E160 if used in scalar slot |
| `n"…"` | `pat` builtin (legacy) | Auto-detect (legacy) | Depends on detected mode | Inherits from detected mode — yes if detected as note or value |

### 4.2 The user's bend() example — now works

```akkado
// Original request:
bend("<0 0.5 -0.5>")  // standalone, no host pattern — semantic? Probably error.

// Concrete usage (the actual user story):
n"c4 e4 g4" |> bend(@, v"<0 0.5 -0.5>") |> mtof(@ + bend(@) * 12) |> osc("sin", @)

// Functional form:
bend(n"c4 e4 g4", v"<0 0.5 -0.5>")

// In a record / pipe-binding:
n"c4 e4 g4" as e |> osc("sin", e.freq + v"<0 -10 10 0>")  // detune scalar pattern
```

### 4.3 Patterns as scalars in arithmetic and DSP slots

```akkado
// v"" patterns work in any Signal slot:
osc("sin", v"<220 440 880>")            // raw Hz values, no mtof
lp(sig, v"<200 800 2000>", v"<0.7 0.5>")  // cutoff and Q both pattern-driven
sig * v"<0.2 0.5 1.0 0.5>"              // amplitude envelope per cycle

// Pattern arithmetic (auto-coerce on each operand):
v"<0 0.5 -0.5>" * 12       // → Signal carrying 0/6/-6 stepped over the cycle
v"<60 64 67>" + v"<0 0 0 12>"  // chromatic + octave-shift overlay; emits sum at sample rate

// Using note + value patterns together:
osc("saw", n"c4 e4 g4" + v"<0 0 12 0>")  // octave displaces the third event
```

### 4.4 Explicit `scalar()` cast

```akkado
// Sugar for .freq on note/midi/chord patterns:
scalar(n"c4 e4 g4")        // Signal carrying mtof(c4), mtof(e4), mtof(g4) stepped
scalar(n"c4 e4 g4") * 2    // 2× freq, same pattern timing
osc("sin", scalar(n"c4 e4 g4"))   // identical to osc("sin", n"c4 e4 g4") — auto-coerce already does this

// Errors:
scalar(s"bd sd")           // E161: cannot cast sample pattern to scalar
scalar(c"Am C G")          // E161: cannot cast chord pattern to scalar; pick a voice or use poly()

// dot-call form:
n"c4 e4 g4".scalar()       // identical to scalar(n"c4 e4 g4")
```

### 4.5 Pipe-binding `as e |> e.<field>` still works

```akkado
// Binding preserves Pattern type:
v"<0.3 0.5 0.7>" as e |> osc("saw", 440 + e.freq * 100)
//   ^^ e is bound as Pattern; e.freq is a Signal pulled from the value buffer.
//   e.vel, e.trig, e.gate also accessible.

n"c4 e4 g4" as e |> osc("sin", e.freq) |> @ * e.vel |> out(@, @)
//   identical to today's behavior; n"" doesn't change pipe-binding semantics.
```

### 4.6 Custom-property accessor (subsumed §11.1)

```akkado
n"c4{cutoff:0.3} e4{cutoff:0.7} g4{cutoff:0.5}" as e
  |> osc("saw", e.freq)
  |> lp(@, 200 + e.cutoff * 4000)   // e.cutoff is Signal pulled from SEQPAT_PROP buffer
  |> out(@, @)
```

`e.cutoff` returns a Signal-typed `TypedValue` resolved through the existing `PatternPayload.custom_fields` map (already wired in `typed_value.cpp:56` for Phase 2.1). This PRD adds the test coverage and the documented behavior; the runtime plumbing landed in commit f8dca4d.

### 4.7 Standalone `bend` / `aftertouch` / `dur` with pattern args (subsumed §11.2)

```akkado
// Pattern-valued bend depth:
n"c4 e4 g4".bend(v"<0 0.5 -0.5>")

// Pattern-valued aftertouch envelope:
n"c4 e4 g4 b4".aftertouch(v"<0 0.25 0.5 1.0>")

// Pattern-valued duration override:
n"c4 e4 g4".dur(v"<0.25 0.5 0.75>")

// Constant arg still works (current Phase 2.1 behavior):
n"c4 e4 g4".bend(0.5)
```

The transform recognizes that its second arg may be a `Pattern` and routes through the auto-coerce path. The resulting Signal is sample-and-held at each event's trigger time and stored on the event's per-property buffer.

---

## 5. Architecture

### 5.1 Lexer — new prefix tokens

`akkado/src/lexer.cpp:425` already handles `n"…"` and `t"…"`. Extend the same path with three new prefix characters: `v`, `n`, `s`, `c`.

```cpp
// At lex_identifier(), after the existing p/t checks:
if (current_ == start_ + 1) {
    char c = source_[start_];
    char next = peek();
    if (next == '"' || next == '`') {
        switch (c) {
            case 'v': return make_token(TokenType::ValuePat);
            case 'n': return make_token(TokenType::NotePat);
            case 's': return make_token(TokenType::SamplePat);
            case 'c': return make_token(TokenType::ChordPat);
            // 'p' and 't' already handled above
        }
    }
}
```

`TokenType` gains four new variants: `ValuePat`, `NotePat`, `SamplePat`, `ChordPat`. Each carries an enum-tagged "parse mode" for the mini-parser.

**Disambiguation with single-letter identifiers.** `s` / `n` / `c` / `v` are all currently legal identifier names. Today, `s` followed by a quote is ambiguous-looking. Resolution:

- Lexer only treats `<letter>"` / `<letter>\`` as a string-prefix when the letter token is exactly one character at this position AND the next character is a quote. Otherwise it lexes as a regular identifier.
- Existing `n"…"` already follows this rule (`current_ == start_ + 1`). New prefixes use the same gate.
- Identifier shadowing: if a user writes `s = 0.5` and then `s"bd"`, the lexer still produces `SamplePat` followed by a string body — never falls through to identifier `s`. Document this in the language reference; the conflict is rare and analogous to existing `n"…"` behavior.

### 5.2 Mini-notation parser — numeric mode

`akkado/src/mini_lexer.cpp` and `akkado/src/mini_parser.cpp` gain a `MiniParseMode` enum that **subsumes** the existing `bool sample_only` and `bool curve_mode` flags on `MiniLexer`'s constructor (`mini_lexer.hpp:35`):

```cpp
enum class MiniParseMode : std::uint8_t {
    Auto,    // n"…" — current behavior, detect per atom
    Note,    // n"…" — note names + bare MIDI ints both → mtof
    Sample,  // s"…" — atom = sample name (replaces `sample_only = true`)
    Chord,   // c"…" — atom = chord symbol
    Value,   // v"…" — atom must be numeric literal; raw value, no mtof
    Curve,   // t"…" — timeline / breakpoint mode (replaces `curve_mode = true`)
};
```

The mode flows from `Lexer` → `Parser::parse_mini_string()` → `MiniLexer` constructor. Migration: every `MiniLexer(view, loc, sample_only, curve_mode)` call site is replaced with `MiniLexer(view, loc, mode)`. The two existing call sites that pass `sample_only=true` become `MiniParseMode::Sample`; the one that passes `curve_mode=true` becomes `MiniParseMode::Curve`. All other call sites pass `MiniParseMode::Auto`.

**Per-mode atom rules:**

| Mode | Atom accepted | Atom rejected | Resulting `MiniAtomData` |
|------|---------------|---------------|--------------------------|
| `Auto` | (current behavior) | — | (current behavior) |
| `Note` | `c4`, `eb5`, `60`, `0` (treated as MIDI 0), `Am'` (chord literal) | None — falls back to current auto-detect within Note semantics | `pitch_hz` populated via mtof |
| `Sample` | Sample-name token: `bd`, `sd`, `kick:2`, `loop@bank` | Numeric atoms (warn or error E162) | `sample_name` populated |
| `Chord` | Chord symbol: `Am`, `C7`, `F#m7b5` | Numeric atoms, sample names | `chord_root_midi` + `chord_intervals` populated |
| `Value` | Numeric literal: `0`, `0.5`, `-0.5`, `1e3`, `-1.25e-2` | Note names (`c4`), sample names (`bd`), chord symbols (`Am`) — parse error E163 | `value` field populated; `pitch_hz` left at default |

**`MiniAtomData` change:** add a `float scalar_value` field used in `Value` mode. `pat_eval` writes `scalar_value` into `Event::values[0]` directly (bypasses mtof).

### 5.3 Type system — implicit cast at visit

The analyzer (currently inlined in `CodeGenerator::visit`) gains a coerce step before each consumer reads its arg's `TypedValue`.

**Where the coerce hook lives.** Today, builtins-call dispatch in `codegen.cpp` walks each arg, calls `visit(arg_node)`, and stores the result. Add:

```cpp
TypedValue CodeGenerator::coerce_arg_for_signal(NodeIndex arg_node) {
    TypedValue tv = visit(arg_node);
    if (tv.type == ValueType::Pattern) {
        return implicit_scalar_cast(tv, arg_node);
    }
    return tv;
}

TypedValue CodeGenerator::implicit_scalar_cast(const TypedValue& tv, NodeIndex node) {
    if (!tv.pattern) {
        return TypedValue::error_val();
    }
    // Reject sample patterns (s"…") and chord/multi-voice patterns.
    if (tv.pattern->is_sample_pattern) {
        emit_error(node, ErrorCode::E160_PatternToScalarSample,
                   "cannot use sample pattern as scalar; pick a field (e.g. p.type) "
                   "or use sampler() / poly() to consume it");
        return TypedValue::error_val();
    }
    if (tv.pattern->max_voices > 1) {
        emit_error(node, ErrorCode::E160_PatternToScalarPolyphonic,
                   "cannot use polyphonic pattern as scalar; pick a field (e.g. p.freq) "
                   "or use poly() to consume it");
        return TypedValue::error_val();
    }
    // Coerce: the freq buffer IS the scalar Signal.
    std::uint16_t buf = tv.pattern->fields[PatternPayload::FREQ];
    if (buf == 0xFFFF) {
        emit_error(node, ErrorCode::E160_PatternToScalarMissing,
                   "pattern has no value buffer to coerce to scalar");
        return TypedValue::error_val();
    }
    return TypedValue::signal(buf);
}
```

**Where `coerce_arg_for_signal` is called.** Three sites — the boundary between Pattern and the rest of the language:

1. **Builtin call args** (most common). For each builtin in `BUILTIN_FUNCTIONS` map (`akkado/include/akkado/builtins.hpp:147`), the dispatch handler reads its args via `coerce_arg_for_signal()` instead of bare `visit()`. **Exceptions** (must use raw `visit()`): handlers that intentionally take a `Pattern` arg — `pat`, `note`, `value`, `chord`, `sample`, `slow`, `fast`, `rev`, every transform in `is_pattern_call()` (defined at `codegen_patterns.cpp:1768`), and `poly`. Mark these with a `bool args_are_signal` flag in `BuiltinInfo`, default true.

> **Note: `args_are_signal` vs the existing `auto_lift` flag.** `BuiltinInfo` already has an `auto_lift` flag that controls Signal→Stereo lifting. The new `args_are_signal` is orthogonal: it controls Pattern→Signal coercion. Both flags can apply to the same builtin (e.g., `osc` lifts mono→stereo when fed a stereo input AND coerces a Pattern arg in its freq slot). Order is fixed: Pattern→Signal first, then Signal→Stereo if applicable.
2. **Binary / unary operators** (`+`, `-`, `*`, `/`, `@`, comparisons). See §5.3.1 below for the operator type rules — operators only coerce when at least one operand is non-Pattern.
3. **Pipe expression** (`a |> b`). When `a` is a Pattern and `b`'s first slot expects Signal, coerce.

#### 5.3.1 Operator type rules

The operator visit branch resolves types using these rules (applies to `+`, `-`, `*`, `/`, `@`, and comparisons; unary operators apply the same rule with one operand):

| LHS         | RHS         | Result type      | Coerce? |
|-------------|-------------|------------------|---------|
| Pattern     | Pattern     | Pattern          | No — both stay as patterns; result is a synthetic pattern whose `freq` buffer holds the pointwise op of both operands' freq buffers. Cycle alignment follows existing pattern-engine rules (longest-cycle-wins). |
| Pattern     | Signal      | Signal           | Yes on Pattern. The pattern coerces to Signal (its freq buffer); the operator runs at sample rate. |
| Pattern     | Number      | Pattern          | No. The constant lifts to a stepped buffer matching the pattern's events; result remains Pattern. (Common case: `n"c4 e4" + 12` is still a Pattern with shifted freqs.) |
| Signal      | Signal      | Signal           | N/A. |
| Signal      | Stereo      | Stereo           | N/A — handled by existing `auto_lift`. |
| Pattern     | Stereo      | **Error E165**   | Stereo coerce is out of scope; user must wrap explicitly: `stereo(scalar(p)) * stereoSig`. |

**Worked examples:**

- `let x = v"<0 0.5>" + 12` — RHS is `Number`, so result is `Pattern`. `x` has `ValueType::Pattern`; `.vel`, `.trig` still accessible.
- `let y = v"<0 0.5>" + sig` — RHS is `Signal`, so result is `Signal`. `y` has `ValueType::Signal`.
- `let z = v"<0 0.5>" + n"c4 e4"` — both Pattern, result is `Pattern`. Synthetic combined pattern.
- `lp(sig, v"<200 800>" * 2, 0.7)` — `v"…" * 2` is `Pattern` (Pattern + Number); `lp`'s cutoff slot expects `Signal`, so coerce fires at the `lp` call site, not at the `*`.
- `osc("sin", v"<0 0.5>" * stereoCarrier)` — **Error E165**: stereo operand on Pattern. Diagnostic points to `stereo(scalar(...))` workaround.

**`PatternPayload` flag changes.** Add two booleans referenced by the coerce hook:

```cpp
struct PatternPayload {
    // … existing fields …
    bool is_sample_pattern = false;   // set by sample() / s"" / legacy pat detect
    std::uint8_t max_voices = 1;      // set by chord() / c"" / legacy pat detect
};
```

`SequenceCompiler::is_sample_pattern()` and the existing `max_voices` plumbing (already in compiler) populate these on payload construction.

### 5.4 `value()` builtin and the `v"…"` literal

```cpp
// In builtins.hpp:
{"value", {Opcode::NOP, 1, 0, false,    // 1 required arg: string literal
           {"pattern_str", "", "", "", ""},
           {NAN, NAN, NAN, NAN, NAN},
           "Numeric pattern literal (raw scalars, no mtof)"}}
```

Codegen for `value(str)` mirrors the legacy `pat` handler (`codegen_patterns.cpp` `handle_pat_call`), but threads `MiniParseMode::Value` to the parser and asserts `MiniAtomData.value` is populated.

The `v"…"` literal token desugars at parse time to `v"…"` exactly the way `n"…"` desugars to `n"…"`.

### 5.5 `note()` / `sample()` / `chord()` builtins

These already exist (`note()` and `chord()`) or have closely-related forms (`sample()` is partially the auto-detect path). Changes:

- `note(str)` — explicit Note mode parse. Distinct from the legacy `pat` builtin only in that it forces the parse mode (no sample-name fallback).
- `sample(str)` — explicit Sample mode parse. Today, sample patterns come out of the legacy auto-detect path; expose a direct entrypoint for `s"…"`.
- `chord(str)` — already exists; now also reachable via `c"…"` literal.
- The existing legacy `pat` builtin sets `MiniParseMode::Auto` and is unchanged.

### 5.6 `scalar()` builtin

```cpp
// In builtins.hpp:
{"scalar", {Opcode::NOP, 1, 0, false,
            {"pattern", "", "", "", ""},
            {NAN, NAN, NAN, NAN, NAN},
            "Cast a note/midi/chord pattern to its primary value buffer as a Signal"}}
```

```cpp
// codegen — handle_scalar_call in codegen_patterns.cpp:
TypedValue handle_scalar_call(const Node& call_node) {
    if (call_node.children.size() != 1) {
        emit_error(call_node, "scalar() takes exactly one pattern argument");
        return TypedValue::error_val();
    }
    TypedValue arg = visit(call_node.children[0]);  // raw visit — DON'T coerce here
    if (arg.type == ValueType::Signal) {
        return arg;  // idempotent: scalar(signal) → signal unchanged
    }
    if (arg.type != ValueType::Pattern || !arg.pattern) {
        emit_error(call_node, "scalar() expects a Pattern or Signal, got " + value_type_name(arg.type));
        return TypedValue::error_val();
    }
    if (arg.pattern->is_sample_pattern) {
        emit_error(call_node, "scalar() cannot cast a sample pattern; pick a field explicitly");
        return TypedValue::error_val();
    }
    if (arg.pattern->max_voices > 1) {
        emit_error(call_node, "scalar() cannot cast a polyphonic pattern; pick a voice with .freq[i] or use poly()");
        return TypedValue::error_val();
    }
    std::uint16_t buf = arg.pattern->fields[PatternPayload::FREQ];
    if (buf == 0xFFFF) {
        emit_error(call_node, "scalar() pattern has no freq buffer");
        return TypedValue::error_val();
    }
    return TypedValue::signal(buf);
}
```

`scalar(p)` is intentionally not free — it explicitly returns a Signal even when the surrounding context would not have triggered auto-coerce. Useful in pipelines where the pattern would otherwise be passed through pattern-aware transforms.

### 5.7 Subsumed Phase 2.1 work

#### 5.7.1 §11.1 custom-property accessor — already shipped, now formalized

Phase 2.1 commit f8dca4d shipped `SEQPAT_PROP` and the `custom_fields` map on `PatternPayload`. `pattern_field()` already falls through to `custom_fields` when a name is not in the fixed-field set (`typed_value.cpp:56`).

This PRD's contribution: the auto-coerce hook treats custom-field Signal results the same as fixed-field Signal results. `n"c4{cutoff:0.3}" as e |> lp(@, e.cutoff * 4000 + 200)` works because `e.cutoff` returns `TypedValue::signal(custom_fields["cutoff"])`, which feeds `lp`'s cutoff slot natively without needing further coercion. Test coverage and a documented happy path are added.

#### 5.7.2 §11.2 standalone `bend`/`aftertouch`/`dur` with pattern args

The transforms already exist as Phase 2.1 dispatch handlers, but they only accept constant `Number` second args. This PRD extends them:

- `handle_bend_call` / `handle_aftertouch_call` / `handle_dur_call` now accept `Signal` as the second arg. When the second arg is a `Pattern`, the auto-coerce pass converts it to `Signal` before the handler runs.
- The Signal is sample-and-held at each host pattern's event trigger and written to the per-event property buffer via the existing `SEQPAT_PROP` machinery.
- For the constant case (e.g. `bend(notes, 0.5)`), behavior is unchanged.

**No new opcodes** — the path reuses `SEQPAT_PROP`, which already accepts a per-event Signal source.

### 5.8 Identity preservation for top-level bindings

```akkado
x = v"<0 0.5 -0.5>"        // x has ValueType::Pattern
osc("sin", x)              // auto-coerce at osc-freq slot — emits Signal usage
y = x.vel                  // x stays Pattern, .vel still works
```

The binding `let` / `=` does not coerce. Coerce fires only at the consumer site. This matches the planned semantics in `prd-compiler-type-system.md`.

---

## 6. Impact Assessment

| Component | Status | Notes |
|-----------|--------|-------|
| `pat` builtin / `n"…"` auto-detect | **Stays** | Unchanged; remains the legacy entrypoint. |
| `chord()` / `c"…"` | **Modified** | `c"…"` literal added to lexer; builtin unchanged behaviorally. |
| `sample()` / `s"…"` | **Modified** | `s"…"` literal added; `sample()` builtin exposed as the explicit form (replaces partial auto-detect for sample patterns). |
| `note()` / `n"…"` | **Modified** | `n"…"` literal added; `note()` builtin exposed as the explicit Note-mode form. |
| `value()` / `v"…"` | **New** | Numeric-only mini-notation pattern. |
| `scalar()` builtin | **New** | Explicit `Pattern → Signal` cast. |
| `MiniLexer` / `MiniParser` | **Modified** | New `MiniParseMode` enum; per-mode atom rules; numeric-atom path. |
| `Lexer` | **Modified** | Four new string-prefix tokens (`v`/`n`/`s`/`c`). |
| `Parser` | **Modified** | New token types desugar to corresponding builtin calls. |
| `MiniAtomData` | **Modified** | Add `float scalar_value` field for `Value`-mode atoms. |
| `PatternPayload` | **Modified** | Add `is_sample_pattern` and `max_voices` flags, populated at compile. |
| `TypedValue` | **Stays** | No new ValueType; coerce produces a `Signal` from the freq buffer. |
| `CodeGenerator::visit` | **Modified** | Add `coerce_arg_for_signal` helper; route builtin/operator/pipe arg evaluation through it. |
| `BuiltinInfo` | **Modified** | Add `bool args_are_signal` (default true); pattern-aware builtins set false. Orthogonal to the existing `auto_lift` flag (Signal→Stereo). |
| Existing pattern-aware builtins (`osc`, `slow`, `pat`, etc.) | **Stays** | Behavior unchanged; opt out of auto-coerce via `args_are_signal = false`. |
| `bend()` / `aftertouch()` / `dur()` transforms (Phase 2.1) | **Modified** | Accept `Pattern` as second arg via the auto-coerce path; constant case unchanged. |
| `SEQPAT_PROP` opcode | **Stays** | Reused for the new pattern-arg path. |
| `cedar::Event` | **Stays** | No new fields. |
| Existing tests | **Stays** | All current behavior preserved. |
| `docs/mini-notation-reference.md` | **Modified** | Document the four prefixes; numeric atom mode. |
| `docs/Akkado A Rhythmic & DSP Language Specification.md` | **Modified** | Typed prefixes + coerce rules + operator type table from §5.3.1. |
| `web/static/docs/reference/pattern/` | **New directory** | Holds the new pattern reference page. |
| `web/static/docs/reference/pattern/literals.md` | **New** | Typed prefixes (`v`/`n`/`s`/`c`/`n`); side-by-side examples; coerce mental model. |
| `web/static/docs/reference/mini-notation/basics.md` | **Modified** | Typed-prefix overview table; pointer to `literals.md`. |
| `web/static/docs/concepts/signals.md` | **Modified** | New section "Patterns and Signals" — coerce rules, operator type rules, idempotent `scalar()`. |
| `web/static/docs/tutorials/06-pattern-modulation.md` | **New** | Tutorial chapter: typed prefixes, Pattern→Signal mental model, `bend(notes, v"…")` flagship, custom-property accessor (`e.cutoff`), the `lp(@, e.cutoff * 4000)` workflow. |
| `web/static/docs/tutorials/04-rhythm.md` | **Modified** | Cross-reference to chapter 06; migrate any legacy pattern examples to typed prefixes. |
| F1 help index | **Regenerated** | `bun run build:docs` regenerates lookup-index for new prefixes/builtins. |

---

## 7. File-Level Changes

| File | Change |
|------|--------|
| `akkado/include/akkado/token.hpp` | Add `TokenType::ValuePat`, `NotePat`, `SamplePat`, `ChordPat`. |
| `akkado/src/lexer.cpp` (~line 425) | Add `v` / `n` / `s` / `c` single-char prefix recognition (mirror `n` / `t`). |
| `akkado/src/parser.cpp` | Recognize new token types and desugar to `value()` / `note()` / `sample()` / `chord()` calls. |
| `akkado/include/akkado/mini_token.hpp` | Add `MiniParseMode` enum. |
| `akkado/src/mini_lexer.cpp` | Accept a `MiniParseMode` parameter; per-mode atom rules. Add numeric-literal lex path for `Value` mode (signed, decimals, scientific). |
| `akkado/src/mini_parser.cpp` | Threads parse mode; rejects mismatched atoms with E162 / E163. |
| `akkado/include/akkado/ast.hpp` | Add `float scalar_value` to `MiniAtomData`. |
| `akkado/src/pattern_eval.cpp` | When emitting a `PatternEvent` from a `Value`-mode atom, write `scalar_value` into `Event::values[0]` directly. |
| `akkado/include/akkado/typed_value.hpp` | Add `bool is_sample_pattern` and `std::uint8_t max_voices` to `PatternPayload`. |
| `akkado/src/typed_value.cpp` | (No change to `pattern_field` — fixed-field + custom-field path already covers what's needed.) |
| `akkado/include/akkado/builtins.hpp` | Add `value`, `scalar` entries. Add `bool args_are_signal` field (default true) and set false on `pat`, `value`, `note`, `sample`, `chord`, `slow`, `fast`, `rev`, `transpose`, `velocity`, `bank`, `variant`, `tune`, `transport`, `early`, `late`, `palindrome`, `compress`, `ply`, `linger`, `zoom`, `segment`, `swing`, `swingBy`, `iter`, `iterBack`, `run`, `binary`, `binaryN`, `anchor`, `mode`, `voicing`, `addVoicings`, `bend`, `aftertouch`, `dur`, `poly`, `scalar`. |
| `akkado/include/akkado/codegen.hpp` | Declare `handle_value_call`, `handle_scalar_call`, `coerce_arg_for_signal`, `implicit_scalar_cast`. |
| `akkado/src/codegen.cpp` | Register `value` and `scalar` dispatch entries. Implement `coerce_arg_for_signal` and route every `args_are_signal == true` builtin's arg evaluation through it. Apply the same coerce in binary-op visit branches. |
| `akkado/src/codegen_patterns.cpp` | Implement `handle_value_call` (mirrors `handle_pat_call` with `MiniParseMode::Value`). Populate `PatternPayload.is_sample_pattern` / `max_voices` on every payload construction (8 sites). Implement `handle_scalar_call`. Update `handle_bend_call`, `handle_aftertouch_call`, `handle_dur_call` to accept `Signal` second args via the coerce path. |
| `akkado/include/akkado/diagnostics.hpp` | Add error codes E160 (pattern→scalar reject), E161 (scalar() reject), E162 (sample mode wrong-atom), E163 (value mode non-numeric atom), E164 (record suffix on value-mode atom), E165 (Pattern × Stereo). Add warning W170 (`n"…"` parses unambiguously as one mode; suggest typed prefix). |
| `akkado/src/codegen_functions.cpp` | When a user-defined function call has `Pattern` args, route through `coerce_arg_for_signal` (default Signal coercion per Round 4 decision). |
| `akkado/tests/test_pattern_prefixes.cpp` (new) | Per-prefix lex / parse tests + auto-coerce integration tests. |
| `akkado/tests/test_pattern_scalar.cpp` (new) | `v"…"` numeric atoms; `scalar()` cast on `n`/`s`/`c` patterns; auto-coerce success and reject paths. |
| `akkado/tests/test_pattern_event.cpp` (existing) | Add `bend(notes, v"…")`, `aftertouch(notes, v"…")`, `dur(notes, v"…")` integration tests. |
| `akkado/tests/test_lexer.cpp` | Coverage for the four new string-prefix tokens; identifier-vs-prefix disambiguation. |
| `akkado/tests/test_mini_parser.cpp` | Per-mode parse-mode tests; reject paths (E162, E163). |
| `experiments/test_op_seqpat_prop.py` | Long-window experiment (300+ s) confirming pattern-driven `bend`/`aftertouch`/`dur` produce correct per-event values across many cycles. |
| `web/static/docs/reference/pattern/literals.md` (new) | Document `v"…"`, `n"…"`, `s"…"`, `c"…"` prefixes; side-by-side examples; coerce mental model. |
| `web/static/docs/reference/mini-notation/basics.md` | Typed-prefix overview table; pointer to `literals.md`. |
| `web/static/docs/concepts/signals.md` | New "Patterns and Signals" section — coerce rules + operator type rules + idempotent `scalar()`. |
| `web/static/docs/tutorials/06-pattern-modulation.md` (new) | Tutorial: typed prefixes, Pattern→Signal mental model, `bend(notes, v"…")` flagship, `e.cutoff` custom property. |
| `web/static/docs/tutorials/04-rhythm.md` | Cross-reference 06; migrate any legacy pattern examples to typed prefixes. |
| `docs/mini-notation-reference.md` | Add the four-prefix table; document numeric-atom mode. |
| `docs/Akkado A Rhythmic & DSP Language Specification.md` | Typed prefixes + coerce rules + operator type table. |
| `web/scripts/build-opcodes.ts` (no change) | No new opcodes. |
| `web/scripts/build-docs-index.ts` (no change in code, run as build step) | F1 index regenerated automatically by `bun run build:docs`. |
| `docs/prd-pattern-array-note-extensions-phase-2.md` | Status line: §11.1 marked SHIPPED (was SHIPPED in f8dca4d); §11.2 marked SUBSUMED into this PRD. |

---

## 8. Implementation Phases

### Phase A — Lexer + Mini-Parser numeric mode

**Goal:** `v"<0 0.5 -0.5>"` parses without error and emits a `MiniLiteral` AST node carrying numeric atoms.

**Steps:**
1. Add token types and lexer paths for `v` / `n` / `s` / `c` prefixes.
2. Add `MiniParseMode` enum; thread it through `MiniLexer` / `MiniParser`.
3. Implement `Value`-mode atom rules (numeric literals only).
4. Implement reject paths for mismatched atoms (E162 / E163).
5. Add `MiniAtomData::scalar_value` field.

**Verification:**
- Lexer test: each of `v"…"`, `n"…"`, `s"…"`, `c"…"` produces the expected token with the expected body.
- Mini-parser test per mode: round-trip parse → AST inspection.
- Reject test: `v"c4 e4"` errors with E163 pointing at `c4`.
- Identifier disambiguation test: `let s = 0.5; s + 1` lexes correctly without confusing the `s` identifier with `s"…"`.

**Acceptance:** `bun run check` clean; new tests pass.

### Phase B — `value()`/`note()`/`sample()`/`chord()` builtin codegen + Pattern flags

**Goal:** Each prefix produces a `Pattern`-typed `TypedValue` with appropriate `is_sample_pattern` / `max_voices` set.

**Steps:**
1. Add `value` and `scalar` entries to `BUILTINS`.
2. Implement `handle_value_call` (mirrors `handle_pat_call` with `MiniParseMode::Value`).
3. Audit all 8 `make_pattern` sites in `codegen_patterns.cpp`; populate `is_sample_pattern` and `max_voices` correctly on each.
4. Wire the four prefix tokens to desugar through the corresponding builtin call.

**Verification:**
- `let p = v"<0 0.5 -0.5>"; p.freq` produces a Signal carrying the parsed scalars.
- `let p = s"bd sd"; p.is_sample_pattern == true` (via inspector).
- `let p = c"Am"; p.max_voices == 3`.

**Acceptance:** Compiler accepts and compiles all four prefixes; payloads have correct flags.

### Phase C — Auto-coerce at type-system level

**Goal:** `Pattern → Signal` implicit cast fires at builtin call args, operators, and pipe boundaries; chord/sample patterns error E160.

**Steps:**
1. Add `bool args_are_signal` flag to `BuiltinInfo` (default true); set false on the existing pattern-aware builtins.
2. Implement `implicit_scalar_cast` and `coerce_arg_for_signal` in `codegen.cpp`.
3. Route all builtin call args through `coerce_arg_for_signal` when `args_are_signal == true`.
4. Route binary/unary operators through the same coerce.
5. Route pipe-`@`-injection through the same coerce when downstream first slot is Signal.
6. Implement `handle_scalar_call` for the explicit cast.

**Verification:**
- `osc("sin", v"<220 440>")` compiles and produces audible alternating frequencies.
- `lp(sig, v"<200 800>", 0.7)` compiles; cutoff buffer matches v-pattern values.
- `osc("sin", c"Am")` errors E160 with a helpful message.
- `osc("sin", s"bd sd")` errors E160.
- `scalar(s"bd")` errors E161.
- `scalar(n"c4 e4 g4")` returns a Signal carrying mtof'd freqs.
- `v"<0 0.5>" * 12` produces a Signal carrying 0/6 stepped.

**Acceptance:** Build + tests clean; new error paths produce diagnostics with expected codes.

### Phase D — Pattern-arg `bend` / `aftertouch` / `dur` (subsume Phase 2.1 §11.2)

**Goal:** `n"c4 e4 g4".bend(v"<0 0.5 -0.5>")` works.

**Steps:**
1. Update `handle_bend_call` to accept `Signal` second arg via `coerce_arg_for_signal`. When second arg is a `Number`/constant, behavior is unchanged.
2. Same for `handle_aftertouch_call` and `handle_dur_call`.
3. Wire the per-event sample-and-hold via `SEQPAT_PROP` (already shipped); extend to read from a Signal source when present.

**Verification:**
- Unit test: `n"c4 e4 g4".bend(v"<0 0.5 -0.5>")` produces three events with `event.bend` ∈ {0, 0.5, -0.5}.
- Integration test: full pipeline compiles and runs in `nkido-cli render` for ≥ 30s without crashes.
- Long-window experiment (`experiments/test_op_seqpat_prop.py`): 300+ s of simulated audio confirming bend depth tracks v-pattern across many cycles.

**Acceptance:** Pattern-arg `bend()` / `aftertouch()` / `dur()` audibly modulate; constant-arg behavior unchanged.

### Phase E — Custom-property accessor coerce coverage (subsume Phase 2.1 §11.1)

**Goal:** `e.cutoff` (Signal from `SEQPAT_PROP`) feeds Signal slots through the same coerce-aware path; tests and docs land.

**Steps:**
1. Confirm `pattern_field()` returns `TypedValue::signal(custom_fields[name])` for custom keys (already shipped per typed_value.cpp:56).
2. Add integration tests covering `n"c4{cutoff:0.3}" as e |> lp(@, e.cutoff * 4000)`.
3. Extend the docs index entry for pipe-binding to call out custom-key access.

**Verification:**
- Integration test passes; renders produce expected per-event filter cutoff modulation.
- F1 help: searching for `cutoff` (in pattern context) surfaces the custom-property docs.

**Acceptance:** Phase 2.1 §11.1 closed; documentation reflects the shipped behavior.

### Phase F — Cross-phase smoke acceptance

**Goal:** All capabilities compose in one program without regression.

**Smoke program:**
```akkado
// Pattern-driven oscillator, filter, and bend depth, plus per-event cutoff
n"c4{cutoff:0.3} e4{cutoff:0.7} g4{cutoff:0.5}" as e
  |> osc("saw", e.freq + v"<0 -10 10>")          // detune via scalar pattern
  |> lp(@, 200 + e.cutoff * 4000, v"<0.3 0.7>")  // cutoff custom-property + Q v-pattern
  |> @ * v"<0.5 1.0 0.7>"                        // amplitude pattern
  |> out(@, @)

// Pattern-driven bend on a separate voice
n"c4 e4 g4 b4".bend(v"<0 0.25 -0.25 0>") as e
  |> osc("sin", mtof(e.freq + e.bend * 12))      // bend is a per-event property
  |> out(@, @)

// Negative coerce path: chord pattern in scalar slot must error
// osc("sin", c"Am C G")  // E160 expected
```

**Acceptance:** First two stanzas compile clean and render audibly correct for ≥ 30 s in `nkido-cli render`. Commented-out third line, when uncommented in a separate test program, produces E160 with the documented message.

This phase blocks PRD closure — no Phase A–E claim is "done" until they all coexist in this program.

### Phase G — Documentation & Tutorial coverage

**Goal:** All user-facing documentation reflects the new behavior. F1 help index is regenerated. The new tutorial chapter teaches the flagship use case end-to-end.

**Steps:**
1. Write `web/static/docs/reference/pattern/literals.md` (new file + parent directory).
2. Write `web/static/docs/tutorials/06-pattern-modulation.md` (new file) with the six required sections from §10.6.
3. Update `web/static/docs/reference/mini-notation/basics.md` with the typed-prefix table and a pointer to `literals.md`.
4. Update `web/static/docs/concepts/signals.md` with the new "Patterns and Signals" section.
5. Update `web/static/docs/tutorials/04-rhythm.md` with a cross-reference; migrate any legacy pattern examples.
6. Update `docs/mini-notation-reference.md` and `docs/Akkado A Rhythmic & DSP Language Specification.md`.
7. Run `bun run build:docs` to regenerate the F1 lookup index.

**Verification:** see §10.6.

**Acceptance:** All listed pages exist or are updated; `bun run build:docs` clean; F1 search surfaces the new prefixes, builtins, and error codes; the new tutorial's code blocks parse clean and run without errors. This phase blocks PRD closure alongside Phase F.

---

## 9. Edge Cases

### 9.0 Error Code Reference

| Code | Trigger | Message template | Resolution hint |
|------|---------|------------------|-----------------|
| **E160** | `Pattern → Signal` coerce reject — sample or polyphonic pattern in a Signal-expecting slot. | `cannot use {sample\|polyphonic} pattern as scalar; pick a field (e.g. p.freq) or use {sampler()\|poly()} to consume it` | Pick a field or wrap in `poly()` / `sampler()`. Diagnostic should name the consumer (e.g. `osc()`'s freq slot) and the rejected pattern shape. |
| **E161** | `scalar()` cast on a sample or polyphonic pattern. | `scalar() cannot cast a {sample\|polyphonic} pattern; pick a field explicitly` | Use `.type` for samples, `poly()` for chords. (Note: `scalar(signal)` is idempotent — see §9.4.) |
| **E162** | `Sample`-mode atom is numeric or otherwise invalid. | `atom '{atom}' is not a valid sample name in s"…" mode` | Use the `v"…"` form for raw values, or `n"…"` for note names. |
| **E163** | `Value`-mode atom is non-numeric (note name, sample name, chord symbol, NaN, Inf). | `atom '{atom}' is not a numeric literal in v"…" mode` | Use `n"…"` for note names, `s"…"` for sample names, `c"…"` for chord symbols. |
| **E164** | Record suffix on a `Value`-mode atom. | `record suffix not allowed on value-mode atoms` | Use the host pattern's record suffix instead, e.g. `n"c4{cutoff:0.3}"`. |
| **E165** | Pattern operand combined with Stereo operand via arithmetic. | `cannot combine pattern with stereo signal; wrap with stereo(scalar(...)) explicitly` | Pattern→Stereo coerce is out of scope; user must opt in via `stereo(scalar(p))` then apply the operator. |

### 9.1 Lexer

- **Identifier collision with prefix.** `s` / `n` / `c` / `v` are valid identifier names. The lexer treats `<letter>"` / `<letter>\`` as a string-prefix only when the letter token is exactly one character at the prefix position. Existing `n"…"` rule already covers this. Document explicitly.
- **Long identifiers starting with the prefix letter.** `value`, `note`, `sample`, `chord` are still legal identifiers / function names — lexer does not consume them as prefixes (they're more than one char, so the `current_ == start_ + 1` gate fails).
- **Empty body.** `v""` parses to an empty `value()` call → empty pattern → `freq` buffer carries 0.0 throughout. No error.
- **Mixed-mode mistake.** `v"c4 e4"` errors E163 ("atom 'c4' is not a numeric literal in value-mode pattern"). Error message names the offending atom and points to the `n"…"` form for note semantics.

### 9.2 Mini-notation in `Value` mode

- **Generators are NOT atoms.** Top-level Akkado builtins like `run(8)`, `binary(0b1010)`, and `binaryN(5,8)` are NOT legal inside a mini-notation string — the mini-parser only recognizes literal atoms, brackets, angle, polymeter, euclidean, record suffix, and rest. `v"run(8)"` rejects at parse with E163. To get a numeric ramp pattern, call `run(8)` at the top level. (Adding generator atoms inside mini-notation is tracked under "Future: generator atoms inside mini-notation" — see §11.)
- **Pattern transforms.** `v"<0 0.5 -0.5>".slow(2)` extends the cycle; numeric values unchanged. `v"<0 0.5>".rev()` reverses the order.
- **Chord brackets.** `v"[1,2,3]"` (chord notation in mini-notation) — atoms parse as numerics, but the result is a multi-voice pattern. In a scalar slot, this errors E160 (multi-voice). Document that chord brackets are legal in `v"…"` syntactically but only usable via field access (`p.freq` returns voice-0 buffer, etc.) or by piping through `poly()`.
- **Polymeter.** `v"{0 0.5 1, -0.5 0.5}"` — legal. Output is monophonic at the polymeter cycle.
- **Euclidean.** `v"1(3,8)"` — legal (numeric atom 1 with euclidean rhythm). Euclidean is supported by the mini-parser as `atom(k,n,r)`.
- **Record suffix.** `v"0.5{cutoff:0.3}"` — atoms in `Value` mode do not accept record suffixes (no semantic field assignment). Reject at parse with E164 ("record suffix not allowed on value-mode atoms").
- **NaN / Inf.** `v"NaN"` / `v"Inf"` — reject E163. Numeric literals are finite reals only.
- **Scientific notation.** `v"1e3 -1.25e-2"` — accepted.
- **Negative numbers.** `v"-0.5 -1 0"` — accepted. Mini-lexer numeric path must handle leading `-`.

### 9.3 Auto-coerce

- **Pattern bound to a name and used twice.** `let x = v"<0 0.5>"; osc("sin", x); lp(sig, x, 0.7)` — coerce fires at each consumer site. Both consumers see `Signal::signal(x.freq_buf)`. No re-evaluation; both share the same buffer (idempotent). `x` itself stays typed `ValueType::Pattern`; coerce never mutates the binding.
- **Operator on Pattern + Number.** `let y = v"<0 0.5>" + 12` — RHS is `Number`, result is `Pattern` (per §5.3.1). `y.vel`, `y.trig`, etc. still accessible.
- **Operator on Pattern + Signal.** `let z = v"<0 0.5>" + sig` — RHS is `Signal`, result is `Signal`. Pattern coerces; `z` is `ValueType::Signal`. Field access on `z` errors.
- **Pattern + Stereo Signal.** `v"<0 0.5>" * stereoSig` — **error E165**. User must wrap: `stereo(scalar(v"<0 0.5>")) * stereoSig`.
- **Pattern fed to a pattern transform.** `slow(v"<0 0.5>", 2)` — slow's first arg is `Pattern` (not Signal); no coerce. Result remains a `Pattern`.
- **Chord pattern in pipe-binding.** `c"Am" as e |> osc("sin", e.freq)` does **not** auto-expand. `e.freq` on a multi-voice pattern returns the voice-0 buffer only (mono Signal); the other voices are dropped silently — likely surprising. Users who want all voices must pipe through `poly()` explicitly: `c"Am" |> poly(@ as e |> osc("sin", e.freq))`. Document this in the literals reference page; a future PRD will add `e.freq[i]` voice indexing or implicit poly-expansion in pipe-binding context.
- **Sample pattern in non-Signal slot.** `s"bd sd" |> sampler(@, ...)` — `sampler` is pattern-aware (`args_are_signal = false`). No coerce. Works as today.
- **Empty value pattern.** `v""` produces a pattern with `freq` buffer initialized to 0.0. `osc("sin", v"")` produces a 0 Hz oscillator (silent). No error.

### 9.4 `scalar()` cast

- **Sample pattern.** `scalar(s"bd")` — E161 with message pointing to `.type` field access if the user wants the type_id buffer.
- **Polyphonic chord pattern.** `scalar(c"Am")` — E161 with message pointing to `poly()` or `.freq[i]` voice-indexed access (the latter is future work).
- **`n"…"` auto-detected as note.** `scalar(n"c4 e4")` — works (auto-detect set max_voices=1, is_sample_pattern=false).
- **`n"…"` auto-detected as sample.** `scalar(n"bd sd")` — E161.
- **Already-Signal arg.** `scalar(my_signal)` — returns the Signal unchanged (idempotent no-op). `scalar(scalar(p))` is therefore safe in pipelines that may have already cast. The codegen handler short-circuits when the arg is already a `Signal`.
- **Dot-call form.** `n"c4 e4".scalar()` — desugars to `scalar(n"c4 e4")` per existing dot-call mechanism.

### 9.5 User-defined functions

- **Pattern arg passed to user fn.** Default coerce to Signal applies. `let f = (x) -> x * 2; f(v"<0 0.5>")` — the `x` param sees a `Signal` (the freq buffer); the `* 2` produces a Signal multiplied by 2.
- **User fn that wants Pattern arg.** Out of scope for this PRD; tracked under `prd-compiler-type-system.md`. Workaround: bind the pattern outside the fn and use `.freq` / `.vel` from inside.
- **Pattern arithmetic inside user fn.** `let f = (x, y) -> x + y; f(v"<0 0.5>", v"<0 12>")` — both args coerce to Signal, `+` operates on Signals, result is a Signal.

### 9.6 Phase 2.1 transforms

- **Constant arg still works.** `n"c4 e4".bend(0.5)` — second arg is `Number`/`Signal` from a literal; behavior unchanged from Phase 2.1.
- **Pattern arg now works.** `n"c4 e4".bend(v"<0 0.5>")` — second arg is `Pattern` → coerce → Signal → SEQPAT_PROP per-event sample-and-hold writes to `event.bend`.
- **Pattern arg with mismatched cycle length.** `n"c4 e4 g4 b4".bend(v"<0 0.5>")` — bend's pattern has 2 events per cycle, host has 4. The bend Signal is sampled at each host trigger, so events at host phases 0, 0.25, 0.5, 0.75 read bend at those phases (which is 0/0/0.5/0.5). Document this — no error; longest-cycle-wins is the existing pattern-engine behavior.
- **Pattern arg with sample pattern.** `n"c4 e4".bend(s"bd sd")` — coerce errors E160 (cannot coerce sample pattern to scalar). Diagnostic must clearly say the failure is in `bend`'s second arg.

### 9.7 Compatibility

- **Existing user code.** The legacy `pat` builtin / `n"…"` works exactly as before. No migration required.
- **Existing tests.** All Phase 2 / Phase 2.1 tests continue to pass. Audit confirms no test relies on `Pattern → Signal` failing — currently failing on this transition is rare because most patterns are consumed via `.freq` access or pattern-aware builtins.
- **Documentation.** `docs/mini-notation-reference.md` adds a new "Typed Prefixes" section. `web/static/docs/reference/pattern/` gains a `literals.md` page indexed by `bun run build:docs`.

---

## 10. Testing & Verification

### 10.1 Unit Tests

**Lexer (`akkado/tests/test_lexer.cpp`):**
- Each of `v"…"`, `n"…"`, `s"…"`, `c"…"` produces the expected token and body.
- `value`, `note`, `sample`, `chord` lex as ordinary identifiers (length > 1 disqualifies prefix).
- `s = 0.5\ns"bd"` lexes as identifier `s`, `=`, number `0.5`, then `SamplePat` token (no confusion).

**Mini-parser (`akkado/tests/test_mini_parser.cpp`):**
- `Value` mode: `<0 0.5 -0.5>` parses to 3 atoms with `scalar_value` ∈ {0, 0.5, -0.5}.
- `Value` mode: `c4` rejects with E163.
- `Value` mode: `1e3 -1.25e-2 0.125` accepted.
- `Value` mode: `NaN` / `Inf` rejected.
- `Value` mode: record suffix `0.5{cutoff:0.3}` rejected with E164.
- `Sample` mode: numeric atom rejects with E162.
- `Value` mode generator-call rejection: `v"run(8)"` rejects at parse with E163 — `run` is a top-level builtin, not a mini-notation atom. Documented workaround in error message: call `run(8)` at the top level outside the string.

**Codegen (`akkado/tests/test_pattern_prefixes.cpp`, `akkado/tests/test_pattern_scalar.cpp`):**
- `osc("sin", v"<220 440>")` compiles; emitted bytecode shows `osc` reading the freq buffer of the value pattern.
- `osc("sin", c"Am")` errors with E160_PatternToScalarPolyphonic.
- `osc("sin", s"bd")` errors with E160_PatternToScalarSample.
- `scalar(n"c4 e4")` returns a Signal-typed `TypedValue` with the freq buffer.
- `scalar(s"bd")` errors with E161.
- `v"<0 0.5>" * 12` compiles; result is a Signal carrying scaled values.
- `let x = v"<0 0.5>"; x.freq` works; `x` still typed Pattern at the binding.
- `let x = v"<0 0.5>"; osc("sin", x); lp(sig, x, 0.7)` — both consumers see the same coerced Signal buffer.

**Phase 2.1 transforms with pattern args (`akkado/tests/test_pattern_event.cpp`):**
- `n"c4 e4 g4".bend(v"<0 0.5 -0.5>")` — events have `bend` ∈ {0, 0.5, -0.5} per their host trigger time.
- `n"c4 e4".aftertouch(v"<0 1>")` — events have `aftertouch` ∈ {0, 1}.
- `n"c4 e4 g4".dur(v"<0.25 0.5 0.75>")` — events have explicit durations matching.
- `n"c4 e4".bend(0.5)` — constant arg behavior unchanged.
- `n"c4 e4".bend(s"bd sd")` errors E160 with diagnostic naming `bend`'s second arg.

### 10.2 Integration Tests

- The Phase F smoke program compiles clean with `cmake --build build`.
- `nkido-cli render` runs the smoke program for 30 s without crashes; output WAV non-trivial.
- `bun run check` and `bun run build` clean in `web/`.

### 10.3 Experiments (Cedar runtime, ≥ 300 s simulated)

Per `docs/dsp-experiment-methodology.md` and the long-window guidance in `CLAUDE.md`:

- `experiments/test_op_seqpat_prop.py` (extend) — verify pattern-driven `bend` / `aftertouch` / `dur` produce correct per-event values across 300+ seconds. No drift, no zombie events, no value drops.
- `experiments/test_op_seqpat_step_value.py` (new) — verify `v"…"`-driven `osc` modulation produces stepped Hz output matching the parsed atoms across 300+ seconds.

### 10.4 Manual Audition

- The user's example: `n"c4 e4 g4" |> bend(@, v"<0 0.5 -0.5>") |> ...` should audibly bend the third event by -0.5 (a downward bend depth applied per voice).
- `osc("sin", v"<220 440 880>")` should cycle three pitches.
- `lp(sig, v"<200 800 2000>", 0.7)` should produce a clearly modulated filter sweep.

### 10.5 Build & Lint

- `cmake --build build` clean.
- `bun run check` clean.
- `bun run build:docs` regenerates the F1 lookup index for the new prefixes / `value` / `scalar` builtins.

### 10.6 Documentation & Tutorials

These are blocking acceptance criteria — the PRD is not closed until each lands and the F1 index is regenerated.

**New pages (must exist, indexed by F1):**

- `web/static/docs/reference/pattern/literals.md` — covers all five prefixes (`v`/`n`/`s`/`c`/`n`), the auto-coerce mental model, and the `scalar()` cast. Includes side-by-side examples that distinguish `n"…"` (mtof'd) from `v"…"` (raw). Frontmatter `keywords` includes `pattern, literal, prefix, value, note, sample, chord, scalar, coerce`.
- `web/static/docs/tutorials/06-pattern-modulation.md` — narrative tutorial. Required sections:
  1. *Patterns are values* — introduce the typed prefixes with simple `osc("sin", n"c4 e4 g4")` examples.
  2. *Numeric patterns with `v"…"`* — `osc("sin", v"<220 440>")`, `lp(sig, v"<200 800>", 0.7)`.
  3. *The flagship: per-event modulation* — `n"c4 e4 g4" |> bend(@, v"<0 0.5 -0.5>") |> ...`.
  4. *Custom-property accessor* — `n"c4{cutoff:0.3} e4{cutoff:0.7}" as e |> lp(@, e.cutoff * 4000 + 200)`.
  5. *Scalar arithmetic* — `v"<60 64 67>" + 12` (still Pattern), `v"<0 0.5>" + sig` (now Signal).
  6. *When coerce fails* — chord/sample patterns, the `poly()` and `sampler()` escape hatches.

**Updated pages (must reflect new behavior):**

- `web/static/docs/reference/mini-notation/basics.md` — add the typed-prefix table from §4.1; link to `literals.md`. Migrate any legacy pattern examples to typed prefixes.
- `web/static/docs/concepts/signals.md` — new "Patterns and Signals" section. Must cover: the coerce rule (Pattern fed to a Signal slot becomes a Signal), the operator type table from §5.3.1, idempotent `scalar()`, and the chord/sample reject path with `poly()` / `sampler()` workaround.
- `web/static/docs/tutorials/04-rhythm.md` — short cross-reference at the end pointing to chapter 06; migrate any legacy pattern examples.
- `docs/mini-notation-reference.md` — add the four-prefix table; document numeric-atom mode and per-mode atom rules.
- `docs/Akkado A Rhythmic & DSP Language Specification.md` — typed prefixes and coerce rules become part of the formal spec.

**Verification:**

- `bun run build:docs` runs clean and regenerates `web/src/lib/docs/lookup-index.ts`. The new prefixes (`v`, `n`, `s`, `c`, `n`), the new builtins (`value`, `scalar`), and the new error codes (E160–E165, W170) all surface in F1 search.
- Manual: pressing F1 with the cursor on `v"…"` opens `literals.md` at the value-prefix anchor.
- Manual: pressing F1 with the cursor on `scalar(` opens the scalar-builtin doc.
- The smoke program from Phase F appears as a runnable code block in the new tutorial.

---

## 11. Open Questions

- **Pipe-binding on `v"…"` patterns: are `.vel` / `.trig` / `.gate` meaningful?**
  Tentative: yes, populated by SEQPAT_STEP / SEQPAT_GATE the same way as for `n"…"`. `.vel` defaults to 1.0 for value-mode atoms unless a record-suffix overrides (currently rejected per §9.2). `.trig` fires at each event boundary. Document; revisit only if user reports confusion.

- **Polyphonic value patterns in non-scalar contexts.** `v"[0.3,0.5,0.7]"` is syntactically a multi-voice value pattern (chord-style brackets). It only becomes useful once we have a multi-voice scalar consumer. Currently no such consumer exists — keep the parse legal but document that the only path to use it is field access (`.freq` returns voice-0 buffer); revisit when poly-scalar consumers appear.

- **`scalar(p)` for `n"…"` vs `n"…".freq` — do we need both?**
  Both work; both produce the same Signal. `scalar()` is more discoverable; `.freq` is more flexible (also handles `.vel`, `.gate`, etc.). Keep both. If the docs feel redundant, the resolution is editorial, not a design change.

- **Future: `m"…"` for midi-int-only patterns.**
  Punted in Round 4 — `n"…"` handles both note names and bare ints. If users hit ambiguities (e.g. they want `60` to literally mean 60 Hz, not MIDI 60), the `v"…"` form gives them that. If the ambiguity becomes a pain point, revisit a dedicated `m"…"` prefix.

- **`n"…"` deprecation timeline.**
  This PRD keeps `n"…"` indefinitely but emits soft warning W170 when the parse is unambiguous. Tutorials and reference docs migrate to typed prefixes throughout. A future PRD may flag `n"…"` for removal once telemetry / community feedback shows the typed prefixes have replaced it.

- **Generator atoms inside mini-notation.**
  Top-level builtins like `run`, `binary`, `binaryN`, `tri`, etc., are NOT recognized as atoms inside mini-notation strings. They produce patterns at the top level only. A follow-up PRD could extend the mini-parser to recognize generator-call atoms, enabling `v"run(8) <0 0.5>"` and similar. Out of scope here — keep `value(run(8))` as the workaround.

- **`Pattern → Stereo Signal` coerce.**
  Out of scope. Patterns remain mono. Users wrap with `stereo(...)` or use `pan()` explicitly.

- **User-defined function pitch-aware param annotations.**
  Out of scope per Round 4. Tracked under `prd-compiler-type-system.md`.
