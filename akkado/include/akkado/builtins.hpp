#pragma once

#include <cedar/vm/instruction.hpp>
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace akkado {

/// Metadata for a built-in function
struct BuiltinInfo {
    cedar::Opcode opcode;       // VM opcode to emit
    std::uint8_t input_count;   // Number of required inputs
    std::uint8_t optional_count; // Number of optional inputs (for future use)
    bool requires_state;        // Whether opcode needs state_id (oscillators, filters)
};

/// Static mapping of Akkado function names to Cedar opcodes
/// Used by semantic analyzer to resolve function calls
inline const std::unordered_map<std::string_view, BuiltinInfo> BUILTIN_FUNCTIONS = {
    // Oscillators (1 input: frequency, stateful for phase)
    {"sin",     {cedar::Opcode::OSC_SIN,    1, 0, true}},
    {"tri",     {cedar::Opcode::OSC_TRI,    1, 0, true}},
    {"saw",     {cedar::Opcode::OSC_SAW,    1, 0, true}},
    {"sqr",     {cedar::Opcode::OSC_SQR,    1, 0, true}},
    {"ramp",    {cedar::Opcode::OSC_RAMP,   1, 0, true}},
    {"phasor",  {cedar::Opcode::OSC_PHASOR, 1, 0, true}},

    // Filters (3 inputs: signal, cutoff, q, stateful for delay lines)
    {"lp",      {cedar::Opcode::FILTER_LP,     3, 0, true}},
    {"hp",      {cedar::Opcode::FILTER_HP,     3, 0, true}},
    {"bp",      {cedar::Opcode::FILTER_BP,     3, 0, true}},
    {"svflp",   {cedar::Opcode::FILTER_SVF_LP, 3, 0, true}},
    {"svfhp",   {cedar::Opcode::FILTER_SVF_HP, 3, 0, true}},
    {"svfbp",   {cedar::Opcode::FILTER_SVF_BP, 3, 0, true}},

    // Arithmetic (2 inputs, stateless) - from binary operator desugaring
    {"add",     {cedar::Opcode::ADD, 2, 0, false}},
    {"sub",     {cedar::Opcode::SUB, 2, 0, false}},
    {"mul",     {cedar::Opcode::MUL, 2, 0, false}},
    {"div",     {cedar::Opcode::DIV, 2, 0, false}},
    {"pow",     {cedar::Opcode::POW, 2, 0, false}},

    // Math unary (1 input)
    {"neg",     {cedar::Opcode::NEG,   1, 0, false}},
    {"abs",     {cedar::Opcode::ABS,   1, 0, false}},
    {"sqrt",    {cedar::Opcode::SQRT,  1, 0, false}},
    {"log",     {cedar::Opcode::LOG,   1, 0, false}},
    {"exp",     {cedar::Opcode::EXP,   1, 0, false}},
    {"floor",   {cedar::Opcode::FLOOR, 1, 0, false}},
    {"ceil",    {cedar::Opcode::CEIL,  1, 0, false}},

    // Math binary (2 inputs)
    {"min",     {cedar::Opcode::MIN, 2, 0, false}},
    {"max",     {cedar::Opcode::MAX, 2, 0, false}},

    // Math ternary (3 inputs)
    {"clamp",   {cedar::Opcode::CLAMP, 3, 0, false}},
    {"wrap",    {cedar::Opcode::WRAP,  3, 0, false}},

    // Utility
    {"noise",   {cedar::Opcode::NOISE, 0, 0, true}},   // No inputs, needs state
    {"mtof",    {cedar::Opcode::MTOF,  1, 0, false}},  // MIDI to frequency
    {"dc",      {cedar::Opcode::DC,    1, 0, false}},  // DC offset
    {"slew",    {cedar::Opcode::SLEW,  2, 0, true}},   // Signal + rate, needs state
    {"sah",     {cedar::Opcode::SAH,   2, 0, true}},   // Signal + trigger, needs state

    // Output (2 inputs: left, right)
    {"out",     {cedar::Opcode::OUTPUT, 2, 0, false}},

    // Timing/Sequencing
    {"clock",   {cedar::Opcode::CLOCK,   0, 0, false}},  // No inputs
    {"lfo",     {cedar::Opcode::LFO,     1, 1, true}},   // Rate (+ optional shape)
    {"trigger", {cedar::Opcode::TRIGGER, 1, 0, true}},   // Division
    {"euclid",  {cedar::Opcode::EUCLID,  2, 1, true}},   // Hits, steps (+ optional rotation)
};

/// Alias mappings for convenience syntax
/// e.g., "sine" -> "sin", "lowpass" -> "lp"
inline const std::unordered_map<std::string_view, std::string_view> BUILTIN_ALIASES = {
    {"sine",      "sin"},
    {"triangle",  "tri"},
    {"sawtooth",  "saw"},
    {"square",    "sqr"},
    {"lowpass",   "lp"},
    {"highpass",  "hp"},
    {"bandpass",  "bp"},
    {"output",    "out"},
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
