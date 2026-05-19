> **Status: COMPLETE** — Microtonal parsing (^, v, +, d, \, x), tune() compile-time scope modifier, EDO / JI (5-limit symmetric) / Bohlen-Pierce tuning contexts, and the SoundFont fractional-MIDI fix all shipped. Numeric-argument syntax (§3.3) was descoped due to ambiguity with the octave digit; see Roadmap.

# **Product Requirement Document: Akkado Microtonal Extension**

**Version:** 2.1

**Status:** COMPLETE

**Feature:** Microtonal Notation & Tuning Engine

## **1. Objective**

To extend the Akkado mini-notation syntax to support arbitrary, agnostic microtonal composition. The system must allow users to express precise pitch deviations (using steps, cents, or ratios) independent of the underlying tuning system (12-EDO, 31-EDO, JI, etc.), preserving the fluidity of live coding.

## **2. Core Philosophy**

* **Agnostic Notation:** The syntax defines *relative relationships* (e.g., "up one step"), not absolute frequencies.
* **Compile-Time Resolution:** `tune()` is a compile-time context modifier that resolves microtonal notation to Hz during pattern compilation. The Cedar runtime sees only Hz — no structural changes to `Sequence::Event`, `OutputEvent`, `PolyAllocState`, or any opcode.
* **Backward Compatibility:** Standard Western notation (`c#4`, `Bb5`) functions exactly as before when no custom tuning is applied (default 12-EDO context).
* **Individual Notes Only:** Microtonal operators apply to single pitch atoms. Chord symbols (`C4'`, `Am7'`) remain in 12-TET. Microtonal chord voicings are deferred.

## **3. Syntax Specification**

### **3.1. Extended Pitch Atom**

The Pitch Atom definition is expanded to accept a **stream of modifiers** between the Note Nominal and the Octave.

**Format:** [Note Nominal] + [Modifier Stream] + [Octave]

### **3.2. Microtonal Operators**

Two new primary operators control micro-steps.

| Operator | Name | Function |
| :---- | :---- | :---- |
| ^ | Step Up | Increments the micro\_offset counter by 1. |
| v | Step Down | Decrements the micro\_offset counter by 1. |

### **3.3. Numeric Arguments — DESCOPED**

> **Status: descoped.** The originally-planned `^n` / `vn` shorthand (e.g., `^4` ≡ `^^^^`) cannot be added without breaking the existing octave-digit syntax: in `c^4`, the trailing `4` is the octave, not an argument to `^`. Disambiguating would require either a delimiter (which clutters the notation) or a context-sensitive lookahead that conflicts with `cn4` (note + numeric ambiguity). Stacking the operator (`c^^^^4`) remains supported and conveys the same intent.

## **4. Parser Implementation Requirements**

### **4.1. The "Stacking" Strategy**

The parser must not rely on rigid token matching (e.g., looking for "bb" or "x"). Instead, it must parse the modifier section as a continuous stream of characters, accumulating values into two distinct registers.

**Registers:**

1. **accidental\_std (Standard):** Tracks chromatic semitones (12-TET basis).
2. **micro\_offset (Micro):** Tracks system-specific micro-steps.

**Accumulation Logic Table:**

| Character | Register Affected | Delta | Description |
| :---- | :---- | :---- | :---- |
| \# | accidental\_std | +1 | Standard Sharp |
| b | accidental\_std | -1 | Standard Flat |
| ^ | micro\_offset | +1 | Micro Step Up |
| v | micro\_offset | -1 | Micro Step Down |

### **4.2. Alias Support**

To support community standards (Stein-Zimmermann, HEJI), the parser must recognize specific ASCII aliases. These aliases map directly to the registers above.

| Alias | Register | Delta | Standard Meaning |
| :---- | :---- | :---- | :---- |
| d | micro\_offset | -1 | Inverted Flat / Quarter-tone Flat |
| \\ | micro\_offset | -1 | Alternative Down |
| + | micro\_offset | +1 | Half Sharp / Quarter-tone Sharp |
| x | accidental\_std | +2 | Double Sharp |

**Parsing Edge Case:**

The character `d` is ambiguous (Note D vs. Alias d). The current lexer structure naturally handles this:

* **Rule:** If d appears immediately following a Note Nominal or another Modifier (e.g., `cd4` or `cbd4`), it is parsed as an Alias — it's consumed as part of the preceding note's modifier stream.
* **Rule:** If d starts a new token (e.g., `c d4` separated by whitespace), it is parsed as a new Pitch Atom (note D).

### **4.3. Parsing Examples & Resulting State**

| Input String | accidental\_std | micro\_offset | Rationale |
| :---- | :---- | :---- | :---- |
| c\#4 | +1 | 0 | Standard sharp |
| c^4 | 0 | +1 | Pure micro step |
| c\#^4 | +1 | +1 | Sharp + Micro step (Mixed) |
| cbb4 | -2 | 0 | Stacked flats |
| cbd4 | -1 | -1 | Flat + Inverted Flat (Sesquiflat) |
| c+4 | 0 | +1 | Alias usage |

