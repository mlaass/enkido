#pragma once

#include "../dsp/constants.hpp"
#include "../opcodes/dsp_state.hpp"
#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <iomanip>

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

    void set_state_id_xor(std::uint32_t xor_val) { state_id_xor_ = xor_val; }
    std::uint32_t state_id_xor() const { return state_id_xor_; }

    // Get or create state for a given semantic ID
    // Template type T must be one of the DSPState variant alternatives
    template<typename T>
    [[nodiscard]] T& get_or_create(std::uint32_t state_id) {
        std::uint32_t effective_id = state_id ^ state_id_xor_;
        std::size_t idx = find_or_insert_slot(effective_id);
        touched_[idx] = true;

        auto& entry = states_[idx];
        if (!entry.occupied) {
            entry.key = effective_id;
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
        std::uint32_t effective_id = state_id ^ state_id_xor_;
        std::size_t idx = find_slot(effective_id);
        touched_[idx] = true;
        return std::get<T>(states_[idx].state);
    }

    // Get existing state if it exists and is the correct type (returns nullptr otherwise)
    template<typename T>
    [[nodiscard]] T* get_if(std::uint32_t state_id) {
        std::uint32_t effective_id = state_id ^ state_id_xor_;
        std::size_t idx = find_slot(effective_id);
        if (idx == INVALID_SLOT) {
            return nullptr;
        }
        auto& entry = states_[idx];
        if (!entry.occupied) {
            return nullptr;
        }
        return std::get_if<T>(&entry.state);
    }

    // Resolved event source — returned by resolve_output_events for any
    // upstream that produces an OutputEvents buffer. Carries the source's
    // cycle_length so the POLY block keeps its existing wrap math without
    // caring whether the upstream is a SequenceState or a MidiQueueState.
    struct ResolvedEvents {
        OutputEvents* events = nullptr;
        float         cycle_length = 0.0f;
    };

    // Resolve a state_id to its OutputEvents buffer regardless of which
    // event-source state type produced it. Returns {nullptr, 0} for unknown
    // ids or non-event-source state types. Used by execute_poly_block to
    // accept both SequenceState (patterns) and MidiQueueState (MIDI_QUERY)
    // upstreams interchangeably.
    [[nodiscard]] ResolvedEvents resolve_output_events(std::uint32_t state_id) {
        std::uint32_t effective_id = state_id ^ state_id_xor_;
        std::size_t idx = find_slot(effective_id);
        if (idx == INVALID_SLOT) return {};
        auto& entry = states_[idx];
        if (!entry.occupied) return {};
        if (auto* seq = std::get_if<SequenceState>(&entry.state)) {
            return {&seq->output, seq->cycle_length};
        }
        if (auto* midi = std::get_if<MidiQueueState>(&entry.state)) {
            return {&midi->output, midi->cycle_length};
        }
        return {};
    }

    // Check if state exists
    [[nodiscard]] bool exists(std::uint32_t state_id) const {
        std::uint32_t effective_id = state_id ^ state_id_xor_;
        return find_slot(effective_id) != INVALID_SLOT;
    }

    // Mark state as touched (for GC tracking)
    void touch(std::uint32_t state_id) {
        std::uint32_t effective_id = state_id ^ state_id_xor_;
        std::size_t idx = find_slot(effective_id);
        if (idx != INVALID_SLOT) {
            touched_[idx] = true;
        }
    }

    // Clear touched set (call at start of program execution)
    void begin_frame() {
        touched_.fill(false);
    }

    // Copy the active-state table from another pool, element-wise.
    // Used by VM::perform_crossfade to snapshot/restore the pool so that
    // dual-execution of old + new programs doesn't double-advance
    // stateful opcodes (OscState phase, EnvState stage, filter memory,
    // delay write_pos, etc.). Fading pool and touched bits are NOT
    // copied — they're owned by their respective pool's GC lifecycle.
    //
    // Non-copyable variant alternatives (currently `MidiQueueState`,
    // whose SPSC atomics + held-note arrays cannot safely be cloned)
    // are not transferred: when `src` has one in slot i, we leave OUR
    // slot i untouched. This is intentional — MidiQueueState is a
    // drain-once event stream, so the live pool's state must persist
    // across snapshot/restore (drains applied by old should remain
    // applied; they should not be re-emitted to new).
    //
    // Buffer pointers into the audio arena (DelayState `buffer`,
    // SequenceState `output.events`, etc.) are copied as-is. This is
    // correct for crossfade since both old and new execute against the
    // same arena and the second program's writes naturally overwrite
    // the first's at the same buffer positions.
    void copy_states_from(const StatePool& src) noexcept {
        state_count_ = src.state_count_;
        state_id_xor_ = src.state_id_xor_;
        for (std::size_t i = 0; i < MAX_STATES; ++i) {
            // Preserve our MidiQueueState — see comment above.
            if (states_[i].occupied &&
                std::holds_alternative<MidiQueueState>(states_[i].state)) {
                continue;
            }
            const auto& s = src.states_[i];
            if (!s.occupied) {
                states_[i].occupied = false;
                states_[i].key = 0;
                states_[i].state.template emplace<std::monostate>();
                continue;
            }
            // Skip slots in src that hold non-copyable variants.
            bool copied = false;
            std::visit([&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_copy_constructible_v<T>) {
                    states_[i].state.template emplace<T>(v);
                    copied = true;
                }
            }, s.state);
            if (copied) {
                states_[i].occupied = true;
                states_[i].key = s.key;
            } else {
                states_[i].occupied = false;
                states_[i].key = 0;
                states_[i].state.template emplace<std::monostate>();
            }
        }
    }

    // PRD prd-midi-input §7.3: walk every MidiQueueState that is occupied but
    // not touched in the current frame and patch its held notes to release at
    // `now_beats`. Called by VM::handle_swap between rebind_states (which
    // marks survivors as touched) and gc_sweep (which evicts the rest), so
    // downstream consumers preserved by the hot-swap see finite durations
    // for notes whose midi() source has disappeared.
    void release_held_notes_on_untouched_midi(float now_beats,
                                              float samples_per_beat) {
        for (std::size_t i = 0; i < MAX_STATES; ++i) {
            if (!states_[i].occupied || touched_[i]) continue;
            if (auto* midi = std::get_if<MidiQueueState>(&states_[i].state)) {
                midi->release_all_held(now_beats, samples_per_beat);
            }
        }
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
    // State Inspection (for debug UI)
    // =========================================================================

    /**
     * Inspect state by ID, returning JSON representation.
     * Returns empty string if state not found.
     */
    [[nodiscard]] std::string inspect_state_json(std::uint32_t state_id) const {
        std::size_t idx = find_slot(state_id);
        if (idx == INVALID_SLOT) {
            return "";
        }

        const auto& entry = states_[idx];
        if (!entry.occupied) {
            return "";
        }

        std::ostringstream json;
        json << std::fixed << std::setprecision(6);

        std::visit([&json](auto&& state) {
            using T = std::decay_t<decltype(state)>;

            if constexpr (std::is_same_v<T, std::monostate>) {
                json << R"({"type":"none"})";
            }
            else if constexpr (std::is_same_v<T, OscState>) {
                json << R"({"type":"OscState")";
                json << R"(,"phase":)" << state.phase;
                json << R"(,"prev_phase":)" << state.prev_phase;
                json << R"(,"initialized":)" << (state.initialized ? "true" : "false");
                json << "}";
            }
            else if constexpr (std::is_same_v<T, SmoochState>) {
                json << R"({"type":"SmoochState")";
                json << R"(,"phase":)" << state.phase;
                json << R"(,"initialized":)" << (state.initialized ? "true" : "false");
                json << "}";
            }
#ifndef CEDAR_NO_MINBLEP
            else if constexpr (std::is_same_v<T, MinBLEPOscState>) {
                json << R"({"type":"MinBLEPOscState")";
                json << R"(,"phase":)" << state.phase;
                json << R"(,"write_pos":)" << state.write_pos;
                json << R"(,"initialized":)" << (state.initialized ? "true" : "false");
                json << "}";
            }
#endif
            else if constexpr (std::is_same_v<T, SVFState>) {
                json << R"({"type":"SVFState")";
                json << R"(,"ic1eq_l":)" << state.ic1eq[0];
                json << R"(,"ic1eq_r":)" << state.ic1eq[1];
                json << R"(,"ic2eq_l":)" << state.ic2eq[0];
                json << R"(,"ic2eq_r":)" << state.ic2eq[1];
                json << R"(,"g":)" << state.g;
                json << R"(,"k":)" << state.k;
                json << R"(,"last_freq":)" << state.last_freq;
                json << R"(,"last_q":)" << state.last_q;
                json << "}";
            }
            else if constexpr (std::is_same_v<T, NoiseState>) {
                json << R"({"type":"NoiseState")";
                json << R"(,"seed":)" << state.seed;
                json << R"(,"phase":)" << state.phase;
                json << R"(,"current_value":)" << state.current_value;
                json << R"(,"initialized":)" << (state.initialized ? "true" : "false");
                json << "}";
            }
            else if constexpr (std::is_same_v<T, SlewState>) {
                json << R"({"type":"SlewState")";
                json << R"(,"current_l":)" << state.current[0];
                json << R"(,"current_r":)" << state.current[1];
                json << R"(,"initialized":)" << (state.initialized ? "true" : "false");
                json << "}";
            }
            else if constexpr (std::is_same_v<T, EdgeState>) {
                json << R"({"type":"EdgeState")";
                json << R"(,"held_value_l":)" << state.held_value[0];
                json << R"(,"held_value_r":)" << state.held_value[1];
                json << R"(,"prev_trigger_l":)" << state.prev_trigger[0];
                json << R"(,"prev_trigger_r":)" << state.prev_trigger[1];
                json << R"(,"prev_reset_trigger_l":)" << state.prev_reset_trigger[0];
                json << R"(,"prev_reset_trigger_r":)" << state.prev_reset_trigger[1];
                json << "}";
            }
            else if constexpr (std::is_same_v<T, CellState>) {
                json << R"({"type":"CellState")";
                json << R"(,"value":)" << state.value;
                json << R"(,"initialized":)" << (state.initialized ? "true" : "false");
                json << "}";
            }
            else if constexpr (std::is_same_v<T, DelayState>) {
                json << R"({"type":"DelayState")";
                json << R"(,"buffer_size":)" << state.buffer_size;
                json << R"(,"write_pos_l":)" << state.write_pos[0];
                json << R"(,"write_pos_r":)" << state.write_pos[1];
                json << "}";
            }
            else if constexpr (std::is_same_v<T, EnvState>) {
                json << R"({"type":"EnvState")";
                json << R"(,"level":)" << state.level;
                json << R"(,"stage":)" << static_cast<int>(state.stage);
                json << R"(,"time_in_stage":)" << state.time_in_stage;
                json << R"(,"prev_gate":)" << state.prev_gate;
                json << R"(,"release_level":)" << state.release_level;
                json << "}";
            }
            else if constexpr (std::is_same_v<T, LFOState>) {
                json << R"({"type":"LFOState")";
                json << R"(,"prev_phase":)" << state.prev_phase;
                json << R"(,"prev_value":)" << state.prev_value;
                json << "}";
            }
            else if constexpr (std::is_same_v<T, EuclidState>) {
                json << R"({"type":"EuclidState")";
                json << R"(,"prev_step":)" << state.prev_step;
                json << R"(,"pattern":)" << state.pattern;
                json << R"(,"last_hits":)" << state.last_hits;
                json << R"(,"last_steps":)" << state.last_steps;
                json << "}";
            }
            else if constexpr (std::is_same_v<T, TriggerState>) {
                json << R"({"type":"TriggerState")";
                json << R"(,"prev_phase":)" << state.prev_phase;
                json << "}";
            }
            else if constexpr (std::is_same_v<T, TimelineState>) {
                json << R"({"type":"TimelineState")";
                json << R"(,"num_points":)" << state.num_points;
                json << R"(,"loop":)" << (state.loop ? "true" : "false");
                json << R"(,"loop_length":)" << state.loop_length;
                json << "}";
            }
            else if constexpr (std::is_same_v<T, TransportState>) {
                json << R"({"type":"TransportState")";
                json << R"(,"beat_pos":)" << state.beat_pos;
                json << R"(,"cycle_length":)" << state.cycle_length;
                json << "}";
            }
            else if constexpr (std::is_same_v<T, SequenceState>) {
                json << R"({"type":"SequenceState")";
                json << R"(,"num_sequences":)" << state.num_sequences;
                json << R"(,"cycle_length":)" << state.cycle_length;
                json << R"(,"current_index":)" << state.current_index;
                json << R"(,"last_beat_pos":)" << state.last_beat_pos;
                json << R"(,"is_sample_pattern":)" << (state.is_sample_pattern ? "true" : "false");
                json << "}";
            }
            else if constexpr (std::is_same_v<T, MoogState>) {
                json << R"({"type":"MoogState")";
                json << R"(,"stage_l":[)" << state.stage[0][0] << "," << state.stage[0][1] << ","
                     << state.stage[0][2] << "," << state.stage[0][3] << "]";
                json << R"(,"stage_r":[)" << state.stage[1][0] << "," << state.stage[1][1] << ","
                     << state.stage[1][2] << "," << state.stage[1][3] << "]";
                json << R"(,"g":)" << state.g;
                json << R"(,"k":)" << state.k;
                json << R"(,"last_freq":)" << state.last_freq;
                json << R"(,"last_res":)" << state.last_res;
                json << "}";
            }
            else if constexpr (std::is_same_v<T, DiodeState>) {
                json << R"({"type":"DiodeState")";
                json << R"(,"cap_l":[)" << state.cap[0][0] << "," << state.cap[0][1] << ","
                     << state.cap[0][2] << "," << state.cap[0][3] << "]";
                json << R"(,"cap_r":[)" << state.cap[1][0] << "," << state.cap[1][1] << ","
                     << state.cap[1][2] << "," << state.cap[1][3] << "]";
                json << R"(,"g":)" << state.g;
                json << R"(,"k":)" << state.k;
                json << R"(,"last_freq":)" << state.last_freq;
                json << R"(,"last_res":)" << state.last_res;
                json << "}";
            }
            else if constexpr (std::is_same_v<T, FormantState>) {
                json << R"({"type":"FormantState")";
                json << R"(,"f1":)" << state.f1;
                json << R"(,"f2":)" << state.f2;
                json << R"(,"f3":)" << state.f3;
                json << R"(,"last_morph":)" << state.last_morph;
                json << "}";
            }
            else if constexpr (std::is_same_v<T, SallenkeyState>) {
                json << R"({"type":"SallenkeyState")";
                json << R"(,"cap1_l":)" << state.cap1[0];
                json << R"(,"cap1_r":)" << state.cap1[1];
                json << R"(,"cap2_l":)" << state.cap2[0];
                json << R"(,"cap2_r":)" << state.cap2[1];
                json << R"(,"g":)" << state.g;
                json << R"(,"k":)" << state.k;
                json << "}";
            }
            else if constexpr (std::is_same_v<T, BitcrushState>) {
                json << R"({"type":"BitcrushState")";
                json << R"(,"held_sample_l":)" << state.held_sample[0];
                json << R"(,"held_sample_r":)" << state.held_sample[1];
                json << R"(,"phase_l":)" << state.phase[0];
                json << R"(,"phase_r":)" << state.phase[1];
                json << "}";
            }
            else if constexpr (std::is_same_v<T, SmoothSatState>) {
                json << R"({"type":"SmoothSatState")";
                json << R"(,"x_prev_l":)" << state.x_prev[0];
                json << R"(,"x_prev_r":)" << state.x_prev[1];
                json << R"(,"ad_prev_l":)" << state.ad_prev[0];
                json << R"(,"ad_prev_r":)" << state.ad_prev[1];
                json << "}";
            }
            else if constexpr (std::is_same_v<T, FoldADAAState>) {
                json << R"({"type":"FoldADAAState")";
                json << R"(,"x_prev_l":)" << state.x_prev[0];
                json << R"(,"x_prev_r":)" << state.x_prev[1];
                json << R"(,"ad_prev_l":)" << state.ad_prev[0];
                json << R"(,"ad_prev_r":)" << state.ad_prev[1];
                json << "}";
            }
            else if constexpr (std::is_same_v<T, TubeState> || std::is_same_v<T, TapeState> ||
                               std::is_same_v<T, XfmrState> || std::is_same_v<T, ExciterState>) {
                json << R"({"type":"OversamplingSatState")";
                json << R"(,"os_idx_l":)" << state.os_idx[0];
                json << R"(,"os_idx_r":)" << state.os_idx[1];
                json << "}";
            }
            else if constexpr (std::is_same_v<T, CombFilterState>) {
                json << R"({"type":"CombFilterState")";
                json << R"(,"write_pos_l":)" << state.write_pos[0];
                json << R"(,"write_pos_r":)" << state.write_pos[1];
                json << R"(,"filter_state_l":)" << state.filter_state[0];
                json << R"(,"filter_state_r":)" << state.filter_state[1];
                json << "}";
            }
            else if constexpr (std::is_same_v<T, FlangerState>) {
                json << R"({"type":"FlangerState")";
                json << R"(,"write_pos_l":)" << state.write_pos[0];
                json << R"(,"write_pos_r":)" << state.write_pos[1];
                json << R"(,"lfo_phase":)" << state.lfo_phase;
                json << "}";
            }
            else if constexpr (std::is_same_v<T, ChorusState>) {
                json << R"({"type":"ChorusState")";
                json << R"(,"write_pos_l":)" << state.write_pos[0];
                json << R"(,"write_pos_r":)" << state.write_pos[1];
                json << R"(,"lfo_phase":)" << state.lfo_phase;
                json << "}";
            }
            else if constexpr (std::is_same_v<T, PhaserState>) {
                json << R"({"type":"PhaserState")";
                json << R"(,"lfo_phase":)" << state.lfo_phase;
                json << R"(,"last_output_l":)" << state.last_output[0];
                json << R"(,"last_output_r":)" << state.last_output[1];
                json << "}";
            }
            else if constexpr (std::is_same_v<T, SamplerState>) {
                // Count active voices
                int active_voices = 0;
                for (std::size_t i = 0; i < SamplerState::MAX_VOICES; ++i) {
                    if (state.voices[i].active) ++active_voices;
                }
                json << R"({"type":"SamplerState")";
                json << R"(,"active_voices":)" << active_voices;
                json << R"(,"prev_trigger":)" << state.prev_trigger;
                json << "}";
            }
            else if constexpr (std::is_same_v<T, CompressorState>) {
                json << R"({"type":"CompressorState")";
                json << R"(,"envelope_l":)" << state.envelope[0];
                json << R"(,"envelope_r":)" << state.envelope[1];
                json << R"(,"gain_reduction_l":)" << state.gain_reduction[0];
                json << R"(,"gain_reduction_r":)" << state.gain_reduction[1];
                json << "}";
            }
            else if constexpr (std::is_same_v<T, LimiterState>) {
                json << R"({"type":"LimiterState")";
                json << R"(,"write_pos_l":)" << state.write_pos[0];
                json << R"(,"write_pos_r":)" << state.write_pos[1];
                json << R"(,"gain_l":)" << state.gain[0];
                json << R"(,"gain_r":)" << state.gain[1];
                json << "}";
            }
            else if constexpr (std::is_same_v<T, GateState>) {
                json << R"({"type":"GateState")";
                json << R"(,"envelope_l":)" << state.envelope[0];
                json << R"(,"envelope_r":)" << state.envelope[1];
                json << R"(,"gain_l":)" << state.gain[0];
                json << R"(,"gain_r":)" << state.gain[1];
                json << R"(,"is_open_l":)" << (state.is_open[0] ? "true" : "false");
                json << R"(,"is_open_r":)" << (state.is_open[1] ? "true" : "false");
                json << "}";
            }
            else if constexpr (std::is_same_v<T, EnvFollowerState>) {
                json << R"({"type":"EnvFollowerState")";
                json << R"(,"level_l":)" << state.level[0];
                json << R"(,"level_r":)" << state.level[1];
                json << "}";
            }
            else if constexpr (std::is_same_v<T, FreeverbState>) {
                json << R"({"type":"FreeverbState")";
                json << "}";
            }
            else if constexpr (std::is_same_v<T, DattorroState>) {
                json << R"({"type":"DattorroState")";
                json << R"(,"mod_phase":)" << state.mod_phase;
                json << "}";
            }
            else if constexpr (std::is_same_v<T, FDNState>) {
                json << R"({"type":"FDNState")";
                json << "}";
            }
            else if constexpr (std::is_same_v<T, OscState4x>) {
                json << R"({"type":"OscStateOversampled")";
                json << R"(,"phase":)" << state.osc.phase;
                json << R"(,"prev_phase":)" << state.osc.prev_phase;
                json << R"(,"initialized":)" << (state.osc.initialized ? "true" : "false");
                json << "}";
            }
            else if constexpr (std::is_same_v<T, ExtendedParams<1>> ||
                               std::is_same_v<T, ExtendedParams<2>> ||
                               std::is_same_v<T, ExtendedParams<3>> ||
                               std::is_same_v<T, ExtendedParams<5>> ||
                               std::is_same_v<T, ExtendedParams<8>>) {
                constexpr std::size_t N =
                    std::is_same_v<T, ExtendedParams<1>> ? 1 :
                    (std::is_same_v<T, ExtendedParams<2>> ? 2 :
                    (std::is_same_v<T, ExtendedParams<3>> ? 3 :
                    (std::is_same_v<T, ExtendedParams<5>> ? 5 : 8)));
                json << R"({"type":"ExtendedParams")";
                json << R"(,"count":)" << N;
                json << R"(,"params":[)";
                for (std::size_t i = 0; i < N; ++i) {
                    if (i > 0) json << ",";
                    const auto& slot = state.params[i];
                    json << R"({"is_constant":)" << (slot.is_constant() ? "true" : "false");
                    if (slot.is_constant()) {
                        json << R"(,"value":)" << slot.constant;
                    } else {
                        json << R"(,"buffer_idx":)" << slot.buffer_idx;
                    }
                    json << "}";
                }
                json << "]}";
            }
            else if constexpr (std::is_same_v<T, PolyAllocState>) {
                json << R"({"type":"PolyAllocState")";
                json << R"(,"max_voices":)" << static_cast<int>(state.max_voices);
                json << R"(,"mode":)" << static_cast<int>(state.mode);
                json << R"(,"steal_strategy":)" << static_cast<int>(state.steal_strategy);
                json << R"(,"seq_state_id":)" << state.seq_state_id;
                json << R"(,"release_window_samples":)" << state.release_window_samples;
                json << R"(,"voices":[)";
                for (int vi = 0; vi < state.max_voices && state.voices; ++vi) {
                    if (vi > 0) json << ",";
                    const auto& v = state.voices[vi];
                    json << R"({"active":)" << (v.active ? "true" : "false");
                    json << R"(,"freq":)" << v.freq;
                    json << R"(,"vel":)" << v.vel;
                    json << R"(,"gate":)" << v.gate;
                    json << R"(,"releasing":)" << (v.releasing ? "true" : "false");
                    json << R"(,"age":)" << v.age;
                    json << R"(,"release_countdown":)" << v.release_countdown;
                    json << "}";
                }
                json << "]}";
            }
            else if constexpr (std::is_same_v<T, ProbeState>) {
                json << R"({"type":"ProbeState")";
                json << R"(,"write_pos":)" << state.write_pos;
                json << R"(,"initialized":)" << (state.initialized ? "true" : "false");
                json << "}";
            }
#ifndef CEDAR_NO_FFT
            else if constexpr (std::is_same_v<T, FFTProbeState>) {
                json << R"({"type":"FFTProbeState")";
                json << R"(,"fft_size":)" << state.fft_size;
                json << R"(,"write_pos":)" << state.write_pos;
                json << R"(,"frame_counter":)" << state.frame_counter;
                json << R"(,"initialized":)" << (state.initialized ? "true" : "false");
                json << "}";
            }
#endif
            else {
                json << R"({"type":"Unknown"})";
            }
        }, entry.state);

        return json.str();
    }

    // =========================================================================
    // Extended Parameter Initialization
    // =========================================================================

    /**
     * Initialize extended parameters for an opcode.
     * Used by codegen to set up parameters beyond inputs[0..4].
     *
     * @tparam N Number of extended parameters (matches ExtendedParams<N>)
     * @param state_id Semantic ID for this opcode instance
     * @param constants Array of N constant values (use NAN for buffer-backed params)
     * @param buffer_indices Array of N buffer indices (0xFFFF = use constant)
     */
    template<std::size_t N>
    void init_extended_params(std::uint32_t state_id,
                              const std::array<float, N>& constants,
                              const std::array<std::uint16_t, N>& buffer_indices) {
        auto& ext = get_or_create<ExtendedParams<N>>(state_id);
        for (std::size_t i = 0; i < N; ++i) {
            if (buffer_indices[i] != 0xFFFF) {
                ext.set_buffer(i, buffer_indices[i]);
            } else {
                ext.set_constant(i, constants[i]);
            }
        }
    }

    /**
     * Initialize extended parameters with all constants.
     * Convenience overload when no parameters are buffer-backed.
     */
    template<std::size_t N>
    void init_extended_params_const(std::uint32_t state_id,
                                    const std::array<float, N>& constants) {
        std::array<std::uint16_t, N> buffer_indices;
        buffer_indices.fill(0xFFFF);
        init_extended_params<N>(state_id, constants, buffer_indices);
    }

    /**
     * Runtime dispatch over the supported ExtendedParams<N> variants. The
     * loader / WASM apply-state-inits path only knows `count` at runtime, so
     * this helper packs the constants/buffer-indices pair into the smallest
     * variant that fits. Supported N values are {1, 2, 3, 5, 8} — counts
     * between those round up (e.g. count=4 uses ExtendedParams<5>). Counts
     * above 8 are clamped to 8 with the tail truncated.
     */
    void init_extended_params_runtime(std::uint32_t state_id,
                                      const float* constants,
                                      const std::uint16_t* buffer_indices,
                                      std::uint8_t count) {
        if (count == 0) return;

        auto run = [&]<std::size_t N>() {
            std::array<float, N> c{};
            std::array<std::uint16_t, N> b{};
            b.fill(0xFFFF);
            const std::size_t copy_n = std::min<std::size_t>(count, N);
            for (std::size_t i = 0; i < copy_n; ++i) {
                c[i] = constants[i];
                b[i] = buffer_indices[i];
            }
            init_extended_params<N>(state_id, c, b);
        };

        if (count == 1)      { run.template operator()<1>(); }
        else if (count == 2) { run.template operator()<2>(); }
        else if (count == 3) { run.template operator()<3>(); }
        else if (count <= 5) { run.template operator()<5>(); }
        else                 { run.template operator()<8>(); }
    }

    // Initialize a SequenceState with compiled sequences (arena-allocated)
    // Used by compiler to set up the simplified sequence-based patterns
    //
    // The sequences parameter contains pointers to compiler-owned event data.
    // This function allocates arena memory and copies all data.
    //
    // @param state_id Unique state ID (FNV-1a hash)
    // @param sequences Array of compiled sequences (with pointers to compiler memory)
    // @param seq_count Number of sequences
    // @param cycle_length Pattern cycle length in beats
    // @param is_sample_pattern True if pattern contains sample IDs
    // @param arena AudioArena for allocating sequence/event memory
    // @param total_events Total event count across all sequences (for output buffer sizing)
    void init_sequence_program(std::uint32_t state_id,
                               const Sequence* sequences, std::size_t seq_count,
                               float cycle_length, bool is_sample_pattern,
                               AudioArena* arena, std::uint32_t total_events) {
        // Snapshot any existing playback state before we overwrite, so that
        // hot-swap recompiles preserve `<...>` alternation, cycle index, and
        // beat-tracking continuity. Without this, every recompile restarts
        // pattern playback (live-coding determinism bug).
        constexpr std::size_t SNAPSHOT_MAX_SEQS = 64;
        struct PlaybackSnapshot {
            bool valid = false;
            std::uint32_t num_sequences = 0;
            std::uint32_t current_index = 0;
            float last_beat_pos = -1.0f;
            float last_queried_cycle = -1.0f;
            std::uint32_t cycle_index = 0;
            std::uint32_t seq_steps[SNAPSHOT_MAX_SEQS] = {};
        };
        PlaybackSnapshot snap{};
        // Capture the existing per-cycle output cache too. SEQPAT_QUERY
        // skips its query when current_cycle == last_queried_cycle AND
        // output.num_events > 0 (see op_seqpat_query). If we restore
        // last_queried_cycle but leave the new output buffer empty (the
        // default after re-alloc below), the FIRST query after recompile
        // misses cache, re-fires for the same cycle, and advances every
        // ALTERNATE sub-sequence's step counter by 1 — one extra step
        // per recompile. We preserve the OLD output pointer + count and
        // copy the events into the freshly allocated buffer after the
        // structural-match check passes.
        const OutputEvents::OutputEvent* old_output_events = nullptr;
        std::uint32_t old_output_num_events = 0;
        if (auto* existing = get_if<SequenceState>(state_id)) {
            snap.valid = true;
            snap.num_sequences = existing->num_sequences;
            snap.current_index = existing->current_index;
            snap.last_beat_pos = existing->last_beat_pos;
            snap.last_queried_cycle = existing->last_queried_cycle;
            snap.cycle_index = existing->cycle_index;
            const std::uint32_t copy_count = std::min(
                existing->num_sequences,
                static_cast<std::uint32_t>(SNAPSHOT_MAX_SEQS));
            for (std::uint32_t i = 0; i < copy_count; ++i) {
                snap.seq_steps[i] = existing->sequences[i].step;
            }
            old_output_events = existing->output.events;
            old_output_num_events = existing->output.num_events;
        }

        auto& state = get_or_create<SequenceState>(state_id);

        if (!arena || seq_count == 0) {
            state.sequences = nullptr;
            state.num_sequences = 0;
            state.seq_capacity = 0;
            state.output.events = nullptr;
            state.output.capacity = 0;
            return;
        }

        // Allocate sequences array from arena
        // Need space for Sequence structs (each ~32 bytes)
        std::size_t seq_bytes = seq_count * sizeof(Sequence);
        std::size_t seq_floats = (seq_bytes + sizeof(float) - 1) / sizeof(float);
        float* seq_mem = arena->allocate(seq_floats);
        if (!seq_mem) {
            state.sequences = nullptr;
            state.num_sequences = 0;
            state.seq_capacity = 0;
            return;
        }
        state.sequences = reinterpret_cast<Sequence*>(seq_mem);
        state.seq_capacity = static_cast<std::uint32_t>(seq_count);
        state.num_sequences = static_cast<std::uint32_t>(seq_count);

        // Copy sequences and allocate event arrays for each
        for (std::size_t i = 0; i < seq_count; ++i) {
            const Sequence& src = sequences[i];
            Sequence& dst = state.sequences[i];

            dst.duration = src.duration;
            dst.mode = src.mode;
            dst.step = src.step;
            dst.num_events = src.num_events;

            if (src.num_events > 0 && src.events) {
                // Allocate events for this sequence
                std::size_t event_bytes = src.num_events * sizeof(Event);
                std::size_t event_floats = (event_bytes + sizeof(float) - 1) / sizeof(float);
                float* event_mem = arena->allocate(event_floats);
                if (event_mem) {
                    dst.events = reinterpret_cast<Event*>(event_mem);
                    dst.capacity = src.num_events;
                    // Copy events
                    for (std::uint32_t j = 0; j < src.num_events; ++j) {
                        dst.events[j] = src.events[j];
                    }
                } else {
                    dst.events = nullptr;
                    dst.capacity = 0;
                    dst.num_events = 0;
                }
            } else {
                dst.events = nullptr;
                dst.capacity = 0;
            }
        }

        // Allocate output events buffer
        // Use total_events * 2 for safety margin (nested sequences can expand)
        std::uint32_t output_capacity = std::max<std::uint32_t>(32u, total_events * 2);
        std::size_t output_bytes = output_capacity * sizeof(OutputEvents::OutputEvent);
        std::size_t output_floats = (output_bytes + sizeof(float) - 1) / sizeof(float);
        float* output_mem = arena->allocate(output_floats);
        if (output_mem) {
            state.output.events = reinterpret_cast<OutputEvents::OutputEvent*>(output_mem);
            state.output.capacity = output_capacity;
        } else {
            state.output.events = nullptr;
            state.output.capacity = 0;
        }
        state.output.num_events = 0;

        // Set metadata
        state.cycle_length = cycle_length;
        state.is_sample_pattern = is_sample_pattern;

        // Initialize pattern seed from state_id (deterministic randomness)
        std::uint64_t seed = state_id;
        seed = (seed ^ (seed >> 30)) * 0xBF58476D1CE4E5B9ull;
        seed = (seed ^ (seed >> 27)) * 0x94D049BB133111EBull;
        state.pattern_seed = seed ^ (seed >> 31);

        // Reset playback state — by default. The hot-swap preservation
        // path below restores these when the same state_id is re-init'd with
        // a structurally compatible program.
        state.current_index = 0;
        state.last_beat_pos = -1.0f;
        state.last_queried_cycle = -1.0f;
        // iter rotation cleared on init; set via init_sequence_iter() below.
        state.cycle_index = 0;
        state.iter_n = 0;
        state.iter_dir = 0;

        // Hot-swap continuity: when a state with this state_id already
        // existed and the new program has the same number of sequences,
        // restore the per-sequence alternation step counters and overall
        // beat/cycle tracking. Recompiling unchanged source must not
        // restart the pattern. We modulo the alternation step by the new
        // event count so a structural-event-count change still produces a
        // well-defined index.
        if (snap.valid && snap.num_sequences == state.num_sequences) {
            state.current_index = snap.current_index;
            state.last_beat_pos = snap.last_beat_pos;
            state.last_queried_cycle = snap.last_queried_cycle;
            state.cycle_index = snap.cycle_index;
            const std::uint32_t restore_count = std::min(
                state.num_sequences,
                static_cast<std::uint32_t>(SNAPSHOT_MAX_SEQS));
            for (std::uint32_t i = 0; i < restore_count; ++i) {
                // Preserve the raw step counter — the ALTERNATE query path
                // applies `step % num_events` itself, so the counter stays
                // monotonic even when num_events changes between compiles.
                state.sequences[i].step = snap.seq_steps[i];
            }
            // Repopulate the per-cycle output cache from the OLD output
            // buffer. The arena is bump-allocated and never frees, so the
            // OLD pointer is still valid here. Copying the events keeps
            // SEQPAT_QUERY's cache-hit check satisfied on the next call,
            // preventing a spurious re-query that would advance ALTERNATE
            // step counters mid-cycle.
            if (old_output_events && state.output.events &&
                state.output.capacity > 0 && old_output_num_events > 0) {
                const std::uint32_t copy_n =
                    std::min(old_output_num_events, state.output.capacity);
                for (std::uint32_t i = 0; i < copy_n; ++i) {
                    state.output.events[i] = old_output_events[i];
                }
                state.output.num_events = copy_n;
            }
        }
    }

    // Configure iter()/iterBack() rotation on a SequenceState. Called after
    // init_sequence_program. n=0 disables rotation; dir is +1 for iter, -1
    // for iterBack. Does nothing if the state slot is not a SequenceState.
    void init_sequence_iter(std::uint32_t state_id,
                            std::uint8_t n, std::int8_t dir) {
        auto& state = get_or_create<SequenceState>(state_id);
        state.iter_n = n;
        state.iter_dir = dir;
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
    std::uint32_t state_id_xor_ = 0; // XOR mask for poly voice state isolation
};

}  // namespace cedar
