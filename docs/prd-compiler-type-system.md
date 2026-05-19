> **Status: PHASE 1 COMPLETE** — TypedValue struct, ValueType enum, and visit() returning TypedValue are implemented (`akkado/include/akkado/typed_value.hpp`). All ad-hoc maps replaced by `node_types_`. Phase 2 (type annotations on builtins) and Phase 3 (leveraging types for new features) remain.

# PRD: Akkado Compiler Type System

## Problem

The Akkado compiler lacks a type system. `visit()` returns `uint16_t` (a buffer index) with no type information. Structural metadata is tracked in six ad-hoc maps on the `CodeGenerator`:

| Map | Key | Value | Purpose |
|-----|-----|-------|---------|
| `node_buffers_` | NodeIndex | buffer | Visit cache |
| `multi_buffers_` | NodeIndex | vector\<buffer\> | Arrays, chords |
| `stereo_outputs_` | NodeIndex | {left, right} | Stereo pairs |
| `record_fields_` | NodeIndex | {name → buffer} | Record/pattern fields |
| `polyphonic_fields_` | NodeIndex | {freq[], vel[], trig[], gate[], type[]} | Per-voice pattern data |
| `array_lengths_` | buffer | uint8 | Array element count |

This causes three concrete problems:

1. **No type checking on builtin arguments.** `BuiltinInfo` specifies `input_count` and `optional_count` but not parameter types. A user can pass a Pattern where a Signal is expected — the compiler emits garbage silently.

2. **Field access relies on string matching.** `handle_field_access()` (codegen.cpp:1206-1389) checks five cases (record literal, pattern literal, identifier, call, nested) by probing the ad-hoc maps. Adding a new compound type means adding another case and another map.

3. **New features can't inspect argument types.** A `transport()` builtin needs to know its first arg is a Pattern to compile the inner sequence and wire up clock override. Today there is no mechanism for this — the codegen would need yet another ad-hoc map.

## Proposed Types

Built-in, not user-extendable. Eight types:

| Type | Representation | Examples |
|------|---------------|----------|
| **Signal** | Single audio-rate buffer | `osc("sin", 440)`, `@ * 0.5` |
| **Number** | Compile-time constant (float) | `440`, `0.5`, `2 * pi` |
| **Pattern** | Record of {freq, vel, trig, gate, type} + SequenceState ref + cycle_length | `n"c4 e4 g4"`, `seq("x _ x _")` |
| **Record** | Named fields → TypedValue | `{freq: 440, vel: 0.8}` |
| **Array** | Ordered typed values | `[1, 2, 3]`, chord expansion `C4'` |
| **String** | Compile-time string literal | `"sin"`, `"kick.wav"` |
| **Function** | Compile-time reference to closure/fn | `(x) -> x * 2`, named functions |
| **Void** | No value (side effects) | `out(sig, sig)` |

Pattern is a specialization of Record with known field names and associated SequenceState. The compiler may treat Pattern as a Record subtype for field access purposes.

Function is a compile-time reference to a closure or function definition. It is not callable at runtime — the compiler inlines function bodies at call sites. This type exists so that higher-order patterns (passing functions as arguments) can be type-checked.

## Core Change: TypedValue

```cpp
enum class ValueType : uint8_t {
    Signal,
    Number,
    Pattern,
    Record,
    Array,
    String,
    Function,
    Void
};

struct PatternPayload {
    std::array<uint16_t, 5> fields;  // freq, vel, trig, gate, type (mono)
    // For polyphonic: per-voice buffers
    std::vector<std::array<uint16_t, 5>> voice_fields;  // empty if mono
    uint32_t state_id;
    float cycle_length;
    uint8_t num_voices;
};

struct RecordPayload {
    std::unordered_map<uint32_t, TypedValue> fields;  // interned name → TypedValue
};

struct ArrayPayload {
    std::vector<TypedValue> elements;
};

struct TypedValue {
    ValueType type;
    uint16_t buffer;          // Primary buffer (Signal/Number) or BUFFER_UNUSED
    bool error = false;       // Poison flag for error recovery

    // Compound payload (null for Signal/Number/String/Void/Function)
    std::shared_ptr<PatternPayload> pattern;    // Pattern
    std::shared_ptr<RecordPayload> record;      // Record
    std::shared_ptr<ArrayPayload> array;        // Array
    uint32_t string_id = 0;                     // String (interned hash)
    // Function: reuses existing FunctionRef / NodeIndex to closure body
};
```

Only one `shared_ptr` is non-null at a time. This is 40-48 bytes per TypedValue, but they are only created during compilation (not in the audio path). Copies are cheap via shared_ptr refcount.

`visit()` returns `TypedValue` instead of `uint16_t`.

### TypedValue for each type

**Signal:**
```cpp
TypedValue { .type = Signal, .buffer = buf_idx }
```

**Number:**
```cpp
TypedValue { .type = Number, .buffer = const_buf_idx }
// const_buf_idx points to a buffer filled with the constant value
// Alternatively, store the float directly and allocate buffer lazily
```