### **4.4. Implementation Mapping to Codebase**

> **Note:** This section reflects the shipped implementation. Modifier parsing lives in `MiniLexer::lex_pitch()` (a dedicated function for character-by-character pitch lexing), not in `is_accidental()` — the gating function `looks_like_pitch()` and `lex_pitch()` share the same modifier alphabet. The `d` alias requires a forward-lookahead that confirms the token reaches an octave digit, otherwise `bd` / `sd` / `cp` etc. would be reinterpreted as pitches.

1. **`MiniLexer::looks_like_pitch()`** (`akkado/src/mini_lexer.cpp:170`) — Modifier-stream gate. Recognizes `#`, `b`, `x`, `^`, `v`, `+`, `\`, and `d` (the last only if the chain reaches an octave digit).

2. **`MiniLexer::lex_pitch()`** (`akkado/src/mini_lexer.cpp:600`) — Character-by-character pitch parsing. Accumulates `accidental_std` (from `#`/`b`/`x`) into the standard MIDI semitone count, and `micro_offset` (from `^`/`v`/`+`/`d`/`\`) into the microtonal step counter.

3. **`MiniPitchData`** (`akkado/include/akkado/mini_token.hpp:122`) — `int8_t micro_offset` field added.

4. **`MiniAtomData`** (`akkado/include/akkado/ast.hpp:196`) — `int8_t micro_offset` field added.

5. **`PatternEvent`** (`akkado/include/akkado/pattern_event.hpp`) — Carries `micro_offset` from AST through pattern evaluation to codegen.

6. **`codegen_patterns.cpp:425`** — Hz conversion calls `tuning_.resolve_hz(midi_note, micro_offset)`.

7. **`TuningContext`** (`akkado/include/akkado/tuning.hpp`) — Holds `Kind {EDO, JI, BP}`, `divisions`, and `interval_cents`. Resolves `(midi, micro_offset) → Hz`.

## **5. Architecture & Data Flow**

### **5.1. Compile-Time Resolution**

The tuning system resolves all pitch information to Hz during pattern compilation. No microtonal data flows into Cedar runtime structures.

```
Mini-notation: "c#^4"
    → MiniLexer: accidental_std=+1, micro_offset=+1, octave=4
    → AST: MiniAtomData { midi_note: 61, micro_offset: +1 }
    → Codegen + TuningContext: resolve(midi=61, micro=+1, "31edo") → Hz
    → Sequence::Event { values[0] = freq_hz }   ← Hz only, as today
    → Runtime: unchanged
```

### **5.2. The Tuning Context (compile-time)**

`tune()` is a compile-time scope modifier that sets the active tuning context for all patterns within its scope.

```akkado
tune("31edo") {
    n"c^4 e4 gv4" |> poly(%, 8, lead) |> out(%, %)
}
```

**Resolution Logic:**

* **EDO (Equal Division of Octave):**
  * StepSize = `interval_cents / divisions` (octave = 1200 cents)
  * Nominal\_Cents = `(midi_note - 69) * 100`   // includes accidental\_std folded into midi\_note
  * Micro\_Shift = `micro_offset * StepSize`
  * freq = `440 * 2^((Nominal_Cents + Micro_Shift) / 1200)`

* **JI (5-limit symmetric, 12-tone):**
  * Anchored at C4 = 12-EDO C4 (≈ 261.626 Hz).
  * Total chromatic step = `(midi_note - 60) + micro_offset`.
  * `pc = step mod 12`, `octave = floor(step / 12)`.
  * Ratio table (relative to C): `1/1, 16/15, 9/8, 6/5, 5/4, 4/3, 45/32, 3/2, 8/5, 5/3, 9/5, 15/8`.
  * freq = `c4_hz * ratios[pc] * 2^octave`.
  * Effect: `micro_offset` shifts by one entry in the JI ratio array — useful for microtonal scale traversal that respects the JI structure.

* **BP (Bohlen-Pierce, 13edt):**
  * Tritave (3:1) = 1200 · log₂(3) ≈ 1901.955 cents, divided into 13 equal steps (~146.3 cents).
  * Anchor: midi=60 → C4_hz (12-EDO).
  * Total step count = `(midi_note - 60) + micro_offset`.
  * freq = `c4_hz * 2^(steps * step_cents / 1200)`.
  * Note: BP is non-octave; midi_note is reinterpreted as a step count along the BP scale, not a 12-EDO position. A4 (midi=69) is *not* 440 Hz in BP.

### **5.3. Instrument Compatibility**

The compile-time Hz resolution means all instrument types work without modification:

