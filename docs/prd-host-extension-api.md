> **Status: NOT STARTED** — Design for a static, C++-level host extension API letting embedders register custom variables, builtins, and opcodes at startup (2026-06-08).

# PRD: Host Extension API (Embedding-Time Variables, Builtins & Opcodes)

**Date:** 2026-06-08

---

## 1. Executive Summary

When Akkado/Cedar is embedded as a library inside a host application — special-feature
hardware, a VST/JUCE plugin, or a game engine — the host frequently needs to extend
the *language itself*: expose host-owned signals (game state, transport, sensor/CV
streams) as Akkado variables, add domain builtins, or provide brand-new DSP opcodes
backed by the host's own code (hardware-accelerated effects, proprietary algorithms).

Today none of that is possible without forking and recompiling the core: `BUILTIN_FUNCTIONS`
is an `inline const` table baked into the binary (`akkado/include/akkado/builtins.hpp:274`),
and Cedar's opcode dispatch is a closed compile-time `switch` (`cedar/src/vm/vm.cpp:1358`)
over a fixed `uint8_t` enum, with state stored in a closed `std::variant` (`DSPState`,
`cedar/include/cedar/opcodes/dsp_state.hpp:1530`). The Godot, JUCE, and ESP32 integration
PRDs all consume the engine strictly "as-is, no modifications."

This PRD adds a **static, C++-header-level host extension API**, registered once at host
init and immutable thereafter. It covers the full stack:

- **Host variables** — `akkado::register_host_variable(...)`: named signals the host feeds
  in. Control-rate scalars (via the existing lock-free `EnvMap` channel) **and** audio-rate
  per-block buffers.
- **Host functions** — `akkado::register_host_function(...)`: one unified call. With an
  `impl_fn` it registers a genuine **host opcode**; with a null `impl_fn` it maps the name
  onto an existing core `Opcode` (aliases, presets, renames).
- **Host opcodes** — a single new `HOST_OP` instruction dispatched through a host-owned
  function-pointer table indexed by `inst.rate`. Core opcodes keep their jump-table; only
  host ops pay one indirect call. Per-instance DSP state is **arena-backed** (a tiny
  `HostOpState{ void* ptr; }` variant member, region reserved at load time like
  `DelayState::ensure_buffer`).

### Key design decisions

- **C++ headers in v1**, modeled on the existing `cedar::UriResolver`/`UriHandler` singleton
  registry (`cedar/include/cedar/io/uri_resolver.hpp`). A stable **C ABI / FFI** wrapper and
  **dynamic dlopen loading** are explicitly deferred to later phases.
- **Static registration at host init; immutable once audio starts.** Both the compile thread
  and the audio thread only *read* the registries after setup → **no locking required**. Late
  registration trips a debug assert and is silently ignored in release.
- **Host opcode impls receive the raw `ExecutionContext&` + `const Instruction&`** — the exact
  interface internal opcodes get. Maximum power; the future C ABI is a translation layer over it.
- **Core subset of `BuiltinInfo`** for host descriptors (≤5 inputs, defaults, `requires_state`,
  mono/stereo output, description). `ExtendedParams`/`OptionSchema`/record-options deferred.
- **Collisions are rejected** at registration time — a host name may not shadow a core builtin
  or a previously-registered host name.
- **Native-only, behind a `CEDAR_HOST_EXTENSIONS` CMake flag** — compiled out of the WASM/web
  build so the web dispatch path is untouched.
- **Full introspection** — host functions and variables appear in `akkado_get_builtins_json()`
  so host-app editors (Godot inspector, JUCE editor) get autocomplete and validation.

---

## 2. Problem Statement / Current State

### 2.1 Three closed extension surfaces

