#pragma once

#include <cedar/vm/instruction.hpp>
#include "akkado/typed_value.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace akkado {

/// Maximum number of parameters for a builtin function (using inputs[0..4] + defaults).
/// Up to 5 of these reach the instruction's input slots; positions 5+ are reserved
/// for codegen-only literals (e.g. phaser packs stages/feedback into inst.rate).
/// For builtins needing more *runtime* parameters, use extended_param_count with
/// ExtendedParams<N>.
constexpr std::size_t MAX_BUILTIN_PARAMS = 8;

/// Maximum number of optional parameters with defaults.
constexpr std::size_t MAX_BUILTIN_DEFAULTS = 7;

/// Maximum number of extended parameters (stored in StatePool)
constexpr std::size_t MAX_EXTENDED_PARAMS = 8;

/// Parameter type annotation for builtin functions.
/// Used for type checking arguments in the generic builtin call path.
/// Default `Any` means no checking — opt-in annotation.
enum class ParamValueType : std::uint8_t {
    Any = 0,     // No checking (default)
    Signal,      // Signal or Number (Number auto-promotes to constant buffer)
    Pattern,     // Pattern only
    String,      // Compile-time string only
    Function,    // Function/closure reference
    Array,       // Array type
    Record,      // Record or Pattern (Pattern is subtype of Record)
};

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
inline bool type_compatible(ValueType actual, ParamValueType expected) {
    switch (expected) {
        case ParamValueType::Any:      return true;
        case ParamValueType::Signal:   return actual == ValueType::Signal || actual == ValueType::Number || actual == ValueType::Pattern;
        case ParamValueType::Pattern:  return actual == ValueType::Pattern;
        case ParamValueType::String:   return actual == ValueType::String;
        case ParamValueType::Function: return actual == ValueType::Function;
        case ParamValueType::Array:    return actual == ValueType::Array;
        case ParamValueType::Record:   return actual == ValueType::Record || actual == ValueType::Pattern;
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
/// Used by semantic analyzer to resolve function calls
inline const std::unordered_map<std::string_view, BuiltinInfo> BUILTIN_FUNCTIONS = {
    // Basic Oscillators
    // All oscillators now support optional phase offset and trigger for phase reset.
    // Phase/trig default to BUFFER_UNUSED, which falls back to BUFFER_ZERO (always 0.0).
    // This avoids emitting PUSH_CONST instructions for the common case.
    {"tri",     {cedar::Opcode::OSC_TRI,    1, 2, true,
                 {"freq", "phase", "trig", "", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "Triangle wave oscillator"}},
    {"saw",     {cedar::Opcode::OSC_SAW,    1, 2, true,
                 {"freq", "phase", "trig", "", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "Band-limited sawtooth oscillator"}},
    {"sqr",     {cedar::Opcode::OSC_SQR,    1, 2, true,
                 {"freq", "phase", "trig", "", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "Band-limited square wave oscillator"}},
    {"ramp",    {cedar::Opcode::OSC_RAMP,   1, 2, true,
                 {"freq", "phase", "trig", "", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "Rising ramp oscillator (0 to 1)"}},
    {"phasor",  {cedar::Opcode::OSC_PHASOR, 1, 2, true,
                 {"freq", "phase", "trig", "", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "Phase accumulator (0 to 1 ramp)"}},
    {"sqr_minblep", {cedar::Opcode::OSC_SQR_MINBLEP, 1, 2, true,
                 {"freq", "phase", "trig", "", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "MinBLEP anti-aliased square wave"}},
    {"sine",    {cedar::Opcode::OSC_SIN,   1, 2, true,
                 {"freq", "phase", "trig", "", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "Sine wave oscillator"}},

    // PWM Oscillators (2 inputs: frequency, pwm amount + optional phase/trig)
    {"sqr_pwm", {cedar::Opcode::OSC_SQR_PWM, 2, 2, true,
                 {"freq", "pwm", "phase", "trig", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "Pulse width modulated square wave"}},
    {"saw_pwm", {cedar::Opcode::OSC_SAW_PWM, 2, 2, true,
                 {"freq", "pwm", "phase", "trig", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "Variable-width sawtooth oscillator"}},
    {"sqr_pwm_minblep", {cedar::Opcode::OSC_SQR_PWM_MINBLEP, 2, 2, true,
                 {"freq", "pwm", "phase", "trig", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "MinBLEP PWM square wave"}},

    // 4x Oversampled PWM (explicit, for when auto-detection isn't desired)
    {"sqr_pwm_4x", {cedar::Opcode::OSC_SQR_PWM_4X, 2, 2, true,
                 {"freq", "pwm", "phase", "trig", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "4x oversampled PWM square wave"}},
    {"saw_pwm_4x", {cedar::Opcode::OSC_SAW_PWM_4X, 2, 2, true,
                 {"freq", "pwm", "phase", "trig", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "4x oversampled PWM sawtooth"}},

    // Wavetable oscillator (Smooch).
    //   smooch("bank_name", freq)
    //   smooch("bank_name", freq, phase)
    //   smooch("bank_name", freq, phase, tablePos)
    // The bank must have been declared earlier via wt_load("bank_name", "path").
    // All three names (smooch / wt / wavetable) are special-handled in
    // codegen — the entries here exist so the analyzer recognizes the
    // identifier and reports unknown-function errors correctly. The PRD
    // also listed "wave" as an alias but that collides with a common
    // variable name (existing tests use `wave = ...`), so we ship three.
    {"smooch",    {cedar::Opcode::OSC_WAVETABLE, 2, 2, true,
                   {"bank", "freq", "phase", "tablePos", "", ""},
                   {0.0f, 0.0f, NAN, NAN, NAN},
                   "Wavetable oscillator (Smooch). smooch(\"bank\", freq, phase?, tablePos?)."}},
    {"wt",        {cedar::Opcode::OSC_WAVETABLE, 2, 2, true,
                   {"bank", "freq", "phase", "tablePos", "", ""},
                   {0.0f, 0.0f, NAN, NAN, NAN},
                   "Wavetable oscillator (alias for smooch)."}},
    {"wavetable", {cedar::Opcode::OSC_WAVETABLE, 2, 2, true,
                   {"bank", "freq", "phase", "tablePos", "", ""},
                   {0.0f, 0.0f, NAN, NAN, NAN},
                   "Wavetable oscillator (alias for smooch)."}},

    // wt_load — compile-time directive that registers a wavetable bank for
    // the host to load. Mirrors `soundfont` in shape: opcode = NOP because
    // there is no audio-time instruction; codegen special-handles it (see
    // codegen.cpp special_handlers table) to extract the string-literal args
    // into result.required_wavetables. The host loads the bank after compile.
    {"wt_load",   {cedar::Opcode::NOP, 2, 0, false,
                   {"name", "path", "", "", "", ""},
                   {NAN, NAN, NAN, NAN, NAN},
                   "Load a wavetable bank (compile-time): wt_load(\"name\", \"path\")."}},

    // samples — compile-time directive that declares a sample-bank URI for
    // the host to load. Mirrors wt_load in shape: opcode = NOP, special-cased
    // by codegen (see codegen.cpp special_handlers) to extract the string-
    // literal argument and append a UriRequest to `required_uris_`. The host
    // fetches each URI via the resolver before swapping bytecode.
    {"samples",   {cedar::Opcode::NOP, 1, 0, false,
                   {"uri", "", "", "", "", ""},
                   {NAN, NAN, NAN, NAN, NAN},
                   "Declare a sample-bank URI (compile-time): samples(\"github:user/repo\")."}},

    // Filters (signal, cutoff required; q optional with default 0.707)
    // Stereo-native (prd-stereo-native-opcodes Phase 4a): per-channel filter
    // memory inside one state struct; coefficient cache shared across L/R
    // because freq/q are mono control signals. Mono input auto-escalates.
    // SVF (State Variable Filter) - stable under modulation
    // Category B: dry/wet via slots 3-4 (default dry=0.0, wet=1.0 = back-compat)
    {"lp",      {cedar::Opcode::FILTER_SVF_LP, 2, 3, true,
                 {"in", "cut", "q", "dry", "wet", ""},
                 {0.707f, 0.0f, 1.0f, NAN, NAN},
                 "State-variable lowpass filter",
                 0, {}, {}, ChannelCount::Stereo, /*stereo_native=*/true}},
    {"hp",      {cedar::Opcode::FILTER_SVF_HP, 2, 3, true,
                 {"in", "cut", "q", "dry", "wet", ""},
                 {0.707f, 0.0f, 1.0f, NAN, NAN},
                 "State-variable highpass filter",
                 0, {}, {}, ChannelCount::Stereo, /*stereo_native=*/true}},
    {"bp",      {cedar::Opcode::FILTER_SVF_BP, 2, 3, true,
                 {"in", "cut", "q", "dry", "wet", ""},
                 {0.707f, 0.0f, 1.0f, NAN, NAN},
                 "State-variable bandpass filter",
                 0, {}, {}, ChannelCount::Stereo, /*stereo_native=*/true}},
    // Moog ladder filter (4-pole with resonance)
    // Optional: max_resonance (self-oscillation threshold), input_scale (preamp drive)
    // Category B: dry/wet via ExtendedParams (input slots full).
    {"moog",    {.opcode = cedar::Opcode::FILTER_MOOG,
                 .input_count = 2, .optional_count = 3, .requires_state = true,
                 .param_names = {"in", "cut", "res", "max_res", "input_scale", ""},
                 .defaults = {1.0f, 4.0f, 0.5f, NAN, NAN},
                 .description = "Moog 4-pole ladder filter with resonance",
                 .extended_param_count = 2,
                 .output_channels = ChannelCount::Stereo,
                 .stereo_native = true,
                 .extended_param_names = {"dry", "wet"},
                 .extended_defaults = {0.0f, 1.0f}}},
    // Diode ladder filter (TB-303 acid) - 5 inputs: in, cut, res, vt, fb_gain
    // Category B: dry/wet via ExtendedParams.
    {"diode",   {.opcode = cedar::Opcode::FILTER_DIODE,
                 .input_count = 2, .optional_count = 3, .requires_state = true,
                 .param_names = {"in", "cut", "res", "vt", "fb_gain", ""},
                 .defaults = {1.0f, 0.026f, 10.0f},
                 .description = "TB-303 style diode ladder filter",
                 .extended_param_count = 2,
                 .output_channels = ChannelCount::Stereo,
                 .stereo_native = true,
                 .extended_param_names = {"dry", "wet"},
                 .extended_defaults = {0.0f, 1.0f}}},
    // Formant filter (vowel morphing) - 5 inputs: in, vowel_a, vowel_b, morph, q
    // Category B: dry/wet via ExtendedParams.
    {"formant", {.opcode = cedar::Opcode::FILTER_FORMANT,
                 .input_count = 2, .optional_count = 3, .requires_state = true,
                 .param_names = {"in", "vowel_a", "vowel_b", "morph", "q", ""},
                 .defaults = {0.0f, 0.5f, 10.0f},
                 .description = "Vowel formant filter with morphing",
                 .extended_param_count = 2,
                 .output_channels = ChannelCount::Stereo,
                 .stereo_native = true,
                 .extended_param_names = {"dry", "wet"},
                 .extended_defaults = {0.0f, 1.0f}}},
    // Sallen-Key filter (MS-20 style) - 5 inputs: in, cut, res, mode, clip_threshold
    // Optional: clip_threshold (feedback clipping point)
    // Category B: dry/wet via ExtendedParams.
    {"sallenkey", {.opcode = cedar::Opcode::FILTER_SALLENKEY,
                   .input_count = 2, .optional_count = 3, .requires_state = true,
                   .param_names = {"in", "cut", "res", "mode", "clip_thresh", ""},
                   .defaults = {1.0f, 0.0f, 0.7f, NAN, NAN},
                   .description = "MS-20 style Sallen-Key filter",
                   .extended_param_count = 2,
                   .output_channels = ChannelCount::Stereo,
                   .stereo_native = true,
                   .extended_param_names = {"dry", "wet"},
                   .extended_defaults = {0.0f, 1.0f}}},

    // Envelopes
    {"adsr",    {cedar::Opcode::ENV_ADSR, 1, 4, true,
                 {"gate", "attack", "decay", "sustain", "release", ""},
                 {0.01f, 0.1f, 0.5f},
                 "Attack-decay-sustain-release envelope"}},
    {"ar",      {cedar::Opcode::ENV_AR, 1, 2, true,
                 {"trig", "attack", "release", "", "", ""},
                 {0.01f, 0.3f, NAN},
                 "Attack-release envelope (one-shot)"}},
    // env_follower — stereo-native (prd-stereo-native-opcodes Phase 4d).
    // Per-channel envelope levels in a dedicated EnvFollowerState.
    // Output width matches input: mono in → mono out (so the CV slots into
    // downstream mono parameter slots without E186); stereo in → per-channel
    // envelopes (per-channel sidechain). adsr/ar stay mono per PRD §3.2.
    {"env_follower", {cedar::Opcode::ENV_FOLLOWER, 1, 2, true,
                      {"in", "attack", "release", "", "", ""},
                      {0.01f, 0.1f, NAN},
                      "Amplitude envelope follower",
                      0, {}, {}, ChannelCount::Match, /*stereo_native=*/true}},

    // Samplers (stereo-native, prd-stereo-native-opcodes Phase 3): mono files
    // broadcast L=R; stereo files preserve channels; 3+ channel files keep the
    // first two and drop the rest.
    {"sample",  {cedar::Opcode::SAMPLE_PLAY, 3, 0, true,
                 {"trig", "pitch", "id", "", "", ""},
                 {NAN, NAN, NAN},
                 "Stereo-native one-shot sample playback. id accepts a sample "
                 "name (\"bd\", \"bd:3\", \"Bank/name:variant\") or numeric "
                 "sample-bank ID.",
                 0, {}, {}, ChannelCount::Stereo,
                 /*stereo_native=*/true}},
    {"sample_loop", {cedar::Opcode::SAMPLE_PLAY_LOOP, 3, 0, true,
                     {"gate", "pitch", "id", "", "", ""},
                     {NAN, NAN, NAN},
                     "Stereo-native looping sample playback. id accepts a "
                     "sample name or numeric sample-bank ID.",
                     0, {}, {}, ChannelCount::Stereo,
                     /*stereo_native=*/true}},
    // PRD prd-midi-input §7.1: requires_state flipped to true so the state
    // pool retains seq_state_id + preset_idx across blocks for the MIDI
    // event-driven path. Legacy pattern path is unaffected (its codegen
    // assigns one state_id per chord voice and lets the runtime alloc).
    {"soundfont", {.opcode = cedar::Opcode::NOP, .input_count = 2, .optional_count = 1, .requires_state = true,
                   .param_names = {"input", "file", "preset", "", "", ""},
                   .defaults = {NAN, NAN, NAN, NAN, NAN},
                   .description = "SoundFont playback: soundfont(pattern, \"file.sf2\", preset). Accepts midi() upstream for live polyphonic SF2 playback.",
                   .consumes_polyphonic_pattern = true}},
    // Single-voice SoundFont player. Custom codegen (handle_sf_voice_call)
    // resolves `file` (literal path or $soundfont_alias) and `preset` at
    // compile time, then emits one SF_VOICE driven by the freq/gate/vel
    // signals — so it slots into poly() like any other instrument.
    {"sf_voice", {.opcode = cedar::Opcode::SF_VOICE, .input_count = 5, .optional_count = 0, .requires_state = true,
                  .param_names = {"file", "preset", "freq", "gate", "vel", ""},
                  .defaults = {NAN, NAN, NAN, NAN, NAN},
                  .description = "Single-voice SoundFont player: sf_voice(file, preset, freq, gate, vel). Stereo output; designed as an instrument for poly().",
                  .output_channels = ChannelCount::Stereo, .stereo_native = true}},
    // PRD prd-midi-input §4.7: runtime MIDI event source. The `options` record
    // selects the source (default device / named device / .mid file) and tunes
    // the live filter. Special-cased in codegen as handle_midi_call.
    {"midi", {.opcode = cedar::Opcode::MIDI_QUERY,
              .input_count = 0, .optional_count = 1, .requires_state = true,
              .param_names = {"options", "", "", "", "", ""},
              .defaults = {NAN, NAN, NAN, NAN, NAN},
              .description = "MIDI event source. Bare midi() opens the default live device. "
                             "midi({device: name}) opens a named device; midi({file: path}) "
                             "plays a .mid file. Options: device, file, channel, loop, tempo.",
              .param_types = {ParamValueType::Record},
              .option_schemas = {OptionSchema{
                  /*param_index=*/0,
                  /*fields=*/{{
                      {"device",  OptionFieldType::String, "",          "Live device name (substring match against host port list)"},
                      {"file",    OptionFieldType::String, "",          "Path/URI of a .mid file (mutually exclusive with device:)"},
                      {"channel", OptionFieldType::Number, "0",         "Channel filter, 1-16 (0 = any)"},
                      {"loop",    OptionFieldType::Bool,   "false",     "Loop file playback at end-of-track"},
                      {"tempo",   OptionFieldType::Enum,   "\"follow\"", "Playback tempo policy (file mode only)", "follow,file"},
                  }},
                  /*field_count=*/5,
              }},
              .option_schema_count = 1}},

    // PRD prd-midi-input §4.8: route an incoming MIDI CC / pitch-bend / channel-
    // aftertouch to a named param() slot via EnvMap. Compile-time directive only
    // (no bytecode emitted); the host MIDI callback evaluates the route and
    // calls vm.set_param(name, value, slew_ms). Special-cased in codegen as
    // handle_midi_cc_call. Defaults: cc 0..1 range, pb -1..+1 range; 5 ms slew.
    {"midi_cc", {.opcode = cedar::Opcode::NOP,
                 .input_count = 1, .optional_count = 1, .requires_state = false,
                 .param_names = {"name", "options", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN, NAN, NAN},
                 .description = "Route an incoming MIDI CC / pitch-bend / channel-aftertouch to a "
                                "named param() slot via EnvMap. Compile-time only — exactly one of "
                                "cc:, pb:, at: must be set. Defaults: 0..1 range (cc/at), -1..+1 (pb).",
                 .param_types = {ParamValueType::String, ParamValueType::Record},
                 .option_schemas = {OptionSchema{
                     /*param_index=*/1,
                     /*fields=*/{{
                         {"cc",      OptionFieldType::Number, "-128",  "CC number 0..127 (omit when pb: or at: is set)"},
                         {"channel", OptionFieldType::Number, "0",     "Channel filter, 1-16 (0 = any)"},
                         {"pb",      OptionFieldType::Bool,   "false", "Route 14-bit pitch-bend instead of a CC"},
                         {"at",      OptionFieldType::Bool,   "false", "Route channel aftertouch instead of a CC"},
                         {"min",     OptionFieldType::Number, "0",     "Output range minimum (defaults to -1 when pb: is set)"},
                         {"max",     OptionFieldType::Number, "1",     "Output range maximum"},
                         {"slew",    OptionFieldType::Number, "5",     "EnvMap slew, in milliseconds"},
                     }},
                     /*field_count=*/7,
                 }},
                 .option_schema_count = 1}},

    // Delays — stereo-native (prd-stereo-native-opcodes Phase 4c).
    // Per-channel ring buffers; mono control inputs (time/fb/dry/wet) shared.
    // Category A (parallel-mix): defaults dry=1.0, wet=0.5.
    {"delay",   {cedar::Opcode::DELAY, 3, 2, true,
                 {"in", "time", "fb", "dry", "wet", ""},
                 {1.0f, 0.5f, NAN, NAN, NAN},
                 "Delay line (time in seconds, 0-10)",
                 0, {}, {}, ChannelCount::Stereo, /*stereo_native=*/true}},
    // Delay variants with different time units
    {"delay_ms",    {cedar::Opcode::DELAY, 3, 2, true,
                     {"in", "time_ms", "fb", "dry", "wet", ""},
                     {1.0f, 0.5f, NAN, NAN, NAN},
                     "Delay line (time in milliseconds, 0-10000)",
                     0, {}, {}, ChannelCount::Stereo, /*stereo_native=*/true, /*inst_rate=*/1}},
    {"delay_smp",   {cedar::Opcode::DELAY, 3, 2, true,
                     {"in", "time_smp", "fb", "dry", "wet", ""},
                     {1.0f, 0.5f, NAN, NAN, NAN},
                     "Delay line (time in samples, direct control)",
                     0, {}, {}, ChannelCount::Stereo, /*stereo_native=*/true, /*inst_rate=*/2}},
    // Tap delay with configurable feedback processing (handled specially by codegen)
    // tap_delay(in, time, fb, processor) where processor is a closure: (x) -> ...
    // The closure receives the delayed signal and its output is mixed back with feedback.
    // Category A (parallel-mix): defaults dry=1.0, wet=0.5.
    {"tap_delay", {cedar::Opcode::DELAY_TAP, 4, 2, true,
                   {"in", "time", "fb", "processor", "dry", "wet"},
                   {1.0f, 0.5f, NAN, NAN, NAN},
                   "Tap delay with feedback chain (time in seconds)"}},
    {"tap_delay_ms", {cedar::Opcode::DELAY_TAP, 4, 2, true,
                      {"in", "time_ms", "fb", "processor", "dry", "wet"},
                      {1.0f, 0.5f, NAN, NAN, NAN},
                      "Tap delay with feedback chain (time in milliseconds)"}},
    {"tap_delay_smp", {cedar::Opcode::DELAY_TAP, 4, 2, true,
                       {"in", "time_smp", "fb", "processor", "dry", "wet"},
                       {1.0f, 0.5f, NAN, NAN, NAN},
                       "Tap delay with feedback chain (time in samples)"}},

    // Reverbs (stateful - large delay networks)
    // All three reverbs are stereo-native (prd-stereo-native-opcodes Phases 0–2):
    // they produce a stereo output pair natively, auto-escalate mono input,
    // and read stereo primary input from inputs[0] (L) / inputs[0]+1 (R).
    // freeverb: room_scale (density factor), room_offset (decay baseline)
    // dry/wet via ExtendedParams (Category A: dry=1.0, wet=0.5)
    {"freeverb", {.opcode = cedar::Opcode::REVERB_FREEVERB,
                  .input_count = 1, .optional_count = 4, .requires_state = true,
                  .param_names = {"in", "room", "damp", "room_scale", "room_offset", ""},
                  .defaults = {0.5f, 0.5f, 0.28f, 0.7f, NAN},
                  .description = "Freeverb algorithmic reverb",
                  .extended_param_count = 2,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"dry", "wet"},
                  .extended_defaults = {1.0f, 0.5f}}},
    // dattorro: input_diffusion (input smoothing), decay_diffusion (tail smoothing).
    // damping/mod_depth were bit-packed in inst.rate and lfo_rate was hardcoded
    // before prd-extended-params-migration; now ExtendedParams<5> alongside
    // dry/wet (Category A: dry=1.0, wet=0.5).
    {"dattorro", {.opcode = cedar::Opcode::REVERB_DATTORRO,
                  .input_count = 1, .optional_count = 4, .requires_state = true,
                  .param_names = {"in", "decay", "predelay", "in_diff", "dec_diff", ""},
                  .defaults = {0.7f, 20.0f, 0.75f, 0.625f, NAN},
                  .description = "Dattorro plate reverb algorithm",
                  .extended_param_count = 5,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"damping", "mod_depth", "lfo_rate", "dry", "wet"},
                  .extended_defaults = {0.0f, 0.0f, 0.5f, 1.0f, 0.5f}}},
    // fdn has free input slots; dry/wet via slots 3-4 (Category A: dry=1.0, wet=0.5)
    {"fdn",      {cedar::Opcode::REVERB_FDN, 1, 4, true,
                  {"in", "decay", "damp", "dry", "wet", ""},
                  {0.8f, 0.3f, 1.0f, 0.5f, NAN},
                  "Feedback delay network reverb",
                  0, {}, {}, ChannelCount::Stereo, /*stereo_native=*/true}},

    // Modulation Effects (stateful - delay lines with LFOs)
    // All three are stereo-native (prd-stereo-native-opcodes Phase 3): mono
    // input auto-escalates to L=R; stereo input drives per-channel processing.
    // R lane reads the shared master LFO at the user-tunable `lfo_phase`
    // offset (turns, default 0.25 = 90°) for L/R decorrelation.
    // chorus: base_delay (ms), depth_range (ms), lfo_phase (turns)
    // dry/wet appended to ExtendedParams (Category A: dry=1.0, wet=0.5).
    {"chorus",   {.opcode = cedar::Opcode::EFFECT_CHORUS,
                  .input_count = 1, .optional_count = 4, .requires_state = true,
                  .param_names = {"in", "rate", "depth", "base_delay", "depth_range", ""},
                  .defaults = {0.5f, 0.5f, 20.0f, 10.0f, NAN},
                  .description = "Stereo-native chorus (mono input widens; stereo input "
                                 "processes per channel). lfo_phase tunes the R-LFO offset.",
                  .extended_param_count = 3,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"lfo_phase", "dry", "wet"},
                  .extended_defaults = {0.25f, 1.0f, 0.5f}}},
    // flanger: min_delay (ms), max_delay (ms). feedback was bit-packed in
    // inst.rate before prd-extended-params-migration; now ExtendedParams<5>
    // alongside lfo_phase + dry/wet (Category A: dry=1.0, wet=0.5).
    {"flanger",  {.opcode = cedar::Opcode::EFFECT_FLANGER,
                  .input_count = 1, .optional_count = 4, .requires_state = true,
                  .param_names = {"in", "rate", "depth", "min_delay", "max_delay", ""},
                  .defaults = {1.0f, 0.7f, 0.1f, 10.0f, NAN},
                  .description = "Stereo-native flanger. lfo_phase tunes the R-LFO offset.",
                  .extended_param_count = 4,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"feedback", "lfo_phase", "dry", "wet"},
                  .extended_defaults = {-0.99f, 0.25f, 1.0f, 0.5f}}},
    // phaser: min_freq, max_freq, plus 5 extended params (feedback, stages,
    // lfo_phase, dry, wet). Feedback/stages used to be packed into inst.rate;
    // they are now full first-class extended params (PRD prd-extended-params §6b).
    // dry/wet appended (Category A: dry=1.0, wet=0.5).
    {"phaser",   {.opcode = cedar::Opcode::EFFECT_PHASER,
                  .input_count = 1, .optional_count = 4, .requires_state = true,
                  .param_names = {"in", "rate", "depth", "min_freq", "max_freq", ""},
                  .defaults = {0.5f, 0.8f, 200.0f, 4000.0f, NAN},
                  .description = "Stereo-native multi-stage phaser. "
                                 "feedback (0-0.99), stages (2-12), lfo_phase (turns).",
                  .extended_param_count = 5,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"feedback", "stages", "lfo_phase", "dry", "wet"},
                  .extended_defaults = {0.5f, 4.0f, 0.25f, 1.0f, 0.5f}}},
    // comb: dry/wet via input slots 3-4 (Category A: dry=1.0, wet=0.5).
    // damping was packed in inst.rate before prd-extended-params-migration;
    // now ExtendedParams<1>.
    {"comb",     {.opcode = cedar::Opcode::EFFECT_COMB,
                  .input_count = 3, .optional_count = 2, .requires_state = true,
                  .param_names = {"in", "time", "fb", "dry", "wet", ""},
                  .defaults = {1.0f, 0.5f, NAN, NAN, NAN},
                  .description = "Comb filter (resonant delay). damping rolls off the tail.",
                  .extended_param_count = 1,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"damping"},
                  .extended_defaults = {0.0f}}},

    // Distortion — all stereo-native (prd-stereo-native-opcodes Phase 4b).
    // Per-channel state arrays inside one state struct; runtime params (drive,
    // threshold, frequency, etc.) are mono control signals shared across L/R.
    // Note: tanh(x) is now a pure math function. Use saturate(in, drive) for distortion.
    // Category B: dry/wet via slots 2-3 (defaults dry=0.0, wet=1.0 = back-compat).
    {"saturate", {cedar::Opcode::DISTORT_TANH, 1, 3, false,
                  {"in", "drive", "dry", "wet", "", ""},
                  {2.0f, 0.0f, 1.0f, NAN, NAN},
                  "Soft saturation (tanh) distortion",
                  0, {}, {}, ChannelCount::Stereo, /*stereo_native=*/true}},
    {"softclip", {cedar::Opcode::DISTORT_SOFT, 1, 3, false,
                  {"in", "thresh", "dry", "wet", "", ""},
                  {0.5f, 0.0f, 1.0f, NAN, NAN},
                  "Soft clipper distortion",
                  0, {}, {}, ChannelCount::Stereo, /*stereo_native=*/true}},
    // dry/wet via slots 3-4.
    {"bitcrush", {cedar::Opcode::DISTORT_BITCRUSH, 1, 4, true,
                  {"in", "bits", "rate", "dry", "wet", ""},
                  {8.0f, 0.5f, 0.0f, 1.0f, NAN},
                  "Bit depth and sample rate reducer",
                  0, {}, {}, ChannelCount::Stereo, /*stereo_native=*/true}},
    // fold: routed via ExtendedParams (opcode reads inputs[2]=symmetry; keep
    // BuiltinInfo legacy single-optional and add dry/wet as Extended slots).
    {"fold",     {.opcode = cedar::Opcode::DISTORT_FOLD,
                  .input_count = 1, .optional_count = 1, .requires_state = false,
                  .param_names = {"in", "thresh", "", "", "", ""},
                  .defaults = {0.5f, NAN, NAN},
                  .description = "Wavefolding distortion",
                  .extended_param_count = 2,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"dry", "wet"},
                  .extended_defaults = {0.0f, 1.0f}}},
    {"tube",     {cedar::Opcode::DISTORT_TUBE, 1, 4, true,
                  {"in", "drive", "bias", "dry", "wet", ""},
                  {5.0f, 0.1f, 0.0f, 1.0f, NAN},
                  "Tube amp emulation with bias",
                  0, {}, {}, ChannelCount::Stereo, /*stereo_native=*/true}},
    {"smooth",   {cedar::Opcode::DISTORT_SMOOTH, 1, 3, true,
                  {"in", "drive", "dry", "wet", "", ""},
                  {5.0f, 0.0f, 1.0f, NAN, NAN},
                  "ADAA alias-free saturation",
                  0, {}, {}, ChannelCount::Stereo, /*stereo_native=*/true}},
    // tape: soft_threshold (saturation onset), warmth_scale (HF rolloff amount)
    // dry/wet via ExtendedParams (input slots full).
    {"tape",     {.opcode = cedar::Opcode::DISTORT_TAPE,
                  .input_count = 1, .optional_count = 4, .requires_state = true,
                  .param_names = {"in", "drive", "warmth", "soft_thresh", "warmth_scale", ""},
                  .defaults = {3.0f, 0.3f, 0.5f, 0.7f, NAN},
                  .description = "Tape saturation with warmth",
                  .extended_param_count = 2,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"dry", "wet"},
                  .extended_defaults = {0.0f, 1.0f}}},
    // xfmr: bass_freq (bass extraction cutoff Hz)
    // dry/wet via ExtendedParams.
    {"xfmr",     {.opcode = cedar::Opcode::DISTORT_XFMR,
                  .input_count = 1, .optional_count = 3, .requires_state = true,
                  .param_names = {"in", "drive", "bass", "bass_freq", "", ""},
                  .defaults = {3.0f, 5.0f, 60.0f, NAN, NAN},
                  .description = "Transformer saturation with bass boost",
                  .extended_param_count = 2,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"dry", "wet"},
                  .extended_defaults = {0.0f, 1.0f}}},
    // excite: harmonic_odd (odd harmonic mix), harmonic_even (even harmonic mix)
    // dry/wet via ExtendedParams.
    {"excite",   {.opcode = cedar::Opcode::DISTORT_EXCITE,
                  .input_count = 1, .optional_count = 4, .requires_state = true,
                  .param_names = {"in", "amount", "freq", "harm_odd", "harm_even", ""},
                  .defaults = {0.5f, 3000.0f, 0.4f, 0.6f, NAN},
                  .description = "Aural exciter (harmonic enhancer)",
                  .extended_param_count = 2,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"dry", "wet"},
                  .extended_defaults = {0.0f, 1.0f}}},

    // Dynamics — stereo-native (prd-stereo-native-opcodes Phase 4d).
    // Per-channel envelope/gain state; coefficient cache shared.
    // Dynamics: Category B (transform). dry/wet defaults dry=0.0, wet=1.0
    // (back-compat). Set dry>0 for parallel/NY compression.
    // comp: attack/release (ms) via ExtendedParams<2> — were bit-packed in
    // inst.rate before prd-extended-params-migration. dry/wet stay in slots 3-4.
    {"comp",     {.opcode = cedar::Opcode::DYNAMICS_COMP,
                  .input_count = 1, .optional_count = 4, .requires_state = true,
                  .param_names = {"in", "thresh", "ratio", "dry", "wet", ""},
                  .defaults = {-12.0f, 4.0f, 0.0f, 1.0f, NAN},
                  .description = "Dynamic range compressor. attack/release in ms.",
                  .extended_param_count = 2,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"attack", "release"},
                  .extended_defaults = {0.1f, 10.0f}}},
    // limiter: lookahead (ms, 0 = off) via ExtendedParams<1> — replaced the
    // inst.rate boolean toggle. dry/wet stay in slots 3-4.
    {"limiter",  {.opcode = cedar::Opcode::DYNAMICS_LIMITER,
                  .input_count = 1, .optional_count = 4, .requires_state = true,
                  .param_names = {"in", "ceiling", "release", "dry", "wet", ""},
                  .defaults = {-0.1f, 0.1f, 0.0f, 1.0f, NAN},
                  .description = "Peak limiter. lookahead in ms (0 = off).",
                  .extended_param_count = 1,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"lookahead"},
                  .extended_defaults = {0.0f}}},
    // gate: hysteresis (dB open/close diff), close_time (ms fade-out).
    // attack/hold/release (ms) were bit-packed in inst.rate before
    // prd-extended-params-migration; now ExtendedParams<5> alongside dry/wet
    // (input slots full).
    {"gate",     {.opcode = cedar::Opcode::DYNAMICS_GATE,
                  .input_count = 1, .optional_count = 4, .requires_state = true,
                  .param_names = {"in", "thresh", "range", "hyst", "close_time", ""},
                  .defaults = {-40.0f, -40.0f, 6.0f, 5.0f, NAN},
                  .description = "Noise gate with hysteresis. attack/hold/release in ms.",
                  .extended_param_count = 5,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"attack", "hold", "release", "dry", "wet"},
                  .extended_defaults = {0.1f, 0.0f, 10.0f, 0.0f, 1.0f}}},

    // Arithmetic (2 inputs, stateless) - from binary operator desugaring
    {"add",     {cedar::Opcode::ADD, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Add two signals"}},
    {"sub",     {cedar::Opcode::SUB, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Subtract two signals"}},
    {"mul",     {cedar::Opcode::MUL, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Multiply two signals"}},
    {"div",     {cedar::Opcode::DIV, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Divide two signals"}},
    {"pow",     {cedar::Opcode::POW, 2, 0, false,
                 {"base", "exp", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Raise base to exponent power"}},

    // Math unary (1 input)
    {"neg",     {cedar::Opcode::NEG,   1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Negate signal (flip sign)"}},
    {"abs",     {cedar::Opcode::ABS,   1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Absolute value"}},
    {"sqrt",    {cedar::Opcode::SQRT,  1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Square root"}},
    {"log",     {cedar::Opcode::LOG,   1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Natural logarithm"}},
    {"exp",     {cedar::Opcode::EXP,   1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Exponential (e^x)"}},
    {"floor",   {cedar::Opcode::FLOOR, 1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Round down to integer"}},
    {"ceil",    {cedar::Opcode::CEIL,  1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Round up to integer"}},

    // Math - Trigonometric (radians)
    // NOTE: sin(x) is the mathematical sine function, NOT a sine oscillator!
    // Use osc("sin", freq) for a sine wave oscillator.
    {"sin",     {cedar::Opcode::MATH_SIN,  1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Sine function (radians)"}},
    {"cos",     {cedar::Opcode::MATH_COS,  1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Cosine function (radians)"}},
    {"tan",     {cedar::Opcode::MATH_TAN,  1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Tangent function (radians)"}},
    {"asin",    {cedar::Opcode::MATH_ASIN, 1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Inverse sine (arc sine)"}},
    {"acos",    {cedar::Opcode::MATH_ACOS, 1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Inverse cosine (arc cosine)"}},
    {"atan",    {cedar::Opcode::MATH_ATAN, 1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Inverse tangent (arc tangent)"}},
    {"atan2",   {cedar::Opcode::MATH_ATAN2, 2, 0, false,
                 {"y", "x", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Two-argument arc tangent"}},

    // Math - Hyperbolic
    {"sinh",    {cedar::Opcode::MATH_SINH, 1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Hyperbolic sine"}},
    {"cosh",    {cedar::Opcode::MATH_COSH, 1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Hyperbolic cosine"}},
    // Pure mathematical tanh - useful for waveshaping: tanh(signal * drive)
    // For convenience distortion with drive parameter, use the tanh effect
    {"tanh",    {cedar::Opcode::MATH_TANH, 1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Hyperbolic tangent (soft clipper)"}},

    // Math binary (2 inputs)
    // min/max can be binary or unary (reduction over array)
    {"min",     {cedar::Opcode::MIN, 1, 1, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Minimum: min(a, b) or min(array)"}},
    {"max",     {cedar::Opcode::MAX, 1, 1, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Maximum: max(a, b) or max(array)"}},

    // Math ternary (3 inputs)
    {"clamp",   {cedar::Opcode::CLAMP, 3, 0, false,
                 {"x", "lo", "hi", "", "", ""},
                 {NAN, NAN, NAN},
                 "Clamp value between lo and hi"}},
    {"wrap",    {cedar::Opcode::WRAP,  3, 0, false,
                 {"x", "lo", "hi", "", "", ""},
                 {NAN, NAN, NAN},
                 "Wrap value between lo and hi"}},

    // Conditionals - Signal Selection
    {"select",  {cedar::Opcode::SELECT, 3, 0, false,
                 {"cond", "a", "b", "", "", ""},
                 {NAN, NAN, NAN},
                 "Select between signals: (cond > 0) ? a : b"}},

    // Conditionals - Block-rate bypass. opcode = NOP: handle_when_call in
    // codegen_control_flow.cpp lowers this to SKIP_IF_* opcodes before the
    // generic emission path runs. Entry exists for arity-checking + autocomplete.
    {"when",    {cedar::Opcode::NOP, 3, 0, false,
                 {"cond", "true_branch", "false_branch", "", "", ""},
                 {NAN, NAN, NAN},
                 "Block-rate conditional bypass: runs only the taken branch"}},

    // Conditionals - Comparisons (return 0.0 or 1.0)
    {"gt",      {cedar::Opcode::CMP_GT, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Greater than: (a > b) ? 1 : 0"}},
    {"lt",      {cedar::Opcode::CMP_LT, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Less than: (a < b) ? 1 : 0"}},
    {"gte",     {cedar::Opcode::CMP_GTE, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Greater or equal: (a >= b) ? 1 : 0"}},
    {"lte",     {cedar::Opcode::CMP_LTE, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Less or equal: (a <= b) ? 1 : 0"}},
    {"eq",      {cedar::Opcode::CMP_EQ, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Approximate equality: |a - b| < epsilon ? 1 : 0"}},
    {"neq",     {cedar::Opcode::CMP_NEQ, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Not equal: |a - b| >= epsilon ? 1 : 0"}},

    // Conditionals - Logical Operations
    {"band",    {cedar::Opcode::LOGIC_AND, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Logical AND: (a > 0 && b > 0) ? 1 : 0"}},
    {"bor",     {cedar::Opcode::LOGIC_OR, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Logical OR: (a > 0 || b > 0) ? 1 : 0"}},
    {"bnot",    {cedar::Opcode::LOGIC_NOT, 1, 0, false,
                 {"a", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Logical NOT: (a > 0) ? 0 : 1"}},

    // Live audio input — produces a stereo signal sourced from the host
    // (microphone, tab/system audio, uploaded file, etc.). The optional source
    // string is a compile-time literal: "mic" | "tab" | "file:NAME". Codegen
    // emits an INPUT instruction and forwards the source string to the host
    // via the required-input-source table. Output is always Stereo.
    {"in",      {cedar::Opcode::INPUT, 0, 1, false,
                 {"source", "", "", "", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "Live audio input. Optional source: 'mic' (default), 'tab', 'file:NAME'.",
                 0,
                 {ParamValueType::String, {}, {}, {}, {}, {}},
                 {}, ChannelCount::Stereo}},

    // Utility
    {"noise",   {cedar::Opcode::NOISE, 0, 3, true,
                 {"freq", "trig", "seed", "", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "Noise generator (freq=0: white, freq>0: sample-and-hold)"}},
    {"mtof",    {cedar::Opcode::MTOF,  1, 0, false,
                 {"note", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "MIDI note number to frequency (Hz)"}},
    {"dc",      {cedar::Opcode::DC,    1, 0, false,
                 {"offset", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "DC offset (constant value)"}},
    {"slew",    {cedar::Opcode::SLEW,  2, 0, true,
                 {"target", "rate", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Slew rate limiter (portamento) — output width matches input",
                 0, {}, {}, ChannelCount::Match, /*stereo_native=*/true}},
    // Time-based interpolators — share Opcode::INTERP_TIME, dispatched by
    // inst_rate (0..3). Stereo-native opcodes with Match output: mono target
    // → mono output (slots into downstream mono params like saw(freq=...)
    // without E186); stereo target → independent per-channel ramps.
    {"interp",          {cedar::Opcode::INTERP_TIME, 2, 0, true,
                         {"target", "time", "", "", "", ""},
                         {NAN, NAN, NAN},
                         "Time-based interpolator (linear) — output width matches input",
                         0, {}, {}, ChannelCount::Match, /*stereo_native=*/true, /*inst_rate=*/0}},
    {"interp_ease_in",  {cedar::Opcode::INTERP_TIME, 2, 0, true,
                         {"target", "time", "", "", "", ""},
                         {NAN, NAN, NAN},
                         "Time-based interpolator (ease-in, t²) — output width matches input",
                         0, {}, {}, ChannelCount::Match, /*stereo_native=*/true, /*inst_rate=*/1}},
    {"interp_ease_out", {cedar::Opcode::INTERP_TIME, 2, 0, true,
                         {"target", "time", "", "", "", ""},
                         {NAN, NAN, NAN},
                         "Time-based interpolator (ease-out, 1-(1-t)²) — output width matches input",
                         0, {}, {}, ChannelCount::Match, /*stereo_native=*/true, /*inst_rate=*/2}},
    {"interp_cos",      {cedar::Opcode::INTERP_TIME, 2, 0, true,
                         {"target", "time", "", "", "", ""},
                         {NAN, NAN, NAN},
                         "Time-based interpolator (cosine S-curve) — output width matches input",
                         0, {}, {}, ChannelCount::Match, /*stereo_native=*/true, /*inst_rate=*/3}},
    // Edge primitives — share Opcode::EDGE_OP, dispatched by inst_rate (0..3).
    // Stereo-native opcodes with Match output: mono primary → mono output
    // (lets the result feed mono-only slots downstream); stereo primary →
    // independent per-channel hold/edge/counter state.
    {"sah",      {cedar::Opcode::EDGE_OP, 2, 0, true,
                 {"in", "trig", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Sample and hold — output width matches input",
                 0, {}, {}, ChannelCount::Match, /*stereo_native=*/true, /*inst_rate=*/0}},
    {"gateup",   {cedar::Opcode::EDGE_OP, 1, 0, true,
                 {"sig", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "1.0 on rising edge of sig — output width matches input",
                 0, {}, {}, ChannelCount::Match, /*stereo_native=*/true, /*inst_rate=*/1}},
    {"gatedown", {cedar::Opcode::EDGE_OP, 1, 0, true,
                 {"sig", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "1.0 on falling edge of sig — output width matches input",
                 0, {}, {}, ChannelCount::Match, /*stereo_native=*/true, /*inst_rate=*/2}},
    {"counter",  {cedar::Opcode::EDGE_OP, 1, 2, true,
                 {"trig", "reset", "start", "", "", ""},
                 {NAN, NAN},
                 "Increment on rising edge of trig; reset to start (or 0) on rising edge of reset — output width matches input",
                 0, {}, {}, ChannelCount::Match, /*stereo_native=*/true, /*inst_rate=*/3}},

    // Output (1 required for mono, 2 for stereo)
    {"out",     {cedar::Opcode::OUTPUT, 1, 1, false,
                 {"L", "R", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Audio output (mono or stereo)", 0,
                 {ParamValueType::Signal, ParamValueType::Signal}}},

    // Stereo Operations (handled specially by codegen for stereo signal propagation)
    // stereo(mono) creates stereo from mono by duplicating to both channels
    // stereo(left, right) creates stereo from two separate signals
    {"stereo",  {cedar::Opcode::NOP, 1, 1, false,
                 {"L", "R", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Create stereo signal from mono or L/R pair",
                 0, {}, {}, ChannelCount::Stereo}},
    // Extract left channel from stereo signal
    {"left",    {cedar::Opcode::NOP, 1, 0, false,
                 {"stereo", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Extract left channel from stereo signal",
                 0, {}, {ChannelCount::Stereo}, ChannelCount::Mono}},
    // Extract right channel from stereo signal
    {"right",   {cedar::Opcode::NOP, 1, 0, false,
                 {"stereo", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Extract right channel from stereo signal",
                 0, {}, {ChannelCount::Stereo}, ChannelCount::Mono}},
    // Pan signal to stereo position. Same-arity overload: mono input emits PAN
    // (constant-power mono→stereo); stereo input emits PAN_STEREO (equal-power
    // DAW-style balance). Dispatch handled by handle_pan_call in codegen_stereo.cpp.
    {"pan",     {cedar::Opcode::PAN, 2, 0, false,
                 {"signal", "pos", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Pan signal at position (-1=L, 0=center, 1=R) — mono→stereo, or DAW-style stereo balance",
                 0, {}, {}, ChannelCount::Stereo}},
    // Stereo width control (0=mono, 1=normal, >1=wide)
    // Convenience: width(stereo, amount) or explicit: width(L, R, amount)
    {"width",   {cedar::Opcode::WIDTH, 2, 0, false,
                 {"stereo/L", "amount/R", "amount?", "", "", ""},
                 {NAN, NAN, NAN},
                 "Stereo width via M/S (0=mono, 1=normal, >1=wide) — width(stereo, amt) or width(L, R, amt)",
                 0, {}, {ChannelCount::Stereo}, ChannelCount::Stereo}},
    // Mid/side encoding
    // Convenience: ms_encode(stereo) or explicit: ms_encode(L, R)
    {"ms_encode", {cedar::Opcode::MS_ENCODE, 1, 0, false,
                   {"stereo/L", "R?", "", "", "", ""},
                   {NAN, NAN, NAN},
                   "Encode stereo to mid/side — ms_encode(stereo) or ms_encode(L, R)",
                   0, {}, {ChannelCount::Stereo}, ChannelCount::Stereo}},
    // Mid/side decoding
    // Convenience: ms_decode(ms) or explicit: ms_decode(M, S)
    {"ms_decode", {cedar::Opcode::MS_DECODE, 1, 0, false,
                   {"ms/M", "S?", "", "", "", ""},
                   {NAN, NAN, NAN},
                   "Decode mid/side to stereo — ms_decode(ms) or ms_decode(M, S)",
                   0, {}, {ChannelCount::Stereo}, ChannelCount::Stereo}},
    // True stereo ping-pong delay
    // Convenience: pingpong(stereo, time, fb) or explicit: pingpong(L, R, time, fb, width?)
    // dry/wet ride in ExtendedParams<2> because all 5 input slots are used (L, R, time, fb, width).
    // optional_count=2 keeps find_param("dry")/("wet") at total_params() + ext_idx = {5, 6}, away
    // from the width slot — both width defaults are 1.0f and the custom handler picks the right one.
    {"pingpong", {.opcode = cedar::Opcode::DELAY_PINGPONG,
                  .input_count = 3, .optional_count = 2, .requires_state = true,
                  .param_names = {"stereo/L", "time/R", "fb/time", "fb?", "width?", ""},
                  .defaults = {1.0f, 1.0f, NAN, NAN, NAN},
                  .description = "Ping-pong stereo delay — pingpong(stereo, t, fb, width?) or pingpong(L, R, t, fb, width?)",
                  .extended_param_count = 2,
                  .input_channels = {ChannelCount::Stereo},
                  .output_channels = ChannelCount::Stereo,
                  .extended_param_names = {"dry", "wet"},
                  .extended_defaults = {1.0f, 0.5f}}},

    // Timing/Sequencing
    {"clock",   {cedar::Opcode::CLOCK,   0, 0, false,
                 {"", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Global clock signal"}},
    {"lfo",     {cedar::Opcode::LFO,     1, 1, true,
                 {"rate", "duty", "", "", "", ""},
                 {0.5f, NAN, NAN},
                 "Low frequency oscillator (-1 to 1)"}},
    {"trigger", {cedar::Opcode::TRIGGER, 1, 0, true,
                 {"div", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Clock divider trigger"}},
    {"euclid",  {cedar::Opcode::EUCLID,  2, 1, true,
                 {"hits", "steps", "rot", "", "", ""},
                 {0.0f, NAN, NAN},
                 "Euclidean rhythm generator"}},
    {"timeline", {cedar::Opcode::TIMELINE, 0, 1, true,
                 {"pattern", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Breakpoint automation timeline"}},

    // Compile-time array functions (handled specially by codegen)
    {"len",     {cedar::Opcode::PUSH_CONST, 1, 0, false,
                 {"arr", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Array length (compile-time for static arrays, runtime for dynamic arrays)"}},

    // Pattern-event chord accessors (PRD prd-pattern-event-arrays). Both are
    // dispatched by name in codegen.cpp's special_handlers map; the opcode
    // here is a placeholder so the analyzer accepts the call. They take a
    // Pattern and return a DynArray of the active event's chord notes.
    // Not reserved — a user binding shadows them, exactly like len/map.
    {"notes",   {cedar::Opcode::SEQPAT_VALUES, 1, 0, true,
                 {"pattern", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Active pattern event's chord notes as a dynamic array of MIDI numbers"}},
    {"freqs",   {cedar::Opcode::SEQPAT_VALUES, 1, 0, true,
                 {"pattern", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Active pattern event's chord notes as a dynamic array of frequencies (Hz)"}},

    // User state cells (Phase 3 of userspace-state PRD). All three are
    // dispatched by name in codegen.cpp's special_handlers map; the opcode
    // here is just a placeholder so the analyzer accepts the call. Names
    // are reserved at the parser level — users cannot rebind them.
    {"state",   {cedar::Opcode::STATE_OP, 1, 0, true,
                 {"init", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Allocate a persistent state cell with the given initial value"}},
    {"get",     {cedar::Opcode::STATE_OP, 1, 0, false,
                 {"cell", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Read the current value of a state cell"}},
    {"set",     {cedar::Opcode::STATE_OP, 2, 0, false,
                 {"cell", "value", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Write a value to a state cell, returns the new value"}},

    // Multi-buffer array primitives for polyphony (handled specially by codegen)
    // These enable user-defined polyphony: fn poly(c, f) = sum(map(c, f)) / len(c)
    {"map",     {cedar::Opcode::NOP, 2, 0, false,
                 {"array", "fn", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Apply function to each element: map(array, (val) -> ...) or map(array, (val, idx) -> ...)"}},
    // sum() is variadic and handled specially in the analyzer + codegen
    // (arity >= 1, not bounded by input_count/optional_count below).
    {"sum",     {cedar::Opcode::NOP, 1, 0, false,
                 {"array", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Sum signals per-channel, preserving stereo: sum(array) or sum(a, b, ...)"}},
    {"reduce",  {cedar::Opcode::NOP, 3, 0, false,
                 {"array", "fn", "init", "", "", ""},
                 {NAN, NAN, NAN},
                 "Reduce array with binary function and initial value"}},
    {"zipWith", {cedar::Opcode::NOP, 3, 0, false,
                 {"a", "b", "fn", "", "", ""},
                 {NAN, NAN, NAN},
                 "Combine two arrays element-wise with binary function"}},
    {"zip",     {cedar::Opcode::NOP, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Interleave two arrays: [a0, b0, a1, b1, ...]"}},
    {"take",    {cedar::Opcode::NOP, 2, 0, false,
                 {"n", "array", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Take first n elements from array"}},
    {"drop",    {cedar::Opcode::NOP, 2, 0, false,
                 {"n", "array", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Drop first n elements from array"}},
    {"reverse", {cedar::Opcode::NOP, 1, 0, false,
                 {"array", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Reverse array order"}},
    {"range",   {cedar::Opcode::NOP, 2, 1, false,
                 {"start", "end", "step", "", "", ""},
                 {NAN, NAN, 1.0f},
                 "Generate array [start, start±step, ...] toward end (exclusive); direction follows start/end, step defaults to 1"}},
    {"repeat",  {cedar::Opcode::NOP, 2, 0, false,
                 {"value", "n", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Repeat value n times: [v, v, ..., v]"}},

    // Array reduction operations
    {"mean",    {cedar::Opcode::NOP, 1, 0, false,
                 {"array", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Average of array elements"}},

    // Array transformation operations
    {"rotate",    {cedar::Opcode::NOP, 2, 0, false,
                   {"array", "n", "", "", "", ""},
                   {NAN, NAN, NAN},
                   "Rotate array elements by n positions"}},
    {"shuffle",   {cedar::Opcode::NOP, 1, 1, false,
                   {"array", "seed", "", "", "", ""},
                   {NAN, NAN, NAN},
                   "Deterministic random permutation of array; optional seed mixes into the path-derived seed"}},
    {"sort",      {cedar::Opcode::NOP, 1, 1, false,
                   {"array", "reverse", "", "", "", ""},
                   {NAN, 0.0f, NAN},
                   "Sort array in ascending order; reverse=true sorts descending"}},
    {"normalize", {cedar::Opcode::NOP, 1, 2, false,
                   {"array", "lo", "hi", "", "", ""},
                   {NAN, 0.0f, 1.0f},
                   "Scale array to [lo, hi] range (defaults to [0, 1])"}},
    {"scale",     {cedar::Opcode::NOP, 3, 0, false,
                   {"array", "lo", "hi", "", "", ""},
                   {NAN, NAN, NAN},
                   "Scale array to [lo, hi] range"}},

    // Polyphony control. `release` (seconds) holds the voice in the mix
    // for that long past note-off so the instrument's own ADSR can finish
    // its release tail instead of being silenced by gate-multiplied mixing
    // — see PRD prd-midi-input §7.2. Default 0 = legacy behavior.
    {"poly",      {cedar::Opcode::NOP, 2, 2, true,
                   {"input", "instrument", "voices", "release", "", ""},
                   {NAN, NAN, 64.0f, 0.0f, NAN},
                   "Polyphonic voice manager: allocates voices driven by a pattern input. Default 64 voices, max 128. `release` (seconds) extends per-voice mix tail past note-off.",
                   0, {}, {}, ChannelCount::Stereo, true}},
    // Higher-order DSL (PRD prd-runtime-functions-control-flow L3 §7.5). All
    // three compile to FOREACH_EVENT + a subprogram block; the lambda's
    // per-event parameter is an event record (n.freq / n.vel / n.gate / ...).
    // each_voice(input, lambda): runs the lambda once per event, mixing all
    // iterations into a stereo signal.
    {"each_voice", {cedar::Opcode::NOP, 2, 0, true,
                    {"input", "lambda", "", "", "", ""},
                    {NAN, NAN, NAN},
                    "Higher-order per-event instrument: each_voice(input, (n) -> ...) runs the lambda once per pattern event and mixes the outputs.",
                    0, {}, {}, ChannelCount::Stereo, true}},
    // each(input, lambda): side-effecting per-event sink — the lambda body
    // calls out() itself; iterations accumulate into the global bus.
    {"each", {cedar::Opcode::NOP, 2, 0, true,
              {"input", "lambda", "", "", "", ""},
              {NAN, NAN, NAN},
              "Higher-order per-event sink: each(input, (n) -> ...) runs the lambda once per event for side effects (the body calls out() itself).",
              0, {}, {}, ChannelCount::Mono, true}},
    // Dual-role builtin: mono(stereo_signal) downmixes stereo→mono via (L+R)*0.5,
    // while mono(instrument) is the monophonic voice manager. The codegen
    // dispatcher routes based on argument type (see handle_mono_call).
    // The 2-arg voice-manager form `mono(input, instrument)` is also accepted
    // but the param_names below describe the 1-arg form; mixed positional +
    // named-arg use (e.g. `mono(synth, release: 0.3)`) is brittle for this
    // dual-role builtin — prefer fully-positional `mono(input, synth, 0.3)`.
    {"mono",      {cedar::Opcode::MONO_DOWNMIX, 1, 2, false,
                   {"signal_or_instrument", "input", "release", "", "", ""},
                   {NAN, NAN, 0.0f, NAN, NAN},
                   "Stereo-to-mono downmix (L+R)*0.5, or monophonic voice manager. `release` (seconds) extends mix tail past note-off in voice-manager mode.",
                   0, {}, {ChannelCount::Stereo}, ChannelCount::Mono}},
    {"legato",    {cedar::Opcode::NOP, 1, 2, false,
                   {"instrument", "input", "release", "", "", ""},
                   {NAN, NAN, 0.0f, NAN, NAN},
                   "Legato voice manager. `release` (seconds) extends mix tail past note-off."}},
    {"spread",    {cedar::Opcode::NOP, 2, 0, false,
                   {"n", "source", "", "", "", ""},
                   {NAN, NAN, NAN},
                   "Force source to specific voice count (pad/truncate)"}},

    // Array generation operations
    {"linspace",  {cedar::Opcode::NOP, 3, 1, false,
                   {"start", "end", "n", "mode", "", ""},
                   {NAN, NAN, NAN, NAN},
                   "Generate n evenly spaced values from start to end; mode=\"linear\" (default), \"log\", or \"geom\""}},
    {"random",    {cedar::Opcode::NOP, 1, 2, false,
                   {"n", "min", "max", "", "", ""},
                   {NAN, 0.0f, 1.0f},
                   "Generate n random values in [min, max) (deterministic; defaults to [0, 1))"}},
    {"harmonics", {cedar::Opcode::NOP, 2, 1, false,
                   {"fundamental", "n", "ratio", "", "", ""},
                   {NAN, NAN, 1.0f},
                   "Generate harmonic series; ratio>1 stretches partials (piano-like inharmonicity), <1 compresses"}},

    // Function composition (handled specially by codegen)
    {"compose",   {cedar::Opcode::NOP, 2, 0, false,
                   {"f", "g", "", "", "", ""},
                   {NAN, NAN, NAN},
                   "Compose functions: compose(f, g)(x) = g(f(x))"}},

    // Chord function (handled specially by codegen)
    // chord("Am") -> array of MIDI notes (root note only for now)
    // chord("Am C7 F G") -> pattern of chord progressions
    {"chord",   {cedar::Opcode::PUSH_CONST, 1, 0, false,
                 {"symbol", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Chord expansion (Am, C7, Fmaj7, etc.)"}},

    // scalar(p) is the explicit Pattern→Signal cast.
    {"scalar",  {cedar::Opcode::NOP, 1, 0, false,
                 {"pattern", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Cast a note/value/chord pattern to its primary value buffer as a Signal."}},
    // Pattern transformation builtins (handled specially by codegen)
    // These transform pattern events at compile time
    {"slow",    {cedar::Opcode::NOP, 2, 0, false,
                 {"pattern", "factor", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Slow down pattern by factor (stretch time)."}},
    {"fast",    {cedar::Opcode::NOP, 2, 0, false,
                 {"pattern", "factor", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Speed up pattern by factor (compress time)."}},
    {"rev",     {cedar::Opcode::NOP, 1, 0, false,
                 {"pattern", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Reverse pattern event order."}},
    {"transpose", {cedar::Opcode::NOP, 2, 0, false,
                 {"pattern", "semitones", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Transpose pattern pitches by semitones."}},
    {"velocity", {cedar::Opcode::NOP, 2, 0, false,
                 {"pattern", "vel", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Set velocity on pattern events (0-1)."}},
    {"transport", {cedar::Opcode::SEQPAT_TRANSPORT, 2, 2, true,
                 {"pattern", "trig", "step", "reset", "", ""},
                 {1.0f, NAN, NAN, NAN, NAN},
                 "Trigger-driven pattern transport — decouples a pattern from "
                 "the global clock; each trigger edge advances playback."}},
    // Phase 2.1 PRD §11.2: standalone note-property transforms
    {"bend",       {cedar::Opcode::NOP, 2, 0, false,
                    {"pattern", "value", "", "", "", ""},
                    {NAN, NAN, NAN},
                    "Set bend value on pattern events; reachable via e.bend."}},
    {"aftertouch", {cedar::Opcode::NOP, 2, 0, false,
                    {"pattern", "value", "", "", "", ""},
                    {NAN, NAN, NAN},
                    "Set aftertouch value on pattern events; reachable via e.aftertouch."}},
    {"dur",        {cedar::Opcode::NOP, 2, 0, false,
                    {"pattern", "factor", "", "", "", ""},
                    {NAN, NAN, NAN},
                    "Multiply event durations by factor."}},
    {"bank",    {cedar::Opcode::NOP, 2, 0, false,
                 {"pattern", "bank_name", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Set sample bank for pattern events."}},
    {"variant",  {cedar::Opcode::NOP, 2, 0, false,
                  {"pattern", "index", "", "", "", ""},
                  {NAN, NAN, NAN},
                  "Set sample variant for pattern events."}},
    {"tune",    {cedar::Opcode::NOP, 2, 0, false,
                 {"tuning", "pattern", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Apply microtonal tuning context to a pattern."}},

    // Phase 2 PRD: time & structure modifiers (Strudel-compatible).
    // All compile-time event-list rewrites; opcode is NOP.
    {"early",      {cedar::Opcode::NOP, 2, 0, false,
                    {"pattern", "amount", "", "", "", ""},
                    {NAN, NAN, NAN},
                    "Shift events earlier by amount cycles (wraps)."}},
    {"late",       {cedar::Opcode::NOP, 2, 0, false,
                    {"pattern", "amount", "", "", "", ""},
                    {NAN, NAN, NAN},
                    "Shift events later by amount cycles (wraps)."}},
    {"palindrome", {cedar::Opcode::NOP, 1, 0, false,
                    {"pattern", "", "", "", "", ""},
                    {NAN, NAN, NAN},
                    "Play pattern forward then reversed (doubles cycle length)."}},
    {"compress",   {cedar::Opcode::NOP, 3, 0, false,
                    {"pattern", "start", "end", "", "", ""},
                    {NAN, NAN, NAN},
                    "Squash pattern into [start, end) of cycle (silence elsewhere)."}},
    {"ply",        {cedar::Opcode::NOP, 2, 0, false,
                    {"pattern", "n", "", "", "", ""},
                    {NAN, NAN, NAN},
                    "Repeat each event n times within its slot."}},
    {"linger",     {cedar::Opcode::NOP, 2, 0, false,
                    {"pattern", "frac", "", "", "", ""},
                    {NAN, NAN, NAN},
                    "Keep first frac of pattern; loop it to fill the cycle."}},
    {"zoom",       {cedar::Opcode::NOP, 3, 0, false,
                    {"pattern", "start", "end", "", "", ""},
                    {NAN, NAN, NAN},
                    "Play only [start, end) portion of pattern, stretched to fill cycle."}},
    {"segment",    {cedar::Opcode::NOP, 2, 0, false,
                    {"pattern", "n", "", "", "", ""},
                    {NAN, NAN, NAN},
                    "Sample pattern at n evenly-spaced points; emit n equal-duration events."}},
    {"swing",      {cedar::Opcode::NOP, 1, 1, false,
                    {"pattern", "n", "", "", "", ""},
                    {NAN, NAN, NAN},
                    "Apply 1/3 swing on n-slice grid (default n=4)."}},
    {"swingBy",    {cedar::Opcode::NOP, 2, 1, false,
                    {"pattern", "amount", "n", "", "", ""},
                    {NAN, NAN, NAN, NAN},
                    "Apply swing of `amount` on n-slice grid (default n=4)."}},
    {"iter",       {cedar::Opcode::NOP, 2, 0, false,
                    {"pattern", "n", "", "", "", ""},
                    {NAN, NAN, NAN},
                    "Rotate pattern start by 1/n per cycle (forward)."}},
    {"iterBack",   {cedar::Opcode::NOP, 2, 0, false,
                    {"pattern", "n", "", "", "", ""},
                    {NAN, NAN, NAN},
                    "Rotate pattern start by 1/n per cycle (backward)."}},

    // Phase 2 PRD: algorithmic pattern generators.
    // These emit a PatternEventStream directly (no inner pattern).
    {"run",        {cedar::Opcode::NOP, 1, 0, false,
                    {"n", "", "", "", "", ""},
                    {NAN, NAN, NAN},
                    "Pattern of integers 0..n-1 evenly distributed in cycle."}},
    {"binary",     {cedar::Opcode::NOP, 1, 0, false,
                    {"n", "", "", "", "", ""},
                    {NAN, NAN, NAN},
                    "Trigger pattern from binary representation of n (MSB first)."}},
    {"binaryN",    {cedar::Opcode::NOP, 2, 0, false,
                    {"n", "bits", "", "", "", ""},
                    {NAN, NAN, NAN},
                    "Trigger pattern from low `bits` bits of n (zero-padded, MSB first)."}},

    // Phase 2 PRD: voicing transforms (chord-event manipulation).
    {"anchor",     {cedar::Opcode::NOP, 2, 0, false,
                    {"pattern", "note", "", "", "", ""},
                    {NAN, NAN, NAN},
                    "Set anchor MIDI note for chord voicing (e.g., \"c4\")."}},
    {"mode",       {cedar::Opcode::NOP, 2, 0, false,
                    {"pattern", "mode", "", "", "", ""},
                    {NAN, NAN, NAN},
                    "Set chord voicing mode: below/above/duck/root."}},
    {"voicing",    {cedar::Opcode::NOP, 2, 0, false,
                    {"pattern", "name", "", "", "", ""},
                    {NAN, NAN, NAN},
                    "Apply named voicing dictionary (close/open/drop2/drop3 or custom)."}},
    {"addVoicings", {cedar::Opcode::NOP, 2, 0, false,
                     {"name", "intervals", "", "", "", ""},
                     {NAN, NAN, NAN},
                     "Register a custom voicing dictionary by name."}},

    // Parameter exposure builtins (handled specially by codegen)
    // These extract metadata at compile time for UI generation
    {"param",   {cedar::Opcode::ENV_GET, 2, 2, false,
                 {"name", "default", "min", "max", "", ""},
                 {NAN, 0.0f, 0.0f, 1.0f},
                 "Continuous parameter (slider). Reads from EnvMap."}},
    {"button",  {cedar::Opcode::ENV_GET, 1, 0, false,
                 {"name", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Momentary button. 1 while pressed, 0 otherwise."}},
    {"toggle",  {cedar::Opcode::ENV_GET, 1, 1, false,
                 {"name", "default", "", "", "", ""},
                 {NAN, 0.0f, NAN},
                 "Boolean toggle. Click to flip between 0 and 1."}},
    {"dropdown", {cedar::Opcode::ENV_GET, 2, 6, false,
                 {"name", "opt1", "opt2", "opt3", "opt4", "opt5"},
                 {NAN, NAN, NAN},
                 "Selection dropdown. Returns index (0, 1, ...) of selected option."}},

    // Visualization builtins (handled specially by codegen)
    // These create visualization widgets in the editor and pass signal through
    // Signature: viz(signal, name?, options?) where options is a record {width, height, ...}
    // Option-field schemas are declared in viz_schemas (see further below in this header).
    {"pianoroll", {.opcode = cedar::Opcode::COPY, .input_count = 1, .optional_count = 2, .requires_state = false,
                   .param_names = {"signal", "name", "options", "", "", ""},
                   .defaults = {NAN, NAN, NAN, NAN, NAN},
                   .description = "Attach piano roll visualization. Signal passes through unchanged.",
                   .param_types = {ParamValueType::Signal, ParamValueType::String, ParamValueType::Record},
                   .option_schemas = {OptionSchema{
                       /*param_index=*/2,
                       /*fields=*/{{
                           {"width",    OptionFieldType::Number, "200",        "Canvas width (pixels, or '%' string for relative)"},
                           {"height",   OptionFieldType::Number, "50",         "Canvas height (pixels, or '%' string for relative)"},
                           {"beats",    OptionFieldType::Number, "",           "Window duration in beats (defaults to cycle length)"},
                           {"showGrid", OptionFieldType::Bool,   "true",       "Display vertical beat grid"},
                           {"scale",    OptionFieldType::Enum,   "\"chromatic\"", "MIDI scale filter", "chromatic,pentatonic,octave"},
                       }},
                       /*field_count=*/5,
                   }},
                   .option_schema_count = 1}},
    {"oscilloscope", {.opcode = cedar::Opcode::COPY, .input_count = 1, .optional_count = 2, .requires_state = true,
                      .param_names = {"signal", "name", "options", "", "", ""},
                      .defaults = {NAN, NAN, NAN, NAN, NAN},
                      .description = "Attach oscilloscope visualization. Signal passes through unchanged.",
                      .param_types = {ParamValueType::Signal, ParamValueType::String, ParamValueType::Record},
                      .option_schemas = {OptionSchema{
                          /*param_index=*/2,
                          /*fields=*/{{
                              {"width",        OptionFieldType::Number, "200",       "Canvas width (pixels, or '%' string for relative)"},
                              {"height",       OptionFieldType::Number, "50",        "Canvas height (pixels, or '%' string for relative)"},
                              {"triggerLevel", OptionFieldType::Number, "0",         "Trigger threshold (signal value)"},
                              {"triggerEdge",  OptionFieldType::Enum,   "\"rising\"", "Trigger direction", "rising,falling"},
                          }},
                          /*field_count=*/4,
                      }},
                      .option_schema_count = 1}},
    {"waveform", {.opcode = cedar::Opcode::COPY, .input_count = 1, .optional_count = 2, .requires_state = true,
                  .param_names = {"signal", "name", "options", "", "", ""},
                  .defaults = {NAN, NAN, NAN, NAN, NAN},
                  .description = "Attach waveform visualization. Signal passes through unchanged.",
                  .param_types = {ParamValueType::Signal, ParamValueType::String, ParamValueType::Record},
                  .option_schemas = {OptionSchema{
                      /*param_index=*/2,
                      /*fields=*/{{
                          {"width",    OptionFieldType::Number, "200",  "Canvas width (pixels, or '%' string for relative)"},
                          {"height",   OptionFieldType::Number, "50",   "Canvas height (pixels, or '%' string for relative)"},
                          {"scale",    OptionFieldType::Number, "1.0",  "Amplitude scaling factor"},
                          {"filled",   OptionFieldType::Bool,   "true", "Use filled envelope vs outline"},
                          {"duration", OptionFieldType::Number, "5",    "Time window in seconds"},
                      }},
                      /*field_count=*/5,
                  }},
                  .option_schema_count = 1}},
    {"spectrum", {.opcode = cedar::Opcode::COPY, .input_count = 1, .optional_count = 2, .requires_state = true,
                  .param_names = {"signal", "name", "options", "", "", ""},
                  .defaults = {NAN, NAN, NAN, NAN, NAN},
                  .description = "Attach spectrum analyzer visualization. Signal passes through unchanged.",
                  .param_types = {ParamValueType::Signal, ParamValueType::String, ParamValueType::Record},
                  .option_schemas = {OptionSchema{
                      /*param_index=*/2,
                      /*fields=*/{{
                          {"width",    OptionFieldType::Number, "200",   "Canvas width (pixels, or '%' string for relative)"},
                          {"height",   OptionFieldType::Number, "50",    "Canvas height (pixels, or '%' string for relative)"},
                          {"logScale", OptionFieldType::Bool,   "false", "Log frequency axis vs linear"},
                          {"minDb",    OptionFieldType::Number, "-90",   "Lower dB limit for normalization"},
                          {"maxDb",    OptionFieldType::Number, "0",     "Upper dB limit for normalization"},
                          {"fft",      OptionFieldType::Enum,   "1024",  "FFT bin count", "256,512,1024,2048"},
                      }},
                      /*field_count=*/6,
                  }},
                  .option_schema_count = 1}},
    {"waterfall", {.opcode = cedar::Opcode::COPY, .input_count = 1, .optional_count = 2, .requires_state = true,
                    .param_names = {"signal", "name", "options", "", "", ""},
                    .defaults = {NAN, NAN, NAN, NAN, NAN},
                    .description = "Attach scrolling spectrogram visualization. Signal passes through unchanged.",
                    .param_types = {ParamValueType::Signal, ParamValueType::String, ParamValueType::Record},
                    .option_schemas = {OptionSchema{
                        /*param_index=*/2,
                        /*fields=*/{{
                            {"width",    OptionFieldType::Number, "300",      "Canvas width (pixels, or '%' string for relative)"},
                            {"height",   OptionFieldType::Number, "150",      "Canvas height (pixels, or '%' string for relative)"},
                            {"angle",    OptionFieldType::Number, "180",      "Scroll direction in degrees (0–360)"},
                            {"speed",    OptionFieldType::Number, "40",       "Scroll speed in pixels/sec"},
                            {"fft",      OptionFieldType::Enum,   "1024",     "FFT bin count", "256,512,1024,2048"},
                            {"gradient", OptionFieldType::Enum,   "\"magma\"", "Color gradient preset", "magma,viridis,inferno,grayscale"},
                            {"minDb",    OptionFieldType::Number, "-90",      "Lower dB limit for normalization"},
                            {"maxDb",    OptionFieldType::Number, "0",        "Upper dB limit for normalization"},
                        }},
                        /*field_count=*/8,
                    }},
                    .option_schema_count = 1}},

    // Builtin variable getters (desugared from identifier reads like `bpm`, `spb`)
    // These are registered in BUILTIN_FUNCTIONS so the analyzer accepts them as builtins.
    // The identifier path in codegen.cpp:433 handles the actual desugaring to ENV_GET.
    {"get_bpm", {cedar::Opcode::ENV_GET, 0, 0, false,
                  {"", "", "", "", "", ""},
                  {NAN, NAN, NAN, NAN, NAN},
                  "Get current BPM (beats per minute)."}},
    {"get_sr",  {cedar::Opcode::ENV_GET, 0, 0, false,
                  {"", "", "", "", "", ""},
                  {NAN, NAN, NAN, NAN, NAN},
                  "Get current sample rate."}},
    {"get_spb", {cedar::Opcode::ENV_GET, 0, 0, false,
                  {"", "", "", "", "", ""},
                  {NAN, NAN, NAN, NAN, NAN},
                  "Get seconds per beat = 60.0 / BPM."}},
};

/// Alias mappings for convenience syntax
/// e.g., "sine" -> "sin", "lowpass" -> "lp"
inline const std::unordered_map<std::string_view, std::string_view> BUILTIN_ALIASES = {
    {"lowpass",   "lp"},
    {"highpass",  "hp"},
    {"bandpass",  "bp"},
    {"output",    "out"},
    {"moogladder", "moog"},
    {"envelope",  "adsr"},
    {"envfollow", "env_follower"},
    {"follower",  "env_follower"},
    // SVF aliases with explicit naming
    {"svflp",     "lp"},
    {"svfhp",     "hp"},
    {"svfbp",     "bp"},
    // SquelchEngine filter aliases
    {"diodeladder", "diode"},
    {"tb303",       "diode"},
    {"acid",        "diode"},
    {"vowel",       "formant"},
    {"sk",          "sallenkey"},
    {"ms20",        "sallenkey"},
    // Reverb aliases
    {"reverb",    "freeverb"},
    {"plate",     "dattorro"},
    {"room",      "fdn"},
    // Distortion aliases
    // Note: tanh(x) is now a pure math function
    // Use saturate(in, drive) for the saturation effect
    {"distort",   "saturate"},
    {"crush",     "bitcrush"},
    {"wavefold",  "fold"},
    {"valve",     "tube"},
    {"triode",    "tube"},
    {"adaa",      "smooth"},
    {"transformer", "xfmr"},
    {"console",   "xfmr"},
    {"exciter",   "excite"},
    {"aural",     "excite"},
    // Dynamics aliases
    // NOTE: `compress` was previously aliased to `comp` (audio compressor) but
    // is now reserved for the Strudel-style pattern transform. Audio users
    // must use `comp(...)` or `compressor(...)`.
    {"compressor", "comp"},
    {"limit",     "limiter"},
    {"noisegate", "gate"},
};

/// Lookup a builtin by name, handling aliases
/// Returns nullptr if not found
inline const BuiltinInfo* lookup_builtin(std::string_view name) {
    // Check for alias first
    auto alias_it = BUILTIN_ALIASES.find(name);
    if (alias_it != BUILTIN_ALIASES.end()) {
        name = alias_it->second;
    }

    // Lookup in main table
    auto it = BUILTIN_FUNCTIONS.find(name);
    if (it != BUILTIN_FUNCTIONS.end()) {
        return &it->second;
    }
    return nullptr;
}

/// Get the canonical name for a function (resolves aliases)
inline std::string_view canonical_name(std::string_view name) {
    auto alias_it = BUILTIN_ALIASES.find(name);
    if (alias_it != BUILTIN_ALIASES.end()) {
        return alias_it->second;
    }
    return name;
}

// ============================================================================
// Builtin Variables (bpm, sr)
// ============================================================================

/// Definition of a builtin variable that desugars to ENV_GET reads and
/// compile-time constant extraction for writes.
struct BuiltinVarDef {
    std::string_view getter_name;   // "get_bpm"
    std::string_view setter_name;   // "set_bpm" (empty = read-only)
    std::string_view env_key;       // "__bpm" — reserved EnvMap key for getter
    float default_value;            // 120.0f
    float min_value;                // 1.0f (0 = no clamping)
    float max_value;                // 999.0f (0 = no clamping)
};

inline const std::unordered_map<std::string_view, BuiltinVarDef> BUILTIN_VARIABLES = {
    {"bpm", {"get_bpm", "set_bpm", "__bpm", 120.0f, 1.0f, 999.0f}},
    {"sr",  {"get_sr",  "",         "__sr",  48000.0f, 0.0f, 0.0f}},
    {"spb", {"get_spb", "",         "__spb", 0.5f, 0.0f, 0.0f}},  // seconds per beat = 60.0 / bpm
};

} // namespace akkado
