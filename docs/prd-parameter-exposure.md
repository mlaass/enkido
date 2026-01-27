# PRD: Parameter Exposure System

**Status:** Draft
**Author:** Claude
**Date:** 2026-01-27

## 1. Overview

### 1.1 Problem Statement

Currently, Akkado programs have no standardized way to declare external parameters that can be controlled without recompiling. Users who want to create interactive patches must manually wire up external controls, and there's no mechanism for:

- Auto-generating UI controls from code
- Preserving parameter values during hot-swap
- Exporting parameter metadata for external systems (DAWs, Godot, MIDI controllers)

### 1.2 Proposed Solution

Add language builtins (`param()`, `button()`, `toggle()`, `select()`) that:
1. Compile to existing `ENV_GET` opcode (leverages EnvMap infrastructure)
2. Extract metadata at compile time (name, type, range, default)
3. Enable auto-generation of UI controls
4. Work identically in web UI and Godot extension

### 1.3 Goals

- **Declarative:** Parameters defined in code, not external config
- **Zero overhead:** Uses existing EnvMap lock-free infrastructure
- **Hot-swap safe:** Values preserved across recompilation
- **Portable:** Same metadata format for web, Godot, DAWs

### 1.4 Non-Goals

- MIDI learn / CC mapping (future PRD)
- Parameter automation recording (future PRD)
- Preset/snapshot system (future PRD)

---

## 2. User Experience

### 2.1 Akkado Code Examples

```akkado
// Basic volume control with slider
vol = param("volume", 0.8, 0, 1)
osc("saw", 220) * vol |> out(%, %)

// Filter with frequency range
cutoff = param("cutoff", 2000, 100, 8000)
res = param("resonance", 0.5, 0, 1)
osc("saw", 110) |> lpf(%, cutoff, res) |> out(%, %)

// Momentary trigger for one-shot sounds
kick_hit = button("kick")
sample("kick") * kick_hit |> out(%, %)

// Toggle for mute/unmute
mute = toggle("mute", 0)
master = osc("saw", 220) * (1 - mute)

// Waveform selection
wave = select("waveform", "sine", "saw", "square", "triangle")
// wave returns 0, 1, 2, or 3 based on selection
osc(wave, 440) |> out(%, %)
```

### 2.2 Web UI Behavior

After compilation, the Parameters panel displays:
- **Sliders** for `param()` with name, current value, min/max labels
- **Buttons** for `button()` that activate on mousedown, deactivate on mouseup
- **Toggles** for `toggle()` with on/off state visualization
- **Dropdowns** for `select()` showing option names

When values change:
- Immediate audio response (EnvMap update with configurable slew)
- Visual feedback (slider position, button highlight)
- Value persists across hot-swap if parameter still exists

### 2.3 Godot Behavior

```gdscript
# After compilation, params are exposed to Inspector
@onready var player = $EnkidoPlayer

func _ready():
    player.source = '''
        vol = param("volume", 0.8, 0, 1)
        osc("saw", 220) * vol |> out(%, %)
    '''
    player.compile()
    player.play()

    # Programmatic access
    player.set_param("volume", 0.5)
    var current = player.get_param("volume")

    # Get metadata
    var params = player.get_params()  # Array of dictionaries
    for p in params:
        print(p.name, p.type, p.default, p.min, p.max)
```

---

## 3. Technical Design

### 3.1 Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Akkado Source                               │
│   vol = param("volume", 0.8, 0, 1)                                  │
│   osc("saw", 220) * vol |> out(%, %)                                │
└───────────────────────────────┬─────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────────┐
│                         Compiler                                     │
│  ┌─────────────────┐    ┌─────────────────┐                         │
│  │ Codegen         │    │ Metadata        │                         │
│  │                 │    │ Extraction      │                         │
│  │ PUSH_CONST 0.8  │    │                 │                         │
│  │ ENV_GET hash    │    │ ParamDecl {     │                         │
│  │                 │    │   name: "volume"│                         │
│  └────────┬────────┘    │   hash: 0x...   │                         │
│           │             │   type: Cont.   │                         │
│           │             │   default: 0.8  │                         │
│           │             │   min: 0, max: 1│                         │
│           │             │ }               │                         │
│           │             └────────┬────────┘                         │
└───────────┼──────────────────────┼──────────────────────────────────┘
            │                      │
            ▼                      ▼
┌───────────────────────┐  ┌─────────────────────────────────────────┐
│      Bytecode         │  │           CompileResult                  │
│                       │  │  .bytecode                               │
│ [PUSH_CONST, ...]     │  │  .param_decls[]                          │
│ [ENV_GET, ...]        │  │  .state_inits[]                          │
└───────────┬───────────┘  └────────────────┬────────────────────────┘
            │                               │
            ▼                               ▼
