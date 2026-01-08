#pragma once

#include "../dsp/constants.hpp"
#include "../opcodes/dsp_state.hpp"
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

namespace cedar {

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

    // Garbage collect: remove states that weren't touched
    // Call after hot-swap to clean up orphaned states
    void gc_sweep() {
        for (auto it = states_.begin(); it != states_.end();) {
            if (touched_.find(it->first) == touched_.end()) {
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
    }

    // Get number of active states
    [[nodiscard]] std::size_t size() const {
        return states_.size();
    }

private:
    std::unordered_map<std::uint32_t, DSPState> states_;
    std::unordered_set<std::uint32_t> touched_;
};

}  // namespace cedar
