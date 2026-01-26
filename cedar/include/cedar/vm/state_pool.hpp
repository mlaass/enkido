#pragma once

#include "../dsp/constants.hpp"
#include "../opcodes/dsp_state.hpp"
#include <array>
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

// Fixed-size entry for open-addressing hash table
struct StateEntry {
    std::uint32_t key = 0;
    DSPState state;
    bool occupied = false;
};

struct FadingEntry {
    std::uint32_t key = 0;
    FadingState fading;
    bool occupied = false;
};

// Persistent state pool for DSP blocks
// Uses semantic ID (FNV-1a 32-bit hash) for lookup to support hot-swapping
// Fixed-size open-addressing hash table - ZERO runtime allocations
class StatePool {
public:
    StatePool() {
        clear_all();
    }

    // Get or create state for a given semantic ID
    // Template type T must be one of the DSPState variant alternatives
    template<typename T>
    [[nodiscard]] T& get_or_create(std::uint32_t state_id) {
        std::size_t idx = find_or_insert_slot(state_id);
        touched_[idx] = true;

        auto& entry = states_[idx];
        if (!entry.occupied) {
            entry.key = state_id;
            entry.state.template emplace<T>();
            entry.occupied = true;
            ++state_count_;
            return std::get<T>(entry.state);
        }

        // If state exists but is wrong type, replace it safely
        if (!std::holds_alternative<T>(entry.state)) {
            entry.state.template emplace<T>();
        }
        return std::get<T>(entry.state);
    }

    // Get existing state (assumes it exists and is correct type)
    template<typename T>
    [[nodiscard]] T& get(std::uint32_t state_id) {
        std::size_t idx = find_slot(state_id);
        touched_[idx] = true;
        return std::get<T>(states_[idx].state);
    }

    // Check if state exists
    [[nodiscard]] bool exists(std::uint32_t state_id) const {
        return find_slot(state_id) != INVALID_SLOT;
    }

    // Mark state as touched (for GC tracking)
    void touch(std::uint32_t state_id) {
        std::size_t idx = find_slot(state_id);
        if (idx != INVALID_SLOT) {
            touched_[idx] = true;
        }
    }

    // Clear touched set (call at start of program execution)
    void begin_frame() {
        touched_.fill(false);
    }

    // Garbage collect: move untouched states to fading pool
    // Call after hot-swap to begin fade-out of orphaned states
    void gc_sweep() {
        for (std::size_t i = 0; i < MAX_STATES; ++i) {
            if (states_[i].occupied && !touched_[i]) {
                // Move to fading pool instead of immediate deletion
                if (fade_blocks_ > 0) {
                    std::size_t fading_idx = find_or_insert_fading_slot(states_[i].key);
                    auto& fe = fading_states_[fading_idx];
                    fe.key = states_[i].key;
                    fe.fading.state = std::move(states_[i].state);
                    fe.fading.blocks_remaining = fade_blocks_;
                    fe.fading.fade_gain = 1.0f;
                    fe.fading.fade_decrement = 1.0f / static_cast<float>(fade_blocks_);
                    fe.occupied = true;
                }
                // Clear the state slot
                states_[i].occupied = false;
                states_[i].key = 0;
                states_[i].state = std::monostate{};
                --state_count_;
            }
        }
    }

    // Reset all states (full program change)
    void reset() {
        clear_all();
    }

    // Get number of active states
    [[nodiscard]] std::size_t size() const {
        return state_count_;
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
        for (std::size_t i = 0; i < MAX_STATES; ++i) {
            if (fading_states_[i].occupied) {
                auto& fs = fading_states_[i].fading;
                if (fs.blocks_remaining > 0) {
                    fs.blocks_remaining--;
                    fs.fade_gain -= fs.fade_decrement;
                    if (fs.fade_gain < 0.0f) fs.fade_gain = 0.0f;
                }
            }
        }
    }

    // Remove states that have finished fading
    void gc_fading() {
        for (std::size_t i = 0; i < MAX_STATES; ++i) {
            if (fading_states_[i].occupied && fading_states_[i].fading.blocks_remaining == 0) {
                fading_states_[i].occupied = false;
                fading_states_[i].key = 0;
                fading_states_[i].fading = FadingState{};
            }
        }
    }

    // Get fade gain for a state (1.0 if active, 0.0-1.0 if fading, 0.0 if not found)
    [[nodiscard]] float get_fade_gain(std::uint32_t state_id) const {
        // Check if it's an active state
        if (find_slot(state_id) != INVALID_SLOT) {
            return 1.0f;
        }
        // Check if it's a fading state
        std::size_t fading_idx = find_fading_slot(state_id);
        if (fading_idx != INVALID_SLOT) {
            return fading_states_[fading_idx].fading.fade_gain;
        }
        return 0.0f;
    }

    // Get fading state if it exists (for reading orphaned state data)
    template<typename T>
    [[nodiscard]] const T* get_fading(std::uint32_t state_id) const {
        std::size_t idx = find_fading_slot(state_id);
        if (idx != INVALID_SLOT) {
            if (std::holds_alternative<T>(fading_states_[idx].fading.state)) {
                return &std::get<T>(fading_states_[idx].fading.state);
            }
        }
        return nullptr;
    }

    // Get number of fading states
    [[nodiscard]] std::size_t fading_count() const {
        std::size_t count = 0;
        for (std::size_t i = 0; i < MAX_STATES; ++i) {
            if (fading_states_[i].occupied) ++count;
        }
        return count;
    }

