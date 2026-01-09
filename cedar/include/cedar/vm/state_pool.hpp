#pragma once

#include "../dsp/constants.hpp"
#include "../opcodes/dsp_state.hpp"
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

namespace cedar {

// State that is being faded out (orphaned during hot-swap)
struct FadingState {
    DSPState state;
    std::uint32_t blocks_remaining = 0;
    float fade_gain = 1.0f;       // 1.0 -> 0.0 during fade
    float fade_decrement = 0.0f;  // Per-block decrement
};

// FNV-1a 32-bit hash for semantic ID generation
inline constexpr std::uint32_t fnv1a_hash(const char* str) noexcept {
    std::uint32_t hash = 2166136261u;  // FNV-1a 32-bit offset basis
    while (*str) {
        hash ^= static_cast<std::uint32_t>(static_cast<unsigned char>(*str++));
        hash *= 16777619u;  // FNV-1a 32-bit prime
    }
    return hash;
}

// Runtime FNV-1a for string_view
inline std::uint32_t fnv1a_hash_runtime(const char* str, std::size_t len) noexcept {
    std::uint32_t hash = 2166136261u;
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= static_cast<std::uint32_t>(static_cast<unsigned char>(str[i]));
        hash *= 16777619u;
    }
    return hash;
}

// Persistent state pool for DSP blocks
// Uses semantic ID (FNV-1a 32-bit hash) for lookup to support hot-swapping
class StatePool {
public:
    StatePool() = default;

    // Get or create state for a given semantic ID
    // Template type T must be one of the DSPState variant alternatives
    template<typename T>
    [[nodiscard]] T& get_or_create(std::uint32_t state_id) {
        touched_.insert(state_id);

        auto it = states_.find(state_id);
        if (it == states_.end()) {
            auto [new_it, _] = states_.emplace(state_id, T{});
            return std::get<T>(new_it->second);
        }

        // If state exists but is wrong type, replace it
        if (!std::holds_alternative<T>(it->second)) {
            it->second = T{};
        }
        return std::get<T>(it->second);
    }

    // Get existing state (assumes it exists and is correct type)
    template<typename T>
    [[nodiscard]] T& get(std::uint32_t state_id) {
        touched_.insert(state_id);
        return std::get<T>(states_.at(state_id));
    }

    // Check if state exists
    [[nodiscard]] bool exists(std::uint32_t state_id) const {
        return states_.find(state_id) != states_.end();
    }

    // Mark state as touched (for GC tracking)
    void touch(std::uint32_t state_id) {
        touched_.insert(state_id);
    }

    // Clear touched set (call at start of program execution)
    void begin_frame() {
        touched_.clear();
    }

    // Garbage collect: move untouched states to fading pool
    // Call after hot-swap to begin fade-out of orphaned states
    void gc_sweep() {
        for (auto it = states_.begin(); it != states_.end();) {
            if (touched_.find(it->first) == touched_.end()) {
                // Move to fading pool instead of immediate deletion
                if (fade_blocks_ > 0) {
                    FadingState fs;
                    fs.state = std::move(it->second);
                    fs.blocks_remaining = fade_blocks_;
                    fs.fade_gain = 1.0f;
                    fs.fade_decrement = 1.0f / static_cast<float>(fade_blocks_);
                    fading_states_.emplace(it->first, std::move(fs));
                }
                it = states_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Reset all states (full program change)
    void reset() {
        states_.clear();
        touched_.clear();
        fading_states_.clear();
    }

    // Get number of active states
    [[nodiscard]] std::size_t size() const {
        return states_.size();
    }

    // =========================================================================
    // Fade-out tracking for orphaned states
    // =========================================================================

    // Set number of blocks for fade-out duration
    void set_fade_blocks(std::uint32_t blocks) {
        fade_blocks_ = blocks;
    }

    // Advance all fading states by one block
    void advance_fading() {
        for (auto& [id, fs] : fading_states_) {
            if (fs.blocks_remaining > 0) {
                fs.blocks_remaining--;
                fs.fade_gain -= fs.fade_decrement;
                if (fs.fade_gain < 0.0f) fs.fade_gain = 0.0f;
            }
        }
    }

    // Remove states that have finished fading
    void gc_fading() {
        for (auto it = fading_states_.begin(); it != fading_states_.end();) {
            if (it->second.blocks_remaining == 0) {
                it = fading_states_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Get fade gain for a state (1.0 if active, 0.0-1.0 if fading, 0.0 if not found)
    [[nodiscard]] float get_fade_gain(std::uint32_t state_id) const {
        // Check if it's an active state
        if (states_.find(state_id) != states_.end()) {
            return 1.0f;
        }
        // Check if it's a fading state
        auto it = fading_states_.find(state_id);
        if (it != fading_states_.end()) {
            return it->second.fade_gain;
        }
        return 0.0f;
    }

    // Get fading state if it exists (for reading orphaned state data)
    template<typename T>
    [[nodiscard]] const T* get_fading(std::uint32_t state_id) const {
        auto it = fading_states_.find(state_id);
        if (it != fading_states_.end()) {
            if (std::holds_alternative<T>(it->second.state)) {
                return &std::get<T>(it->second.state);
            }
        }
        return nullptr;
    }

    // Get number of fading states
    [[nodiscard]] std::size_t fading_count() const {
        return fading_states_.size();
    }

private:
    std::unordered_map<std::uint32_t, DSPState> states_;
    std::unordered_set<std::uint32_t> touched_;
    std::unordered_map<std::uint32_t, FadingState> fading_states_;
    std::uint32_t fade_blocks_ = 3;  // Default: match crossfade duration
};

}  // namespace cedar