| Surface | Today | Why it's closed |
|---|---|---|
| **Variables** (`bpm`, `sr`, `spb`) | `BUILTIN_VARIABLES` static map (`builtins.hpp:1723`) + live `EnvMap*` on the context | Names are compile-time literals; no API to add one |
| **Builtins** | `BUILTIN_FUNCTIONS` `inline const unordered_map<string_view, BuiltinInfo>` (`builtins.hpp:274`); keys are literals | `const`, baked in; string_view keys require static storage |
| **Opcodes** | `Opcode` `uint8_t` enum + a ~149-case compile-time `switch` (`vm.cpp:1358`); state is a closed `std::variant` (`dsp_state.hpp:1530`) | Jump-table dispatch and the variant are both compile-time-fixed |

### 2.2 What embedders need (from the target PRDs)

- **Special hardware** (`prd-cedar-esp32.md`, `prd-daisysp-integration.md`): genuinely new DSP
  ops — access to accelerators, CV outputs, sensors. DaisySP today must *vendor source and add
  static opcodes 210–254*; a host can't add its own at all.
- **VST/JUCE plugin** (`prd-juce-plugin.md`): host tempo/transport as readable signals; expose
  host parameters. Currently only user-authored `param()` slots, nothing host-injected.
- **Game engine** (`prd-godot-extension.md`, SHIPPED external): feed game state (health,
  position, enemy count) into audio. Currently only `set_param()` on user-declared `param()`s —
  the host cannot introduce a first-class variable or a custom op.

### 2.3 Existing precedent to reuse

`cedar::UriResolver` already implements exactly this shape for I/O: an abstract handler base,
a process-global singleton, `register_handler()` populated by the host at startup, "last
registration wins" (`cedar/include/cedar/io/uri_resolver.hpp:36-63`). The new registries follow
this template (with **reject-on-collision** instead of last-wins).

---

## 3. Goals and Non-Goals

### 3.1 Goals (v1)

1. A host can register, at init, **named variables** (control-rate scalar and audio-rate buffer)
   that Akkado code references like `bpm`.
2. A host can register **builtins that map to existing core opcodes** (null `impl_fn`).
3. A host can register **new opcodes** with a host-provided audio-thread implementation and
   arena-backed per-instance state.
4. Registered extensions are **type-checked and code-generated identically to core builtins** and
   appear in **introspection JSON** for editor tooling.
5. **Zero changes to the audio-thread fast path** for programs that use no host ops; the engine
   builds and behaves identically when `CEDAR_HOST_EXTENSIONS` is off.
6. The C++ API is shaped so the **future C ABI is a thin wrapper**, not a redesign.

### 3.2 Non-Goals (deferred to future PRDs)

- **Stable C ABI / FFI** (flat C view of context + registration). Phase boundary; see §12.
- **Dynamic plugin loading** (`dlopen`/discovery of `.so`/`.dll`). Requires the C ABI first.
- **Closed-source binary plugin distribution** — enabled by the C ABI, out of scope here.
- **`ExtendedParams`/`OptionSchema`/record-options** on host descriptors (core subset only).
- **Host-defined control-flow** (poly/foreach/block-call analogues). Host ops are leaf DSP nodes.
- **Custom hot-swap state migration hooks** — host op state follows the standard semantic-ID
  matching and GC like core state.
- **WASM/web exposure** — native-only in v1.

---

## 4. Target API / User Experience

The API spans three tiers of host extension, in increasing power:

- **Tier 1 — Host variables** (§4.1–4.2): named host-fed signals (control-rate scalar or
  audio-rate buffer). No new opcode; resolves like `bpm`.
- **Tier 2 — Host functions over core opcodes** (§4.3): a host-named builtin that emits an
  *existing* `Opcode` (alias/preset). Null `impl_fn`; no new runtime behavior.
- **Tier 3 — Host opcodes** (§4.4): a genuinely new DSP node with a host-provided audio-thread
  `impl_fn` and arena-backed state.

### 4.1 Host variables — control-rate scalar

```cpp
// Host init (e.g. Godot _ready, JUCE prepareToPlay, firmware setup):
akkado::register_host_variable({
    .name        = "health",        // referenced verbatim in Akkado source
    .default_val = 1.0f,
    .min         = 0.0f,
    .max         = 1.0f,
    .rate        = akkado::HostVarRate::Control,   // one value per block
});

// Each frame, lock-free (reuses the existing EnvMap/set_param channel):
vm.set_param("health", player.health / player.max_health);
```