┌───────────────────────┐  ┌─────────────────────────────────────────┐
│      Cedar VM         │  │            UI / Godot                    │
│                       │  │                                          │
│  ┌─────────────────┐  │  │  ┌─────────────────────────────────┐    │
│  │    EnvMap       │◄─┼──┼──│  setParam("volume", 0.5)        │    │
│  │                 │  │  │  └─────────────────────────────────┘    │
│  │  "volume" → 0.5 │  │  │                                          │
│  └────────┬────────┘  │  │  ┌─────────────────────────────────┐    │
│           │           │  │  │  Auto-generated slider           │    │
│           ▼           │  │  │  [=========|----] 0.5            │    │
│  ENV_GET reads 0.5    │  │  └─────────────────────────────────┘    │
│                       │  │                                          │
└───────────────────────┘  └─────────────────────────────────────────┘
```

### 3.2 Data Structures

#### ParamType Enum

```cpp
// akkado/include/akkado/codegen.hpp

enum class ParamType : std::uint8_t {
    Continuous = 0,  // Float value in range [min, max]
    Button = 1,      // Momentary: 1 while pressed, 0 otherwise
    Toggle = 2,      // Boolean: 0 or 1, click to flip
    Select = 3       // Discrete: integer index into options array
};
```

#### ParamDecl Structure

```cpp
// akkado/include/akkado/codegen.hpp

struct ParamDecl {
    std::string name;              // Display name and EnvMap key
    std::uint32_t name_hash;       // FNV-1a hash for ENV_GET lookup
    ParamType type;                // Control type
    float default_value = 0.0f;    // Initial value
    float min_value = 0.0f;        // Minimum (Continuous only)
    float max_value = 1.0f;        // Maximum (Continuous only)
    std::vector<std::string> options;  // Option names (Select only)
    SourceLocation location;       // Source position for UI linking

    // Computed at compile time
    std::uint16_t source_offset = 0;   // Byte offset in source
    std::uint16_t source_length = 0;   // Length in source
};
```

#### CompileResult Extension

```cpp
// akkado/include/akkado/akkado.hpp

struct CompileResult {
    bool success = false;
    std::vector<std::uint8_t> bytecode;
    std::vector<Diagnostic> diagnostics;
    std::vector<StateInitData> state_inits;
    std::vector<std::string> required_samples;
    std::vector<ParamDecl> param_decls;  // NEW: Declared parameters
};
```

### 3.3 Builtin Definitions

```cpp
// akkado/include/akkado/builtins.hpp - Add to BUILTIN_FUNCTIONS

// param(name, default, min?, max?) -> continuous value
{"param", {cedar::Opcode::ENV_GET, 2, 2, false,
           {"name", "default", "min", "max", "", ""},
           {NAN, 0.0f, 0.0f, 1.0f, NAN, NAN},
           "Declare continuous parameter (slider)"}},

// button(name) -> 1 while pressed, 0 otherwise
{"button", {cedar::Opcode::ENV_GET, 1, 0, false,
            {"name", "", "", "", "", ""},
            {NAN, NAN, NAN, NAN, NAN, NAN},
            "Declare momentary button"}},

// toggle(name, default?) -> 0 or 1
{"toggle", {cedar::Opcode::ENV_GET, 1, 1, false,
            {"name", "default", "", "", "", ""},
            {NAN, 0.0f, NAN, NAN, NAN, NAN},
            "Declare boolean toggle"}},

// select(name, opt1, opt2, ...) -> index
{"select", {cedar::Opcode::ENV_GET, 1, 16, false,  // Up to 16 options
            {"name", "", "", "", "", ""},
            {NAN, NAN, NAN, NAN, NAN, NAN},
            "Declare selection dropdown"}}
```

### 3.4 Codegen Implementation

#### Handler Registration

```cpp
// akkado/src/codegen.cpp - In visit() Call case

