#pragma once

#include <cstdint>

namespace cedar {

// Opcode categories organized for easy extension
// Each category has room for 10 opcodes
enum class Opcode : std::uint8_t {
    // Stack/Constants (0-9)
    NOP = 0,
    PUSH_CONST = 1,   // Fill buffer with constant value
    COPY = 2,         // Copy buffer to buffer

    // Arithmetic (10-19)
    ADD = 10,
    SUB = 11,
    MUL = 12,
    DIV = 13,
    POW = 14,
    NEG = 15,         // Negate

    // Oscillators (20-29)
    OSC_SIN = 20,
    OSC_TRI = 21,
    OSC_SAW = 22,
    OSC_SQR = 23,
    OSC_RAMP = 24,
    OSC_PHASOR = 25,

    // Filters (30-39)
    // Note: Opcodes 30-32 removed (biquad filters deprecated in favor of SVF)
    FILTER_SVF_LP = 33,
    FILTER_SVF_HP = 34,
    FILTER_SVF_BP = 35,
    FILTER_MOOG = 36,     // 4-pole Moog-style ladder filter with resonance

    // Math (40-49)
    ABS = 40,
    SQRT = 41,
    LOG = 42,
    EXP = 43,
    MIN = 44,
    MAX = 45,
    CLAMP = 46,
    WRAP = 47,
    FLOOR = 48,
    CEIL = 49,

    // Utility (50-59)
    OUTPUT = 50,      // Write to output buffer
    NOISE = 51,       // White noise
    MTOF = 52,        // MIDI to frequency
    DC = 53,          // DC offset
    SLEW = 54,        // Slew rate limiter
    SAH = 55,         // Sample and hold
    ENV_GET = 56,     // Read external parameter from EnvMap

    // Envelopes (60-69) - reserved
    ENV_ADSR = 60,
    ENV_AR = 61,

    // Delays (70-79) - reserved
    DELAY = 70,

    // Effects (80-89) - reserved

    // Sequencers & Timing (90-99)
    CLOCK = 90,       // Beat/bar phase output (rate field: 0=beat, 1=bar, 2=cycle)
    LFO = 91,         // Beat-synced LFO (reserved field: shape 0-6)
    SEQ_STEP = 92,    // Step sequencer
    EUCLID = 93,      // Euclidean rhythm trigger generator
    TRIGGER = 94,     // Beat-division impulse generator
    TIMELINE = 95,    // Breakpoint automation

    INVALID = 255
};

// 128-bit (16 byte) fixed-width instruction for fast decoding
// Layout: [opcode:8][rate:8][out:16][in0:16][in1:16][in2:16][reserved:16][state_id:32]
struct alignas(16) Instruction {
    Opcode opcode;              // Operation to perform
    std::uint8_t rate;          // 0=audio-rate, 1=control-rate
    std::uint16_t out_buffer;   // Output buffer index
    std::uint16_t inputs[3];    // Input buffer indices (0xFFFF = unused)
    std::uint16_t reserved;     // Reserved for future use
    std::uint32_t state_id;     // Semantic hash for state lookup (32-bit FNV-1a)

    // Convenience constructors
    static Instruction make_nullary(Opcode op, std::uint16_t out, std::uint32_t state = 0) {
        return {op, 0, out, {0xFFFF, 0xFFFF, 0xFFFF}, 0, state};
    }

    static Instruction make_unary(Opcode op, std::uint16_t out, std::uint16_t in0, std::uint32_t state = 0) {
        return {op, 0, out, {in0, 0xFFFF, 0xFFFF}, 0, state};
    }

    static Instruction make_binary(Opcode op, std::uint16_t out, std::uint16_t in0, std::uint16_t in1, std::uint32_t state = 0) {
        return {op, 0, out, {in0, in1, 0xFFFF}, 0, state};
    }

    static Instruction make_ternary(Opcode op, std::uint16_t out, std::uint16_t in0, std::uint16_t in1, std::uint16_t in2, std::uint32_t state = 0) {
        return {op, 0, out, {in0, in1, in2}, 0, state};
    }
};

static_assert(sizeof(Instruction) == 16, "Instruction must be 16 bytes (128-bit)");

}  // namespace cedar