```akkado
// Akkado source — 'health' resolves like a builtin variable (bpm/sr/spb):
saw(110) |> lp(@, 200 + health * 3000) |> out(@, @)
```

### 4.2 Host variables — audio-rate buffer

```cpp
akkado::register_host_variable({
    .name = "cv_in",
    .rate = akkado::HostVarRate::Audio,            // per-sample, 128-float block
});

// Host owns the buffer; fills it before each process_block():
float cv[128];
fill_from_hardware_adc(cv);
vm.set_host_buffer("cv_in", cv);   // binds buffer for the next block
vm.process_block(L, R);
```

### 4.3 Host function → existing core opcode (null impl, Tier 2)

```cpp
// Alias / preset: a named wrapper over the built-in Dattorro reverb with
// host-chosen default size/decay — no new DSP code, just a new spelling.
akkado::register_host_function({
    .name        = "myreverb_alias",
    .core_opcode = cedar::Opcode::REVERB_DATTORRO,  // emit an existing opcode
    .input_count = 1,
    .optional_count = 4,
    .param_names = {"in", "size", "decay", "dry", "wet"},
    .defaults    = {0.8f, 0.5f, 0.0f, 1.0f},
    .requires_state = true,
    .output_channels = akkado::ChannelCount::Stereo,
    .description = "Host preset over the built-in Dattorro reverb",
}, /*impl_fn=*/nullptr);
```

### 4.4 Host opcode — genuinely new DSP (Tier 3)

```cpp
// 1. The audio-thread implementation — same signature as internal opcodes.
void op_hardware_bitcrush(cedar::ExecutionContext& ctx, const cedar::Instruction& inst) {
    float*       out = ctx.buffers->get(inst.out_buffer);
    const float* in  = ctx.buffers->get(inst.inputs[0]);
    // Optional input: codegen already wired inst.inputs[1] to either the
    // caller's buffer or a constant buffer holding the descriptor default
    // (8.0f), exactly like core builtins. `get_input_or_zero` (oscillators.hpp)
    // is the zero-fallback variant when a slot may be BUFFER_UNUSED.
    const float* bits = ctx.buffers->get(inst.inputs[1]);

    // Arena-backed per-instance state (reserved at load; see §6.4):
    auto& st = ctx.states->get_or_create<cedar::HostOpState>(inst.state_id);
    auto* mine = static_cast<MyCrushState*>(st.ptr);   // host-defined layout

    for (std::size_t i = 0; i < cedar::BLOCK_SIZE; ++i) {
        const float step = std::exp2(bits[i]);
        out[i] = std::round(in[i] * step) / step;      // (toy example)
    }
}

// 2. Register descriptor + impl at host init. Returns a handle/error.
auto result = akkado::register_host_function({
    .name        = "hwcrush",
    .input_count = 1,
    .optional_count = 1,
    .param_names = {"in", "bits"},
    .defaults    = {8.0f},
    .requires_state = true,
    .state_bytes = sizeof(MyCrushState),   // arena reservation size per instance
    .output_channels = akkado::ChannelCount::Mono,
    .description = "Hardware-accelerated bitcrusher",
}, &op_hardware_bitcrush);

if (!result) { /* collision or post-audio registration — handle */ }
```

```akkado
// Akkado source uses it like any builtin, incl. dot-call and pipes:
saw(220).hwcrush(6) |> out(@, @)
```

### 4.5 What the host must NOT do inside `impl_fn`

The impl runs on the audio thread. The documented real-time contract (not enforced):
**no heap allocation, no locks, no blocking I/O, no syscalls.** Scratch space comes from
the reserved arena region; anything larger must be pre-reserved at registration via `state_bytes`.

---

