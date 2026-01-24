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
    OSC_SQR_MINBLEP = 26,      // Square wave with MinBLEP (perfect harmonic purity)
    OSC_SQR_PWM = 27,          // Square wave with PWM (PolyBLEP)
    OSC_SAW_PWM = 28,          // Variable-slope saw (morphs saw↔tri↔ramp)
    OSC_SQR_PWM_MINBLEP = 29,  // Square PWM with MinBLEP (highest quality)

    // Filters (30-39)
    // Note: Opcodes 30-32 removed (biquad filters deprecated in favor of SVF)
    FILTER_SVF_LP = 33,
    FILTER_SVF_HP = 34,
    FILTER_SVF_BP = 35,
    FILTER_MOOG = 36,       // 4-pole Moog-style ladder filter with resonance
    FILTER_DIODE = 37,      // ZDF diode ladder filter (TB-303 acid)
    FILTER_FORMANT = 38,    // 3-band vowel morphing filter
    FILTER_SALLENKEY = 39,  // MS-20 style filter with diode feedback

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
    ENV_FOLLOWER = 62,

    // Samplers (63-69)
    SAMPLE_PLAY = 63,       // One-shot sample playback
    SAMPLE_PLAY_LOOP = 64,  // Looping sample playback

    // Delays & Reverbs (70-79)
    DELAY = 70,
    REVERB_FREEVERB = 71,    // Schroeder-Moorer algorithm
    REVERB_DATTORRO = 72,    // Plate reverb
    REVERB_FDN = 73,         // Feedback Delay Network

    // Effects - Modulation (80-83)
    EFFECT_CHORUS = 80,      // Multi-voice chorus
    EFFECT_FLANGER = 81,     // Modulated delay with feedback
    EFFECT_PHASER = 82,      // Cascaded allpass filters
    EFFECT_COMB = 83,        // Feedback comb filter

    // Effects - Distortion (84-89, 96-99)
    DISTORT_TANH = 84,       // Tanh saturation
    DISTORT_SOFT = 85,       // Polynomial soft clipping
    DISTORT_BITCRUSH = 86,   // Bit/sample rate reduction
    DISTORT_FOLD = 87,       // Wavefolder
    DISTORT_TUBE = 88,       // Asymmetric tube saturation (even harmonics)
    DISTORT_SMOOTH = 89,     // ADAA alias-free saturation

    // Sequencers & Timing (90-95)
    CLOCK = 90,       // Beat/bar phase output (rate field: 0=beat, 1=bar, 2=cycle)
    LFO = 91,         // Beat-synced LFO (reserved field: shape 0-6)
    SEQ_STEP = 92,    // Step sequencer
    EUCLID = 93,      // Euclidean rhythm trigger generator
    TRIGGER = 94,     // Beat-division impulse generator
    TIMELINE = 95,    // Breakpoint automation

    // Effects - Distortion continued (96-99)
    DISTORT_TAPE = 96,       // Tape-style saturation with warmth
    DISTORT_XFMR = 97,       // Transformer saturation (bass emphasis)
    DISTORT_EXCITE = 98,     // Harmonic exciter (HF enhancement)

    // Dynamics (100-109)
    DYNAMICS_COMP = 100,     // Feedforward compressor
    DYNAMICS_LIMITER = 101,  // Brick-wall limiter
    DYNAMICS_GATE = 102,     // Noise gate with hysteresis

    // Oversampled Oscillators (110-119) - for alias-free FM synthesis
    OSC_SIN_2X = 110,        // 2x oversampled sine
    OSC_SIN_4X = 111,        // 4x oversampled sine
    OSC_SAW_2X = 112,        // 2x oversampled saw with PolyBLEP
    OSC_SAW_4X = 113,        // 4x oversampled saw with PolyBLEP
    OSC_SQR_2X = 114,        // 2x oversampled square with PolyBLEP
    OSC_SQR_4X = 115,        // 4x oversampled square with PolyBLEP
    OSC_TRI_2X = 116,        // 2x oversampled triangle
    OSC_TRI_4X = 117,        // 4x oversampled triangle
    OSC_SQR_PWM_4X = 118,    // 4x oversampled PWM square
    OSC_SAW_PWM_4X = 119,    // 4x oversampled variable-slope saw

    // Trigonometric Math (120-129) - pure mathematical functions
    MATH_SIN = 120,          // sin(x) - radians, NOT oscillator
    MATH_COS = 121,          // cos(x)
    MATH_TAN = 122,          // tan(x)
    MATH_ASIN = 123,         // asin(x)
    MATH_ACOS = 124,         // acos(x)
    MATH_ATAN = 125,         // atan(x)
    MATH_ATAN2 = 126,        // atan2(y, x) - binary

    // Hyperbolic Math (130-139)
    MATH_SINH = 130,         // sinh(x)
    MATH_COSH = 131,         // cosh(x)
    MATH_TANH = 132,         // tanh(x) - pure math, also useful as waveshaper

    INVALID = 255
};

// 160-bit (20 byte) fixed-width instruction for fast decoding
// Layout: [opcode:8][rate:8][out:16][in0:16][in1:16][in2:16][in3:16][in4:16][state_id:32]
// Note: rate field also used for extra packed parameters (e.g., LFO shape)
// State ID uses full 32-bit FNV-1a hash to avoid collisions (birthday paradox at 256 states with 16-bit)
struct alignas(4) Instruction {
    Opcode opcode;              // Operation to perform
    std::uint8_t rate;          // 0=audio-rate, 1=control-rate, or packed params
    std::uint16_t out_buffer;   // Output buffer index
    std::uint16_t inputs[5];    // Input buffer indices (0xFFFF = unused)
    std::uint32_t state_id;     // Semantic hash for state lookup (full 32-bit FNV-1a)

    // Convenience constructors
    static Instruction make_nullary(Opcode op, std::uint16_t out, std::uint32_t state = 0) {
        return {op, 0, out, {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}, state};
    }

    static Instruction make_unary(Opcode op, std::uint16_t out, std::uint16_t in0, std::uint32_t state = 0) {
        return {op, 0, out, {in0, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}, state};
    }

    static Instruction make_binary(Opcode op, std::uint16_t out, std::uint16_t in0, std::uint16_t in1, std::uint32_t state = 0) {
        return {op, 0, out, {in0, in1, 0xFFFF, 0xFFFF, 0xFFFF}, state};
    }

    static Instruction make_ternary(Opcode op, std::uint16_t out, std::uint16_t in0, std::uint16_t in1, std::uint16_t in2, std::uint32_t state = 0) {
        return {op, 0, out, {in0, in1, in2, 0xFFFF, 0xFFFF}, state};
    }

    static Instruction make_quaternary(Opcode op, std::uint16_t out, std::uint16_t in0, std::uint16_t in1, std::uint16_t in2, std::uint16_t in3, std::uint32_t state = 0) {
        return {op, 0, out, {in0, in1, in2, in3, 0xFFFF}, state};
    }

    static Instruction make_quinary(Opcode op, std::uint16_t out, std::uint16_t in0, std::uint16_t in1, std::uint16_t in2, std::uint16_t in3, std::uint16_t in4, std::uint32_t state = 0) {
        return {op, 0, out, {in0, in1, in2, in3, in4}, state};
    }
};

static_assert(sizeof(Instruction) == 20, "Instruction must be 20 bytes (160-bit)");

}  // namespace cedar
