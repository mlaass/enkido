#pragma once

#include <cstdint>
#include <memory>
#include <algorithm>
#include <variant>
#include <cstring>
#include "../vm/audio_arena.hpp"

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

// Delay state with arena-allocated ring buffer
// Buffer is allocated from AudioArena on first use (zero heap allocation during audio)
struct DelayState {
    // Maximum delay time: 2 seconds at 96kHz = 192000 samples
    static constexpr std::size_t MAX_DELAY_SAMPLES = 192000;

    // Ring buffer (allocated from arena)
    float* buffer = nullptr;
    std::size_t buffer_size = 0;    // Allocated size in floats
    std::size_t write_pos = 0;

    // Ensure buffer is allocated with requested size
    // arena: AudioArena to allocate from (from ExecutionContext)
    void ensure_buffer(std::size_t samples, AudioArena* arena) {
        if (buffer && buffer_size >= samples) {
            return;  // Already have enough space
        }
        if (!arena) return;

        std::size_t new_size = std::min(samples, MAX_DELAY_SAMPLES);
        float* new_buffer = arena->allocate(new_size);
        if (new_buffer) {
            buffer = new_buffer;
            buffer_size = new_size;
            write_pos = 0;
        }
    }

    // Reset buffer to silence (for seek)
    void reset() {
        if (buffer && buffer_size > 0) {
            std::memset(buffer, 0, buffer_size * sizeof(float));
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

// ============================================================================
// Distortion States
// ============================================================================

// Bitcrusher state (sample rate reduction)
struct BitcrushState {
    float held_sample = 0.0f;
    float phase = 0.0f;
};

// ============================================================================
// Modulation Effect States
// ============================================================================

// Comb filter state with arena-allocated buffer
struct CombFilterState {
    static constexpr std::size_t MAX_COMB_SAMPLES = 4800;  // 100ms at 48kHz

    float* buffer = nullptr;
    std::size_t write_pos = 0;
    float filter_state = 0.0f;  // For damping lowpass

    void ensure_buffer(AudioArena* arena) {
        if (buffer) return;
        if (!arena) return;
        buffer = arena->allocate(MAX_COMB_SAMPLES);
    }

    void reset() {
        if (buffer) {
            std::memset(buffer, 0, MAX_COMB_SAMPLES * sizeof(float));
            write_pos = 0;
            filter_state = 0.0f;
        }
    }
};

// Flanger state with arena-allocated buffer
struct FlangerState {
    static constexpr std::size_t MAX_FLANGER_SAMPLES = 960;  // 20ms at 48kHz

    float* buffer = nullptr;
    std::size_t write_pos = 0;
    float lfo_phase = 0.0f;

    void ensure_buffer(AudioArena* arena) {
        if (buffer) return;
        if (!arena) return;
        buffer = arena->allocate(MAX_FLANGER_SAMPLES);
    }

    void reset() {
        if (buffer) {
            std::memset(buffer, 0, MAX_FLANGER_SAMPLES * sizeof(float));
            write_pos = 0;
        }
    }
};

// Chorus state (multi-voice) with arena-allocated buffer
struct ChorusState {
    static constexpr std::size_t MAX_CHORUS_SAMPLES = 2400;  // 50ms at 48kHz
    static constexpr std::size_t NUM_VOICES = 3;

    float* buffer = nullptr;
    std::size_t write_pos = 0;
    float lfo_phase = 0.0f;

    void ensure_buffer(AudioArena* arena) {
        if (buffer) return;
        if (!arena) return;
        buffer = arena->allocate(MAX_CHORUS_SAMPLES);
    }

    void reset() {
        if (buffer) {
            std::memset(buffer, 0, MAX_CHORUS_SAMPLES * sizeof(float));
            write_pos = 0;
        }
    }
};

// Phaser state (cascaded allpass filters)
struct PhaserState {
    static constexpr std::size_t NUM_STAGES = 12;  // Max stages

    float allpass_state[NUM_STAGES] = {};
    float allpass_delay[NUM_STAGES] = {};
    float lfo_phase = 0.0f;
    float last_output = 0.0f;
};

// ============================================================================
// Dynamics States
// ============================================================================

// Compressor state
struct CompressorState {
    float envelope = 0.0f;
    float gain_reduction = 1.0f;

    // Cached coefficients
    float attack_coeff = 0.0f;
    float release_coeff = 0.0f;
    float last_attack = -1.0f;
    float last_release = -1.0f;
};

// Limiter state with lookahead
struct LimiterState {
    static constexpr std::size_t LOOKAHEAD_SAMPLES = 48;  // 1ms at 48kHz

    float lookahead_buffer[LOOKAHEAD_SAMPLES] = {};
    std::size_t write_pos = 0;
    float gain = 1.0f;
};

// Gate state
struct GateState {
    float envelope = 0.0f;
    float gain = 0.0f;
    bool is_open = false;
    float hold_counter = 0.0f;

    // Cached coefficients
    float attack_coeff = 0.0f;
    float release_coeff = 0.0f;
    float last_attack = -1.0f;
    float last_release = -1.0f;
};

// ============================================================================
// Reverb States
// ============================================================================

// Freeverb state (Schroeder-Moorer algorithm) with arena-allocated buffers
struct FreeverbState {
    static constexpr std::size_t NUM_COMBS = 8;
    static constexpr std::size_t NUM_ALLPASSES = 4;

    // Comb filter delay times (samples at 48kHz, prime-like spacing)
    static constexpr std::size_t COMB_SIZES[NUM_COMBS] = {
        1557, 1617, 1491, 1422, 1277, 1356, 1188, 1116
    };
    // Allpass delay times
    static constexpr std::size_t ALLPASS_SIZES[NUM_ALLPASSES] = {225, 556, 441, 341};

    // Arena-allocated buffers
    float* comb_buffers[NUM_COMBS] = {};
    std::size_t comb_pos[NUM_COMBS] = {};
    float comb_filter_state[NUM_COMBS] = {};

    float* allpass_buffers[NUM_ALLPASSES] = {};
    std::size_t allpass_pos[NUM_ALLPASSES] = {};

    void ensure_buffers(AudioArena* arena) {
        if (!arena) return;
        for (std::size_t i = 0; i < NUM_COMBS; ++i) {
            if (!comb_buffers[i]) {
                comb_buffers[i] = arena->allocate(COMB_SIZES[i]);
            }
        }
        for (std::size_t i = 0; i < NUM_ALLPASSES; ++i) {
            if (!allpass_buffers[i]) {
                allpass_buffers[i] = arena->allocate(ALLPASS_SIZES[i]);
            }
        }
    }

    void reset() {
        for (std::size_t i = 0; i < NUM_COMBS; ++i) {
            if (comb_buffers[i]) {
                std::memset(comb_buffers[i], 0, COMB_SIZES[i] * sizeof(float));
                comb_pos[i] = 0;
                comb_filter_state[i] = 0.0f;
            }
        }
        for (std::size_t i = 0; i < NUM_ALLPASSES; ++i) {
            if (allpass_buffers[i]) {
                std::memset(allpass_buffers[i], 0, ALLPASS_SIZES[i] * sizeof(float));
                allpass_pos[i] = 0;
            }
        }
    }
};

// Dattorro plate reverb state with arena-allocated buffers
struct DattorroState {
    static constexpr std::size_t NUM_INPUT_DIFFUSERS = 4;
    static constexpr std::size_t PREDELAY_SIZE = 4800;  // 100ms at 48kHz
    static constexpr std::size_t MAX_DELAY_SIZE = 5000;

    // Input diffuser sizes (samples)
    static constexpr std::size_t INPUT_DIFFUSER_SIZES[NUM_INPUT_DIFFUSERS] = {142, 107, 379, 277};
    // Decay diffuser sizes
    static constexpr std::size_t DECAY_DIFFUSER_SIZES[2] = {672, 908};
    // Tank delay sizes
    static constexpr std::size_t DELAY_SIZES[2] = {4453, 4217};

    // Pre-delay buffer (static, small enough)
    float predelay_buffer[PREDELAY_SIZE] = {};
    std::size_t predelay_pos = 0;

    // Arena-allocated buffers
    float* input_diffusers[NUM_INPUT_DIFFUSERS] = {};
    std::size_t input_pos[NUM_INPUT_DIFFUSERS] = {};

    float* decay_diffusers[2] = {};
    std::size_t decay_pos[2] = {};

    float* delays[2] = {};
    std::size_t delay_pos[2] = {};

    // Damping filters
    float damp_state[2] = {};

    // Tank feedback (for figure-8 topology)
    float tank_feedback[2] = {};

    // Modulation
    float mod_phase = 0.0f;

    void ensure_buffers(AudioArena* arena) {
        if (!arena) return;
        for (std::size_t i = 0; i < NUM_INPUT_DIFFUSERS; ++i) {
            if (!input_diffusers[i]) {
                input_diffusers[i] = arena->allocate(INPUT_DIFFUSER_SIZES[i]);
            }
        }
        for (std::size_t i = 0; i < 2; ++i) {
            if (!decay_diffusers[i]) {
                decay_diffusers[i] = arena->allocate(DECAY_DIFFUSER_SIZES[i]);
            }
            if (!delays[i]) {
                delays[i] = arena->allocate(MAX_DELAY_SIZE);
            }
        }
    }

    void reset() {
        std::memset(predelay_buffer, 0, PREDELAY_SIZE * sizeof(float));
        predelay_pos = 0;
        for (std::size_t i = 0; i < NUM_INPUT_DIFFUSERS; ++i) {
            if (input_diffusers[i]) {
                std::memset(input_diffusers[i], 0, INPUT_DIFFUSER_SIZES[i] * sizeof(float));
                input_pos[i] = 0;
            }
        }
        for (std::size_t i = 0; i < 2; ++i) {
            if (decay_diffusers[i]) {
                std::memset(decay_diffusers[i], 0, DECAY_DIFFUSER_SIZES[i] * sizeof(float));
                decay_pos[i] = 0;
            }
            if (delays[i]) {
                std::memset(delays[i], 0, MAX_DELAY_SIZE * sizeof(float));
                delay_pos[i] = 0;
            }
            damp_state[i] = 0.0f;
            tank_feedback[i] = 0.0f;
        }
        mod_phase = 0.0f;
    }
};

// FDN (Feedback Delay Network) state
struct FDNState {
    static constexpr std::size_t NUM_DELAYS = 4;
    static constexpr std::size_t MAX_DELAY_SIZE = 5000;

    // Prime-ratio delay sizes for dense reverb
    static constexpr std::size_t DELAY_SIZES[NUM_DELAYS] = {1931, 2473, 3181, 3671};

    float delay_buffers[NUM_DELAYS][MAX_DELAY_SIZE] = {};
    std::size_t write_pos[NUM_DELAYS] = {};
    float damp_state[NUM_DELAYS] = {};

    void ensure_buffers([[maybe_unused]] AudioArena* arena) {
        // FDN uses static allocation, no arena needed
    }

    void reset() {
        for (std::size_t i = 0; i < NUM_DELAYS; ++i) {
            std::fill_n(delay_buffers[i], MAX_DELAY_SIZE, 0.0f);
            write_pos[i] = 0;
            damp_state[i] = 0.0f;
        }
    }
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
    // Filter states
    MoogState,
    // Distortion states
    BitcrushState,
    // Modulation states
    CombFilterState,
    FlangerState,
    ChorusState,
    PhaserState,
    // Dynamics states
    CompressorState,
    LimiterState,
    GateState,
    // Reverb states
    FreeverbState,
    DattorroState,
    FDNState
>;

}  // namespace cedar