## 5. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| `Opcode` enum (`instruction.hpp`) | **Modified** | Add one `HOST_OP` value in the free `201–209` gap (e.g. `209`). Avoid the in-use `210–223` band (control-flow + event opcodes, `SKIP_IF_ZERO`…`EVENT_FANOUT`) and `INVALID=255`; see §6.6 |
| VM `execute()` switch (`vm.cpp:1358`) | **Modified** | One `case HOST_OP:` → indirect dispatch through the host op table |
| `DSPState` variant (`dsp_state.hpp:1530`) | **Modified** | Add `HostOpState{ void* ptr; std::uint32_t bytes; }` (one pointer + size; far smaller than the largest existing variant member, so no slot bloat) |
| `BuiltinInfo` / `lookup_builtin` (`builtins.hpp`) | **Modified** | `lookup_builtin` consults the host registry after the static map |
| Codegen call path (`codegen.cpp:1265`) | **Modified** | Emit `HOST_OP` (rate = host index) for host opcodes; emit mapped core opcode for null-impl |
| Builtin variable codegen (`prd-builtin-variables`) | **Modified** | Extend the `bpm/sr/spb` mechanism to host-registered names |
| `akkado_get_builtins_json()` (`builtins_json.cpp`, `nkido_wasm.cpp:1279`) | **Modified** | Include host functions + variables |
| `UriResolver` (`uri_resolver.hpp`) | **Stays** | Pattern reused, not changed |
| StatePool GC / hot-swap (`state_pool.hpp`) | **Stays** | Host op state matched/swept by semantic ID like core |
| WASM build (`web/wasm/`) | **Stays** | `CEDAR_HOST_EXTENSIONS` off → no `HOST_OP`, no registries compiled in |
| `cedar::HostOpRegistry` | **New** | Cedar-side fn-ptr table (index → impl) |
| `akkado::HostFunctionRegistry` / `HostVariableRegistry` | **New** | Akkado-side owned descriptors + names |

---

## 6. Architecture / Technical Design

### 6.1 Registration is split across the two libraries

A host function has two halves: a **language-facing descriptor** (params, defaults, channels —
needed by the akkado compiler) and an **audio-thread implementation** (needed by the cedar VM).
`akkado` already links `cedar`, so the public entry points live in `akkado` and fan out:

```
akkado::register_host_function(desc, impl_fn)
   │
   ├─ allocate host op index N (0..255)                 [akkado registry]
   ├─ store owned descriptor under desc.name            [akkado HostFunctionRegistry]
   │     (owns std::string name + param names; synthesizes a BuiltinInfo view)
   └─ if impl_fn: cedar::HostOpRegistry::instance()
                     .register_op(N, impl_fn, state_bytes)   [cedar registry]
```

`register_host_variable` registers an owned name + metadata in the akkado
`HostVariableRegistry` and, for audio-rate variables, reserves a host-buffer binding slot in
the VM.

### 6.2 Compile-time lookup

`lookup_builtin(name)` (`builtins.hpp:1684`) gains a fallback: after the static `BUILTIN_FUNCTIONS`
/ `BUILTIN_ALIASES` miss, consult the host registry. Because **collisions are rejected at
registration**, a host name can never shadow a core name, so ordering is purely a miss-path
optimization. Host variables hook the identifier-resolution path the same way `bpm/sr/spb` do.

### 6.3 Code generation

- **Host opcode** (`impl_fn != null`): emit a single `HOST_OP` instruction with
  `inst.rate = host_index`, inputs wired from the call arguments exactly like a core builtin,
  `requires_state` → allocate a `state_id` (FNV-1a semantic hash, same as core), defaults applied
  from the descriptor.
- **Core-opcode alias** (`impl_fn == null`): emit `desc.core_opcode` directly — codegen treats
  the descriptor as if it were a normal `BuiltinInfo`. No new runtime behavior.

### 6.4 Runtime dispatch and state

```
            ┌──────────────────── VM::execute(inst) switch ────────────────────┐
 core ops → │ case OSC_SIN: op_osc_sin(ctx,inst);  // jump table, no indirection │
            │ ...                                                                │
 host op  → │ case HOST_OP:                                                      │
            │     HostOpRegistry::instance().table[inst.rate](ctx, inst);  ←─────┼─ one indirect call
            └────────────────────────────────────────────────────────────────────┘
```

