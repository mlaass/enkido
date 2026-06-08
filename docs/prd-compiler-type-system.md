> **Status: MOSTLY SHIPPED** — Phases 1–4 landed; overload resolution spun out
> to its own PRD (`prd-builtin-overload-resolution.md`).
>
> - **Phase 1 — complete.** `TypedValue` struct, `ValueType` enum, and `visit()`
>   returning `TypedValue` are implemented (`akkado/include/akkado/typed_value.hpp`).
>   All ad-hoc maps (`node_buffers_`, `multi_buffers_`, `record_fields_`,
>   `polyphonic_fields_`, `array_lengths_`, `pattern_state_ids_`) are subsumed by
>   `node_types_`. Only `stereo_outputs_` remains, as the PRD intended.
> - **Phase 2 — complete.** `ParamValueType` enum, `type_compatible()`,
>   `param_types` on `BuiltinInfo`, and the type-checking loop in `visit_call()`
>   emitting `E160` diagnostics with source locations all exist and run.
> - **Phase 3 — complete.** `Symbol` carries `TypedValue` (so `as` bindings
>   propagate types), closures propagate types (`codegen_functions.cpp`), and
>   `EventSource` typing wires `midi()` → `poly()`.
> - **Phase 4 (Coverage) — shipped (2026-06-08).** Rather than spelling
>   `{Signal, …}` on all ~186 entries, the `visit_call()` check now treats an
>   unannotated slot on an `args_are_signal` builtin as **coerce-friendly
>   Signal**: Signal/Number/Pattern/Array/Record/String all pass (they have a
>   defensible signal/expansion/coerce path, per the live-coding philosophy);
>   only Function/StateCell (no audio meaning) are rejected. Explicit
>   annotations (out, visualizers, midi, each_voice/each Function slots,
>   transport Pattern) stay strict. Phase-3 type-driven features finished:
>   `transport()` arg-0 Pattern (via its E133 handler + the annotation) and
>   runtime match-arm `ValueType` agreement (E160). Parameter-type annotations
>   also moved to uppercase PascalCase names (`Signal`/`Number`/`Pattern`/
>   `Record`/`Array`/`String`/`Function`/`Stream`) — see
>   `prd-parameter-type-annotations.md`.
>
> **Deferred → own PRD:** builtin overload resolution keyed on argument
> `ValueType` has no mechanism here; it is now specified in
> `prd-builtin-overload-resolution.md` (a declarative, first-match dispatch
> model unifying builtins, operators, and user-fn overloading). See "Phase 4"
> below for the context that motivated splitting it out.
>
> **Coverage-acceptance correction (2026-06-08).** The original Phase-4
> acceptance — "every builtin has a non-`Any` `param_types` entry" — is
> **intentionally not pursued**, because it does not hold up against how
> `param_types` is actually enforced. The generic `param_types`→E160 check runs
> only in the `visit_call` loop (`codegen.cpp:1529-1573`), which every
> specific-typed builtin **bypasses via a custom handler** (`transport`→E133,
> `midi`/visualizers→options-schema, `poly`→E403/E423, `sample`/`smooch`→E161/
> E198, UI controls→E151–E157). On those builtins `param_types` is decorative;
> the generic loop only meaningfully sees ordinary DSP builtins, whose slots
> correctly ride the coerce-friendly `args_are_signal` fallback. So blanket-
> annotating `Signal` would *tighten* ~400 builtins and break Record/String
> coercion (locked in by `test_param_type_annotations.cpp:573/592`). Real
> coverage therefore = coerce-friendly fallback + targeted specific-type
> annotations + per-handler diagnostics. Unifying these two enforcement paths is
> exactly the job of `prd-builtin-overload-resolution.md`.
>
> One concrete gap this audit *did* find and fix: `poly()`/`legato()` silently
> accepted a non-pattern input (`handle_poly_call` left `seq_state_id=0`).
> Now rejected with **E423**, with regression tests in
> `test_param_type_annotations.cpp` (`[poly]`).

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
| **Signal** | Single audio-rate buffer | `sine(440)`, `@ * 0.5` |
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

### Phase 4: Close the coverage gaps (recommended consolidation)

Phases 2 and 3 each shipped their core mechanism but stopped short of full
coverage. Rather than leaving two PRD phases open-ended, fold the remaining work
into one bounded pass:

- **~~Annotate the remaining builtins.~~** *Superseded — see the
  coverage-acceptance correction in the status block.* A blanket `param_types`
  sweep was found to be either inert (specific-typed builtins are custom-handled
  and bypass the generic check) or harmful (annotating pure-signal slots
  `Signal` breaks Record/String coercion). Coverage is achieved via the
  coerce-friendly fallback + targeted annotations + per-handler diagnostics, and
  the two enforcement paths are unified by
  `prd-builtin-overload-resolution.md`.
- **Phase 3 type-driven features — landed:**
  - `transport()` arg-0 Pattern check — **done** (E133, `codegen_patterns.cpp`)
  - match-arm `ValueType` agreement — **done** (E160, `codegen_functions.cpp`)
  - builtin overload resolution based on argument types — **spun out** to
    `prd-builtin-overload-resolution.md`
- **Acceptance (revised):** the type-driven Phase-3 features above are
  implemented and tested; a type-mismatch test exists per `ParamValueType` that
  is enforceable (Signal/Number/Pattern/Record/Array/String/Function via the
  user-fn annotation path's E184 matrix and the builtin E160 path; Stream via
  `poly()` E423); and existing tests still pass. The "annotate every builtin"
  bar is explicitly retired (see status block).

## Type Propagation through Bindings

### `as` bindings

`n"c4" as e` stores a full TypedValue in the symbol table, not just a buffer index.

- `handle_pipe_binding()` stores `TypedValue` for the bound symbol
- `e.freq` resolves by looking up `e` → `TypedValue{Pattern}` → `pattern->fields[0]`

### Closure parameters

`n"c4" |> ((f) -> sine(f.freq))` — the analyzer rewrites pipe-to-closure by substituting `@` as today. But codegen propagates the pipe source's TypedValue to the parameter symbol.

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

Example: `[sine(220), sine(330), sine(440)] |> filter_lp(@, 1000)` clones the filter instruction 3 times.

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
    "sin" -> sine(440),
    "saw" -> saw(440),
}

// Error: arm 1 is Signal, arm 2 is Pattern
match mode {
    "osc" -> sine(440),
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