    // =========================================================================
    // State Initialization (for SEQ_STEP and other stateful opcodes)
    // =========================================================================

    // Initialize a SeqStepState with timed events
    // Used by compiler to set up sequence data before program execution
    void init_seq_step(std::uint32_t state_id,
                       const float* times, const float* values, const float* velocities,
                       std::size_t count, float cycle_length) {
        auto& state = get_or_create<SeqStepState>(state_id);
        state.num_events = static_cast<std::uint32_t>(std::min(count, SeqStepState::MAX_EVENTS));
        state.cycle_length = cycle_length;
        state.current_index = 0;
        state.last_beat_pos = -1.0f;
        for (std::size_t i = 0; i < state.num_events; ++i) {
            state.times[i] = times[i];
            state.values[i] = values[i];
            state.velocities[i] = velocities[i];
        }
    }

    // Initialize a PatternQueryState with a compiled pattern program
    // Used by compiler to set up lazy queryable patterns
    void init_pattern_program(std::uint32_t state_id,
                              const PatternNode* nodes, std::size_t node_count,
                              float cycle_length, bool is_sample_pattern) {
        auto& state = get_or_create<PatternQueryState>(state_id);

        // Copy pattern nodes
        state.num_nodes = static_cast<std::uint32_t>(
            std::min(node_count, PatternQueryState::MAX_NODES));
        for (std::size_t i = 0; i < state.num_nodes; ++i) {
            state.nodes[i] = nodes[i];
        }

        // Set metadata
        state.cycle_length = cycle_length;
        state.is_sample_pattern = is_sample_pattern;

        // Initialize pattern seed from state_id (deterministic randomness)
        // Use splitmix64-style mixing for good distribution
        std::uint64_t seed = state_id;
        seed = (seed ^ (seed >> 30)) * 0xBF58476D1CE4E5B9ull;
        seed = (seed ^ (seed >> 27)) * 0x94D049BB133111EBull;
        state.pattern_seed = seed ^ (seed >> 31);

        // Reset playback state
        state.current_index = 0;
        state.last_beat_pos = -1.0f;
        state.num_events = 0;
        state.query_start = 0.0f;
        state.query_end = 0.0f;
    }

private:
    static constexpr std::size_t INVALID_SLOT = ~std::size_t{0};

    // Open-addressing hash table lookup (linear probing)
    // Returns slot index if found, INVALID_SLOT otherwise
    [[nodiscard]] std::size_t find_slot(std::uint32_t key) const {
        std::size_t start = key % MAX_STATES;
        std::size_t idx = start;
        do {
            if (!states_[idx].occupied) {
                return INVALID_SLOT;  // Empty slot, key not found
            }
            if (states_[idx].key == key) {
                return idx;  // Found
            }
            idx = (idx + 1) % MAX_STATES;
        } while (idx != start);
        return INVALID_SLOT;  // Table full, key not found
    }

    // Find existing slot or first empty slot for insertion
    [[nodiscard]] std::size_t find_or_insert_slot(std::uint32_t key) {
        std::size_t start = key % MAX_STATES;
        std::size_t idx = start;
        std::size_t first_empty = INVALID_SLOT;
        do {
            if (!states_[idx].occupied) {
                if (first_empty == INVALID_SLOT) {
                    first_empty = idx;
                }
                // Continue searching in case key exists later (due to deletions)
            } else if (states_[idx].key == key) {
                return idx;  // Found existing
            }
            idx = (idx + 1) % MAX_STATES;
        } while (idx != start);
        // Key not found, return first empty slot
        return first_empty != INVALID_SLOT ? first_empty : 0;  // Fallback to 0 if somehow full
    }

    // Same for fading states
    [[nodiscard]] std::size_t find_fading_slot(std::uint32_t key) const {
        std::size_t start = key % MAX_STATES;
        std::size_t idx = start;
        do {
            if (!fading_states_[idx].occupied) {
                return INVALID_SLOT;
            }
            if (fading_states_[idx].key == key) {
                return idx;
            }
            idx = (idx + 1) % MAX_STATES;
        } while (idx != start);
        return INVALID_SLOT;
    }

    [[nodiscard]] std::size_t find_or_insert_fading_slot(std::uint32_t key) {
        std::size_t start = key % MAX_STATES;
        std::size_t idx = start;
        std::size_t first_empty = INVALID_SLOT;
        do {
            if (!fading_states_[idx].occupied) {
                if (first_empty == INVALID_SLOT) {
                    first_empty = idx;
                }
            } else if (fading_states_[idx].key == key) {
                return idx;
            }
            idx = (idx + 1) % MAX_STATES;
        } while (idx != start);
        return first_empty != INVALID_SLOT ? first_empty : 0;
    }

    void clear_all() {
        for (std::size_t i = 0; i < MAX_STATES; ++i) {
            states_[i] = StateEntry{};
            fading_states_[i] = FadingEntry{};
            touched_[i] = false;
        }
        state_count_ = 0;
    }

    std::array<StateEntry, MAX_STATES> states_;
    std::array<FadingEntry, MAX_STATES> fading_states_;
    std::array<bool, MAX_STATES> touched_;
    std::size_t state_count_ = 0;
    std::uint32_t fade_blocks_ = 3;  // Default: match crossfade duration
};

}  // namespace cedar
