#pragma once

#include <cedar/vm/instruction.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace akkado {

/// Maximum number of parameters for a builtin function
constexpr std::size_t MAX_BUILTIN_PARAMS = 6;

/// Maximum number of optional parameters with defaults
constexpr std::size_t MAX_BUILTIN_DEFAULTS = 5;

/// Metadata for a built-in function
struct BuiltinInfo {
    cedar::Opcode opcode;       // VM opcode to emit
    std::uint8_t input_count;   // Number of required inputs
    std::uint8_t optional_count; // Number of optional inputs with defaults
    bool requires_state;        // Whether opcode needs state_id (oscillators, filters)
    std::array<std::string_view, MAX_BUILTIN_PARAMS> param_names;  // Parameter names for named args
    std::array<float, MAX_BUILTIN_DEFAULTS> defaults;              // Default values (NaN = required)

    /// Get total parameter count (required + optional)
    [[nodiscard]] std::uint8_t total_params() const {
        return input_count + optional_count;
    }

    /// Find parameter index by name, returns -1 if not found
    [[nodiscard]] int find_param(std::string_view name) const {
        for (std::size_t i = 0; i < MAX_BUILTIN_PARAMS; ++i) {
            if (param_names[i].empty()) break;
            if (param_names[i] == name) return static_cast<int>(i);
        }
        return -1;
    }

    /// Check if parameter at index has a default value
    [[nodiscard]] bool has_default(std::size_t index) const {
        if (index < input_count) return false;  // Required params don't have defaults
        std::size_t default_idx = index - input_count;
        if (default_idx >= MAX_BUILTIN_DEFAULTS) return false;
        return !std::isnan(defaults[default_idx]);
    }

    /// Get default value for parameter at index (must check has_default first)
    [[nodiscard]] float get_default(std::size_t index) const {
        std::size_t default_idx = index - input_count;
        return defaults[default_idx];
    }
};