**Pattern (monophonic):**
```cpp
TypedValue { .type = Pattern, .buffer = freq_buf,
    .pattern = make_shared<PatternPayload>({
        .fields = {freq_buf, vel_buf, trig_buf, gate_buf, type_buf},
        .voice_fields = {},
        .state_id = state_id,
        .cycle_length = 4.0f,
        .num_voices = 1 }) }
```

**Pattern (polyphonic):**
Same as above but `voice_fields` contains N arrays (one per voice), and `num_voices > 1`.

**Record:**
```cpp
TypedValue { .type = Record, .buffer = first_field_buf,
    .record = make_shared<RecordPayload>({
        {hash("freq"), TypedValue{Signal, buf1}},
        {hash("vel"), TypedValue{Signal, buf2}} }) }
```

**Array:**
```cpp
TypedValue { .type = Array, .buffer = elements[0].buffer,
    .array = make_shared<ArrayPayload>({
        TypedValue{Signal, buf1}, TypedValue{Signal, buf2}, TypedValue{Signal, buf3} }) }
```

**String:**
```cpp
TypedValue { .type = String, .buffer = BUFFER_UNUSED, .string_id = interned_id }
```

**Function:**
```cpp
TypedValue { .type = Function, .buffer = BUFFER_UNUSED }
// Function body resolved via existing FunctionRef in the AST
```

**Void:**
```cpp
TypedValue { .type = Void, .buffer = BUFFER_UNUSED }
```

## Impact on Pipes and Holes

Today `@` (hole) is substituted at AST level by `substitute_holes()` in the analyzer (analyzer.cpp:381-646). The analyzer clones the replacement node. The codegen never sees a hole — it sees the substituted expression.

With TypedValue, **no change to hole substitution logic is needed.** The AST rewrite remains the same. The difference is that when codegen visits the substituted expression, it returns a TypedValue instead of a buffer index. Field access (`@.freq`) resolves via TypedValue's type rather than probing maps.

Example flow:
```
n"c4 e4" |> transport(@, trigger(2))
```
1. Analyzer rewrites to: `transport(n"c4 e4", trigger(2))`
2. Codegen visits `n"c4 e4"` → returns `TypedValue{Pattern, ...}`
3. Codegen visits `transport(...)` → sees arg 0 is Pattern → compiles inner sequence, wires clock override

## Impact on Field Access

`handle_field_access()` simplifies from 5 cases to:

```
1. Visit the expression → get TypedValue
2. Switch on type:
   - Pattern: look up field in pattern fields (freq/vel/trig/gate/type)
   - Record:  look up field in record map
   - Other:   type error
```

The current ad-hoc maps (`record_fields_`, `polyphonic_fields_`) are subsumed by the TypedValue payload.

## Impact on Builtins

`BuiltinInfo` gains type annotations:

```cpp
struct BuiltinInfo {
    cedar::Opcode opcode;
    uint8_t input_count;
    uint8_t optional_count;
    bool requires_state;
    std::array<std::string_view, MAX_BUILTIN_PARAMS> param_names;
    std::array<ValueType, MAX_BUILTIN_PARAMS> param_types;    // NEW
    std::array<float, MAX_BUILTIN_DEFAULTS> defaults;
    std::string_view description;
    uint8_t extended_param_count = 0;
};
```

`param_types` array marks each parameter's expected type. `Signal` is the default (backward compatible — any audio-rate value). Special values:

- `Signal` — accepts Signal or Number (auto-promoted to constant buffer). **Pattern is not accepted** — require explicit field access (e.g., `.freq`).
- `Pattern` — requires Pattern typed value
- `String` — requires compile-time string literal
- `Number` — requires compile-time constant

Type checking happens in `visit_call()` after visiting each argument:
```cpp
for (int i = 0; i < num_args; i++) {
    TypedValue arg = visit(arg_node);
    ValueType expected = builtin.param_types[i];
    if (!type_compatible(arg.type, expected)) {
        error("argument '{}' expects {}, got {}", builtin.param_names[i], expected, arg.type);
    }
}
```

### Type compatibility rules

| Expected | Accepts |
|----------|---------|
| Signal | Signal, Number (promoted) |
| Number | Number only |
| Pattern | Pattern only |
| Record | Record, Pattern (subtype) |
| Array | Array only |
| String | String only |
| Function | Function only |

Pattern → Signal coercion is deliberately **not** supported. Passing a Pattern where a Signal is expected is a type error. Users must access a specific field: `n"c4".freq` or `e.freq` via `as` binding. This prevents silent "which field did you mean?" ambiguity.

## Migration Strategy

Three phases. The original Phase 1 (introduce `visit_typed()` wrapper alongside `visit()`) is skipped — go directly to changing `visit()` return type. This is a mechanical refactor: every call site adds `.buffer` or `buffer_of()`.

### Phase 1: Change visit() → TypedValue