// Add to special handler map
static const std::unordered_map<std::string_view,
    std::uint16_t (CodeGenerator::*)(NodeIndex, const Node&)> special_handlers = {
    // ... existing handlers ...
    {"param",  &CodeGenerator::handle_param_call},
    {"button", &CodeGenerator::handle_button_call},
    {"toggle", &CodeGenerator::handle_toggle_call},
    {"select", &CodeGenerator::handle_select_call},
};
```

#### param() Handler

```cpp
std::uint16_t CodeGenerator::handle_param_call(NodeIndex node, const Node& n) {
    // 1. Extract name argument (must be string literal)
    NodeIndex name_arg = n.first_child;
    if (name_arg == NULL_NODE) {
        error("E150", "param() requires a name argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& name_node = ast_->arena[unwrap_argument(name_arg)];
    if (name_node.type != NodeType::StringLit) {
        error("E151", "param() name must be a string literal", name_node.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::string name = std::string(name_node.as_string());
    std::uint32_t name_hash = cedar::fnv1a_hash_runtime(name.data(), name.size());

    // 2. Extract default, min, max values
    float default_val = 0.0f;
    float min_val = 0.0f;
    float max_val = 1.0f;

    NodeIndex arg = ast_->arena[name_arg].next_sibling;
    if (arg != NULL_NODE) {
        default_val = evaluate_const_float(unwrap_argument(arg));
        arg = ast_->arena[arg].next_sibling;
    }
    if (arg != NULL_NODE) {
        min_val = evaluate_const_float(unwrap_argument(arg));
        arg = ast_->arena[arg].next_sibling;
    }
    if (arg != NULL_NODE) {
        max_val = evaluate_const_float(unwrap_argument(arg));
    }

    // 3. Validate range
    if (min_val > max_val) {
        warning("W050", "param() min > max, swapping values", n.location);
        std::swap(min_val, max_val);
    }

    // Clamp default to range
    default_val = std::clamp(default_val, min_val, max_val);

    // 4. Record parameter declaration (deduplicate by name)
    auto existing = std::find_if(param_decls_.begin(), param_decls_.end(),
        [&](const ParamDecl& p) { return p.name == name; });

    if (existing == param_decls_.end()) {
        ParamDecl decl;
        decl.name = name;
        decl.name_hash = name_hash;
        decl.type = ParamType::Continuous;
        decl.default_value = default_val;
        decl.min_value = min_val;
        decl.max_value = max_val;
        decl.location = n.location;
        decl.source_offset = static_cast<std::uint16_t>(n.location.offset);
        decl.source_length = static_cast<std::uint16_t>(n.location.length);
        param_decls_.push_back(std::move(decl));
    } else {
        // Verify consistent declaration
        if (existing->min_value != min_val || existing->max_value != max_val) {
            warning("W051", "param() '" + name + "' redeclared with different range",
                    n.location);
        }
    }

    // 5. Emit fallback value (for when parameter not set in EnvMap)
    std::uint16_t fallback_buf = buffers_.allocate();
    cedar::Instruction push_inst{};
    push_inst.opcode = cedar::Opcode::PUSH_CONST;
    push_inst.out_buffer = fallback_buf;
    push_inst.state_id = std::bit_cast<std::uint32_t>(default_val);
    emit(push_inst);

    // 6. Emit ENV_GET instruction
    std::uint16_t out_buf = buffers_.allocate();
    cedar::Instruction env_inst{};
    env_inst.opcode = cedar::Opcode::ENV_GET;
    env_inst.out_buffer = out_buf;
    env_inst.inputs[0] = fallback_buf;  // Fallback if param not in EnvMap
    env_inst.state_id = name_hash;       // Parameter lookup key
    emit(env_inst);

    node_buffers_[node] = out_buf;
    return out_buf;
}
```

#### button() Handler

```cpp
std::uint16_t CodeGenerator::handle_button_call(NodeIndex node, const Node& n) {
    // 1. Extract name
    NodeIndex name_arg = n.first_child;
    if (name_arg == NULL_NODE) {
        error("E152", "button() requires a name argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& name_node = ast_->arena[unwrap_argument(name_arg)];
    if (name_node.type != NodeType::StringLit) {
        error("E153", "button() name must be a string literal", name_node.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::string name = std::string(name_node.as_string());
    std::uint32_t name_hash = cedar::fnv1a_hash_runtime(name.data(), name.size());

    // 2. Record declaration
    auto existing = std::find_if(param_decls_.begin(), param_decls_.end(),
        [&](const ParamDecl& p) { return p.name == name; });

    if (existing == param_decls_.end()) {
        ParamDecl decl;
        decl.name = name;
        decl.name_hash = name_hash;
        decl.type = ParamType::Button;
        decl.default_value = 0.0f;  // Buttons default to not pressed
        decl.min_value = 0.0f;
        decl.max_value = 1.0f;
        decl.location = n.location;
        param_decls_.push_back(std::move(decl));
    }

    // 3. Emit fallback (0.0 = not pressed)
    std::uint16_t fallback_buf = buffers_.allocate();
    cedar::Instruction push_inst{};
    push_inst.opcode = cedar::Opcode::PUSH_CONST;
    push_inst.out_buffer = fallback_buf;
    push_inst.state_id = 0;  // 0.0f as bits
    emit(push_inst);

    // 4. Emit ENV_GET
    std::uint16_t out_buf = buffers_.allocate();
    cedar::Instruction env_inst{};
    env_inst.opcode = cedar::Opcode::ENV_GET;
    env_inst.out_buffer = out_buf;
    env_inst.inputs[0] = fallback_buf;
    env_inst.state_id = name_hash;
    emit(env_inst);

    node_buffers_[node] = out_buf;
    return out_buf;
}
```

#### toggle() Handler

```cpp
std::uint16_t CodeGenerator::handle_toggle_call(NodeIndex node, const Node& n) {
    // Extract name
    NodeIndex name_arg = n.first_child;
    // ... validation same as button ...

    std::string name = std::string(name_node.as_string());
    std::uint32_t name_hash = cedar::fnv1a_hash_runtime(name.data(), name.size());

    // Extract default (optional, defaults to 0)
    float default_val = 0.0f;
    NodeIndex arg = ast_->arena[name_arg].next_sibling;
    if (arg != NULL_NODE) {
        default_val = evaluate_const_float(unwrap_argument(arg)) > 0.5f ? 1.0f : 0.0f;
    }

    // Record declaration
    ParamDecl decl;
    decl.name = name;
    decl.name_hash = name_hash;
    decl.type = ParamType::Toggle;
    decl.default_value = default_val;
    decl.min_value = 0.0f;
    decl.max_value = 1.0f;
    // ... emit ENV_GET same as button ...
}
```

#### select() Handler

```cpp
std::uint16_t CodeGenerator::handle_select_call(NodeIndex node, const Node& n) {
    // Extract name
    NodeIndex name_arg = n.first_child;
    // ... validation ...

    std::string name = std::string(name_node.as_string());
    std::uint32_t name_hash = cedar::fnv1a_hash_runtime(name.data(), name.size());

    // Extract options (remaining string arguments)
    std::vector<std::string> options;
    NodeIndex arg = ast_->arena[name_arg].next_sibling;
    while (arg != NULL_NODE) {
        const Node& opt_node = ast_->arena[unwrap_argument(arg)];
        if (opt_node.type != NodeType::StringLit) {
            error("E155", "select() options must be string literals", opt_node.location);
            return BufferAllocator::BUFFER_UNUSED;
        }
        options.push_back(std::string(opt_node.as_string()));
        arg = ast_->arena[arg].next_sibling;
    }

    if (options.empty()) {
        error("E156", "select() requires at least one option", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Record declaration
    ParamDecl decl;
    decl.name = name;
    decl.name_hash = name_hash;
    decl.type = ParamType::Select;
    decl.default_value = 0.0f;  // First option selected
    decl.min_value = 0.0f;
    decl.max_value = static_cast<float>(options.size() - 1);
    decl.options = std::move(options);
    // ... emit ENV_GET ...
}
```

### 3.5 WASM Interface

```cpp
// web/wasm/enkido_wasm.cpp

// Number of declared parameters in last compile result
WASM_EXPORT uint32_t akkado_get_param_decl_count() {
    return static_cast<uint32_t>(g_compile_result.param_decls.size());
}

// Get parameter type (0=continuous, 1=button, 2=toggle, 3=select)
WASM_EXPORT int akkado_get_param_type(uint32_t index) {
    if (index >= g_compile_result.param_decls.size()) return -1;
    return static_cast<int>(g_compile_result.param_decls[index].type);
}

// Get parameter name (returns pointer to internal string)
WASM_EXPORT const char* akkado_get_param_name(uint32_t index) {
    if (index >= g_compile_result.param_decls.size()) return nullptr;
    return g_compile_result.param_decls[index].name.c_str();
}

// Get FNV-1a hash of parameter name
WASM_EXPORT uint32_t akkado_get_param_hash(uint32_t index) {
    if (index >= g_compile_result.param_decls.size()) return 0;
    return g_compile_result.param_decls[index].name_hash;
}

// Get numeric properties
WASM_EXPORT float akkado_get_param_default(uint32_t index) {
    if (index >= g_compile_result.param_decls.size()) return 0.0f;
    return g_compile_result.param_decls[index].default_value;
}

WASM_EXPORT float akkado_get_param_min(uint32_t index) {
    if (index >= g_compile_result.param_decls.size()) return 0.0f;
    return g_compile_result.param_decls[index].min_value;
}

WASM_EXPORT float akkado_get_param_max(uint32_t index) {
    if (index >= g_compile_result.param_decls.size()) return 1.0f;
    return g_compile_result.param_decls[index].max_value;
}

// Get source location for UI linking
WASM_EXPORT uint32_t akkado_get_param_source_offset(uint32_t index) {
    if (index >= g_compile_result.param_decls.size()) return 0;
    return g_compile_result.param_decls[index].source_offset;
}

WASM_EXPORT uint32_t akkado_get_param_source_length(uint32_t index) {
    if (index >= g_compile_result.param_decls.size()) return 0;
    return g_compile_result.param_decls[index].source_length;
}

// For select type: get option count and names
WASM_EXPORT uint32_t akkado_get_param_option_count(uint32_t index) {
    if (index >= g_compile_result.param_decls.size()) return 0;
    return static_cast<uint32_t>(g_compile_result.param_decls[index].options.size());
}

WASM_EXPORT const char* akkado_get_param_option(uint32_t index, uint32_t opt_index) {
    if (index >= g_compile_result.param_decls.size()) return nullptr;
    const auto& opts = g_compile_result.param_decls[index].options;
    if (opt_index >= opts.size()) return nullptr;
    return opts[opt_index].c_str();
}

// JSON export for Godot/DAW compatibility
static std::string g_params_json;

WASM_EXPORT const char* akkado_get_params_json() {
    std::ostringstream json;
    json << "[";

    for (size_t i = 0; i < g_compile_result.param_decls.size(); ++i) {
        const auto& p = g_compile_result.param_decls[i];
        if (i > 0) json << ",";

        json << "{";
        json << "\"name\":\"" << escape_json(p.name) << "\",";
        json << "\"hash\":" << p.name_hash << ",";
        json << "\"type\":" << static_cast<int>(p.type) << ",";
        json << "\"default\":" << p.default_value << ",";
        json << "\"min\":" << p.min_value << ",";
        json << "\"max\":" << p.max_value;

        if (!p.options.empty()) {
            json << ",\"options\":[";
            for (size_t j = 0; j < p.options.size(); ++j) {
                if (j > 0) json << ",";
                json << "\"" << escape_json(p.options[j]) << "\"";
            }
            json << "]";
        }

        json << "}";
    }

    json << "]";
    g_params_json = json.str();
    return g_params_json.c_str();
}
```

### 3.6 Worklet Integration

```javascript
// web/static/worklet/cedar-processor.js

// In compile() success path, after extracting bytecode:
extractParamDecls() {
    const count = this.module._akkado_get_param_decl_count();
    const params = [];

    for (let i = 0; i < count; i++) {
        const namePtr = this.module._akkado_get_param_name(i);
        const type = this.module._akkado_get_param_type(i);

        const param = {
            name: this.module.UTF8ToString(namePtr),
            hash: this.module._akkado_get_param_hash(i),
            type: ['continuous', 'button', 'toggle', 'select'][type],
            default: this.module._akkado_get_param_default(i),
            min: this.module._akkado_get_param_min(i),
            max: this.module._akkado_get_param_max(i),
            sourceOffset: this.module._akkado_get_param_source_offset(i),
            sourceLength: this.module._akkado_get_param_source_length(i)
        };

        // For select type, extract options
        if (type === 3) {
            const optCount = this.module._akkado_get_param_option_count(i);
            param.options = [];
            for (let j = 0; j < optCount; j++) {
                const optPtr = this.module._akkado_get_param_option(i, j);
                param.options.push(this.module.UTF8ToString(optPtr));
            }
        }

        params.push(param);
    }

    return params;
}

// Store with pending program
compile(source) {
    // ... existing compile logic ...

    if (success) {
        const paramDecls = this.extractParamDecls();
        this.pendingProgram = { bytecode, stateInits, requiredSamples, paramDecls };

        this.port.postMessage({
            type: 'compiled',
            success: true,
            bytecodeSize,
            requiredSamples,
            paramDecls  // Include in response
        });
    }
}
```

### 3.7 Audio Store Updates

```typescript
// web/src/lib/stores/audio.svelte.ts

// Type definitions
interface ParamDecl {
    name: string;
    hash: number;
    type: 'continuous' | 'button' | 'toggle' | 'select';
    default: number;
    min: number;
    max: number;
    options?: string[];
    sourceOffset: number;
    sourceLength: number;
}

// In createAudioEngine()
let currentParams = $state<ParamDecl[]>([]);
let paramValues = $state<Map<string, number>>(new Map());

// Handle compile response
function handleCompileResponse(data: any) {
    if (data.success && data.paramDecls) {
        // Preserve values for params that still exist
        const preservedValues = new Map(paramValues);

        currentParams = data.paramDecls;
        paramValues = new Map();

        for (const p of data.paramDecls) {
            let value = p.default;

            // Try to preserve previous value
            if (preservedValues.has(p.name)) {
                const oldValue = preservedValues.get(p.name)!;
                // Clamp to new range
                value = Math.max(p.min, Math.min(p.max, oldValue));
            }

            paramValues.set(p.name, value);

            // Initialize in EnvMap
            setParam(p.name, value, 0);  // No slew for initialization
        }
    }
}

// Set parameter value with optional slew
function setParamValue(name: string, value: number, slewMs: number = 20) {
    paramValues.set(name, value);
    setParam(name, value, slewMs);
}

// Button helpers
function pressButton(name: string) {
    setParamValue(name, 1, 0);  // Immediate
}

function releaseButton(name: string) {
    setParamValue(name, 0, 0);  // Immediate
}

// Toggle helper
function toggleParam(name: string) {
    const current = paramValues.get(name) ?? 0;
    setParamValue(name, current > 0.5 ? 0 : 1, 0);
}

// Export in return object
return {
    // ... existing exports ...
    get params() { return currentParams; },
    get paramValues() { return paramValues; },
    setParamValue,
    pressButton,
    releaseButton,
    toggleParam
};
```

---

## 4. Web UI Components

### 4.1 Params Store

```typescript
// web/src/lib/stores/params.svelte.ts

import { audioEngine } from './audio.svelte';

function createParamsStore() {
    // Derived from audio engine
    let params = $derived(audioEngine.params);
    let values = $derived(audioEngine.paramValues);

    return {
        get params() { return params; },
        get values() { return values; },

        setValue(name: string, value: number, immediate = false) {
            audioEngine.setParamValue(name, value, immediate ? 0 : 20);
        },

        pressButton(name: string) {
            audioEngine.pressButton(name);
        },

        releaseButton(name: string) {
            audioEngine.releaseButton(name);
        },

        toggle(name: string) {
            audioEngine.toggleParam(name);
        },

        select(name: string, index: number) {
            audioEngine.setParamValue(name, index, 0);
        }
    };
}

export const paramsStore = createParamsStore();
```

### 4.2 ParamSlider Component

```svelte
<!-- web/src/lib/components/Params/ParamSlider.svelte -->
<script lang="ts">
    import { paramsStore } from '$lib/stores/params.svelte';

    interface Props {
        name: string;
        min: number;
        max: number;
        default: number;
    }

    let { name, min, max, default: defaultValue }: Props = $props();

    let value = $derived(paramsStore.values.get(name) ?? defaultValue);

    function handleInput(e: Event) {
        const target = e.target as HTMLInputElement;
        paramsStore.setValue(name, parseFloat(target.value));
    }

    function handleDoubleClick() {
        paramsStore.setValue(name, defaultValue, true);
    }
</script>

<div class="param-slider">
    <label for={name}>{name}</label>
    <input
        type="range"
        id={name}
        min={min}
        max={max}
        step={(max - min) / 1000}
        value={value}
        oninput={handleInput}
        ondblclick={handleDoubleClick}
    />
    <span class="value">{value.toFixed(2)}</span>
</div>

<style>
    .param-slider {
        display: grid;
        grid-template-columns: 1fr 2fr auto;
        align-items: center;
        gap: var(--spacing-sm);
        padding: var(--spacing-xs) 0;
    }

    label {
        font-size: var(--font-size-sm);
        color: var(--text-secondary);
    }

    input[type="range"] {
        width: 100%;
        accent-color: var(--accent);
    }

    .value {
        font-family: var(--font-mono);
        font-size: var(--font-size-xs);
        min-width: 4ch;
        text-align: right;
    }
</style>
```

### 4.3 ParamButton Component

```svelte
<!-- web/src/lib/components/Params/ParamButton.svelte -->
<script lang="ts">
    import { paramsStore } from '$lib/stores/params.svelte';

    interface Props {
        name: string;
    }

    let { name }: Props = $props();

    let pressed = $derived((paramsStore.values.get(name) ?? 0) > 0.5);

    function handleMouseDown() {
        paramsStore.pressButton(name);
    }

    function handleMouseUp() {
        paramsStore.releaseButton(name);
    }
</script>

<button
    class="param-button"
    class:pressed
    onmousedown={handleMouseDown}
    onmouseup={handleMouseUp}
    onmouseleave={handleMouseUp}
>
    {name}
</button>

<style>
    .param-button {
        padding: var(--spacing-sm) var(--spacing-md);
        border: 1px solid var(--border);
        border-radius: var(--radius-sm);
        background: var(--bg-secondary);
        color: var(--text-primary);
        cursor: pointer;
        transition: all 0.1s;
    }

    .param-button:hover {
        background: var(--bg-tertiary);
    }

    .param-button.pressed {
        background: var(--accent);
        color: var(--bg-primary);
    }
</style>
```

### 4.4 ParamToggle Component

```svelte
<!-- web/src/lib/components/Params/ParamToggle.svelte -->
<script lang="ts">
    import { paramsStore } from '$lib/stores/params.svelte';

    interface Props {
        name: string;
        default: number;
    }

    let { name, default: defaultValue }: Props = $props();

    let active = $derived((paramsStore.values.get(name) ?? defaultValue) > 0.5);

    function handleClick() {
        paramsStore.toggle(name);
    }
</script>

<button
    class="param-toggle"
    class:active
    onclick={handleClick}
>
    <span class="indicator"></span>
    {name}
</button>

<style>
    .param-toggle {
        display: flex;
        align-items: center;
        gap: var(--spacing-sm);
        padding: var(--spacing-sm) var(--spacing-md);
        border: 1px solid var(--border);
        border-radius: var(--radius-sm);
        background: var(--bg-secondary);
        color: var(--text-primary);
        cursor: pointer;
    }

    .indicator {
        width: 12px;
        height: 12px;
        border-radius: 50%;
        background: var(--text-tertiary);
        transition: background 0.1s;
    }

    .param-toggle.active .indicator {
        background: var(--accent);
        box-shadow: 0 0 8px var(--accent);
    }
</style>
```

### 4.5 ParamSelect Component

```svelte
<!-- web/src/lib/components/Params/ParamSelect.svelte -->
<script lang="ts">
    import { paramsStore } from '$lib/stores/params.svelte';

    interface Props {
        name: string;
        options: string[];
    }

    let { name, options }: Props = $props();

    let selectedIndex = $derived(Math.round(paramsStore.values.get(name) ?? 0));

    function handleChange(e: Event) {
        const target = e.target as HTMLSelectElement;
        paramsStore.select(name, parseInt(target.value));
    }
</script>

<div class="param-select">
    <label for={name}>{name}</label>
    <select id={name} value={selectedIndex} onchange={handleChange}>
        {#each options as option, i}
            <option value={i}>{option}</option>
        {/each}
    </select>
</div>

<style>
    .param-select {
        display: flex;
        align-items: center;
        gap: var(--spacing-sm);
    }

    label {
        font-size: var(--font-size-sm);
        color: var(--text-secondary);
    }

    select {
        flex: 1;
        padding: var(--spacing-xs) var(--spacing-sm);
        border: 1px solid var(--border);
        border-radius: var(--radius-sm);
        background: var(--bg-secondary);
        color: var(--text-primary);
    }
</style>
```

### 4.6 ParamsPanel Component

```svelte
<!-- web/src/lib/components/Params/ParamsPanel.svelte -->
<script lang="ts">
    import { paramsStore } from '$lib/stores/params.svelte';
    import ParamSlider from './ParamSlider.svelte';
    import ParamButton from './ParamButton.svelte';
    import ParamToggle from './ParamToggle.svelte';
    import ParamSelect from './ParamSelect.svelte';

    let params = $derived(paramsStore.params);
</script>

<div class="params-panel">
    {#if params.length === 0}
        <p class="empty">No parameters declared.</p>
        <p class="hint">Use param(), button(), toggle(), or select() in your code.</p>
    {:else}
        {#each params as p}
            {#if p.type === 'continuous'}
                <ParamSlider name={p.name} min={p.min} max={p.max} default={p.default} />
            {:else if p.type === 'button'}
                <ParamButton name={p.name} />
            {:else if p.type === 'toggle'}
                <ParamToggle name={p.name} default={p.default} />
            {:else if p.type === 'select'}
                <ParamSelect name={p.name} options={p.options ?? []} />
            {/if}
        {/each}
    {/if}
</div>

<style>
    .params-panel {
        padding: var(--spacing-md);
        display: flex;
        flex-direction: column;
        gap: var(--spacing-sm);
    }

    .empty {
        color: var(--text-tertiary);
        font-style: italic;
    }

    .hint {
        font-size: var(--font-size-sm);
        color: var(--text-tertiary);
    }
</style>
```

---

## 5. Testing Strategy

### 5.1 Unit Tests (C++)

```cpp
// akkado/tests/test_codegen.cpp

TEST_CASE("param() generates ENV_GET and records declaration", "[codegen][params]") {
    auto result = akkado::compile(R"(
        vol = param("volume", 0.8, 0, 1)
        osc("saw", 220) * vol |> out(%, %)
    )");

    REQUIRE(result.success);
    REQUIRE(result.param_decls.size() == 1);

    auto& decl = result.param_decls[0];
    CHECK(decl.name == "volume");
    CHECK(decl.type == akkado::ParamType::Continuous);
    CHECK(decl.default_value == Approx(0.8f));
    CHECK(decl.min_value == Approx(0.0f));
    CHECK(decl.max_value == Approx(1.0f));

    // Verify ENV_GET instruction emitted with correct hash
    CHECK(decl.name_hash == cedar::fnv1a_hash("volume"));
}

TEST_CASE("param() clamps default to range", "[codegen][params]") {
    auto result = akkado::compile(R"(
        x = param("x", 2.0, 0, 1)  // Default 2.0 clamped to 1.0
    )");

    REQUIRE(result.success);
    CHECK(result.param_decls[0].default_value == Approx(1.0f));
}

TEST_CASE("Multiple param() calls with same name deduplicate", "[codegen][params]") {
    auto result = akkado::compile(R"(
        a = param("vol", 0.5)
        b = param("vol", 0.5)  // Same name, should deduplicate
    )");

    REQUIRE(result.success);
    CHECK(result.param_decls.size() == 1);
}

TEST_CASE("button() defaults to 0", "[codegen][params]") {
    auto result = akkado::compile(R"(
        kick = button("kick")
    )");

    REQUIRE(result.success);
    REQUIRE(result.param_decls.size() == 1);
    CHECK(result.param_decls[0].type == akkado::ParamType::Button);
    CHECK(result.param_decls[0].default_value == 0.0f);
}

TEST_CASE("toggle() records type and default", "[codegen][params]") {
    auto result = akkado::compile(R"(
        mute = toggle("mute", 1)
    )");

    REQUIRE(result.success);
    REQUIRE(result.param_decls.size() == 1);
    CHECK(result.param_decls[0].type == akkado::ParamType::Toggle);
    CHECK(result.param_decls[0].default_value == 1.0f);
}

TEST_CASE("select() records options", "[codegen][params]") {
    auto result = akkado::compile(R"(
        wave = select("wave", "sine", "saw", "square")
    )");

    REQUIRE(result.success);
    REQUIRE(result.param_decls.size() == 1);

    auto& decl = result.param_decls[0];
    CHECK(decl.type == akkado::ParamType::Select);
    CHECK(decl.options.size() == 3);
    CHECK(decl.options[0] == "sine");
    CHECK(decl.options[1] == "saw");
    CHECK(decl.options[2] == "square");
    CHECK(decl.max_value == Approx(2.0f));  // 0, 1, 2
}

TEST_CASE("param() requires string literal name", "[codegen][params]") {
    auto result = akkado::compile(R"(
        name = "vol"
        x = param(name, 0.5)  // Variable, not literal
    )");

    REQUIRE_FALSE(result.success);
    CHECK(result.diagnostics[0].code == "E151");
}
```

### 5.2 Integration Tests

```typescript
// web/tests/params.test.ts

import { test, expect } from '@playwright/test';

test('param sliders auto-generate after compilation', async ({ page }) => {
    await page.goto('/');

    // Enter code with params
    await page.fill('[data-testid="editor"]', `
        vol = param("volume", 0.8, 0, 1)
        osc("saw", 220) * vol |> out(%, %)
    `);

    // Compile
    await page.click('[data-testid="compile"]');
    await page.waitForSelector('[data-testid="params-panel"]');

    // Verify slider appears
    const slider = page.locator('input[type="range"][id="volume"]');
    await expect(slider).toBeVisible();
    await expect(slider).toHaveAttribute('min', '0');
    await expect(slider).toHaveAttribute('max', '1');
});

test('parameter values preserved across hot-swap', async ({ page }) => {
    // Initial compile
    await page.fill('[data-testid="editor"]', `
        vol = param("volume", 0.5)
        osc("saw", 220) * vol |> out(%, %)
    `);
    await page.click('[data-testid="compile"]');

    // Change value
    await page.fill('input[id="volume"]', '0.3');

    // Recompile (hot-swap)
    await page.fill('[data-testid="editor"]', `
        vol = param("volume", 0.5)
        osc("saw", 440) * vol |> out(%, %)
    `);
    await page.click('[data-testid="compile"]');

    // Value should be preserved
    const slider = page.locator('input[id="volume"]');
    await expect(slider).toHaveValue('0.3');
});
```

---

## 6. Implementation Phases

### Phase 1: Compiler Backend (2-3 days)
1. Add `ParamDecl` struct and `ParamType` enum
2. Add `param_decls` to `CodeGenResult` and `CompileResult`
3. Implement `handle_param_call()` handler
4. Implement `handle_button_call()`, `handle_toggle_call()`, `handle_select_call()`
5. Write unit tests

### Phase 2: WASM Interface (1 day)
6. Add WASM export functions for param metadata
7. Add JSON export for Godot compatibility
8. Test extraction from JavaScript

### Phase 3: Worklet Integration (1 day)
9. Add `extractParamDecls()` method
10. Include params in compile response
11. Test message flow

### Phase 4: Audio Store (1 day)
12. Add `ParamDecl` TypeScript type
13. Add reactive param state
14. Implement value preservation on hot-swap
15. Add helper methods

### Phase 5: UI Components (2 days)
16. Create `params.svelte.ts` store
17. Create slider/button/toggle/select components
18. Create `ParamsPanel.svelte`
19. Add to `SidePanel.svelte`
20. Style to match theme system

### Phase 6: Documentation & Polish (1 day)
21. Add `parameters.md` documentation
22. Update builtins JSON for autocomplete
23. Final testing and bug fixes

---

## 7. Open Questions

1. **Scaling:** Should sliders support log/exp scaling for frequency params?
2. **Groups:** Should params support grouping (e.g., `param("filter/cutoff")`)?
3. **MIDI Learn:** How should CC mapping work? (Future PRD)
4. **Presets:** Should there be a preset save/load system? (Future PRD)
5. **Modulation:** Can one param modulate another? (Routing complexity)

---

## 8. Success Metrics

- Compile succeeds with param declarations
- UI generates correct controls from metadata
- Parameter changes affect audio within 20ms
- Values survive hot-swap
- No audio glitches during parameter changes
- Works in both web and Godot environments