/// Static mapping of Akkado function names to Cedar opcodes
/// Used by semantic analyzer to resolve function calls
///
/// NOTE: The `osc(type, freq)` function is handled specially by codegen.
/// It resolves the string type ("sin", "sine", "saw", etc.) at compile-time
/// to the appropriate OSC_* opcode. See codegen.cpp for the implementation.
inline const std::unordered_map<std::string_view, BuiltinInfo> BUILTIN_FUNCTIONS = {
    // Strudel-style unified oscillator function: osc(type, freq) or osc(type, freq, pwm)
    // Type is resolved at compile-time from a string literal.
    // Examples: osc("sin", 440), osc("saw", freq), osc("sqr_pwm", freq, 0.5)
    // The opcode here is a placeholder - actual opcode is determined by type string in codegen.
    {"osc",     {cedar::Opcode::OSC_SIN, 2, 1, true,
                 {"type", "freq", "pwm", "", "", ""},
                 {NAN, NAN, NAN}}},  // pwm optional for PWM oscillators

    // Basic Oscillators - kept for backwards compatibility and direct access
    // For Strudel-style syntax, use osc("type", freq) instead
    {"tri",     {cedar::Opcode::OSC_TRI,    1, 0, true,
                 {"freq", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"saw",     {cedar::Opcode::OSC_SAW,    1, 0, true,
                 {"freq", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"sqr",     {cedar::Opcode::OSC_SQR,    1, 0, true,
                 {"freq", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"ramp",    {cedar::Opcode::OSC_RAMP,   1, 0, true,
                 {"freq", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"phasor",  {cedar::Opcode::OSC_PHASOR, 1, 0, true,
                 {"freq", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"sqr_minblep", {cedar::Opcode::OSC_SQR_MINBLEP, 1, 0, true,
                 {"freq", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    // Sine oscillator renamed to avoid conflict with sin() math function
    {"sine_osc", {cedar::Opcode::OSC_SIN,   1, 0, true,
                 {"freq", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},

    // PWM Oscillators (2 inputs: frequency, pwm amount)
    {"sqr_pwm", {cedar::Opcode::OSC_SQR_PWM, 2, 0, true,
                 {"freq", "pwm", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"saw_pwm", {cedar::Opcode::OSC_SAW_PWM, 2, 0, true,
                 {"freq", "pwm", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"sqr_pwm_minblep", {cedar::Opcode::OSC_SQR_PWM_MINBLEP, 2, 0, true,
                 {"freq", "pwm", "", "", "", ""},
                 {NAN, NAN, NAN}}},

    // 4x Oversampled PWM (explicit, for when auto-detection isn't desired)
    {"sqr_pwm_4x", {cedar::Opcode::OSC_SQR_PWM_4X, 2, 0, true,
                 {"freq", "pwm", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"saw_pwm_4x", {cedar::Opcode::OSC_SAW_PWM_4X, 2, 0, true,
                 {"freq", "pwm", "", "", "", ""},
                 {NAN, NAN, NAN}}},

    // Filters (signal, cutoff required; q optional with default 0.707)
    // SVF (State Variable Filter) - stable under modulation
    {"lp",      {cedar::Opcode::FILTER_SVF_LP, 2, 1, true,
                 {"in", "cut", "q", "", "", ""},
                 {0.707f, NAN, NAN}}},
    {"hp",      {cedar::Opcode::FILTER_SVF_HP, 2, 1, true,
                 {"in", "cut", "q", "", "", ""},
                 {0.707f, NAN, NAN}}},
    {"bp",      {cedar::Opcode::FILTER_SVF_BP, 2, 1, true,
                 {"in", "cut", "q", "", "", ""},
                 {0.707f, NAN, NAN}}},
    // Moog ladder filter (4-pole with resonance)
    // Optional: max_resonance (self-oscillation threshold), input_scale (preamp drive)
    {"moog",    {cedar::Opcode::FILTER_MOOG, 2, 3, true,
                 {"in", "cut", "res", "max_res", "input_scale", ""},
                 {1.0f, 4.0f, 0.5f, NAN, NAN}}},
    // Diode ladder filter (TB-303 acid) - 5 inputs: in, cut, res, vt, fb_gain
    {"diode",   {cedar::Opcode::FILTER_DIODE, 2, 3, true,
                 {"in", "cut", "res", "vt", "fb_gain", ""},
                 {1.0f, 0.026f, 10.0f}}},  // res=1.0, vt=0.026, fb_gain=10.0
    // Formant filter (vowel morphing) - 5 inputs: in, vowel_a, vowel_b, morph, q
    {"formant", {cedar::Opcode::FILTER_FORMANT, 2, 3, true,
                 {"in", "vowel_a", "vowel_b", "morph", "q", ""},
                 {0.0f, 0.5f, 10.0f}}},  // vowel_b=0, morph=0.5, q=10.0
    // Sallen-Key filter (MS-20 style) - 5 inputs: in, cut, res, mode, clip_threshold
    // Optional: clip_threshold (feedback clipping point)
    {"sallenkey", {cedar::Opcode::FILTER_SALLENKEY, 2, 3, true,
                   {"in", "cut", "res", "mode", "clip_thresh", ""},
                   {1.0f, 0.0f, 0.7f, NAN, NAN}}},  // res=1.0, mode=0.0 (LP), clip=0.7

    // Envelopes
    {"adsr",    {cedar::Opcode::ENV_ADSR, 1, 4, true,
                 {"gate", "attack", "decay", "sustain", "release", ""},
                 {0.01f, 0.1f, 0.5f}}},  // sustain default 0.5, release default handled specially
    {"ar",      {cedar::Opcode::ENV_AR, 1, 2, true,
                 {"trig", "attack", "release", "", "", ""},
                 {0.01f, 0.3f, NAN}}},
    {"env_follower", {cedar::Opcode::ENV_FOLLOWER, 1, 2, true,
                      {"in", "attack", "release", "", "", ""},
                      {0.01f, 0.1f, NAN}}},  // Fast attack, medium release

    // Samplers
    {"sample",  {cedar::Opcode::SAMPLE_PLAY, 3, 0, true,
                 {"trig", "pitch", "id", "", "", ""},
                 {NAN, NAN, NAN}}},  // Trigger, pitch/speed, sample ID
    {"sample_loop", {cedar::Opcode::SAMPLE_PLAY_LOOP, 3, 0, true,
                     {"gate", "pitch", "id", "", "", ""},
                     {NAN, NAN, NAN}}},  // Gate, pitch/speed, sample ID

    // Delays
    {"delay",   {cedar::Opcode::DELAY, 3, 0, true,
                 {"in", "time", "fb", "", "", ""},
                 {NAN, NAN, NAN}}},  // mix packed in reserved field

    // Reverbs (stateful - large delay networks)
    // freeverb: room_scale (density factor), room_offset (decay baseline)
    {"freeverb", {cedar::Opcode::REVERB_FREEVERB, 1, 4, true,
                  {"in", "room", "damp", "room_scale", "room_offset", ""},
                  {0.5f, 0.5f, 0.28f, 0.7f, NAN}}},  // mix packed in reserved
    // dattorro: input_diffusion (input smoothing), decay_diffusion (tail smoothing)
    {"dattorro", {cedar::Opcode::REVERB_DATTORRO, 1, 4, true,
                  {"in", "decay", "predelay", "in_diff", "dec_diff", ""},
                  {0.7f, 20.0f, 0.75f, 0.625f, NAN}}},  // damping + mod in reserved
    {"fdn",      {cedar::Opcode::REVERB_FDN, 1, 2, true,
                  {"in", "decay", "damp", "", "", ""},
                  {0.8f, 0.3f, NAN}}},  // size_mod in reserved

    // Modulation Effects (stateful - delay lines with LFOs)
    // chorus: base_delay (ms), depth_range (ms)
    {"chorus",   {cedar::Opcode::EFFECT_CHORUS, 1, 4, true,
                  {"in", "rate", "depth", "base_delay", "depth_range", ""},
                  {0.5f, 0.5f, 20.0f, 10.0f, NAN}}},  // mix in reserved
    // flanger: min_delay (ms), max_delay (ms)
    {"flanger",  {cedar::Opcode::EFFECT_FLANGER, 1, 4, true,
                  {"in", "rate", "depth", "min_delay", "max_delay", ""},
                  {1.0f, 0.7f, 0.1f, 10.0f, NAN}}},  // feedback + mix in reserved
    // phaser: min_freq (Hz), max_freq (Hz)
    {"phaser",   {cedar::Opcode::EFFECT_PHASER, 1, 4, true,
                  {"in", "rate", "depth", "min_freq", "max_freq", ""},
                  {0.5f, 0.8f, 200.0f, 4000.0f, NAN}}},  // feedback + stages in reserved
    {"comb",     {cedar::Opcode::EFFECT_COMB, 3, 0, true,
                  {"in", "time", "fb", "", "", ""},
                  {NAN, NAN, NAN}}},  // damping in reserved

    // Distortion
    // Note: tanh(x) is now a pure math function. Use saturate(in, drive) for distortion.
    {"saturate", {cedar::Opcode::DISTORT_TANH, 1, 1, false,
                  {"in", "drive", "", "", "", ""},
                  {2.0f, NAN, NAN}}},  // Default drive = 2x
    {"softclip", {cedar::Opcode::DISTORT_SOFT, 1, 1, false,
                  {"in", "thresh", "", "", "", ""},
                  {0.5f, NAN, NAN}}},  // Default threshold
    {"bitcrush", {cedar::Opcode::DISTORT_BITCRUSH, 1, 2, true,
                  {"in", "bits", "rate", "", "", ""},
                  {8.0f, 0.5f, NAN}}},  // stateful for S&H
    {"fold",     {cedar::Opcode::DISTORT_FOLD, 1, 1, false,
                  {"in", "thresh", "", "", "", ""},
                  {0.5f, NAN, NAN}}},  // symmetry in reserved
    {"tube",     {cedar::Opcode::DISTORT_TUBE, 1, 2, true,
                  {"in", "drive", "bias", "", "", ""},
                  {5.0f, 0.1f, NAN}}},  // Default drive=5, bias=0.1 for even harmonics
    {"smooth",   {cedar::Opcode::DISTORT_SMOOTH, 1, 1, true,
                  {"in", "drive", "", "", "", ""},
                  {5.0f, NAN, NAN}}},  // ADAA alias-free saturation
    // tape: soft_threshold (saturation onset), warmth_scale (HF rolloff amount)
    {"tape",     {cedar::Opcode::DISTORT_TAPE, 1, 4, true,
                  {"in", "drive", "warmth", "soft_thresh", "warmth_scale", ""},
                  {3.0f, 0.3f, 0.5f, 0.7f, NAN}}},
    // xfmr: bass_freq (bass extraction cutoff Hz)
    {"xfmr",     {cedar::Opcode::DISTORT_XFMR, 1, 3, true,
                  {"in", "drive", "bass", "bass_freq", "", ""},
                  {3.0f, 5.0f, 60.0f, NAN, NAN}}},
    // excite: harmonic_odd (odd harmonic mix), harmonic_even (even harmonic mix)
    {"excite",   {cedar::Opcode::DISTORT_EXCITE, 1, 4, true,
                  {"in", "amount", "freq", "harm_odd", "harm_even", ""},
                  {0.5f, 3000.0f, 0.4f, 0.6f, NAN}}},

    // Dynamics (stateful - envelope followers)
    {"comp",     {cedar::Opcode::DYNAMICS_COMP, 1, 2, true,
                  {"in", "thresh", "ratio", "", "", ""},
                  {-12.0f, 4.0f, NAN}}},  // attack/release in reserved
    {"limiter",  {cedar::Opcode::DYNAMICS_LIMITER, 1, 2, true,
                  {"in", "ceiling", "release", "", "", ""},
                  {-0.1f, 0.1f, NAN}}},  // lookahead in state
    // gate: hysteresis (dB open/close diff), close_time (ms fade-out)
    {"gate",     {cedar::Opcode::DYNAMICS_GATE, 1, 4, true,
                  {"in", "thresh", "range", "hyst", "close_time", ""},
                  {-40.0f, -40.0f, 6.0f, 5.0f, NAN}}},  // attack/release in reserved

    // Arithmetic (2 inputs, stateless) - from binary operator desugaring
    {"add",     {cedar::Opcode::ADD, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"sub",     {cedar::Opcode::SUB, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"mul",     {cedar::Opcode::MUL, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"div",     {cedar::Opcode::DIV, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"pow",     {cedar::Opcode::POW, 2, 0, false,
                 {"base", "exp", "", "", "", ""},
                 {NAN, NAN, NAN}}},

    // Math unary (1 input)
    {"neg",     {cedar::Opcode::NEG,   1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"abs",     {cedar::Opcode::ABS,   1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"sqrt",    {cedar::Opcode::SQRT,  1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"log",     {cedar::Opcode::LOG,   1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"exp",     {cedar::Opcode::EXP,   1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"floor",   {cedar::Opcode::FLOOR, 1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"ceil",    {cedar::Opcode::CEIL,  1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},

    // Math - Trigonometric (radians)
    // NOTE: sin(x) is the mathematical sine function, NOT a sine oscillator!
    // Use osc("sin", freq) for a sine wave oscillator.
    {"sin",     {cedar::Opcode::MATH_SIN,  1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"cos",     {cedar::Opcode::MATH_COS,  1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"tan",     {cedar::Opcode::MATH_TAN,  1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"asin",    {cedar::Opcode::MATH_ASIN, 1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"acos",    {cedar::Opcode::MATH_ACOS, 1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"atan",    {cedar::Opcode::MATH_ATAN, 1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"atan2",   {cedar::Opcode::MATH_ATAN2, 2, 0, false,
                 {"y", "x", "", "", "", ""},
                 {NAN, NAN, NAN}}},

    // Math - Hyperbolic
    {"sinh",    {cedar::Opcode::MATH_SINH, 1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"cosh",    {cedar::Opcode::MATH_COSH, 1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    // Pure mathematical tanh - useful for waveshaping: tanh(signal * drive)
    // For convenience distortion with drive parameter, use the tanh effect
    {"tanh",    {cedar::Opcode::MATH_TANH, 1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},

    // Math binary (2 inputs)
    {"min",     {cedar::Opcode::MIN, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"max",     {cedar::Opcode::MAX, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN}}},

    // Math ternary (3 inputs)
    {"clamp",   {cedar::Opcode::CLAMP, 3, 0, false,
                 {"x", "lo", "hi", "", "", ""},
                 {NAN, NAN, NAN}}},
    {"wrap",    {cedar::Opcode::WRAP,  3, 0, false,
                 {"x", "lo", "hi", "", "", ""},
                 {NAN, NAN, NAN}}},

    // Utility
    {"noise",   {cedar::Opcode::NOISE, 0, 0, true,
                 {"", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},   // No inputs, needs state
    {"mtof",    {cedar::Opcode::MTOF,  1, 0, false,
                 {"note", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},  // MIDI to frequency
    {"dc",      {cedar::Opcode::DC,    1, 0, false,
                 {"offset", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},  // DC offset
    {"slew",    {cedar::Opcode::SLEW,  2, 0, true,
                 {"target", "rate", "", "", "", ""},
                 {NAN, NAN, NAN}}},   // Signal + rate, needs state
    {"sah",     {cedar::Opcode::SAH,   2, 0, true,
                 {"in", "trig", "", "", "", ""},
                 {NAN, NAN, NAN}}},   // Signal + trigger, needs state

    // Output (1 required for mono, 2 for stereo)
    {"out",     {cedar::Opcode::OUTPUT, 1, 1, false,
                 {"L", "R", "", "", "", ""},
                 {NAN, NAN, NAN}}},  // R defaults to L (mono to stereo)

    // Timing/Sequencing
    {"clock",   {cedar::Opcode::CLOCK,   0, 0, false,
                 {"", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},  // No inputs
    {"lfo",     {cedar::Opcode::LFO,     1, 1, true,
                 {"rate", "duty", "", "", "", ""},
                 {0.5f, NAN, NAN}}},   // Rate + optional duty
    {"trigger", {cedar::Opcode::TRIGGER, 1, 0, true,
                 {"div", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},   // Division
    {"euclid",  {cedar::Opcode::EUCLID,  2, 1, true,
                 {"hits", "steps", "rot", "", "", ""},
                 {0.0f, NAN, NAN}}},   // Hits, steps + optional rotation
    {"seq_step", {cedar::Opcode::SEQ_STEP, 1, 0, true,
                 {"speed", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},   // Step sequencer
    {"timeline", {cedar::Opcode::TIMELINE, 0, 0, true,
                 {"", "", "", "", "", ""},
                 {NAN, NAN, NAN}}},   // Breakpoint automation
};

/// Alias mappings for convenience syntax
/// e.g., "sine" -> "sin", "lowpass" -> "lp"
inline const std::unordered_map<std::string_view, std::string_view> BUILTIN_ALIASES = {
    // Oscillator aliases - now use osc() function with type string
    // e.g., osc("sine", 440) or osc("triangle", freq)
    {"triangle",  "tri"},
    {"sawtooth",  "saw"},
    {"square",    "sqr"},
    // Note: "sine" no longer aliases to oscillator - use osc("sine", freq)
    // sin(x) is now the mathematical sine function
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
    {"compress",  "comp"},
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

} // namespace akkado
