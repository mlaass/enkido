#pragma once

#include <cstdint>
#include <memory>
#include <algorithm>
#include <variant>

namespace cedar {

// Oscillator state - maintains phase for continuity
struct OscState {
    float phase = 0.0f;       // 0.0 to 1.0
    float prev_phase = 0.0f;  // Previous phase for PolyBLEP discontinuity detection
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

// Delay state with dynamically allocated ring buffer
// Note: Uses heap allocation for the buffer, but only when the delay is actually used
struct DelayState {
    // Maximum delay time: 2 seconds at 96kHz = 192000 samples
    static constexpr std::size_t MAX_DELAY_SAMPLES = 192000;

    // Ring buffer (lazily allocated)
    std::unique_ptr<float[]> buffer = nullptr;
    std::size_t buffer_size = 0;    // Actual allocated size
    std::size_t write_pos = 0;

    // Ensure buffer is allocated with requested size
    void ensure_buffer(std::size_t samples) {
        if (!buffer || buffer_size < samples) {
            std::size_t new_size = std::min(samples, MAX_DELAY_SAMPLES);
            buffer = std::make_unique<float[]>(new_size);
            std::fill_n(buffer.get(), new_size, 0.0f);
            buffer_size = new_size;
            write_pos = 0;
        }
    }

    // Reset buffer to silence (for seek)
    void reset() {
        if (buffer && buffer_size > 0) {
            std::fill_n(buffer.get(), buffer_size, 0.0f);
            write_pos = 0;
        }
    }
};

// Envelope state for ADSR
struct EnvState {
    float level = 0.0f;
    std::uint8_t stage = 0;     // 0=idle, 1=attack, 2=decay, 3=sustain, 4=release
    float time_in_stage = 0.0f;
    float prev_gate = 0.0f;     // For gate edge detection
    float release_level = 0.0f; // Level when release triggered (for smooth release)

    // Cached exponential coefficients for each stage
    float attack_coeff = 0.0f;
    float decay_coeff = 0.0f;
    float release_coeff = 0.0f;

    // Cached parameters for coefficient invalidation
    float last_attack = -1.0f;
    float last_decay = -1.0f;
    float last_release = -1.0f;
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

// Moog-style 4-pole ladder filter state
struct MoogState {
    // 4 cascaded 1-pole lowpass stages
    float stage[4] = {};
    float delay[4] = {};  // Unit delays for trapezoidal integration

    // Cached parameters for coefficient invalidation
    float last_freq = -1.0f;
    float last_res = -1.0f;

    // Cached coefficients
    float g = 0.0f;  // Cutoff coefficient (tan-based)
    float k = 0.0f;  // Resonance coefficient (0-4 range)
};

// Variant holding all possible DSP state types
// std::monostate represents stateless operations
using DSPState = std::variant<
    std::monostate,
    OscState,
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
    TimelineState,
    // Additional filter states
    MoogState
>;

}  // namespace cedar
