#pragma once

#include "../dsp/constants.hpp"
#include <array>
#include <cmath>
#include <cstdint>

namespace cedar {

// Crossfade configuration
struct CrossfadeConfig {
    static constexpr std::uint32_t DEFAULT_BLOCKS = 3;  // ~8ms at 128 samples/48kHz
    static constexpr std::uint32_t MIN_BLOCKS = 2;      // ~5.3ms
    static constexpr std::uint32_t MAX_BLOCKS = 5;      // ~13.3ms

    std::uint32_t duration_blocks = DEFAULT_BLOCKS;

    void set_duration(std::uint32_t blocks) noexcept {
        if (blocks < MIN_BLOCKS) blocks = MIN_BLOCKS;
        if (blocks > MAX_BLOCKS) blocks = MAX_BLOCKS;
        duration_blocks = blocks;
    }
};

// State machine for crossfade execution
struct CrossfadeState {
    enum class Phase : std::uint8_t {
        Idle,           // No crossfade active
        Pending,        // New program ready, will start next block
        Active,         // Crossfading between old and new
        Completing      // Final block, cleaning up
    };

    Phase phase = Phase::Idle;
    std::uint32_t blocks_remaining = 0;
    std::uint32_t total_blocks = 0;

    // Crossfade position (0.0 = all old, 1.0 = all new)
    [[nodiscard]] float position() const noexcept {
        if (total_blocks == 0) return 1.0f;
        return 1.0f - (static_cast<float>(blocks_remaining) /
                       static_cast<float>(total_blocks));
    }

    // Begin a new crossfade
    void begin(std::uint32_t duration_blocks) noexcept {
        phase = Phase::Pending;
        blocks_remaining = duration_blocks;
        total_blocks = duration_blocks;
    }

    // Advance one block
    void advance() noexcept {
        if (phase == Phase::Pending) {
            phase = Phase::Active;
        }

        if (phase == Phase::Active && blocks_remaining > 0) {
            --blocks_remaining;
            if (blocks_remaining == 0) {
                phase = Phase::Completing;
            }
        }
    }

    // Complete the crossfade
    void complete() noexcept {
        phase = Phase::Idle;
        blocks_remaining = 0;
        total_blocks = 0;
    }

    [[nodiscard]] bool is_active() const noexcept {
        return phase == Phase::Active || phase == Phase::Pending;
    }

    [[nodiscard]] bool is_completing() const noexcept {
        return phase == Phase::Completing;
    }

    [[nodiscard]] bool is_idle() const noexcept {
        return phase == Phase::Idle;
    }
};

// Temporary buffers for crossfade mixing
struct CrossfadeBuffers {
    alignas(32) std::array<float, BLOCK_SIZE> old_left{};
    alignas(32) std::array<float, BLOCK_SIZE> old_right{};
    alignas(32) std::array<float, BLOCK_SIZE> new_left{};
    alignas(32) std::array<float, BLOCK_SIZE> new_right{};

    // Mix old and new with equal-power crossfade
    // Equal-power maintains perceived loudness during transition
    void mix_equal_power(float* out_left, float* out_right, float position) noexcept {
        // Equal-power: old_gain = cos(pos * PI/2), new_gain = sin(pos * PI/2)
        const float angle = position * HALF_PI;
        const float old_gain = std::cos(angle);
        const float new_gain = std::sin(angle);

        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            out_left[i] = old_left[i] * old_gain + new_left[i] * new_gain;
            out_right[i] = old_right[i] * old_gain + new_right[i] * new_gain;
        }
    }

    // Linear crossfade (simpler, slight loudness dip at center)
    void mix_linear(float* out_left, float* out_right, float position) noexcept {
        const float old_gain = 1.0f - position;
        const float new_gain = position;

        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            out_left[i] = old_left[i] * old_gain + new_left[i] * new_gain;
            out_right[i] = old_right[i] * old_gain + new_right[i] * new_gain;
        }
    }

    // Clear all buffers
    void clear() noexcept {
        old_left.fill(0.0f);
        old_right.fill(0.0f);
        new_left.fill(0.0f);
        new_right.fill(0.0f);
    }
};

}  // namespace cedar
