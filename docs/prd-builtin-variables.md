# PRD: Builtin Variables (`bpm`, `sr`)

> **Status: SHIPPED** — The full feature works end-to-end: `BUILTIN_VARIABLES` registry, analyzer E170/E171/E172, codegen Identifier desugar to `ENV_GET`, compile-time `bpm` override path, WASM override exports, worklet override application, audio-store sync, and Transport `$effect` re-bind.
>
> Live tracking is wired through a cleaner path than PRD step 8 described: instead of the worklet calling `cedar_set_param("__bpm")` separately, `VM::set_bpm()` itself writes `__bpm` and `__spb` into the EnvMap (`cedar/src/vm/vm.cpp:1249-1253`), and `VM::set_sample_rate()` writes `__sr` (`vm.cpp:1243-1247`). The worklet's `setBpm` handler calls `_cedar_set_bpm()`, so Transport BPM changes propagate to `ENV_GET` reads with no recompile. Reads like `60 / bpm` and `spb` track the live Transport tempo.
>
> A third builtin variable, **`spb`** (seconds-per-beat, read-only, `= 60 / bpm`), was added during implementation. User-facing documentation lives at `web/static/docs/concepts/builtin-variables.md`.

## Context

`bpm = 120` appears in the default editor code, tutorials, language spec, and examples — but it does nothing. It compiles as a regular variable assignment (stores 120 in a buffer). The actual BPM is controlled externally via the Transport UI → AudioWorklet → WASM → `VM::set_bpm()`. We need to make `bpm = 120` actually set the tempo, and add the general concept of "builtin variables" (readable/writable identifiers backed by VM state).

## Approach: Desugar to getter/setter builtins (no new opcodes)

Each builtin variable `X` maps to a pair of getter/setter builtin functions. The compiler desugars variable syntax into function calls:

- **`X = value`** → desugars to `set_X(value)` — a special call handler (like `param()`) that extracts the constant at compile time and stores it as metadata in `CodeGenResult`. No runtime opcode emitted.
- **`expr using X`** → desugars to `get_X()` — emits `ENV_GET` with a reserved key (e.g., `"__bpm"`). The host writes the current value into the EnvMap, so reads track live changes (including Transport UI adjustments).

No new opcodes. No new SymbolKind. The getter/setter functions reuse existing infrastructure (ENV_GET, special call handlers, compile-time metadata).

**Builtin variables**: `bpm` (read-write), `sr` (read-only), and `spb` (read-only, seconds-per-beat = `60 / bpm`).

## Changes

### 1. Builtin variable registry — new data structure

**`akkado/include/akkado/builtins.hpp`** — Add a builtin variable definition table (alongside existing `BUILTIN_FUNCTIONS`):

```cpp
struct BuiltinVarDef {
    std::string_view getter_name;   // "get_bpm"
    std::string_view setter_name;   // "set_bpm" (empty = read-only)
    std::string_view env_key;       // "__bpm" — reserved EnvMap key for getter
    float default_value;            // 120.0f
    float min_value;                // 1.0f
    float max_value;                // 999.0f
};

inline const std::unordered_map<std::string_view, BuiltinVarDef> BUILTIN_VARIABLES = {
    {"bpm", {"get_bpm", "set_bpm", "__bpm", 120.0f, 1.0f, 999.0f}},
    {"sr",  {"get_sr",  "",         "__sr",  48000.0f, 0.0f, 0.0f}},
    {"spb", {"get_spb", "",         "__spb", 0.5f, 0.0f, 0.0f}},  // seconds per beat = 60.0 / bpm
};
```

### 2. Register getter/setter as regular builtins

**`akkado/src/symbol_table.cpp`** in `register_builtins()`:

After registering `BUILTIN_FUNCTIONS`, register getter/setter functions for each builtin variable. The getter is a zero-arg builtin that maps to a special call handler. The setter is a one-arg builtin that also maps to a special call handler.

