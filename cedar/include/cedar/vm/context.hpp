#pragma once

#include "../dsp/constants.hpp"
#include "buffer_pool.hpp"
#include "state_pool.hpp"
#include <cmath>
#include <cstdint>

namespace cedar {

// Forward declaration
class EnvMap;

// Execution context passed to every opcode
// Contains all runtime state needed for audio processing
struct ExecutionContext {
    // Buffer pool (registers for signal flow)
    BufferPool* buffers = nullptr;

    // State pool (persistent DSP state)
    StatePool* states = nullptr;

    // Environment parameter map (external inputs)
    EnvMap* env_map = nullptr;

    // Output buffers (stereo, provided by caller)
    float* output_left = nullptr;
    float* output_right = nullptr;

    // Audio parameters
    float sample_rate = DEFAULT_SAMPLE_RATE;
    float inv_sample_rate = 1.0f / DEFAULT_SAMPLE_RATE;
    float bpm = DEFAULT_BPM;

    // Timing
    std::uint64_t global_sample_counter = 0;
    std::uint64_t block_counter = 0;

    // Derived timing values (updated per block)
    float beat_phase = 0.0f;       // 0-1 phase within current beat
    float bar_phase = 0.0f;        // 0-1 phase within current bar (4 beats)

    // Update derived timing values
    void update_timing() {
        float samples_per_beat = (60.0f / bpm) * sample_rate;
        float samples_per_bar = samples_per_beat * 4.0f;

        float sample_in_beat = static_cast<float>(global_sample_counter % static_cast<std::uint64_t>(samples_per_beat));
        float sample_in_bar = static_cast<float>(global_sample_counter % static_cast<std::uint64_t>(samples_per_bar));

        beat_phase = sample_in_beat / samples_per_beat;
        bar_phase = sample_in_bar / samples_per_bar;
    }

    // Set sample rate and update derived values
    void set_sample_rate(float rate) {
        sample_rate = rate;
        inv_sample_rate = 1.0f / rate;
    }

    // Timing helper methods for sequencing opcodes
    [[nodiscard]] float samples_per_beat() const noexcept {
        return (60.0f / bpm) * sample_rate;
    }

    [[nodiscard]] float samples_per_bar() const noexcept {
        return samples_per_beat() * 4.0f;
    }

    [[nodiscard]] float samples_per_cycle() const noexcept {
        return samples_per_bar();  // 1 cycle = 4 beats = 1 bar
    }

    // Get beat position for a specific sample offset within current block
    [[nodiscard]] float beat_at_sample(std::size_t sample_offset) const noexcept {
        return static_cast<float>(global_sample_counter + sample_offset) / samples_per_beat();
    }

    // Get phase (0-1) within beat for a sample offset
    [[nodiscard]] float beat_phase_at_sample(std::size_t sample_offset) const noexcept {
        float spb = samples_per_beat();
        return std::fmod(static_cast<float>(global_sample_counter + sample_offset), spb) / spb;
    }
};

}  // namespace cedar
