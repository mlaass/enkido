#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>

namespace cedar {

// ============================================================================
// Sequence System - Simple, Verifiable Pattern Model
// ============================================================================
//
// This replaces the complex pattern system (15 operators, PatternOp, etc.)
// with a minimal model:
//
// - Only 2 event types: DATA (concrete values) and SUB_SEQ (sequence reference)
// - 3 sequence modes: NORMAL (all events), ALTERNATE (one per call), RANDOM
// - Modifiers (*N, !N) are compile-time transformations, not runtime operators

// ============================================================================
// Event Types
// ============================================================================

enum class EventType : std::uint8_t {
    DATA,       // Concrete values (up to 16 numbers)
    SUB_SEQ     // Reference to another sequence
};

// ============================================================================
// Event - A single occurrence in a sequence
// ============================================================================

// Maximum values per event (covers single notes and basic chords)
static constexpr std::size_t MAX_VALUES_PER_EVENT = 4;

struct Event {
    float time;              // Position in cycle (beats)
    float duration;          // Event duration (beats)
    float chance;            // 0.0-1.0, 1.0 = always plays
    EventType type;
    std::uint8_t num_values; // For DATA type (max 4)
    std::uint16_t source_offset; // For UI highlighting
    std::uint16_t source_length;
    union {
        float values[MAX_VALUES_PER_EVENT]; // DATA: up to 4 voices
        std::uint16_t seq_id;               // SUB_SEQ: which sequence to call
    };

    Event() : time(0.0f), duration(1.0f), chance(1.0f),
              type(EventType::DATA), num_values(0),
              source_offset(0), source_length(0) {
        values[0] = 0.0f;
    }
};

// ============================================================================
// Sequence Modes
// ============================================================================

enum class SequenceMode : std::uint8_t {
    NORMAL,      // Return all events (for [a b c])
    ALTERNATE,   // Return one event per call, advance step (for <a b c>)
    RANDOM       // Return one random event per call (for a | b | c)
};

// ============================================================================
// Sequence - A collection of events with a playback mode
// ============================================================================

static constexpr std::size_t MAX_EVENTS_PER_SEQ = 8;

struct Sequence {
    Event events[MAX_EVENTS_PER_SEQ]; // Max 16 events per sequence
    std::uint32_t num_events = 0;
    float duration = 4.0f;            // Total duration in beats
    std::uint32_t step = 0;           // Current step (for ALTERNATE mode)
    SequenceMode mode = SequenceMode::NORMAL;

    void add_event(const Event& e) {
        if (num_events < MAX_EVENTS_PER_SEQ) {
            events[num_events++] = e;
        }
    }
};

// ============================================================================
// OutputEvents - Collection of events produced by query
// ============================================================================

static constexpr std::size_t MAX_OUTPUT_EVENTS = 16;

struct OutputEvents {
    struct OutputEvent {
        float time;
        float duration;
        float values[MAX_VALUES_PER_EVENT];
        std::uint8_t num_values;
        std::uint16_t source_offset;
        std::uint16_t source_length;
    };

    OutputEvent events[MAX_OUTPUT_EVENTS];
    std::uint32_t num_events = 0;

    void add(float time, float duration, const float* vals, std::uint8_t count,
             std::uint16_t src_off = 0, std::uint16_t src_len = 0) {
        if (num_events < MAX_OUTPUT_EVENTS) {
            auto& e = events[num_events++];
            e.time = time;
            e.duration = duration;
            e.num_values = std::min(count, static_cast<std::uint8_t>(MAX_VALUES_PER_EVENT));
            e.source_offset = src_off;
            e.source_length = src_len;
            for (std::uint8_t i = 0; i < e.num_values; ++i) {
                e.values[i] = vals[i];
            }
        }
    }

    void clear() { num_events = 0; }

    // Sort events by time (insertion sort for small arrays)
    void sort_by_time() {
        for (std::uint32_t i = 1; i < num_events; ++i) {
            OutputEvent key = events[i];
            std::int32_t j = static_cast<std::int32_t>(i) - 1;
            while (j >= 0 && events[j].time > key.time) {
                events[j + 1] = events[j];
                --j;
            }
            events[j + 1] = key;
        }
    }
};

// ============================================================================
// SequenceState - Runtime state for sequence playback
// ============================================================================

static constexpr std::size_t MAX_SEQUENCES = 4;

struct SequenceState {
    // Compiled sequences (set at init time)
    Sequence sequences[MAX_SEQUENCES];
    std::uint32_t num_sequences = 0;

    // Pattern parameters
    float cycle_length = 4.0f;
    std::uint64_t pattern_seed = 0;
    bool is_sample_pattern = false;