`HostOpRegistry` (cedar): a process-global singleton holding
`std::array<HostOpFn, 256> table` plus per-index `state_bytes`. `HostOpFn` is
`void(*)(ExecutionContext&, const Instruction&)`.

**State** is arena-backed to avoid inflating every StatePool slot. The variant gains:

```cpp
struct HostOpState { void* ptr = nullptr; std::uint32_t bytes = 0; };  // ptr + reserved size (for GC/reclamation — §13-Q1)
```

At program load (off the audio thread), for each `HOST_OP` with `requires_state`, the loader
reserves `state_bytes` from the `AudioArena` and stores the pointer in the `HostOpState` slot
keyed by `inst.state_id` — mirroring `DelayState::ensure_buffer(samples, arena)`. The impl casts
`st.ptr` to its own layout. First-touch zero-initialization is guaranteed by the loader.

### 6.5 Host variables at runtime

- **Control-rate scalar**: reuses the existing lock-free `EnvMap`/`set_param` path. The registry
  synthesizes a `BuiltinVarDef` for the name (reserved `env_key`, getter/setter — see §6.7), so the
  analyzer desugars the identifier to the same `ENV_GET` read used by `bpm/sr/spb`. No new codegen
  path; only the variable-resolution table gains the host entry.
- **Audio-rate buffer**: the host registers a name and, each block, calls
  `vm.set_host_buffer(name, float* /*[128]*/)` before `process_block()`. The VM binds that pointer
  to the buffer index the compiler reserved for the variable. Buffer ownership stays with the host;
  the VM only reads it during the block. (Single-producer/single-consumer at the block boundary,
  matching the existing input-buffer contract `set_input_buffers`.)

### 6.6 The `HOST_OP` enum slot

The opcode enum is nearly full at the top. As of today the band **210–223 is already in use**
by shipped opcodes — `SKIP_IF_ZERO=210`, `SKIP_IF_NONZERO=211`, `LOOP_STATIC=212`,
`BLOCK_CALL=213`, `RET=214`, `FOREACH_EVENT=215`, `BLOCK_BIND=216`, `SEQPAT_VALUES=217`,
`EVENT_MAP=218`, `EVENT_FILTER=219`, `FMOD=220`, `EVENT_RATE_SCALE=221`, `EVENT_REORDER=222`,
`EVENT_FANOUT=223` (`instruction.hpp:217`+) — and `INVALID=255`. The actual free ranges are the
**`201–209` gap** (`OSC_WAVETABLE=200` is the last value before the control-flow band) and
**`224–254`**.

To avoid scarce-enum contention, host extensibility uses **a single `HOST_OP` value** with a
256-wide `inst.rate` index — it consumes exactly one enum slot regardless of how many host ops
exist. Proposed value: **`HOST_OP = 209`**, which sits in the free `201–209` gap, with
`INVALID = 255` untouched.

> **Note:** `prd-daisysp-integration.md` proposes a DaisySP opcode range of `210–254`, but that
> range *already collides* with the shipped `210–223` control-flow/event opcodes above. A single
> source of truth for opcode-range allocation should reconcile this (see §13-Q4); it does not block
> `HOST_OP=209`, which lives in a distinct free gap.

### 6.7 Data structures (sketch)

