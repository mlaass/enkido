#pragma once

#include <cedar/vm/instruction.hpp>
#include "akkado/typed_value.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <unordered_map>

#include <frozen/string.h>
#include <frozen/unordered_map.h>

namespace akkado {

/// Maximum number of parameters for a builtin function (using inputs[0..4] + defaults).
/// Up to 5 of these reach the instruction's input slots; positions 5+ are reserved
/// for codegen-only literals (e.g. phaser packs stages/feedback into inst.rate).
/// For builtins needing more *runtime* parameters, use extended_param_count with
/// ExtendedParams<N>.
constexpr std::size_t MAX_BUILTIN_PARAMS = 8;

/// Maximum number of optional parameters with defaults.
constexpr std::size_t MAX_BUILTIN_DEFAULTS = 7;

// MAX_EXTENDED_PARAMS lives in typed_value.hpp (see note there).

// `ParamValueType` is defined in typed_value.hpp (alongside `ValueType`) so the
// AST and parser headers can reference it without pulling in the heavy builtin-
// metadata header. The helpers below (`param_value_type_name`, `type_compatible`)
// live here because they are only consumed by the builtin / codegen layers.

/// Human-readable name for a ParamValueType (for error messages)
constexpr const char* param_value_type_name(ParamValueType type) {
    switch (type) {
        case ParamValueType::Any:      return "Any";
        case ParamValueType::Signal:   return "Signal";
        case ParamValueType::Pattern:  return "Pattern";
        case ParamValueType::String:   return "String";
        case ParamValueType::Function: return "Function";
        case ParamValueType::Array:    return "Array";
        case ParamValueType::Record:   return "Record";
        case ParamValueType::Stream:   return "Stream";
        case ParamValueType::Number:   return "Number";
    }
    return "Unknown";
}

/// Check if actual ValueType is compatible with expected ParamValueType.
/// Rules:
///   Any       — always compatible
///   Signal    — accepts Signal or Number (Number auto-promotes to constant buffer)
///   Pattern   — accepts Pattern only
///   String    — accepts String only
///   Function  — accepts Function only
///   Array     — accepts Array only
///   Record    — accepts Record or Pattern (Pattern is structurally a record)
///   Stream    — accepts Pattern (post-Phase-5-Commit-I covers runtime event sources via PatternPayload::is_runtime_event_source)
///   Number    — accepts Number only (strict compile-time constant; PRD prd-parameter-type-annotations-phase-2 §4.2)
inline bool type_compatible(ValueType actual, ParamValueType expected) {
    switch (expected) {
        case ParamValueType::Any:      return true;
        // Signal accepts Signal, Number (auto-promoted), and mono Pattern
        // (voice-0 coerce). Array is deliberately NOT accepted: chord/array
        // expansion (`expand_call_arguments`) flattens an array into per-element
        // Signals BEFORE this check runs, so a raw Array reaching here means the
        // array was not expandable in this position — a genuine type error
        // (e.g. `out([1,2,3])`). DynArray is handled separately (E181) earlier.
        case ParamValueType::Signal:   return actual == ValueType::Signal || actual == ValueType::Number || actual == ValueType::Pattern;
        case ParamValueType::Pattern:  return actual == ValueType::Pattern;
        case ParamValueType::String:   return actual == ValueType::String;
        case ParamValueType::Function: return actual == ValueType::Function;
        case ParamValueType::Array:    return actual == ValueType::Array;
        case ParamValueType::Record:   return actual == ValueType::Record || actual == ValueType::Pattern;
        case ParamValueType::Stream:   return actual == ValueType::Pattern;
        case ParamValueType::Number:   return actual == ValueType::Number;
    }
    return false;
}

/// Concrete value type for an individual option field within a record-shaped
/// parameter. Distinct from ParamValueType (which describes top-level function
/// parameter types) — option fields are always one of: number, string, bool, enum.
enum class OptionFieldType : std::uint8_t {
    Number = 0,
    String,
    Bool,
    Enum,
};

constexpr const char* option_field_type_name(OptionFieldType t) {
    switch (t) {
        case OptionFieldType::Number: return "number";
        case OptionFieldType::String: return "string";
        case OptionFieldType::Bool:   return "bool";
        case OptionFieldType::Enum:   return "enum";
    }
    return "unknown";
}

/// One field of a record-shaped option parameter. The default_repr is the
/// textual representation as it would appear in source — "180", "\"viridis\"",
/// "true". Empty default_repr means "no default" (omitted from JSON).
struct OptionField {
    std::string_view name = {};
    OptionFieldType  type = OptionFieldType::Number;
    std::string_view default_repr = {};
    std::string_view description = {};
    std::string_view enum_values = {};  // comma-separated, only when type == Enum
};

constexpr std::size_t MAX_OPTION_FIELDS_PER_SCHEMA = 16;
constexpr std::size_t MAX_OPTION_SCHEMAS_PER_BUILTIN = 2;

/// Schema for one record-typed parameter slot of a builtin. param_index points
/// at the parameter (0-based). field_count is the number of populated entries
/// in fields[].
struct OptionSchema {
    std::uint8_t                                          param_index = 0;
    std::array<OptionField, MAX_OPTION_FIELDS_PER_SCHEMA> fields = {};
    std::uint8_t                                          field_count = 0;
    bool                                                  accepts_spread = true;
};

// PRD prd-codegen-sprawl-cleanup Phase 4: per-builtin codegen dispatch.
// CodeGenerator is incomplete here (codegen.hpp includes this header);
// member pointers to incomplete classes are fine. The NodeIndex parameter
// is spelled std::uint32_t (its underlying type, see ast.hpp) to avoid an
// ast.hpp include.
class CodeGenerator;
struct Node;
using CodegenHandler = TypedValue (CodeGenerator::*)(std::uint32_t /*NodeIndex*/,
                                                     const Node&);

/// Structural classification driving the visit() Call-branch dispatch.
/// Builtins with fully custom codegen set `codegen_handler` instead —
/// there is deliberately no `Special` kind (one mechanism, not two).
enum class BuiltinKind : std::uint8_t {
    Function,           // Default — generic emission path
    StereoNative,       // (reserved: stereo_native flag still drives this today)
    Visualization,      // pianoroll, oscilloscope, waveform, spectrum, waterfall
    Param,              // param, button, toggle, dropdown/select
    PatternTransform,   // bank, variant, transport, tune, palindrome, ...
    Sequencer,          // seq, timeline, pat
    Bus,                // bus/out, mixer/master
    SampleScalar,       // sample("name", ...) scalar form
};

/// Metadata for a built-in function
struct BuiltinInfo {
    cedar::Opcode opcode;       // VM opcode to emit
    std::uint8_t input_count;   // Number of required inputs
    std::uint8_t optional_count; // Number of optional inputs with defaults
    bool requires_state;        // Whether opcode needs state_id (oscillators, filters)
    std::array<std::string_view, MAX_BUILTIN_PARAMS> param_names;  // Parameter names for named args
    std::array<float, MAX_BUILTIN_DEFAULTS> defaults;              // Default values (NaN = required)
    std::string_view description;  // One-line docstring for autocomplete
    std::uint8_t extended_param_count = 0;  // Parameters beyond inputs[5] (stored in ExtendedParams)
    std::array<ParamValueType, MAX_BUILTIN_PARAMS> param_types = {};  // All Any by default