| Instrument | Pitch Input | Microtonal Status |
| :---- | :---- | :---- |
| **Oscillators** (via POLY) | Hz per-sample buffer | Works automatically — Hz-native |
| **Sampler** | Pitch multiplier (continuous) | Works automatically — no quantization |
| **SoundFont** | Hz → MIDI conversion | **Needs fix** (see Section 5.4) |
| **Future** (KS, granular, wavetable) | Hz buffer (convention) | Works automatically |

The POLY system is tuning-agnostic: it receives Hz from pattern events, fills `voice_freq_buf` per voice, and the instrument body reads Hz. Microtonal patterns through POLY work identically to 12-TET patterns.

### **5.4. SoundFont Microtonal Fix**

The SoundFont player (`soundfont.hpp`) converts Hz back to MIDI at runtime for zone lookup:

```cpp
float midi_note = 69.0f + 12.0f * std::log2(freq / 440.0f);
uint8_t note = static_cast<uint8_t>(std::roundf(midi_note));  // ← QUANTIZES to semitone
```

This `roundf()` destroys microtonal information. The pitch calculation then uses the quantized value:

```cpp
float pitch_cents = (note - zone.root_key + zone.transpose) * 100.0f + zone.tune;
```

**Fix:** Preserve the exact fractional MIDI note for pitch calculation. Use rounded MIDI only for zone selection:

```cpp
float midi_exact = 69.0f + 12.0f * std::log2(freq / 440.0f);
uint8_t note = static_cast<uint8_t>(std::roundf(midi_exact));  // zone lookup only
// ... zone selection uses rounded 'note' ...
float pitch_cents = (midi_exact - static_cast<float>(zone.root_key)
                     + static_cast<float>(zone.transpose)) * 100.0f
                    + static_cast<float>(zone.tune);
float pitch_ratio = std::pow(2.0f, pitch_cents / 1200.0f);
voice->speed = pitch_ratio * (zone.sample_rate / ctx.sample_rate);
```

## **6. Standard Library & Configuration**

### **6.1. Built-in Library**

`parse_tuning(name)` accepts the following formats (`akkado/src/tuning.cpp`):

* **`Nedo` / `N-edo` / `N-EDO`** — any positive-integer EDO. Common values include `12edo` (default), `17edo`, `19edo`, `22edo`, `24edo` (quarter-tone), `31edo` (meantone/Huygens), `41edo`, `53edo`.
* **`ji`** — 5-limit symmetric 12-tone just intonation (anchored at C4 = 12-EDO C4).
* **`bp`** — Bohlen-Pierce (13 equal divisions of the tritave 3:1).

### **6.2. External Loading API (Future)**

Users will be able to load .scl (Scala) and .tun files. Deferred to post-MVP.

```akkado
load_tuning("slendro", "assets/slendro.scl")
tune("slendro") {
    n"n0 n1 n2" |> poly(%, 4, inst) |> out(%, %)
}
```

## **7. Development Roadmap**

### Phase 1: Parser Update — ✅ Shipped
- `MiniLexer::lex_pitch()` parses the modifier stream `(^, v, +, d, \, #, b, x)` between note letter and octave.
- `d` alias requires a forward-lookahead that confirms the token reaches an octave digit (PRD §4.2 disambiguation; protects sample names like `bd`/`sd` from accidental pitch interpretation).
- `micro_offset` field added to `MiniPitchData`, `MiniAtomData`, and `PatternEvent`.
- Numeric-argument shorthand (`^4`) descoped — see §3.3.

### Phase 2: Tuning Context & Codegen — ✅ Shipped
- `TuningContext` (`akkado/include/akkado/tuning.hpp`) supports `Kind {EDO, JI, BP}` with arbitrary `divisions` and `interval_cents`.
- `tune("name", pattern)` is a compile-time scope modifier (`handle_tune_call` in `codegen_patterns.cpp:4038`; transform-recursion path at `:2154`).
- Codegen Hz conversion (`codegen_patterns.cpp:425`) calls `tuning_.resolve_hz(midi, micro)`.
- `parse_tuning()` accepts `Nedo` / `N-edo`, `ji`, `bp`.

### Phase 3: SoundFont Microtonal Fix — ✅ Shipped
- `cedar/include/cedar/opcodes/soundfont.hpp` preserves fractional MIDI for pitch calculation (line 127) and filter key tracking (line 168). Rounded MIDI is used only for zone selection (line 78).

### Future
- **Numeric-argument syntax** (descoped per §3.3) — would need a new delimiter to coexist with the octave digit; revisit only if a non-conflicting syntax is proposed.
- Scala/TUN file loading.
- JI ratio tables with configurable prime limits and tonic anchor (currently fixed at C).
- Microtonal chord voicings (reinterpret chord intervals through tuning context).
- Runtime `detune()` parameter for real-time micro-adjustments on top of compiled tuning.
- BP-friendly note nominal scheme (current BP reuses 12-EDO MIDI as a step counter, which is functional but unintuitive for users writing pure BP).
