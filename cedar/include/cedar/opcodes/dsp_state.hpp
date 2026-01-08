#pragma once

#include <cstdint>
#include <variant>

namespace cedar {

// Oscillator state - maintains phase for continuity
struct OscState {
    float phase = 0.0f;  // 0.0 to 1.0
};

// Biquad filter state - 2 samples of I/O history plus coefficients
struct BiquadState {
    // Filter memory
    float x1 = 0.0f, x2 = 0.0f;  // Input history
    float y1 = 0.0f, y2 = 0.0f;  // Output history

    // Cached coefficients (normalized by a0)
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;

    // Last parameters (for coefficient caching)
    float last_freq = -1.0f;
    float last_q = -1.0f;
};

// SVF (State Variable Filter) state
struct SVFState {
    float ic1eq = 0.0f;
    float ic2eq = 0.0f;

    // Cached coefficients
    float g = 0.0f;
    float k = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    float a3 = 0.0f;

    // Last parameters
    float last_freq = -1.0f;
    float last_q = -1.0f;
};

// Noise generator state (LCG for deterministic noise)
struct NoiseState {
    std::uint32_t seed = 12345;
};

// Slew rate limiter state
struct SlewState {
    float current = 0.0f;
};

// Sample and hold state
struct SAHState {
    float held_value = 0.0f;
    float prev_trigger = 0.0f;
};

// Delay state - placeholder for future implementation
struct DelayState {
    // Will contain delay line buffer pointer and indices
    float* buffer = nullptr;
    std::size_t write_pos = 0;
    std::size_t max_samples = 0;
};

// Envelope state - placeholder for future implementation
struct EnvState {
    float level = 0.0f;
    std::uint8_t stage = 0;  // 0=idle, 1=attack, 2=decay, 3=sustain, 4=release
    float time_in_stage = 0.0f;
};

// Variant holding all possible DSP state types
// std::monostate represents stateless operations
using DSPState = std::variant<
    std::monostate,
    OscState,
    BiquadState,
    SVFState,
    NoiseState,
    SlewState,
    SAHState,
    DelayState,
    EnvState
>;

}  // namespace cedar