```cpp
namespace akkado {

enum class HostVarRate { Control, Audio };

struct HostVariableDesc {
    std::string  name;
    float        default_val = 0.0f, min = 0.0f, max = 0.0f;
    HostVarRate  rate = HostVarRate::Control;
};

// Internally, a registered Control-rate host variable is materialized as a
// `BuiltinVarDef` (the shipped bpm/sr/spb mechanism, builtins.hpp:1723) so it
// reuses identifier→ENV_GET desugar unchanged:
//   - env_key     := "__host_" + name   (reserved EnvMap key; collision-checked)
//   - getter_name := "get_host_" + name
//   - setter_name := "set_host_" + name (host vars are always writable via set_param)
//   - default/min/max copied from the desc.
// The registry owns these synthesized strings (process lifetime). `set_param(name, x)`
// writes the env_key channel; the identifier resolves to ENV_GET on that key.

struct HostFunctionDesc {
    std::string                     name;
    cedar::Opcode                   core_opcode = cedar::Opcode::INVALID; // null-impl path
    std::uint8_t                    input_count = 0, optional_count = 0;
    std::array<std::string, 5>      param_names{};
    std::array<float, 5>            defaults{};       // NaN = required
    bool                            requires_state = false;
    std::uint32_t                   state_bytes = 0;  // arena reservation per instance
    ChannelCount                    output_channels = ChannelCount::Mono;
    std::string                     description;
};

// Returns false on collision or post-audio registration.
[[nodiscard]] bool register_host_variable(const HostVariableDesc&);
[[nodiscard]] bool register_host_function(const HostFunctionDesc&,
                                          cedar::HostOpFn impl_fn /*= nullptr*/);
}  // namespace akkado
```

Registry-owned `std::string`s back the names/param names; the compiler synthesizes a
`BuiltinInfo` whose `string_view`s point into that owned storage (lifetime = process).

---

## 7. File-Level Changes

| File | Change |
|---|---|
| `cedar/include/cedar/vm/instruction.hpp` | Add `HOST_OP` opcode value in the free `201–209` gap (e.g. `209`); avoid the in-use `210–223` band — see §6.6 |
| `cedar/include/cedar/vm/host_op_registry.hpp` | **New** — `HostOpRegistry` singleton, `HostOpFn` typedef, 256-entry table + `state_bytes` |
| `cedar/src/vm/host_op_registry.cpp` | **New** — registry impl |
| `cedar/include/cedar/opcodes/dsp_state.hpp` | Add `HostOpState` to the `DSPState` variant (`:1530`) |
| `cedar/src/vm/vm.cpp` | Add `case HOST_OP:` to `execute()` (`:1358`); reserve arena state for `HOST_OP` in the load path |
| `akkado/include/akkado/host_extensions.hpp` | **New** — public `register_host_*` API + desc structs |
| `akkado/src/host_extensions.cpp` | **New** — `HostFunctionRegistry` / `HostVariableRegistry`, owned storage, collision + immutability checks |
| `akkado/include/akkado/builtins.hpp` | `lookup_builtin` consults host registry; identifier resolution for host variables |
| `akkado/src/codegen.cpp` | Emit `HOST_OP` (rate=index) / mapped core opcode; allocate host-op `state_id` |
| `akkado/src/builtins_json.cpp` | Include host functions + variables in introspection JSON |
| `cmake/…` + `cedar`/`akkado` `CMakeLists.txt` | `CEDAR_HOST_EXTENSIONS` option; `#if`-gate all of the above |
| `web/wasm/CMakeLists.txt` | Leave `CEDAR_HOST_EXTENSIONS` OFF for WASM |
| `docs/concepts/host-extensions.md` | **New** — authoring guide + real-time contract |

Files explicitly requiring **no change**: `uri_resolver.*`, `state_pool.hpp` GC logic,
the WASM dispatch path, every core opcode handler.

---

## 8. Implementation Phases

### Phase 1 — Host variables (control-rate)
**Goal:** A host-registered scalar variable resolves in Akkado and is driven via `set_param`.
- `host_extensions.hpp/.cpp` skeleton; `HostVariableRegistry` (Control rate only).
- Hook identifier resolution + codegen onto the existing `bpm/sr/spb` path.
- `CEDAR_HOST_EXTENSIONS` flag wired; OFF for WASM.
- **Verify:** register `health`; compile `lp(saw(110), 200 + health*3000)`; drive `set_param("health", x)`; assert filter cutoff tracks.