    // Channel-type signature (PRD prd-stereo-support §5.2, G1).
    // Only consulted for slots where param_types[i] == Signal; non-signal slots ignore.
    // Default-initialized = all Mono, output Mono — matches the mono-only
    // contract most non-audio builtins have.
    //
    // `output_channels = Match` (only valid on stereo-native builtins) means
    // the result width follows the primary signal input: mono in → mono out,
    // stereo in → stereo out. Use this for control-rate utilities (slew,
    // interp_*, env_follower, sah, gateup, gatedown, counter) so their output
    // does not collide with downstream mono parameter slots.
    std::array<ChannelCount, MAX_BUILTIN_PARAMS> input_channels = {};
    ChannelCount output_channels = ChannelCount::Mono;

    // PRD prd-stereo-native-opcodes §5.1: when true, this opcode handles both
    // stereo channels in one dispatch with one state struct. The codegen
    // allocates an adjacent L/R output buffer pair and emits a single
    // instruction with the STEREO_OUTPUT flag set; STEREO_INPUT is added when
    // the primary signal argument is stereo. The opcode body itself splits
    // L/R internally (per-channel arrays in the state struct). Auto-escalates
    // mono inputs by reading the same buffer for both internal lanes.
    //
    // Every audio-signal builtin is stereo_native as of Phase 5; the legacy
    // auto-lift mechanism (run-the-opcode-twice via STEREO_INPUT) is retired.
    // A Stereo signal in a Mono slot of a non-stereo_native builtin is a
    // compile error (E186). See the STEREO_INPUT / STEREO_OUTPUT truth table
    // in cedar/vm/instruction.hpp.
    bool stereo_native = false;

    // Static value to assign to inst.rate when this builtin lowers to its opcode.
    // Used by mode-dispatched opcodes (EDGE_OP modes 0-3, etc.) so multiple
    // builtin names share one opcode. Defaults to 0 — most opcodes ignore rate.
    std::uint8_t inst_rate = 0;

    // PRD prd-patterns-as-scalar-values §5.3: when true, the generic
    // dispatcher coerces any Pattern arg to Signal (via the freq buffer)
    // before this builtin runs. Pattern-aware builtins (`pat`, `slow`,
    // `transpose`, `bend`, ...) opt out by setting `args_are_signal = false`
    // in their entry. Orthogonal to `stereo_native` (Mono→Stereo).
    bool args_are_signal = true;

    // PRD prd-records-system-unification §5.1: option-field schemas for any
    // record-typed parameter slot. Empty by default; populated for builtins
    // whose record param has a known field shape (e.g. visualizers). Editor
    // autocomplete consumes these to suggest fields inside record literals.
    std::array<OptionSchema, MAX_OPTION_SCHEMAS_PER_BUILTIN> option_schemas = {};
    std::uint8_t option_schema_count = 0;