No new SymbolKind needed — these are just regular `SymbolKind::Builtin` entries with special call handlers.

### 3. Analyzer — prevent shadowing

**`akkado/src/analyzer.cpp`** in `collect_definitions()` (~line 189):

When the analyzer sees `bpm = 120`, it currently tries to register `bpm` as a `Variable`. Since `bpm` is in the `BUILTIN_VARIABLES` table:
- If the variable is writable: don't register as Variable, skip the rest (the codegen will handle it via the setter)
- If the variable is read-only (`sr`): emit error E170
- `const bpm = 120`: emit error — cannot declare builtin variable as const

### 4. Codegen — desugaring in Identifier and Assignment

**`akkado/src/codegen.cpp`**:

**Assignment case** (~line 510): Before the existing pattern check, check if `var_name` is in `BUILTIN_VARIABLES`:
```cpp
auto bv_it = BUILTIN_VARIABLES.find(var_name);
if (bv_it != BUILTIN_VARIABLES.end()) {
    const auto& bv = bv_it->second;
    if (bv.setter_name.empty()) {
        error("E170", "Cannot assign to read-only builtin '" + var_name + "'", n.location);
        return TypedValue::error_val();
    }
    // Evaluate RHS as compile-time constant
    ConstEvaluator evaluator(*ast_, *symbols_);
    auto const_val = evaluator.evaluate(value_idx);
    if (const_val) {
        if (auto* scalar = std::get_if<double>(&*const_val)) {
            builtin_var_overrides_.push_back({var_name, static_cast<float>(*scalar), n.location});
            return cache_and_return(node, TypedValue::void_val());
        }
        error("E171", "Builtin variable '" + var_name + "' requires a scalar value", n.location);
        return TypedValue::error_val();
    }
    error("E172", "'" + var_name + "' must be a constant (e.g., bpm = 120)", n.location);
    return TypedValue::error_val();
}
```

**Identifier case** (~line 421): Before the existing Variable/Parameter check, check if identifier is in `BUILTIN_VARIABLES`:
```cpp
auto bv_it = BUILTIN_VARIABLES.find(name);
if (bv_it != BUILTIN_VARIABLES.end()) {
    // Desugar to ENV_GET with reserved key
    const auto& bv = bv_it->second;
    std::uint32_t key_hash = cedar::fnv1a_hash_runtime(bv.env_key.data(), bv.env_key.size());
    
    // Emit PUSH_CONST for default value, then ENV_GET
    std::uint16_t default_buf = buffers_.allocate();
    cedar::Instruction push{};
    push.opcode = cedar::Opcode::PUSH_CONST;
    push.out_buffer = default_buf;
    encode_const_value(push, bv.default_value);
    emit(push);
    
    std::uint16_t out = buffers_.allocate();
    cedar::Instruction env{};
    env.opcode = cedar::Opcode::ENV_GET;
    env.out_buffer = out;
    env.inputs[0] = default_buf;
    env.state_id = key_hash;
    emit(env);
    
    return cache_and_return(node, TypedValue::signal(out));
}
```

### 5. CodeGenResult metadata

**`akkado/include/akkado/codegen.hpp`** — Add:
```cpp
struct BuiltinVarOverride {
    std::string name;
    float value;
    SourceLocation location;
};
```

Add to `CodeGenResult`:
```cpp
std::vector<BuiltinVarOverride> builtin_var_overrides;
```

Add `builtin_var_overrides_` to `CodeGenerator` private members. Wire into `generate()` return value.

### 6. CompileResult passthrough

**`akkado/include/akkado/akkado.hpp`** — Add to `CompileResult`:
```cpp
std::vector<BuiltinVarOverride> builtin_var_overrides;
```

**`akkado/src/akkado.cpp`** — Wire through after param_decls.

### 7. WASM bindings