    // Query results
    OutputEvents output;

    // Playback state
    std::uint32_t current_index = 0;
    float last_beat_pos = -1.0f;
    float last_queried_cycle = -1.0f;

    // Active event for UI highlighting
    std::uint16_t active_source_offset = 0;
    std::uint16_t active_source_length = 0;

    // Add a sequence and return its ID
    std::uint16_t add_sequence(const Sequence& seq) {
        if (num_sequences < MAX_SEQUENCES) {
            sequences[num_sequences] = seq;
            return static_cast<std::uint16_t>(num_sequences++);
        }
        return 0; // Overflow - return root
    }
};

// Ensure size is reasonable for state pool
static_assert(sizeof(SequenceState) < 2000, "SequenceState too large for state pool");

// ============================================================================
// Deterministic Randomness
// ============================================================================

// Splitmix64-style mixer for deterministic pseudo-random values
[[gnu::always_inline]]
inline std::uint64_t splitmix64_seq(std::uint64_t x) {
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Mix pattern seed with time position for deterministic randomness
// Same seed + time always produces same result (important for seek/scrub)
[[gnu::always_inline]]
inline float deterministic_random_seq(std::uint64_t pattern_seed, float time_position) {
    // Quantize time to avoid floating point issues (10000 quanta per beat)
    std::uint64_t time_quant = static_cast<std::uint64_t>(time_position * 10000.0f);
    std::uint64_t h = splitmix64_seq(pattern_seed ^ time_quant);
    return static_cast<float>(h & 0xFFFFFFFFull) / 4294967296.0f;
}

// ============================================================================
// Runtime Evaluation
// ============================================================================

// Forward declaration
void process_event(SequenceState& state, const Event& e, std::uint64_t seed,
                   float time_offset, float time_scale, OutputEvents& out);

// Query a sequence and add events to output
inline void query_sequence(SequenceState& state, std::uint16_t seq_id, std::uint64_t seed,
                           float time_offset, float time_scale, OutputEvents& out) {
    if (seq_id >= state.num_sequences) return;
    Sequence& seq = state.sequences[seq_id];

    switch (seq.mode) {
        case SequenceMode::ALTERNATE: {
            // Return one event, advance step
            if (seq.num_events == 0) return;
            std::uint32_t idx = seq.step % seq.num_events;
            seq.step++;
            process_event(state, seq.events[idx], seed, time_offset, time_scale, out);
            break;
        }

        case SequenceMode::RANDOM: {
            // Pick one event randomly
            if (seq.num_events == 0) return;
            float rnd = deterministic_random_seq(seed, time_offset);
            std::uint32_t pick = static_cast<std::uint32_t>(rnd * static_cast<float>(seq.num_events));
            pick = pick % seq.num_events; // Safety clamp
            process_event(state, seq.events[pick], seed ^ (pick + 1), time_offset, time_scale, out);
            break;
        }

        case SequenceMode::NORMAL:
        default:
            // Process all events
            for (std::uint32_t i = 0; i < seq.num_events; ++i) {
                const Event& e = seq.events[i];
                float event_time = time_offset + e.time * time_scale / seq.duration;
                process_event(state, e, seed ^ i, event_time, time_scale, out);
            }
            break;
    }
}

// Process a single event (DATA or SUB_SEQ)
inline void process_event(SequenceState& state, const Event& e, std::uint64_t seed,
                          float time_offset, float time_scale, OutputEvents& out) {
    // Chance filter (degrade)
    if (e.chance < 1.0f) {
        float rnd = deterministic_random_seq(seed, time_offset);
        if (rnd >= e.chance) return;
    }

    if (e.type == EventType::DATA) {
        // Add concrete event
        out.add(time_offset, e.duration * time_scale,
                e.values, e.num_values,
                e.source_offset, e.source_length);
    } else {
        // SUB_SEQ: recursively query the referenced sequence
        // Scale time_scale by event duration so child events fit within this event's span
        query_sequence(state, e.seq_id, seed ^ e.seq_id, time_offset, e.duration * time_scale, out);
    }
}

// ============================================================================
// High-Level Query API
// ============================================================================

// Query the root sequence (sequence 0) for the current cycle
inline void query_pattern(SequenceState& state, std::uint64_t cycle, float cycle_length) {
    state.output.clear();

    // Query root sequence (always ID 0)
    std::uint64_t seed = state.pattern_seed + cycle;
    query_sequence(state, 0, seed, 0.0f, cycle_length, state.output);

    state.output.sort_by_time();
    state.current_index = 0;
}

}  // namespace cedar