    // True for instruments that natively dispatch chord polyphony — i.e. they
    // accept a polyphonic pattern (max_voices > 1) without an enclosing
    // `poly()` because they have an internal voice allocator. When set, the
    // builtin's handler is responsible for erasing the pattern node from
    // CodeGenerator::polyphonic_pattern_nodes_ so the E410 warning doesn't
    // fire. Today: `soundfont`. Future: SF_PLAY / SAMPLE_VOICE.
    bool consumes_polyphonic_pattern = false;

    // PRD prd-extended-params §5 (canonical extended-param mechanism). When
    // extended_param_count > 0, args at positions
    // [total_params() .. total_params() + extended_param_count) lower into
    // an ExtendedParams<N> state init rather than inst.inputs[]. Names are
    // searched by reorder_named_arguments() just like the input names;
    // defaults are emitted as constant slots when the caller omits the arg.
    // These live at the end of the struct so existing positional aggregate-
    // init call sites do not need to change.
    std::array<std::string_view, MAX_EXTENDED_PARAMS> extended_param_names = {};
    std::array<float, MAX_EXTENDED_PARAMS> extended_defaults = {};  // NaN = required (rare)

    // PRD prd-codegen-sprawl-cleanup Phase 4. Non-null → visit()'s Call
    // branch dispatches straight to this member function (replaces the old
    // 100-entry name-keyed dispatch map in codegen.cpp). Null → per-kind
    // emitter selected by `kind`.
    CodegenHandler codegen_handler = nullptr;
    BuiltinKind kind = BuiltinKind::Function;

    /// Get total parameter count (required + optional)
    [[nodiscard]] std::uint8_t total_params() const {
        return input_count + optional_count;
    }

    /// Check if this builtin uses extended parameters (stored in StatePool)
    [[nodiscard]] bool has_extended_params() const {
        return extended_param_count > 0;
    }

    /// Get total parameter count including extended params
    [[nodiscard]] std::uint8_t total_with_extended() const {
        return total_params() + extended_param_count;
    }

    /// Find parameter index by name, returns -1 if not found. Searches
    /// regular input names first, then extended-param names; extended
    /// matches return total_params() + ext_idx (so callers can use the
    /// same index space for reordering).
    [[nodiscard]] int find_param(std::string_view name) const {
        for (std::size_t i = 0; i < MAX_BUILTIN_PARAMS; ++i) {
            if (param_names[i].empty()) break;
            if (param_names[i] == name) return static_cast<int>(i);
        }
        const std::size_t base = total_params();
        for (std::size_t i = 0; i < extended_param_count && i < MAX_EXTENDED_PARAMS; ++i) {
            if (extended_param_names[i].empty()) break;
            if (extended_param_names[i] == name) return static_cast<int>(base + i);
        }
        return -1;
    }

    /// Check if parameter at index has a default value (covers both input
    /// slots and extended-param slots).
    [[nodiscard]] bool has_default(std::size_t index) const {
        if (index < input_count) return false;  // Required input params don't have defaults
        if (index < total_params()) {
            std::size_t default_idx = index - input_count;
            if (default_idx >= MAX_BUILTIN_DEFAULTS) return false;
            return !std::isnan(defaults[default_idx]);
        }
        // Extended-param slot
        std::size_t ext_idx = index - total_params();
        if (ext_idx >= extended_param_count || ext_idx >= MAX_EXTENDED_PARAMS) return false;
        return !std::isnan(extended_defaults[ext_idx]);
    }

    /// Get default value for parameter at index (must check has_default first).
    [[nodiscard]] float get_default(std::size_t index) const {
        if (index < total_params()) {
            std::size_t default_idx = index - input_count;
            return defaults[default_idx];
        }
        std::size_t ext_idx = index - total_params();
        return extended_defaults[ext_idx];
    }

    /// Find the option-field schema attached to the parameter at `param_index`,
    /// or nullptr if no schema is declared for that slot. PRD prd-records-
    /// system-unification §5.5 — used by codegen::extract_options to validate
    /// caller-supplied record-literal field names.
    [[nodiscard]] const OptionSchema* find_option_schema(std::uint8_t param_index) const {
        for (std::uint8_t i = 0; i < option_schema_count; ++i) {
            if (option_schemas[i].param_index == param_index) {
                return &option_schemas[i];
            }
        }
        return nullptr;
    }
};

/// Static mapping of Akkado function names to Cedar opcodes
/// Used by semantic analyzer to resolve function calls.
/// Hardening PRD Phase 3: frozen (compile-time perfect-hash) map — zero
/// construction cost at process start, immutable by type. Heterogeneous
/// find/count/at accept std::string_view / literals directly.

struct BuiltinVarDef {
    std::string_view getter_name;   // "get_bpm"
    std::string_view setter_name;   // "set_bpm" (empty = read-only)
    std::string_view env_key;       // "__bpm" — reserved EnvMap key for getter
    float default_value;            // 120.0f
    float min_value;                // 1.0f (0 = no clamping)
    float max_value;                // 999.0f (0 = no clamping)
};

} // namespace akkado
