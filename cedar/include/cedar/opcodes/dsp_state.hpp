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

// ============================================================================
// Sequencing & Timing States
// ============================================================================

// LFO state - beat-synced low frequency oscillator
struct LFOState {
    float phase = 0.0f;        // 0.0 to 1.0
    float prev_value = 0.0f;   // For SAH mode (last sampled value)
};

// Step sequencer state
struct SeqStepState {
    float phase = 0.0f;              // Current position within step (0-1)
    std::uint32_t current_step = 0;  // Current step index

    // Embedded sequence data for cache locality
    static constexpr std::size_t MAX_STEPS = 32;
    float values[MAX_STEPS] = {};
    std::uint32_t num_steps = 0;
};

// Euclidean rhythm generator state
struct EuclidState {
    float phase = 0.0f;              // Phase within current step
    std::uint32_t current_step = 0;  // Current step in pattern

    // Precomputed pattern as bitmask (1 = trigger, 0 = rest)
    std::uint32_t pattern = 0;

    // Cached parameters for invalidation
    std::uint32_t last_hits = 0;
    std::uint32_t last_steps = 0;
    std::uint32_t last_rotation = 0;
};

// Trigger/impulse generator state
struct TriggerState {
    float phase = 0.0f;  // Phase within trigger period
};

// Timeline/breakpoint automation state
struct TimelineState {
    static constexpr std::size_t MAX_BREAKPOINTS = 64;

    struct Breakpoint {
        float time = 0.0f;      // Time in beats
        float value = 0.0f;     // Target value
        std::uint8_t curve = 0; // 0=linear, 1=exponential, 2=hold
    };

    Breakpoint points[MAX_BREAKPOINTS] = {};
    std::uint32_t num_points = 0;
    bool loop = false;
    float loop_length = 0.0f;  // Loop length in beats (0 = no loop)
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
    EnvState,
    // Sequencing states
    LFOState,
    SeqStepState,
    EuclidState,
    TriggerState,
    TimelineState
>;

}  // namespace cedar