**`web/wasm/nkido_wasm.cpp`** — Add exports:
```cpp
WASM_EXPORT uint32_t akkado_get_builtin_var_override_count();
WASM_EXPORT const char* akkado_get_builtin_var_override_name(uint32_t index);
WASM_EXPORT float akkado_get_builtin_var_override_value(uint32_t index);
```

### 8. Host writes builtin var values into EnvMap

**`web/static/worklet/cedar-processor.js`**:
- When BPM changes (setBpm message), also write to EnvMap via `cedar_set_param("__bpm", value)` — so reads of `bpm` via ENV_GET return the live value.
- Similarly for sample rate: write `__sr` into EnvMap at init time.
- On compile: extract overrides via new WASM exports, apply `bpm` override via `_cedar_set_bpm()`, and also update EnvMap `__bpm` key.
- Post overrides back to main thread in compiled message.

### 9. Web — Audio Store

**`web/src/lib/stores/audio.svelte.ts`**:
- In `handleWorkletMessage` for `'compiled'` case: if BPM override present, update `state.bpm` to sync Transport UI.

### 10. Web — Transport UI

**`web/src/lib/components/Transport/Transport.svelte`**: `bpmInput` is `$state` (not `$derived`), so it won't auto-sync when `audioEngine.bpm` changes from compilation. Add a `$effect` to re-sync:
```ts
$effect(() => { bpmInput = audioEngine.bpm.toString(); });
```

## Edge Cases

- **`bpm = 60 * 2`**: Works — `ConstEvaluator` handles arithmetic on constants → override value 120.0
- **`bpm = param("tempo", 120, 60, 200)`**: Rejected with E172 — not a constant expression
- **`const bpm = 120`**: Error — cannot declare builtin variable as const
- **`sr = 44100`**: Error E170 — read-only
- **`x = 60 / bpm`**: Works — desugars to ENV_GET("__bpm"), returns live runtime value
- **Multiple `bpm =` lines**: Last one wins (overrides applied in order)
- **Hot-swap**: BPM override applied before program load, crossfade uses new BPM
- **Removing `bpm =` from code**: BPM stays at Transport UI value (no override = no change)
- **Transport UI changes BPM after code set it**: Works — both paths write to ctx.bpm and EnvMap

## Key Files
- `akkado/include/akkado/builtins.hpp` — BuiltinVarDef table
- `akkado/src/symbol_table.cpp` — register getter/setter builtins
- `akkado/src/analyzer.cpp` — collect_definitions() skip for builtin vars
- `akkado/include/akkado/codegen.hpp` — BuiltinVarOverride struct, CodeGenResult field
- `akkado/src/codegen.cpp` — Identifier + Assignment desugaring
- `akkado/include/akkado/akkado.hpp` — CompileResult field
- `akkado/src/akkado.cpp` — Wire overrides through
- `web/wasm/nkido_wasm.cpp` — WASM exports
- `web/static/worklet/cedar-processor.js` — EnvMap writes + override extraction
- `web/src/lib/stores/audio.svelte.ts` — Sync Transport UI
- `web/src/lib/components/Transport/Transport.svelte` — $effect for bpmInput sync
- `web/static/docs/concepts/builtin-variables.md` — user-facing documentation

## Verification

All items below are met. C++ coverage lives in `akkado/tests/test_codegen.cpp` (`[builtins]` tag) — including a runtime test that confirms `spb` tracks `VM::set_bpm()`.

1. **C++ tests**:
   - `bpm = 120` → `builtin_var_overrides` contains `{bpm, 120.0}`
   - `bpm = 60 * 2` → override value is 120.0
   - `x = bpm` → emits ENV_GET with `__bpm` key hash
   - `sr = 44100` → error E170
   - `const bpm = 120` → error
   - `bpm = sin(1)` → error E172
2. **Integration**: Compile `bpm = 140`, verify Transport shows 140, patterns play at 140 BPM
3. **Live tracking**: Set `bpm = 120` in code, change to 160 via Transport, verify `60 / bpm` uses 160