### Phase 2 — Host functions over existing opcodes (null impl)
**Goal:** A host-registered name maps to a core `Opcode` and codegens identically to a builtin.
- `HostFunctionRegistry` with owned strings; `lookup_builtin` fallback; collision rejection.
- Codegen emits the mapped core opcode.
- **Verify:** register an alias over `REVERB_DATTORRO`; compile + render; output matches calling the core builtin directly. Collision with `lp` returns `false`.

### Phase 3 — Host opcodes (the real extension)
**Goal:** A host `impl_fn` runs on the audio thread with arena-backed state.
- `HOST_OP` enum value; `HostOpRegistry` + `execute()` dispatch case.
- `HostOpState` variant member; arena reservation in the load path.
- Codegen emits `HOST_OP` with `rate = index`, allocates `state_id`.
- **Verify:** register `hwcrush`; `saw(220).hwcrush(6)` renders; state persists across blocks and hot-swap (matched by semantic ID); a program using no host ops produces byte-identical bytecode and output to a `CEDAR_HOST_EXTENSIONS=OFF` build.

### Phase 4 — Audio-rate variables + introspection + docs
**Goal:** Audio-rate host buffers; editor metadata; authoring guide.
- `set_host_buffer` binding; `HostVarRate::Audio` codegen.
- Host functions/variables in `akkado_get_builtins_json()`.
- Immutability guard (debug assert / release ignore) once `process_block` has run.
- `docs/concepts/host-extensions.md`.
- **Verify:** feed a 128-float ramp via `set_host_buffer("cv_in", …)`, confirm per-sample modulation; confirm host entries appear in builtins JSON; late registration asserts in debug, is ignored in release.

### Future (separate PRDs) — see §12
Stable C ABI wrapper; dynamic `dlopen` loading; `ExtendedParams`/`OptionSchema` parity.

---

## 9. Edge Cases

- **Name collision (core).** `register_host_function("lp", …)` → returns `false`, nothing
  registered. (Reject policy; override would require an explicit future `unregister`/`replace`.)
- **Name collision (host-vs-host).** Second registration of the same name → `false`.
- **Reference to an unregistered name.** `hwcrush(…)` with no registration → existing `E107
  Unknown function`; a bare unregistered identifier → existing unknown-identifier error. No new
  failure path.
- **Registration after `process_block()`.** Debug build: assert. Release build: silently ignored,
  returns `false`. (No data race introduced on the audio thread.)
- **`requires_state` but `state_bytes == 0`.** Registration error (`false`) — a stateful host op
  must declare its reservation size.
- **Host op state across hot-swap.** Matched by semantic `state_id` like core; arena region
  re-reserved on full reload, preserved across micro-crossfade swaps. **[OPEN QUESTION §13]** —
  arena reclamation when a host-op instance is GC'd as untouched.
- **Audio-rate variable not bound for a block.** If the host doesn't call `set_host_buffer` before
  `process_block`, the variable reads the `BUFFER_ZERO` constant-zero buffer (silence), never a
  dangling pointer.
- **`CEDAR_HOST_EXTENSIONS=OFF`.** `register_host_*` symbols absent; programs that reference a
  host name simply fail to compile with the standard unknown-name error. The audio path has no
  `HOST_OP` case and no registry — identical to today.
- **More than 256 host opcodes.** Registration beyond index 255 returns `false`. (256 is far
  beyond any realistic host; documented limit, not silent truncation.)
- **Stereo host op.** `output_channels = Stereo` follows the existing stereo-native codegen wiring
  used by core stereo builtins; the impl writes both `ctx.output`/buffer channels per the same flags.

---

## 10. Testing / Verification Strategy

**Build matrix:** every test runs with `CEDAR_HOST_EXTENSIONS` ON; a smoke build with it OFF must
compile and pass the existing suite unchanged.

**Akkado tests** (`akkado/tests/`):
- Register a control variable → `akkado::compile("… health …")` succeeds; `lookup_builtin`
  resolves it; bytecode reads the env buffer.
- Register a null-impl function over a core opcode → emitted bytecode equals the bytecode for the
  equivalent core-builtin call (instruction-by-instruction).
