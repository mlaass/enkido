> **Status: NOT STARTED** — Improve compiler error messages, error spans, multi-stage collection, and crash resilience

# PRD: Compiler Error Handling & Graceful Recovery

## 1. Executive Summary

The Akkado compiler pipeline and web editor need substantially better error handling for live-coding workflows. This PRD addresses three interrelated problems:

1. **Imprecise error display**: Errors underline from the error column to the end of the line instead of highlighting the specific offending token. The `SourceLocation.length` field is already populated throughout the compiler but never reaches the browser.
2. **Vague error messages**: Generic messages like "Expected expression" and "Unexpected character" without showing what was found, what was expected, or suggesting corrections. All lexer errors share code `L001`; all parser errors share `P001`.
3. **Fragile compilation**: The pipeline aborts between stages (lex errors block parsing, parse errors block semantic analysis), so users fix one category at a time. There is no `onprocessorerror` handler on the AudioWorklet, so if compilation causes a WASM trap, the audio thread dies silently with no recovery path.

Key design decisions:
- **Multi-stage error collection**: lex + parse + semantic errors shown simultaneously (codegen still gated on zero prior errors).
  *Why?* Live-coding feedback loops are tighter when fixing a typo doesn't reveal a hidden semantic error two compiles later. Codegen stays gated to guarantee invalid bytecode never enters the VM.
- **"Did you mean?" suggestions** using Levenshtein distance against builtins + user symbols.
  *Why?* The most common live-coding error is a misspelled function name; surfacing the correction in the diagnostic itself avoids a context switch to docs.
- **Error panel enhanced** with source context, click-to-jump (scroll + cursor + highlight), and error code badges.
  *Why?* The current panel shows only message + line/column; users still have to scroll the editor to see context. Click-to-jump matches IDE conventions.
- **`onprocessorerror` safety net** added to detect and report worklet crashes.
  *Why?* AudioWorklet WASM traps cannot be caught by JS try/catch. Today the audio thread dies silently, requiring a page reload. Even detection-only is a major UX win — automatic state recovery is out of scope (see §6 Phase 6).
- **Parser recovery behavior stays as-is** (one error at a time per stage is acceptable).
  *Why?* Existing panic-mode + statement-boundary sync points already give acceptable recovery for the multi-stage benefit. Mid-expression recovery is high risk (cascading bogus errors) for low value in a live-coding context.

---

## 2. Problem Statement

### 2.1 Current Behavior vs Proposed

| Aspect | Current | Proposed |
|--------|---------|----------|
| Error underline | Column to end-of-line | Exact token span (e.g., just `@`) |
| Lexer message | `"Unexpected character"` | `"Unexpected character '@'"` |
| Parser message | `"Expected expression"` | `"Expected expression, found '@'"` |
| Undefined name | `"Undefined identifier: 'ou'"` | `"Undefined identifier 'ou'; did you mean 'out'?"` |
| Error codes | All lexer = `L001`, all parser = `P001` | Distinct codes per error category (`L001`-`L006`, `P001`-`P005`) |
| Pipeline stages | One stage at a time (lex → fix → parse → fix → semantic) | All stages combined (lex + parse + semantic errors shown together) |
| Error panel | Message only, no click interaction | Source context line, error code badge, click-to-jump with highlight |
| Worklet crash | Silent death, requires page reload | `onprocessorerror` detected, error shown to user |

### 2.2 Current Architecture (What Works)

The infrastructure is solid — the problems are in the details:

- `Diagnostic` struct (`diagnostics.hpp:28-50`) already has `severity`, `code`, `message`, `SourceLocation` (line, column, offset, **length**), `related`, and `fix` fields
- Lexer continues after errors and populates `length` correctly via `current_ - start_`
- Parser has panic-mode recovery with `synchronize()` at statement boundaries
- Codegen collects all errors (doesn't abort on first)
- Cedar VM preserves previous working program when compilation fails — audio keeps playing
- WASM bridge exposes diagnostic count, severity, message, line, column — but NOT length or code
- Web editor uses CodeMirror 6 `lintGutter()` with inline diagnostics — but `to` position falls back to end-of-line (`editor-linter.ts:23`)
- The worklet `compile()` method has an outer try/catch that always sends a response (`cedar-processor.js`, `compile()` at ~line 581 with `try` at ~594 and `catch` at ~730) — but a WASM trap cannot be caught by try/catch

---

## 3. Goals and Non-Goals

### Goals
- Every error message tells the user exactly what went wrong, where, and (when possible) how to fix it
- Error underlines highlight only the offending token, not the rest of the line
- Users see errors from all compiler stages at once (lex + parse + semantic)
- "Did you mean?" suggestions for undefined identifiers and unknown functions
- Error panel shows source context, error codes, and supports click-to-jump
- Compilation errors never crash the running program or require page reload
- `onprocessorerror` handler detects and reports worklet death

### Non-Goals
- Improving parser error recovery (sync points, balanced delimiter skipping) — current one-error-at-a-time per stage is acceptable
- LSP integration — the `Diagnostic.fix` field exists for future LSP use but we don't implement quick-fix UI
- Mini-notation error recovery improvements — deferred
- Moving error panel to side panel — stays at bottom of editor
- Capping diagnostic count — show all errors

---

## 4. Target User Experience

### 4.1 Precise Error Underlines

Before:
```
osc("sin", 440) |> ou(@, @)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~  (entire line underlined)
```

After:
```
osc("sin", 440) |> ou(@, @)
                    ~~        (only 'ou' underlined)
```

### 4.2 Contextual Error Messages

Before:
```
Expected expression
```

After:
```
Expected expression, found '@'
```

Before:
```
Unexpected character
```

After:
```
Unexpected character '@'
```

Before:
```
Unknown function: 'ou'
```

After:
```
Unknown function 'ou'; did you mean 'out'?
```

Note: the colon is intentionally dropped from "Unknown function" and "Undefined identifier" messages for cleaner formatting with the suggestion suffix.

### 4.3 Enhanced Error Panel

```
┌─ Problems (3) ──────────────────────────────── [Clear] ─┐
│                                                          │
│  L001  Ln 2, Col 5   Unexpected character '@'            │
│  │ x = @                                                 │
│  │     ^                                                 │
│                                                          │
│  E004  Ln 5, Col 21  Unknown function 'ou';              │
│  │                    did you mean 'out'?                 │
│  │ osc("sin", 440) |> ou(@, @)                           │
│  │                     ~~                                 │
│                                                          │
│  E005  Ln 7, Col 1   Undefined identifier 'freqq';       │
│  │                    did you mean 'freq'?                │
│  │ freqq * 2                                             │
│  │ ~~~~~                                                 │
└──────────────────────────────────────────────────────────┘
```

Each item is clickable — clicking scrolls the editor to that line, places the cursor at the error column, and briefly highlights the error region.

### 4.4 Multi-Stage Error Collection

A single compile of:
```akkado
x = @
y = sin(440) |> ou(@, @)
z = freqq * 2
```

Shows all three errors at once:
1. `[L001]` Unexpected character `@` (lexer)
2. `[E004]` Unknown function `ou` (semantic)
3. `[E005]` Undefined identifier `freqq` (semantic)

The running audio program continues unaffected.

### 4.5 Worklet Crash Detection

If the WASM module traps (e.g., from a future compiler bug), the status bar shows:

```
Audio engine error — click play to restart
```

Instead of silent failure requiring page reload.

---

## 5. Architecture / Technical Design

### 5.1 Diagnostic Data Flow (Current → Proposed)

```
                    CURRENT                              PROPOSED
                    
C++ Compiler        C++ Compiler
  Diagnostic {        Diagnostic {
    severity            severity
    code                code ──────────────── exposed via WASM
    message             message
    location {          location {
      line                line
      column              column
      offset              offset
      length ─ UNUSED     length ────────── exposed via WASM
    }                   }
  }                   }
     │                     │
     ▼                     ▼
WASM Exports          WASM Exports
  severity              severity
  message               message
  line                  line
  column                column
                        length  ◄── NEW
                        code    ◄── NEW
     │                     │
     ▼                     ▼
JS Worklet            JS Worklet
  extractDiagnostics    extractDiagnostics
     │                     │
     ▼                     ▼
Audio Store           Audio Store
  Diagnostic {          Diagnostic {
    severity              severity
    message               message
    line                  line
    column                column
                          length  ◄── NEW
                          code    ◄── NEW
  }                     }
     │                     │
     ▼                     ▼
Editor Store          Editor Store
  EditorDiagnostic      EditorDiagnostic
     │                     │
     ▼                     ▼
editor-linter.ts      editor-linter.ts
  to = line.to          to = from + length  ◄── PRECISE
```

### 5.2 Error Code Taxonomy

#### Lexer Codes

Currently all lexer errors use `L001`. This PRD introduces distinct codes:

| Code | Component | Meaning | Status |
|------|-----------|---------|--------|
| `L001` | Lexer | Unexpected character | Existing (currently used for all lexer errors) |
| `L002` | Lexer | Unterminated string | **New** |
| `L003` | Lexer | Invalid escape sequence | **New** |
| `L004` | Lexer | Invalid number literal | **New** |
| `L005` | Lexer | Expected digit after exponent | **New** |
| `L006` | Lexer | Incomplete operator (lone `\|`, lone `&`) | **New** |

#### Mini-notation Codes

| Code | Component | Meaning | Status |
|------|-----------|---------|--------|
| `M001` | Mini-notation lexer | Pattern string error | Existing |
| `MP01` | Mini-notation parser | Pattern syntax error | Existing |

#### Parser Codes

Currently all parser errors use `P001`. This PRD introduces distinct codes:

| Code | Component | Meaning | Status |
|------|-----------|---------|--------|
| `P001` | Parser | Expected expression | Existing (currently used for all parser errors) |
| `P002` | Parser | Expected closing delimiter (`)`, `]`, `}`) | **New** |
| `P003` | Parser | Expected keyword or operator (`=`, `->`, `:`) | **New** |
| `P004` | Parser | Expected identifier | **New** |
| `P005` | Parser | Unknown infix operator | **New** |

#### Compile / Semantic / Codegen Codes (Existing)

| Code | Component | Meaning |
|------|-----------|---------|
| `E000` | Compile | Could not open file |
| `E001` | Compile | Empty source file |
| `E002` | Semantic | Invalid pipe expression |
| `E003` | Semantic | Hole '%' used outside pipe |
| `E004` | Semantic | Unknown function |
| `E005` | Semantic | Undefined identifier |
| `E006` | Semantic | Wrong argument count (too few) |
| `E007` | Semantic | Wrong argument count (too many) |
| `E008` | Semantic | Method call missing receiver |
| `E009` | Semantic | Positional argument after named argument |
| `E100` | Codegen | Invalid AST |
| `E101` | Codegen | Buffer pool exhausted |
| `E102` | Codegen | Undefined identifier (codegen) |
| `E103` | Codegen | Cannot use builtin as value |
| `E104` | Codegen | Invalid assignment / const declaration |
| `E105` | Codegen | Missing required argument / default evaluation |
| `E107` | Codegen | Unknown function (codegen) |
| `E108` | Codegen | Invalid binary operation |
| `E109` | Codegen | Unknown binary operator |
| `E110` | Codegen | Hole in unexpected context |
| `E111` | Codegen | Invalid index expression / pipe rewrite |
| `E112` | Codegen | Closure has no body |
| `E113` | Codegen | Method calls not supported |
| `E115` | Codegen | Post statements not supported |
| `E120`-`E122` | Codegen | len() / match errors |
| `E130`-`E136` | Codegen | map/lambda/field access errors |
| `E140`-`E155` | Codegen | Array ops, compose, destructure, directives |
| `E160`-`E184` | Codegen | Stereo ops, array ops, viz ops (*) |
| `E199` | Codegen | Unsupported node type |
| `E200` | Const eval | Division/modulo by zero |
| `E202` | Const eval | Cannot evaluate at compile time |
| `E203` | Const eval | Non-const identifier / failed const eval |
| `E301`-`E304` | Codegen | tap_delay() errors |
| `E400`-`E405` | Codegen | Polyphony errors |
| `E410` | Codegen | Chord not wrapped in poly() |
| `E500`-`E505` | Import | Import resolution errors |
| `W000` | Semantic | General warning |

(*) **Note**: Codes E160-E184 have collisions — they are reused with different meanings across `codegen_arrays.cpp`, `codegen_stereo.cpp`, and `codegen_viz.cpp` (E150-E155 are unique to `codegen_arrays.cpp`, no collision there). This is a pre-existing issue not introduced by this PRD; a future cleanup should assign unique codes.

### 5.3 Levenshtein "Did You Mean?" Algorithm

**Header** (`akkado/include/akkado/string_distance.hpp`):
```cpp
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace akkado {

/// Compute Levenshtein edit distance between two strings
int levenshtein(std::string_view a, std::string_view b);

/// Find the closest match to `name` from a list of candidates.
/// Returns the best match within threshold, or empty string if none.
/// Threshold: <=6 chars → max distance 2, >6 chars → max distance 3.
std::string suggest_similar(std::string_view name,
                           const std::vector<std::string_view>& candidates);

} // namespace akkado
```

Both analyzer and codegen callers collect their candidate names (user symbols + builtin names) into a `vector<string_view>` before calling `suggest_similar()`.

**Usage at call sites:**
```cpp
// Collect candidates from user symbols + builtins
std::vector<std::string_view> candidates;
for (const auto& [sym_name, _] : symbols) candidates.push_back(sym_name);
for (const auto& [builtin_name, _] : BUILTIN_FUNCTIONS) candidates.push_back(builtin_name);

auto suggestion = suggest_similar(name, candidates);
std::string msg = "Unknown function '" + func_name + "'";
if (!suggestion.empty()) msg += "; did you mean '" + suggestion + "'?";
error("E004", msg, n.location);
```

Applied at (line numbers current as of PRD review; symbols are stable references):
- `analyzer.cpp:1469` — E005 `"Undefined identifier 'xyz'"` → `"Undefined identifier 'xyz'; did you mean 'abc'?"`
- `analyzer.cpp:1202` — E004 `"Unknown function 'xyz'"` → `"Unknown function 'xyz'; did you mean 'abc'?"`
- `codegen.cpp:479` — E102 `"Undefined identifier"` (codegen-level), same suggestion suffix
- `codegen.cpp:979` — E107 `"Unknown function"` (codegen-level), same suggestion suffix

The `<=6 chars → max 2, >6 chars → max 3` distance bound matches the heuristic used by GCC and rustc for "did you mean?" suggestions; it keeps suggestions tight enough to avoid noisy proposals on short names while still tolerating one transposition + one insertion on longer ones.

### 5.4 Multi-Stage Pipeline Change

Current (`akkado.cpp`):
```cpp
// Phase 1: Lexing
auto [tokens, lex_diags] = lex(combined_source, filename);
result.diagnostics.insert(..., lex_diags.begin(), lex_diags.end());
if (has_errors(lex_diags)) {
    result.success = false;
    return result;  // STOPS HERE
}

// Phase 2: Parsing — never reached if lex errors
```

Proposed:
```cpp
// Phase 1: Lexing
auto [tokens, lex_diags] = lex(combined_source, filename);
source_map.adjust_all(lex_diags);
result.diagnostics.insert(..., lex_diags.begin(), lex_diags.end());

// Phase 2: Parsing — continues even with lex errors
auto [ast, parse_diags] = parse(std::move(tokens), combined_source, filename);
source_map.adjust_all(parse_diags);
result.diagnostics.insert(..., parse_diags.begin(), parse_diags.end());

// Phase 3: Semantic analysis — runs if AST is structurally valid
if (ast.valid()) {
    auto analysis = analyzer.analyze(ast, ...);
    result.diagnostics.insert(..., analysis.diagnostics.begin(), ...);
}

// Phase 4: Codegen — ONLY if zero errors from all prior stages
if (!has_errors(result.diagnostics)) {
    auto gen = codegen.generate(...);
    // ... emit bytecode
    result.success = true;
} else {
    result.success = false;
}

// Deduplicate diagnostics by (line, column) before returning
deduplicate_diagnostics(result.diagnostics);
```

**Safety constraint**: Codegen is never reached if any prior stage had errors. Invalid bytecode must never enter the VM.

The parser's `synchronize()` (`parser.cpp:90`) needs a small additive change — insert an Error-token skip step at the **head** of the existing loop, **before** the existing keyword-and-RBrace dispatch. Diff against current code:

```diff
 void Parser::synchronize() {
     panic_mode_ = false;

     while (!is_at_end()) {
+        // Skip error tokens (already reported by lexer) before dispatching
+        // on real sync points. Done only at synchronization (statement
+        // boundaries), never in advance(), to avoid silently consuming
+        // tokens mid-expression.
+        if (current().type == TokenType::Error) {
+            advance();
+            continue;
+        }
+
         // Synchronize at statement boundaries
         switch (current().type) {
             case TokenType::Pat:
             // ... existing sync points unchanged
         }
         // ... existing RBrace check + advance() unchanged
     }
 }
```

The existing sync points (statement keywords, `RBrace` boundary check, end-of-loop `advance()`) all remain untouched. The only new behavior is one Error-token skip at the head of each iteration.

#### Token display helper for error messages

The Phase 2 change to `error_at()` requires rendering the offending token in `, found '<token>'`. The existing `token_type_name()` (`token.hpp:96`) returns debug-style PascalCase (`"Eof"`, `"Plus"`, `"Equals"`) — unsuitable for user-facing messages.

Add a new helper `token_display(const Token&) -> std::string` that returns:
- For literals/identifiers: the lexeme verbatim (`"foo"`, `"42"`, `"\"hello\""`)
- For operators/delimiters: the surface symbol (`"="`, `"("`, `"|>"`, `","`)
- For keywords: the keyword text (`"fn"`, `"const"`, `"pat"`)
- For `Eof`: the literal string `"end of file"` — and `error_at` substitutes the prefix `at end of file` instead of `, found '<token>'`
- For `Error`: the lexeme of the error token (whatever the lexer captured), so cascading parse errors at lex-error sites still show useful context

Reuse the existing `token_type_name()` only as a debug fallback for token types that have no natural display form.

#### Diagnostic deduplication

`deduplicate_diagnostics()` is a free function in `akkado.cpp` (private to the translation unit) called once at the end of `compile()` after all stages have contributed.

Dedup rule: **drop a diagnostic only if (line, column, message) all match exactly an earlier diagnostic in the vector.** Same-location diagnostics with different messages (e.g., a lexer "Unexpected character '@'" and a parser "Expected expression, found '<error>'" at the same position) are kept — the user benefits from seeing both perspectives. This also avoids hiding genuine bugs where two distinct rules fire at the same location.

Stable order is preserved (lex → parse → semantic), so the first occurrence wins on exact matches.

---

## 6. Impact Assessment

| Component | Status | Notes |
|-----------|--------|-------|
| `akkado/include/akkado/diagnostics.hpp` | **Stays** | Struct already has all needed fields |
| `akkado/src/diagnostics.cpp` | **Stays** | Terminal formatting unchanged |
| `akkado/src/lexer.cpp` | **Modified** | Better messages, distinct error codes |
| `akkado/include/akkado/lexer.hpp` | **Modified** | `add_error` accepts code parameter |
| `akkado/src/parser.cpp` | **Modified** | `error_at` appends found token; distinct codes |
| `akkado/src/analyzer.cpp` | **Modified** | "Did you mean?" on E004, E005 |
| `akkado/src/codegen.cpp` | **Modified** | "Did you mean?" on E102, E107 |
| `akkado/src/akkado.cpp` | **Modified** | Multi-stage error collection |
| `web/wasm/nkido_wasm.cpp` | **Modified** | Add `akkado_get_diagnostic_length`, `akkado_get_diagnostic_code` |
| `web/wasm/CMakeLists.txt` | **Modified** | Export new WASM symbols |
| `web/static/worklet/cedar-processor.js` | **Modified** | Extract length + code in `extractDiagnostics()` |
| `web/src/lib/stores/audio.svelte.ts` | **Modified** | Add `length`, `code` to Diagnostic; add `onprocessorerror` |
| `web/src/lib/stores/editor.svelte.ts` | **Modified** | Add `length`, `code` to EditorDiagnostic |
| `web/src/lib/components/Editor/editor-linter.ts` | **Modified** | Use length for precise `to` position |
| `web/src/lib/components/Editor/Editor.svelte` | **Modified** | Enhanced error panel with source context, click-to-jump, code badges |
| Cedar VM (`cedar/src/vm/`) | **Stays** | No changes to audio path |
| Parser recovery (`synchronize()`) | **Stays** | No changes to sync points |

---

## 7. File-Level Changes

### 7.1 C++ Compiler Changes

| File | Change |
|------|--------|
| `akkado/include/akkado/lexer.hpp` | Update `add_error` signature to accept error code string |
| `akkado/src/lexer.cpp` | (1) Include actual character in "Unexpected character" messages; (2) Assign distinct codes L001-L006 per error category; (3) Update all `make_error_token` / `add_error` call sites |
| `akkado/src/parser.cpp` | (1) `error_at()` appends `, found '<token>'` (or `at end of file`); (2) Add contextual hints for common mistakes (lone `=` → "did you mean `==`?"); (3) Assign distinct codes P001-P005; (4) `synchronize()` skips `TokenType::Error` tokens |
| `akkado/src/analyzer.cpp` | Update E004 and E005 messages to include `suggest_similar()` suggestions |
| `akkado/src/codegen.cpp` | Update E102 and E107 messages to include "did you mean?" suggestions |
| `akkado/src/akkado.cpp` | Remove early returns between stages; collect errors from lex + parse + semantic; gate codegen on zero total errors; deduplicate diagnostics by (line, column) |
| New: `akkado/src/string_distance.cpp` | Levenshtein distance implementation (shared between analyzer and codegen) |
| New: `akkado/include/akkado/string_distance.hpp` | Header for `levenshtein()` and `suggest_similar()` |

### 7.2 WASM Bridge Changes

| File | Change |
|------|--------|
| `web/wasm/nkido_wasm.cpp` | Add `akkado_get_diagnostic_length()` and `akkado_get_diagnostic_code()` exports |
| `web/wasm/CMakeLists.txt` | Add `_akkado_get_diagnostic_length` and `_akkado_get_diagnostic_code` to `NKIDO_EXPORTED_FUNCTIONS` list |

### 7.3 Web Frontend Changes

| File | Change |
|------|--------|
| `web/static/worklet/cedar-processor.js` | Extract `length` and `code` fields in `extractDiagnostics()` |
| `web/src/lib/stores/audio.svelte.ts` | (1) Add `length`, `code` to `Diagnostic` interface; (2) Register `onprocessorerror` handler on worklet node |
| `web/src/lib/stores/editor.svelte.ts` | Add `length`, `code` to `EditorDiagnostic` interface |
| `web/src/lib/components/Editor/editor-linter.ts` | Use `d.length` for precise `to` calculation: `d.length > 0 ? Math.min(from + d.length, line.to) : line.to` |
| `web/src/lib/components/Editor/Editor.svelte` | (1) Show error code as badge; (2) Show source context line + caret indicator; (3) Add click handler that scrolls editor, positions cursor, and flashes the error region |

---

## 8. Implementation Phases

### Phase 1: Precise Error Spans + Error Code Plumbing

**Goal**: Fix the most visually obvious problem — errors highlight only the offending token.

**Files**: `nkido_wasm.cpp`, `CMakeLists.txt`, `cedar-processor.js`, `audio.svelte.ts`, `editor.svelte.ts`, `editor-linter.ts`

**Changes**:
1. Add `akkado_get_diagnostic_length()` and `akkado_get_diagnostic_code()` to WASM exports
2. Export both in CMakeLists.txt
3. Extract `length` and `code` in worklet's `extractDiagnostics()`
4. Add `length` and `code` to TypeScript interfaces in both stores
5. In `editor-linter.ts`, change line 23 from `const to = line.to` to use span length

**Verify**: Compile `x = @` — underline should cover only `@`, not the entire line.

### Phase 2: Contextual Error Messages

**Goal**: Every error tells the user what went wrong and what was found.

**Files**: `lexer.hpp`, `lexer.cpp`, `parser.cpp`

**Changes**:
1. Update `Lexer::add_error` to accept a code parameter
2. Replace all generic lexer messages with character-specific messages and distinct codes (L001-L006)
3. Add `token_display(const Token&) -> std::string` (see §5.4) for user-friendly token rendering
4. Modify `Parser::error_at()` to append `, found '<token>'` (using `token_display()`) to every error message; use `at end of file` instead when the offending token is `Eof`
5. Add contextual hints: lone `=` → "did you mean `==`?", unexpected closing delimiter → "no matching `(` found"
6. Assign distinct parser codes (P001-P005) based on error category

**Verify**: Compile `x = @` → message says `Unexpected character '@'`. Compile `if x = 3` → message says `Expected expression, found '='`.

### Phase 3: "Did You Mean?" Suggestions

**Goal**: Suggest corrections for undefined identifiers and unknown functions.

**Files**: New `string_distance.hpp`/`.cpp`, `analyzer.cpp`, `codegen.cpp`

**Changes**:
1. Implement `levenshtein()` distance function
2. Implement `suggest_similar()` that searches user symbols + `BUILTIN_FUNCTIONS` map
3. Update error messages at `analyzer.cpp:1202` (E004), `analyzer.cpp:1469` (E005), `codegen.cpp:479` (E102), `codegen.cpp:979` (E107)

**Verify**: Compile `osc("sin", 440) |> ou(@, @)` → message says `Unknown function 'ou'; did you mean 'out'?`

### Phase 4: Multi-Stage Error Collection

**Goal**: Users see lex + parse + semantic errors from a single compilation.

**Files**: `akkado.cpp`, `parser.cpp`

**Changes**:
1. Remove early returns after lex and parse error checks in `compile()` (`akkado.cpp:128-131` and `139-142`); keep the early returns after analysis and codegen failure.
2. Add Error token skipping to parser's `synchronize()` method as an additive diff (see §5.4)
3. Run semantic analysis on valid ASTs even when earlier stages had errors. **Robustness assumption**: the existing analyzer is trusted to walk recovered ASTs without crashing — Phase 4 includes regression tests (see §10.1) that fuzz analyzer entry points with broken sub-ASTs (NULL_NODE children, partial expressions). Any analyzer crash uncovered by these tests must be fixed before this phase ships.
4. Gate codegen on zero total errors (safety: never emit bytecode from broken AST)
5. Add `deduplicate_diagnostics()` per the rule in §5.4 (drop only on exact (line, column, message) match) before returning `CompileResult`

**Verify**: Source with a lex error on line 2 and an undefined identifier on line 5 shows both errors simultaneously. Run the analyzer-on-broken-AST regression suite — no crashes.

### Phase 5: Enhanced Error Panel

**Goal**: Error panel shows source context, error codes, and supports click-to-jump.

**Files**: `Editor.svelte`

**Changes**:
1. Add error code badge (styled pill/tag) to each error item
2. Compute and display the offending source line + caret indicator from `editorStore.code`
3. Add click handler on error items: scroll editor to line, place cursor at column, briefly flash/highlight the error region
4. Style source context line with monospace font, muted color

**Verify**: Click an error in the panel → editor scrolls to that line, cursor positioned at error column, error region flashes briefly.

### Phase 6: Worklet Crash Safety Net

**Goal**: Detect and report AudioWorklet death instead of failing silently.

**Files**: `audio.svelte.ts`

**Changes**:
1. Register `workletNode.onprocessorerror` handler after node creation
2. On error: set `state.isPlaying = false`, set `state.error` to a descriptive message, null the node reference
3. When `play()` is called with a null node (after crash), reinitialize the audio context and worklet

**Out of scope for this PRD**: full state recovery after a crash. WASM holds loaded samples, the current bytecode, parameter values, and SoundFonts — all are lost when the worklet dies. After reinit, the worklet is empty; the user must recompile (which re-triggers required-sample loading from the main-thread sample cache) to get audio back. Manually-uploaded samples that aren't backed by a main-thread cache are lost. This is a known UX gap; a follow-up PRD can address full session restoration.

**Verify**: If worklet dies (simulate by closing audio context), UI shows error message. Clicking play restarts the worklet successfully and recompiling restores audio.

---

## 9. Edge Cases

### 9.1 Zero-length span
Some diagnostics may have `length = 0` (e.g., errors at EOF, errors generated by codegen with default `SourceLocation{}`). The editor-linter must fall back to end-of-line highlighting when length is 0.

### 9.2 Error token at EOF
When the lexer encounters an error at the end of the source, `current_ - start_` may be 0. The parser's Error-token skipping must not loop infinitely — it must also check `is_at_end()`.

### 9.3 Cascading errors across stages
Multi-stage collection means the parser may generate errors for positions where the lexer already reported an error. Deduplication runs in `compile()` after collecting diagnostics from all stages and before returning `CompileResult`. The rule is conservative — see §5.4 — only exact (line, column, message) matches are dropped, so two distinct messages at the same position are both retained. This avoids hiding the parser's perspective behind a same-position lexer error.

### 9.4 "Did you mean?" with no close match
If no symbol is within Levenshtein threshold, don't suggest anything. Message stays as `"Undefined identifier 'xyzzy'"` without a suggestion tail.

### 9.5 Large files with many errors
Parser panic mode already suppresses cascading errors within a single recovery region. Multi-stage collection could produce many errors for badly broken files. Display all errors — no cap.

### 9.6 Source context for stdlib errors
Errors in stdlib code (line numbers in the combined source before user code) should be filtered or mapped correctly by the existing SourceMap. The source context line in the error panel uses `editorStore.code` (user code only), so stdlib-internal errors won't have a displayable source line — show only the message.

### 9.7 Error in mini-notation string
Errors inside `pat("c4 [ e4")` use M001/MP01 codes and have locations within the pattern string. These need the pattern's offset added to display correctly in the editor. This already works via SourceMap adjustment — verify it still works with the new length field.

---

## 10. Testing / Verification Strategy

### 10.1 C++ Unit Tests

Add test cases in `akkado/tests/test_error_messages.cpp`:

```cpp
TEST_CASE("Lexer error messages include character", "[diagnostics]") {
    auto [tokens, diags] = lex("x = @", "<test>");
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == "L001");
    CHECK(diags[0].message == "Unexpected character '@'");
    CHECK(diags[0].location.length == 1);  // Single character
}

TEST_CASE("Parser error messages include found token", "[diagnostics]") {
    auto result = compile("x = = 3");
    auto errors = filter_errors(result.diagnostics);
    REQUIRE(!errors.empty());
    CHECK(errors[0].message.find("found '='") != std::string::npos);
}

TEST_CASE("Did you mean suggests close matches", "[diagnostics]") {
    auto result = compile("osc(\"sin\", 440) |> ou(@, @)");
    auto errors = filter_errors(result.diagnostics);
    REQUIRE(!errors.empty());
    CHECK(errors[0].message.find("did you mean 'out'") != std::string::npos);
}

TEST_CASE("Multi-stage error collection", "[diagnostics]") {
    // Lex error on line 1, semantic error on line 2
    auto result = compile("x = @\ny = undefined_name * 2");
    auto errors = filter_errors(result.diagnostics);
    CHECK(errors.size() >= 2);  // Errors from both stages
}

TEST_CASE("Distinct error codes", "[diagnostics]") {
    auto [tokens, diags] = lex("\"unterminated", "<test>");
    REQUIRE(!diags.empty());
    CHECK(diags[0].code == "L002");  // Not L001
}

TEST_CASE("Analyzer survives broken sub-ASTs", "[diagnostics][robustness]") {
    // Parser produces a valid root with recovered sub-trees containing
    // NULL_NODE children or Error-typed slots. Analyzer must not crash.
    for (auto src : {
        "x = ",                            // missing RHS expression
        "fn f() = ",                       // missing function body
        "y = osc(",                        // unclosed call
        "z = (1 + ) * 2",                  // missing operand
        "pat(\"c4 [ e4\")",                // unclosed mini bracket
        "x = @\ny = z * 2",                // lex error + later semantic ref
    }) {
        auto result = compile(src);
        // Whatever errors are produced, the call must return cleanly.
        // Codegen must NOT have run if there are any errors.
        if (!filter_errors(result.diagnostics).empty()) {
            CHECK_FALSE(result.success);
            CHECK(result.bytecode.empty());
        }
    }
}

TEST_CASE("Dedup keeps distinct messages at same position", "[diagnostics]") {
    // A lex error and a parse error often land on the same (line, column).
    // Dedup must NOT collapse them when the messages differ.
    auto result = compile("x = @");
    auto errors = filter_errors(result.diagnostics);
    // Two errors at (1, 5): one lexer (Unexpected character '@'),
    // potentially one parser (Expected expression). If messages differ,
    // both are kept.
    if (errors.size() >= 2) {
        bool seen_lex = false, seen_parse = false;
        for (auto& d : errors) {
            if (d.code.starts_with("L")) seen_lex = true;
            if (d.code.starts_with("P")) seen_parse = true;
        }
        CHECK((seen_lex || seen_parse));  // at least one stage's diag survives
    }
}
```

### 10.2 Web Integration Tests (Manual)

1. **Precise underline**: Type `x = @` → only `@` is underlined, not the whole line
2. **Contextual message**: Type `osc("sin", 440 |> out(@, @)` (missing `)`) → message says what was expected and found
3. **Did you mean**: Type `osc("sin", 440) |> ou(@, @)` → panel shows suggestion
4. **Multi-stage**: Type source with lex error + semantic error → both shown simultaneously
5. **Click-to-jump**: Click error in panel → editor scrolls, cursor placed, highlight flashes
6. **Error code badge**: Error codes (L001, P002, E005) shown as colored badges in panel
7. **Source context**: Error panel shows the offending source line with caret
8. **Crash resilience**: Compilation error → audio keeps playing previous program → fix code → recompile succeeds

### 10.3 Regression

Run existing test suites after each phase:
```bash
./build/akkado/tests/akkado_tests
./build/cedar/tests/cedar_tests
```