- `visit()` return type changes to `TypedValue`
- Each `visit_*` method returns `TypedValue` instead of `uint16_t`
- Add `buffer_of(TypedValue)` helper for callers that just need the buffer index
- `node_buffers_` becomes `node_types_: unordered_map<NodeIndex, TypedValue>`
- Ad-hoc maps (`record_fields_`, `polyphonic_fields_`, `multi_buffers_`, `array_lengths_`) gradually subsumed by TypedValue payloads
- Keep `stereo_outputs_` for now (stereo is orthogonal to type — a Signal can be stereo)
- **All existing tests must pass identically.** This phase changes representation, not behavior.

### Phase 2: Add type annotations to builtins

- Add `param_types` to `BuiltinInfo`
- Implement type checking in `visit_call()`
- Emit clear error messages with source locations

### Phase 3: Leverage types for new features

- `transport()` checks arg 0 is Pattern
- `as` bindings and closure params carry TypedValue (see Type Propagation section)
- Builtin overload resolution based on argument types
- Better error messages everywhere

## Type Propagation through Bindings

### `as` bindings

`n"c4" as e` stores a full TypedValue in the symbol table, not just a buffer index.

- `handle_pipe_binding()` stores `TypedValue` for the bound symbol
- `e.freq` resolves by looking up `e` → `TypedValue{Pattern}` → `pattern->fields[0]`

### Closure parameters

`n"c4" |> ((f) -> osc("sin", f.freq))` — the analyzer rewrites pipe-to-closure by substituting `@` as today. But codegen propagates the pipe source's TypedValue to the parameter symbol.

- `handle_closure()` receives the incoming TypedValue to type the parameter
- `f.freq` resolves via the propagated Pattern type, not via map probing

### Symbol table change

The `Symbol` struct gains a `std::optional<TypedValue> typed_value` field. `SymbolKind` remains for backward compatibility but becomes derivable from `typed_value->type` where present.

## Error Recovery

When a type error is detected:

1. Emit an error diagnostic with source location
2. Return `TypedValue{Signal, BUFFER_UNUSED, .error = true}` — a "poison" value
3. Downstream codegen checks `.error` and skips emission (or emits silence)
4. This prevents cascading errors from a single type mismatch

The poison value is typed as Signal so it can flow through any context that expects a buffer. The `.error` flag ensures it is never treated as valid output.

## Multi-Buffer Expansion

When an Array TypedValue is passed to a builtin parameter expecting Signal, the existing instruction-cloning expansion logic triggers:

1. Each element of the Array is type-checked individually (must be Signal or Number)
2. The expansion count comes from `array->elements.size()`, replacing the `array_lengths_` map
3. One instruction is emitted per array element, each wired to the corresponding element's buffer
4. Only the **first** Array argument triggers expansion (existing limitation, documented not changed)

Example: `[osc("sin", 220), osc("sin", 330), osc("sin", 440)] |> filter_lp(@, 1000)` clones the filter instruction 3 times.

## Relationship to SymbolKind

`SymbolKind` (in the analyzer/codegen) and `ValueType` overlap. Strategy:

- **Keep SymbolKind for now.** It encodes symbol-table-specific concerns: `Builtin`, `Parameter`, `UserFunction`, `Variable`, etc.
- **ValueType is the semantic type of a value.** It describes what a compiled expression produces.
- Some SymbolKinds map directly to ValueTypes: `Pattern`, `Record`, `Array`
- Others are symbol-table-only: `Builtin`, `Parameter`, `UserFunction`
- Long term: `SymbolKind` could be reduced to `{Value, Builtin, UserFunction}` with all type information on `TypedValue`

## Match Expression Types

All arms of a `match` expression must produce the same `ValueType`. If arms disagree, it is a type error. The "winning" arm's `TypedValue` becomes the match result.

```akkado
// OK: all arms produce Signal
match mode {
    "sin" -> osc("sin", 440),
    "saw" -> osc("saw", 440),
}

// Error: arm 1 is Signal, arm 2 is Pattern
match mode {
    "osc" -> osc("sin", 440),
    "pat" -> n"c4 e4",       // type error
}
```

## Key Files

| File | Change |
|------|--------|
| `akkado/include/akkado/codegen.hpp` | TypedValue struct, visit() signature, remove ad-hoc maps |
| `akkado/src/codegen.cpp` | All visit_* return TypedValue, simplified field access |
| `akkado/src/codegen_patterns.cpp` | Pattern codegen returns Pattern TypedValue |
| `akkado/include/akkado/builtins.hpp` | param_types on BuiltinInfo |
| `akkado/src/codegen_builtins.cpp` | Type checking in visit_call() |

## Non-Goals

- **User-defined types.** The type system is fixed to the eight built-in types. Users cannot define new types or interfaces.
- **Type inference across functions.** User-defined functions and the standard library are inlined at call sites. Type propagation through function bodies is handled by visiting the inlined AST — no need for Hindley-Milner or function-level type signatures.
- **Runtime type tags.** Types exist only at compile time. No runtime type checking — the Cedar VM is untyped.
- **Stereo as a type.** Stereo is orthogonal to ValueType. A Signal can be mono or stereo. `stereo_outputs_` remains a separate map for now; it may become a flag on TypedValue later, but TypedValue does not distinguish mono/stereo Signal.
- **Changing the Cedar VM.** This is purely a compiler change. Same opcodes, same instruction format, same bytecode output.