- Collision (`lp`, duplicate host name) → `register_host_function` returns `false`, registry
  unchanged.
- Introspection: registered entries present in `akkado_get_builtins_json()` with correct params.

**Cedar tests** (`cedar/tests/`):
- Register a host opcode that writes `out[i] = in[i] * 2`; hand-assemble a `HOST_OP` instruction;
  `process_block` yields the doubled signal.
- Stateful host op (running counter in arena state) → state persists across blocks and survives a
  hot-swap that keeps the same semantic ID.
- A program with no `HOST_OP` → confirm the `execute()` dispatch is unchanged (golden bytecode +
  output parity against an OFF build).

**Manual / experiments** (`experiments/`): a `test_op_hostcrush.py`-style harness registering a
host op through the Python bindings (if exposed) or a small native harness, rendering ≥300 s of
audio per the project's long-run rule, with a WAV for human listening.

**Build commands:**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCEDAR_HOST_EXTENSIONS=ON
cmake --build build
./build/cedar/tests/cedar_tests "*host op*"
./build/akkado/tests/akkado_tests "[host-extensions]"
# OFF-build smoke:
cmake -B build-off -DCEDAR_HOST_EXTENSIONS=OFF && cmake --build build-off
```

---

## 11. Real-Time & Threading Contract

- Registries are populated **only at host init** and read-only afterward → no locks on either
  thread. The immutability guard (Phase 4) catches violations in debug.
- `impl_fn` runs on the **audio thread**: no alloc/lock/blocking. Scratch comes from the arena
  region declared via `state_bytes`.
- Control-rate variables ride the existing lock-free `EnvMap` SPSC channel.
- Audio-rate host buffers follow the existing `set_input_buffers` SPSC contract: host writes
  before `process_block`, VM reads during it, host owns the memory.

---

## 12. Future Work (post-v1)

1. **Stable C ABI / FFI** — a flat C view (`cedar_hostop_ctx_get_input(ctx, i) -> float*`,
   `…_get_output`, `…_get_state`, `sample_rate`, `block_size`, `bpm`) plus `extern "C"`
   registration. Because v1 hands impls the raw `ExecutionContext&`, this phase adds a translation
   shim, not a redesign. Unlocks **closed-source** and **cross-compiler** extensions.
2. **Dynamic plugin loading** — `dlopen`/`LoadLibrary` discovery of extension modules that
   self-register through the C ABI. Needs (1) first.
3. **Descriptor parity** — `ExtendedParams<N>` (6+ params) and `OptionSchema` record-options for
   host functions, reusing `ext_params_state_id()` (`instruction.hpp:363`) and
   `extract_options()`.
4. **Host control-flow primitives** — if a host ever needs poly/foreach-style host nodes.

---

## 13. Open Questions

1. **Arena reclamation for GC'd host-op state.** When a host-op instance goes untouched and the
   StatePool sweep drops its `HostOpState` slot, the reserved arena region should be reclaimable.
   Does the current `AudioArena` support per-region free, or do host-op regions live until the next
   full reload (acceptable for v1)? Confirm against the arena allocator's actual semantics.
2. **Owned-string storage shape.** Confirm the cleanest way to back host `BuiltinInfo`
   `string_view`s with process-lifetime owned strings (deque of `std::string` that never
   reallocates vs. an interning arena) without touching the static-map fast path.
3. **Python-bindings exposure.** Should `register_host_function` be reachable from `cedar_core`
   for experiments/testing, or is a small native test harness sufficient for v1?
4. **`HOST_OP` enum value.** `209` is confirmed free today (it sits in the `201–209` gap between
   `OSC_WAVETABLE=200` and `SKIP_IF_ZERO=210`). The open coordination item is that
   `prd-daisysp-integration.md`'s proposed `210–254` range already collides with the shipped
   `210–223` control-flow/event opcodes — establish a single source of truth for opcode-range
   allocation so DaisySP and host ops don't both land on occupied slots.
```
