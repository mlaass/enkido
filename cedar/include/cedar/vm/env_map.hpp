#pragma once

#include "../dsp/constants.hpp"
#include "state_pool.hpp"  // For fnv1a_hash
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace cedar {

// Single environment parameter with atomic target for lock-free access
struct EnvParam {
    std::atomic<float> target{0.0f};  // Written by host, read by audio
    float current = 0.0f;              // Interpolated value (audio thread only)
    float slew_coeff = 0.05f;          // Per-sample smoothing coefficient
    std::atomic<bool> active{false};   // Is this slot in use?
};

// Hash table slot for name -> index mapping
struct EnvParamSlot {
    std::atomic<std::uint32_t> name_hash{0};   // FNV-1a hash of parameter name
    std::atomic<std::uint16_t> param_index{0}; // Index into params array
    std::atomic<bool> occupied{false};
};

// Thread-safe environment parameter map for external input binding
// Host thread writes values, audio thread reads with automatic interpolation
class EnvMap {
public:
    static constexpr std::size_t HASH_TABLE_SIZE = 512;  // Power of 2 for fast modulo
    static constexpr float DEFAULT_SLEW_MS = 5.0f;       // 5ms default smoothing

    EnvMap() = default;

    // =========================================================================
    // Host Thread API (thread-safe writes)
    // =========================================================================

    // Set parameter value (creates if doesn't exist)
    // Returns false if MAX_ENV_PARAMS reached
    bool set_param(const char* name, float value) {
        return set_param(name, value, default_slew_ms_);
    }

    // Set parameter with custom slew time in milliseconds
    bool set_param(const char* name, float value, float slew_ms) {
        std::uint32_t hash = fnv1a_hash_runtime(name, std::strlen(name));

        // Find existing or create new slot
        std::int16_t index = find_or_create_slot(hash);
        if (index < 0) {
            return false;  // No space available
        }

        auto& param = params_[static_cast<std::size_t>(index)];
        bool was_active = param.active.load(std::memory_order_acquire);
        
        param.target.store(value, std::memory_order_relaxed);

        // Calculate slew coefficient from milliseconds
        float coeff = calc_slew_coeff(slew_ms);
        param.slew_coeff = coeff;
        
        // If this is a new parameter, initialize current to target to avoid ramping from zero
        if (!was_active) {
            param.current = value;
        }
        
        param.active.store(true, std::memory_order_release);

        return true;
    }

    // Remove parameter (mark as inactive)
    void remove_param(const char* name) {
        std::uint32_t hash = fnv1a_hash_runtime(name, std::strlen(name));
        std::int16_t slot_idx = find_slot(hash);
        if (slot_idx >= 0) {
            std::uint16_t param_idx = hash_table_[static_cast<std::size_t>(slot_idx)]
                                        .param_index.load(std::memory_order_acquire);
            params_[param_idx].active.store(false, std::memory_order_release);
            hash_table_[static_cast<std::size_t>(slot_idx)].occupied.store(
                false, std::memory_order_release);
        }
    }

    // Check if parameter exists
    [[nodiscard]] bool has_param(const char* name) const {
        std::uint32_t hash = fnv1a_hash_runtime(name, std::strlen(name));
        return find_slot(hash) >= 0;
    }

    // =========================================================================
    // Audio Thread API (lock-free reads)
    // =========================================================================

    // Get interpolated value for parameter by hash
    // Returns 0.0 if parameter doesn't exist
    [[nodiscard]] float get(std::uint32_t name_hash) const {
        std::int16_t slot_idx = find_slot(name_hash);
        if (slot_idx < 0) {
            return 0.0f;
        }
        std::uint16_t param_idx = hash_table_[static_cast<std::size_t>(slot_idx)]
                                    .param_index.load(std::memory_order_acquire);
        const auto& param = params_[param_idx];
        if (!param.active.load(std::memory_order_acquire)) {
            return 0.0f;
        }
        return param.current;
    }

    // Get raw target value (no interpolation)
    [[nodiscard]] float get_target(std::uint32_t name_hash) const {
        std::int16_t slot_idx = find_slot(name_hash);
        if (slot_idx < 0) {
            return 0.0f;
        }
        std::uint16_t param_idx = hash_table_[static_cast<std::size_t>(slot_idx)]
                                    .param_index.load(std::memory_order_acquire);
        return params_[param_idx].target.load(std::memory_order_relaxed);
    }

    // Check if parameter exists by hash (for audio thread)
    [[nodiscard]] bool has_param_hash(std::uint32_t name_hash) const {
        std::int16_t slot_idx = find_slot(name_hash);
        if (slot_idx < 0) {
            return false;
        }
        std::uint16_t param_idx = hash_table_[static_cast<std::size_t>(slot_idx)]
                                    .param_index.load(std::memory_order_acquire);
        return params_[param_idx].active.load(std::memory_order_acquire);
    }

    // Update all interpolations (call once per sample)
    void update_interpolation_sample() {
        std::uint16_t count = param_count_.load(std::memory_order_acquire);
        for (std::uint16_t i = 0; i < count; ++i) {
            auto& p = params_[i];
            if (p.active.load(std::memory_order_relaxed)) {
                float target = p.target.load(std::memory_order_relaxed);
                p.current += (target - p.current) * p.slew_coeff;
            }
        }
    }

    // Update all interpolations for a block (optimized version)
    void update_interpolation_block() {
        std::uint16_t count = param_count_.load(std::memory_order_acquire);
        for (std::uint16_t i = 0; i < count; ++i) {
            auto& p = params_[i];
            if (p.active.load(std::memory_order_relaxed)) {
                float target = p.target.load(std::memory_order_relaxed);
                // Apply multiple iterations for block-rate update
                for (std::size_t s = 0; s < BLOCK_SIZE; ++s) {
                    p.current += (target - p.current) * p.slew_coeff;
                }
            }
        }
    }

    // =========================================================================
    // Configuration
    // =========================================================================

    void set_sample_rate(float rate) {
        sample_rate_ = rate;
        // Recalculate slew coefficients for all active params would require
        // storing slew_ms per param. For simplicity, only affects new params.
    }

    void set_default_slew_ms(float ms) {
        default_slew_ms_ = ms;
    }

    // =========================================================================
    // Query
    // =========================================================================

    [[nodiscard]] std::size_t param_count() const {
        return param_count_.load(std::memory_order_acquire);
    }

    void reset() {
        for (auto& slot : hash_table_) {
            slot.occupied.store(false, std::memory_order_relaxed);
            slot.name_hash.store(0, std::memory_order_relaxed);
            slot.param_index.store(0, std::memory_order_relaxed);
        }
        for (auto& param : params_) {
            param.active.store(false, std::memory_order_relaxed);
            param.target.store(0.0f, std::memory_order_relaxed);
            param.current = 0.0f;
        }
        param_count_.store(0, std::memory_order_release);
    }

private:
    // Find slot for parameter hash, returns -1 if not found
    [[nodiscard]] std::int16_t find_slot(std::uint32_t name_hash) const {
        std::size_t start = name_hash % HASH_TABLE_SIZE;

        for (std::size_t i = 0; i < HASH_TABLE_SIZE; ++i) {
            std::size_t idx = (start + i) % HASH_TABLE_SIZE;
            const auto& slot = hash_table_[idx];

            if (!slot.occupied.load(std::memory_order_acquire)) {
                return -1;  // Empty slot means not found
            }

            if (slot.name_hash.load(std::memory_order_acquire) == name_hash) {
                return static_cast<std::int16_t>(idx);
            }
        }
        return -1;  // Table full, not found
    }

    // Find or create slot for parameter hash
    // Returns param index on success, -1 if table full
    [[nodiscard]] std::int16_t find_or_create_slot(std::uint32_t name_hash) {
        std::size_t start = name_hash % HASH_TABLE_SIZE;

        for (std::size_t i = 0; i < HASH_TABLE_SIZE; ++i) {
            std::size_t idx = (start + i) % HASH_TABLE_SIZE;
            auto& slot = hash_table_[idx];

            // Check if already exists
            if (slot.occupied.load(std::memory_order_acquire)) {
                if (slot.name_hash.load(std::memory_order_acquire) == name_hash) {
                    return static_cast<std::int16_t>(
                        slot.param_index.load(std::memory_order_acquire));
                }
                continue;  // Collision, try next slot
            }

            // Empty slot - try to claim it
            bool expected = false;
            if (slot.occupied.compare_exchange_strong(expected, true,
                    std::memory_order_acq_rel)) {
                // Successfully claimed slot
                std::uint16_t param_idx = param_count_.fetch_add(1,
                    std::memory_order_acq_rel);

                if (param_idx >= MAX_ENV_PARAMS) {
                    // Rollback
                    param_count_.fetch_sub(1, std::memory_order_relaxed);
                    slot.occupied.store(false, std::memory_order_release);
                    return -1;
                }

                slot.name_hash.store(name_hash, std::memory_order_release);
                slot.param_index.store(param_idx, std::memory_order_release);

                // Initialize param
                params_[param_idx].current = 0.0f;
                params_[param_idx].target.store(0.0f, std::memory_order_relaxed);

                return static_cast<std::int16_t>(param_idx);
            }
            // Another thread claimed it, check if it's our hash
            if (slot.name_hash.load(std::memory_order_acquire) == name_hash) {
                return static_cast<std::int16_t>(
                    slot.param_index.load(std::memory_order_acquire));
            }
        }
        return -1;  // Table full
    }

    // Calculate slew coefficient from time in milliseconds
    [[nodiscard]] float calc_slew_coeff(float slew_ms) const {
        if (slew_ms <= 0.0f) {
            return 1.0f;  // Instant
        }
        // coeff such that we reach ~63% of target in slew_ms
        // Using simple approximation: coeff = 1 / (samples)
        float samples = slew_ms * sample_rate_ * 0.001f;
        float coeff = 1.0f / samples;
        return std::clamp(coeff, 0.0001f, 1.0f);
    }

    // Parameter storage (pre-allocated)
    std::array<EnvParam, MAX_ENV_PARAMS> params_{};
    std::atomic<std::uint16_t> param_count_{0};

    // Hash table for name -> index lookup
    std::array<EnvParamSlot, HASH_TABLE_SIZE> hash_table_{};

    // Configuration
    float sample_rate_ = DEFAULT_SAMPLE_RATE;
    float default_slew_ms_ = DEFAULT_SLEW_MS;
};

}  // namespace cedar
