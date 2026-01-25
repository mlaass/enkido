# PRD: Editor Autocomplete & Code Assistance

## Overview

Add IDE-standard autocomplete and signature help to the NKIDO web editor using CodeMirror 6. All completion data is extracted at runtime from the Akkado compiler (no build-time duplication).

## Problem Statement

Currently, users must memorize function names, parameter orders, and defaults when writing Akkado code. This creates a steep learning curve, especially given the 90+ builtin functions with varying parameter signatures.

## Goals

1. **Reduce friction** for new users learning Akkado syntax
2. **Improve productivity** for experienced users with quick parameter lookup
3. **Maintain single source of truth** - completion data comes from the compiler
4. **Zero latency** - completions appear instantly without network requests

## Non-Goals

- Full Language Server Protocol (LSP) implementation (future work)
- Mini-notation completions inside pattern strings (future work)
- Jump-to-definition, find-references (future work)
- Type checking or type hints (future work)

## Features

### 1. Autocomplete Popup

**Trigger conditions:**
- Typing alphanumeric characters (activate on typing)
- Manual trigger with `Ctrl+Space`

**Displays:**
- Function name
- Parameter signature (e.g., `lp(in, cut, q?)`)
- One-line description

**Completion sources:**
- **Builtin functions** (93+ opcodes) with signatures from compiler
- **Builtin aliases** (e.g., `lowpass` → `lp`, `moogladder` → `moog`)
- **Keywords** (`fn`, `pat`, `seq`, `timeline`, `note`, `true`, `false`, `match`, `post`)
- **User-defined variables/functions** (extracted from current code)

**Selection:**
- Arrow keys to navigate
- `Tab` or `Enter` to accept
- `Escape` to dismiss

### 2. Signature Help Tooltip

**Trigger:**
- Automatically shown when cursor enters function call parentheses
- Updates as user types arguments

**Displays:**
- Full signature: `fn_name(param1, param2?, param3 = default)`
- Current parameter highlighted (bold or underlined)
- Function description
- For user functions: extracted `/// docstring` comment

**Behavior:**
- Works for both builtins and user-defined functions
- Persists while inside parentheses
- Dismisses when cursor exits function call

### 3. Docstrings

**Builtins:**
- One-line descriptions stored alongside definitions in `builtins.hpp`
- Examples: "State-variable lowpass filter", "MIDI note to frequency"

**User functions:**
- Extracted from `/// comment` immediately preceding `fn` declaration
- Example:
  ```akkado
  /// Detuned saw oscillator with stereo spread
  fn fatsaw(freq, detune) = ...
  ```

## Architecture

```
builtins.hpp (C++ with descriptions)
        │
        ▼
enkido_wasm.cpp ──new export──▶ akkado_get_builtins_json()
        │
        ▼
AudioWorklet ──postMessage──▶ Main Thread
        │
        ▼
akkado-completions.ts (CodeMirror CompletionSource)
```

**Key principle:** No duplication. All completion metadata comes from the compiler at runtime.

## Technical Design

### C++ Changes

**builtins.hpp modifications:**
```cpp
struct BuiltinInfo {
    cedar::Opcode opcode;
    std::uint8_t input_count;
    std::uint8_t optional_count;
    bool requires_state;
    std::array<std::string_view, 6> param_names;
    std::array<float, 5> defaults;
    std::string_view description;  // NEW
};
```

**WASM export:**
```cpp
WASM_EXPORT const char* akkado_get_builtins_json();
```

Returns JSON:
```json
{
  "lp": {
    "params": [
      {"name": "in", "required": true},
      {"name": "cut", "required": true},
      {"name": "q", "required": false, "default": 0.707}
    ],
    "description": "State-variable lowpass filter",
    "aliases": ["lowpass", "svflp"]
  }
}
```

### TypeScript Changes

**New files:**
- `web/src/lib/editor/akkado-completions.ts` - CodeMirror completion source
- `web/src/lib/editor/signature-help.ts` - Signature tooltip widget

**Editor.svelte integration:**
```typescript
import { akkadoCompletions } from '$lib/editor/akkado-completions';
import { signatureHelp } from '$lib/editor/signature-help';

// In extensions array:
autocompletion({
  override: [akkadoCompletions],
  activateOnTyping: true
}),
signatureHelp(),
```

### User-Defined Symbol Extraction

**Current approach (regex-based):**
```typescript
const varPattern = /^(\w+)\s*=/gm;
const fnPattern = /(?:\/\/\/\s*(.+)\n)?fn\s+(\w+)\s*\(([^)]*)\)/g;
```

**Future enhancement (compiler-based):**
- Add WASM export: `akkado_get_symbols_json(source)`
- Returns AST-derived symbol table with accurate scoping

## User Experience

### Autocomplete Flow

1. User types `lp`
2. Popup appears with matching functions:
   ```
   lp(in, cut, q?) - State-variable lowpass filter
   limiter(in, ceiling?, release?) - Peak limiter
   lfo(rate, duty?) - Low frequency oscillator
   ```
3. User presses Tab to select `lp`
4. `lp()` is inserted with cursor inside parentheses
5. Signature tooltip appears: `lp(in, cut, q?)`

### Signature Help Flow

1. User types `moog(x, `
2. Tooltip shows: `moog(in, cut, res?, max_res?, input_scale?)`
3. `res?` is highlighted as current parameter
4. User continues typing, highlight moves to next parameter
5. Tooltip dismisses when user types `)` or moves cursor out

## Success Metrics

1. **Adoption:** >80% of editor sessions use autocomplete at least once
2. **Error reduction:** 20% fewer "unknown function" errors
3. **Performance:** Completion popup appears in <50ms

## Risks & Mitigations

| Risk | Mitigation |
|------|------------|
| WASM startup delay | Cache builtins JSON on first request |
| Large completion list | Fuzzy matching with scoring, limit to top 20 |
| User function extraction errors | Regex handles common cases; AST parser for v2 |

## Future Roadmap

1. **Phase 2:** Mini-notation completions inside pattern strings
2. **Phase 3:** Parameter value suggestions (oscillator types, vowel names)
3. **Phase 4:** Full LSP with jump-to-definition, find-references
4. **Phase 5:** Inline type hints and error squiggles

## Implementation Checklist

- [ ] Add `description` field to `BuiltinInfo` struct
- [ ] Add descriptions to all 93+ builtin functions
- [ ] Add `akkado_get_builtins_json()` WASM export
- [ ] Handle builtin request in AudioWorklet
- [ ] Create `akkado-completions.ts` completion source
- [ ] Create `signature-help.ts` tooltip widget
- [ ] Wire up extensions in Editor.svelte
- [ ] Test all builtin functions appear
- [ ] Test user-defined function completion
- [ ] Test signature help parameter highlighting
