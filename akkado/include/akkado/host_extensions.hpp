#pragma once

// Host extension API (prd-host-extension-api §6.7).
//
// An embedder registers host variables, builtins and opcodes once at init;
// the registries are read-only afterward, so the compile thread and the audio
// thread need no locking. Names that collide with a core builtin — or with an
// earlier host registration — are rejected, never shadowed.
//
// Compiled only when CEDAR_HOST_EXTENSIONS is on. The WASM/web build leaves it
// off, so `plugin("Diva")` in a shared patch is an ordinary unknown-function
// error there rather than a silently-degraded node. That is a deliberate
// product decision (owner, 2026-07-10): the hosting surface belongs to the
// studio, not to the open language.

#ifdef CEDAR_HOST_EXTENSIONS

#include "akkado/builtins.hpp"
#include "akkado/typed_value.hpp"

#include <cedar/vm/host_op_registry.hpp>
#include <cedar/vm/instruction.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace akkado {

enum class HostVarRate : std::uint8_t { Control, Audio };

/// A host-owned named signal. Control-rate variables ride the existing
/// lock-free EnvMap channel and desugar to the same ENV_GET read `bpm`/`sr`
/// use, so they cost no new codegen path.
struct HostVariableDesc {
    std::string name;
    float default_val = 0.0f;
    float min = 0.0f;
    float max = 0.0f;
    HostVarRate rate = HostVarRate::Control;
};

/// A host-registered callable. Two modes:
///   - `impl_fn == nullptr`: `core_opcode` must be set. The name is an alias
///     over an existing core opcode; codegen emits that opcode directly and no
///     new runtime behavior exists.
///   - `impl_fn != nullptr`: a genuine host opcode. Codegen emits HOST_OP with
///     `rate` = the allocated registry index.
struct HostFunctionDesc {
    std::string name;
    cedar::Opcode core_opcode = cedar::Opcode::INVALID;
    std::uint8_t input_count = 0;
    std::uint8_t optional_count = 0;
    std::array<std::string, 5> param_names{};
    std::array<float, 5> defaults{};                 // NaN = required
    std::array<ParamValueType, 5> param_types{};     // all Any by default
    bool requires_state = false;
    std::uint32_t state_bytes = 0;                   // arena reservation per instance
    ChannelCount output_channels = ChannelCount::Mono;
    bool stereo_native = false;
    std::string description;
};

/// A host-registered *node*: a stateful, potentially event-driven callable
/// whose heavyweight instance the host owns and pools by semantic ID
/// (prd-studio-plugin-hosting §6.2/§6.3 — the OQ12 resolution).
///
/// This is a sibling of `register_host_function`, not an extension of it: a
/// plain host op stays a plain host op (5 float inputs, audio-thread impl,
/// arena scratch). A node additionally:
///
///   - takes a leading **string literal** argument naming the instance
///     (`plugin("Diva")`), which never reaches an input buffer. It is recorded
///     in `CompileResult::required_host_nodes` alongside the node's `state_id`,
///     exactly as `midi()` records `RequiredMidiSource`.
/// The engine never loads, prepares or destroys the instance. The host walks
/// the manifest off the audio thread, binds instances into its own pool keyed
/// by `state_id`, and the audio-thread `impl_fn` looks the instance up by
/// `inst.state_id`. An unbound `state_id` is the host's cue to pass through.
/// That keeps the whole plugin lifecycle — and JUCE — out of the open engine.
struct HostNodeDesc {
    HostFunctionDesc fn;

    // Event-stream input (pattern events → the node, rather than coercion to a
    // freq/gate buffer). Currently REJECTED at registration: the engine has no
    // consumer for it until the studio's VST3 backend needs pattern→MIDI, and
    // an accepted-but-unwired flag would be a stub. Lands with that consumer,
    // together with `RequiredHostNode::seq_state_id`.
    // ponytail: reject rather than ignore, so the gap is loud.
    bool accepts_events = false;
};

/// Compile-time record of one hosted-node call site. Emitted into
/// `CompileResult::required_host_nodes` for the host to pre-bind before the
/// audio thread resumes.
struct RequiredHostNode {
    std::uint32_t state_id = 0;   // semantic-ID hash of the call site
    std::uint8_t host_index = 0;  // HostOpRegistry dispatch index
    std::string name;             // the string-literal argument, e.g. "Diva"
};

/// All three return false on collision, on a malformed descriptor, or if
/// registration is attempted after the first `process_block()`.
[[nodiscard]] bool register_host_variable(const HostVariableDesc& desc);
[[nodiscard]] bool register_host_function(const HostFunctionDesc& desc,
                                          cedar::HostOpFn impl_fn = nullptr);
[[nodiscard]] bool register_host_node(const HostNodeDesc& desc, cedar::HostOpFn impl_fn);

/// The reserved EnvMap key a host variable reads from. Drive the variable with
/// `vm.set_param(host_variable_env_key("health"), x)` — the raw name is not the
/// channel, exactly as `bpm` reads `__bpm`. Empty if `name` is not registered.
[[nodiscard]] std::string_view host_variable_env_key(std::string_view name);

/// Compile-thread lookups. Consulted only after the static tables miss, so the
/// core fast path is untouched. Return nullptr when the name is not a host one.
[[nodiscard]] const BuiltinInfo* lookup_host_builtin(std::string_view name);
[[nodiscard]] const BuiltinVarDef* lookup_host_variable(std::string_view name);

/// True when the name was registered via `register_host_node` (codegen routes
/// its string-literal argument into the manifest instead of an input buffer).
[[nodiscard]] bool is_host_node(std::string_view name);

/// Argument index of a host node's String slot (the instance name), or -1 if
/// `name` is not a registered host node.
[[nodiscard]] int host_node_string_slot(std::string_view name);

/// Every registered host function, in registration order. `SymbolTable::
/// register_builtins` walks this so host names resolve like core ones.
[[nodiscard]] std::vector<std::pair<std::string_view, const BuiltinInfo*>> host_functions();

/// Test-only: drop every registration. Registration is otherwise immutable for
/// the process lifetime.
void reset_host_extensions_for_testing();

}  // namespace akkado

#endif  // CEDAR_HOST_EXTENSIONS
