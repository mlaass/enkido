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
    std::string_view description;  // One-line docstring for autocomplete

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
    // Strudel-style unified oscillator function: osc(type, freq, pwm, phase, trig)
    // Type is resolved at compile-time from a string literal.
    // Examples: osc("sin", 440), osc("saw", freq), osc("sqr_pwm", freq, 0.5)
    // The opcode here is a placeholder - actual opcode is determined by type string in codegen.
    {"osc",     {cedar::Opcode::OSC_SIN, 2, 3, true,
                 {"type", "freq", "pwm", "phase", "trig", ""},
                 {0.5f, NAN, NAN, NAN, NAN},
                 "Band-limited oscillator (sin, saw, sqr, tri, ramp, phasor)"}},

    // Basic Oscillators - kept for backwards compatibility and direct access
    // For Strudel-style syntax, use osc("type", freq) instead
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
    // Sine oscillator renamed to avoid conflict with sin() math function
    {"sine_osc", {cedar::Opcode::OSC_SIN,   1, 2, true,
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

    // Filters (signal, cutoff required; q optional with default 0.707)
    // SVF (State Variable Filter) - stable under modulation
    {"lp",      {cedar::Opcode::FILTER_SVF_LP, 2, 1, true,
                 {"in", "cut", "q", "", "", ""},
                 {0.707f, NAN, NAN},
                 "State-variable lowpass filter"}},
    {"hp",      {cedar::Opcode::FILTER_SVF_HP, 2, 1, true,
                 {"in", "cut", "q", "", "", ""},
                 {0.707f, NAN, NAN},
                 "State-variable highpass filter"}},
    {"bp",      {cedar::Opcode::FILTER_SVF_BP, 2, 1, true,
                 {"in", "cut", "q", "", "", ""},
                 {0.707f, NAN, NAN},
                 "State-variable bandpass filter"}},
    // Moog ladder filter (4-pole with resonance)
    // Optional: max_resonance (self-oscillation threshold), input_scale (preamp drive)
    {"moog",    {cedar::Opcode::FILTER_MOOG, 2, 3, true,
                 {"in", "cut", "res", "max_res", "input_scale", ""},
                 {1.0f, 4.0f, 0.5f, NAN, NAN},
                 "Moog 4-pole ladder filter with resonance"}},
    // Diode ladder filter (TB-303 acid) - 5 inputs: in, cut, res, vt, fb_gain
    {"diode",   {cedar::Opcode::FILTER_DIODE, 2, 3, true,
                 {"in", "cut", "res", "vt", "fb_gain", ""},
                 {1.0f, 0.026f, 10.0f},
                 "TB-303 style diode ladder filter"}},
    // Formant filter (vowel morphing) - 5 inputs: in, vowel_a, vowel_b, morph, q
    {"formant", {cedar::Opcode::FILTER_FORMANT, 2, 3, true,
                 {"in", "vowel_a", "vowel_b", "morph", "q", ""},
                 {0.0f, 0.5f, 10.0f},
                 "Vowel formant filter with morphing"}},
    // Sallen-Key filter (MS-20 style) - 5 inputs: in, cut, res, mode, clip_threshold
    // Optional: clip_threshold (feedback clipping point)
    {"sallenkey", {cedar::Opcode::FILTER_SALLENKEY, 2, 3, true,
                   {"in", "cut", "res", "mode", "clip_thresh", ""},
                   {1.0f, 0.0f, 0.7f, NAN, NAN},
                   "MS-20 style Sallen-Key filter"}},

    // Envelopes
    {"adsr",    {cedar::Opcode::ENV_ADSR, 1, 4, true,
                 {"gate", "attack", "decay", "sustain", "release", ""},
                 {0.01f, 0.1f, 0.5f},
                 "Attack-decay-sustain-release envelope"}},
    {"ar",      {cedar::Opcode::ENV_AR, 1, 2, true,
                 {"trig", "attack", "release", "", "", ""},
                 {0.01f, 0.3f, NAN},
                 "Attack-release envelope (one-shot)"}},
    {"env_follower", {cedar::Opcode::ENV_FOLLOWER, 1, 2, true,
                      {"in", "attack", "release", "", "", ""},
                      {0.01f, 0.1f, NAN},
                      "Amplitude envelope follower"}},

    // Samplers
    {"sample",  {cedar::Opcode::SAMPLE_PLAY, 3, 0, true,
                 {"trig", "pitch", "id", "", "", ""},
                 {NAN, NAN, NAN},
                 "One-shot sample playback"}},
    {"sample_loop", {cedar::Opcode::SAMPLE_PLAY_LOOP, 3, 0, true,
                     {"gate", "pitch", "id", "", "", ""},
                     {NAN, NAN, NAN},
                     "Looping sample playback"}},

    // Delays
    {"delay",   {cedar::Opcode::DELAY, 3, 0, true,
                 {"in", "time", "fb", "", "", ""},
                 {NAN, NAN, NAN},
                 "Delay line with feedback"}},

    // Reverbs (stateful - large delay networks)
    // freeverb: room_scale (density factor), room_offset (decay baseline)
    {"freeverb", {cedar::Opcode::REVERB_FREEVERB, 1, 4, true,
                  {"in", "room", "damp", "room_scale", "room_offset", ""},
                  {0.5f, 0.5f, 0.28f, 0.7f, NAN},
                  "Freeverb algorithmic reverb"}},
    // dattorro: input_diffusion (input smoothing), decay_diffusion (tail smoothing)
    {"dattorro", {cedar::Opcode::REVERB_DATTORRO, 1, 4, true,
                  {"in", "decay", "predelay", "in_diff", "dec_diff", ""},
                  {0.7f, 20.0f, 0.75f, 0.625f, NAN},
                  "Dattorro plate reverb algorithm"}},
    {"fdn",      {cedar::Opcode::REVERB_FDN, 1, 2, true,
                  {"in", "decay", "damp", "", "", ""},
                  {0.8f, 0.3f, NAN},
                  "Feedback delay network reverb"}},

    // Modulation Effects (stateful - delay lines with LFOs)
    // chorus: base_delay (ms), depth_range (ms)
    {"chorus",   {cedar::Opcode::EFFECT_CHORUS, 1, 4, true,
                  {"in", "rate", "depth", "base_delay", "depth_range", ""},
                  {0.5f, 0.5f, 20.0f, 10.0f, NAN},
                  "Stereo chorus effect"}},
    // flanger: min_delay (ms), max_delay (ms)
    {"flanger",  {cedar::Opcode::EFFECT_FLANGER, 1, 4, true,
                  {"in", "rate", "depth", "min_delay", "max_delay", ""},
                  {1.0f, 0.7f, 0.1f, 10.0f, NAN},
                  "Classic flanger effect"}},
    // phaser: min_freq (Hz), max_freq (Hz)
    {"phaser",   {cedar::Opcode::EFFECT_PHASER, 1, 4, true,
                  {"in", "rate", "depth", "min_freq", "max_freq", ""},
                  {0.5f, 0.8f, 200.0f, 4000.0f, NAN},
                  "Multi-stage phaser effect"}},
    {"comb",     {cedar::Opcode::EFFECT_COMB, 3, 0, true,
                  {"in", "time", "fb", "", "", ""},
                  {NAN, NAN, NAN},
                  "Comb filter (resonant delay)"}},

    // Distortion
    // Note: tanh(x) is now a pure math function. Use saturate(in, drive) for distortion.
    {"saturate", {cedar::Opcode::DISTORT_TANH, 1, 1, false,
                  {"in", "drive", "", "", "", ""},
                  {2.0f, NAN, NAN},
                  "Soft saturation (tanh) distortion"}},
    {"softclip", {cedar::Opcode::DISTORT_SOFT, 1, 1, false,
                  {"in", "thresh", "", "", "", ""},
                  {0.5f, NAN, NAN},
                  "Soft clipper distortion"}},
    {"bitcrush", {cedar::Opcode::DISTORT_BITCRUSH, 1, 2, true,
                  {"in", "bits", "rate", "", "", ""},
                  {8.0f, 0.5f, NAN},
                  "Bit depth and sample rate reducer"}},
    {"fold",     {cedar::Opcode::DISTORT_FOLD, 1, 1, false,
                  {"in", "thresh", "", "", "", ""},
                  {0.5f, NAN, NAN},
                  "Wavefolding distortion"}},
    {"tube",     {cedar::Opcode::DISTORT_TUBE, 1, 2, true,
                  {"in", "drive", "bias", "", "", ""},
                  {5.0f, 0.1f, NAN},
                  "Tube amp emulation with bias"}},
    {"smooth",   {cedar::Opcode::DISTORT_SMOOTH, 1, 1, true,
                  {"in", "drive", "", "", "", ""},
                  {5.0f, NAN, NAN},
                  "ADAA alias-free saturation"}},
    // tape: soft_threshold (saturation onset), warmth_scale (HF rolloff amount)
    {"tape",     {cedar::Opcode::DISTORT_TAPE, 1, 4, true,
                  {"in", "drive", "warmth", "soft_thresh", "warmth_scale", ""},
                  {3.0f, 0.3f, 0.5f, 0.7f, NAN},
                  "Tape saturation with warmth"}},
    // xfmr: bass_freq (bass extraction cutoff Hz)
    {"xfmr",     {cedar::Opcode::DISTORT_XFMR, 1, 3, true,
                  {"in", "drive", "bass", "bass_freq", "", ""},
                  {3.0f, 5.0f, 60.0f, NAN, NAN},
                  "Transformer saturation with bass boost"}},
    // excite: harmonic_odd (odd harmonic mix), harmonic_even (even harmonic mix)
    {"excite",   {cedar::Opcode::DISTORT_EXCITE, 1, 4, true,
                  {"in", "amount", "freq", "harm_odd", "harm_even", ""},
                  {0.5f, 3000.0f, 0.4f, 0.6f, NAN},
                  "Aural exciter (harmonic enhancer)"}},

    // Dynamics (stateful - envelope followers)
    {"comp",     {cedar::Opcode::DYNAMICS_COMP, 1, 2, true,
                  {"in", "thresh", "ratio", "", "", ""},
                  {-12.0f, 4.0f, NAN},
                  "Dynamic range compressor"}},
    {"limiter",  {cedar::Opcode::DYNAMICS_LIMITER, 1, 2, true,
                  {"in", "ceiling", "release", "", "", ""},
                  {-0.1f, 0.1f, NAN},
                  "Peak limiter with lookahead"}},
    // gate: hysteresis (dB open/close diff), close_time (ms fade-out)
    {"gate",     {cedar::Opcode::DYNAMICS_GATE, 1, 4, true,
                  {"in", "thresh", "range", "hyst", "close_time", ""},
                  {-40.0f, -40.0f, 6.0f, 5.0f, NAN},
                  "Noise gate with hysteresis"}},

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
    {"min",     {cedar::Opcode::MIN, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Minimum of two values"}},
    {"max",     {cedar::Opcode::MAX, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Maximum of two values"}},

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
                 "Slew rate limiter (portamento)"}},
    {"sah",     {cedar::Opcode::SAH,   2, 0, true,
                 {"in", "trig", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Sample and hold"}},

    // Output (1 required for mono, 2 for stereo)
    {"out",     {cedar::Opcode::OUTPUT, 1, 1, false,
                 {"L", "R", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Audio output (mono or stereo)"}},

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
    {"seq_step", {cedar::Opcode::SEQ_STEP, 1, 0, true,
                 {"speed", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Step sequencer"}},
    {"timeline", {cedar::Opcode::TIMELINE, 0, 0, true,
                 {"", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Breakpoint automation timeline"}},

    // Compile-time array functions (handled specially by codegen)
    {"len",     {cedar::Opcode::PUSH_CONST, 1, 0, false,
                 {"arr", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Array length (compile-time)"}},

    // Multi-buffer array primitives for polyphony (handled specially by codegen)
    // These enable user-defined polyphony: fn poly(c, f) = sum(map(c, f)) / len(c)
    {"map",     {cedar::Opcode::NOP, 2, 0, false,
                 {"array", "fn", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Apply function to each element of array"}},
    {"sum",     {cedar::Opcode::NOP, 1, 0, false,
                 {"array", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Sum all elements of array"}},
    {"fold",    {cedar::Opcode::NOP, 3, 0, false,
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
    {"range",   {cedar::Opcode::NOP, 2, 0, false,
                 {"start", "end", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Generate array [start, start+1, ..., end-1]"}},
    {"repeat",  {cedar::Opcode::NOP, 2, 0, false,
                 {"value", "n", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Repeat value n times: [v, v, ..., v]"}},

    // Chord function (handled specially by codegen)
    // chord("Am") -> array of MIDI notes (root note only for now)
    // chord("Am C7 F G") -> pattern of chord progressions
    {"chord",   {cedar::Opcode::PUSH_CONST, 1, 0, false,
                 {"symbol", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Chord expansion (Am, C7, Fmaj7, etc.)"}},

    // Pattern keywords (handled specially by parser, not codegen)
    // These appear in builtins for signature help but parse as MiniLiteral nodes
    {"pat",     {cedar::Opcode::PUSH_CONST, 1, 0, false,
                 {"pattern", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Mini-notation pattern. Returns values based on cycle position."}},
    {"seq",     {cedar::Opcode::PUSH_CONST, 1, 1, false,
                 {"pattern", "closure", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Sequence with optional closure (t, v, p) -> expr."}},
    {"note",    {cedar::Opcode::PUSH_CONST, 1, 0, false,
                 {"pattern", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Note pattern. Returns MIDI note values."}},

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
