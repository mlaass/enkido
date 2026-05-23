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

// Maximum values per event. 16 covers full 13th chords (1,3,5,7,9,11,13)
// plus user-registered jazz voicings with up-to-7-note dictionaries and
// large polyrhythm sample patterns. Bumped from 4 because the original
// cap silently truncated 5+ voice voicings registered via addVoicings().
static constexpr std::size_t MAX_VALUES_PER_EVENT = 16;

// Maximum custom property slots per event (Phase 2.1, PRD §11). Slot
// assignment is compile-time, pattern-local. Used by SEQPAT_PROP to surface
// values from `c4{cutoff:0.3}` and standalone `bend()/aftertouch()` transforms.
static constexpr std::size_t MAX_PROPS_PER_EVENT = 4;

struct Event {
    float time;              // Position in cycle (beats)
    float duration;          // Event duration (beats)
    float chance;            // 0.0-1.0, 1.0 = always plays
    float velocity;          // 0.0-1.0, 1.0 = full velocity. Sample patterns
                             // pin this to 1.0 and use velocities[] per voice;
                             // pitched/value events use this scalar field.
    float midi_note;         // MIDI note number (0..127) for the primary voice;
                             // 0 for sample-only events without pitch. Carries
                             // microtonal precision so %.note round-trips the
                             // user-written note rather than reconstructing
                             // from frequency.
    EventType type;
    std::uint8_t num_values; // For DATA type (max 4)
    std::uint16_t type_id;   // Type identifier for routing (0 = no type, 1+ = sample types)
    std::uint16_t source_offset; // For UI highlighting
    std::uint16_t source_length;
    union {
        float values[MAX_VALUES_PER_EVENT]; // DATA: up to 4 voices
        std::uint16_t seq_id;               // SUB_SEQ: which sequence to call
    };
    // Per-voice velocity, parallel to values[]. Required by sample patterns
    // so `[cp, bd{vel:0.05}]` plays cp at 1.0 and bd at 0.05 independently
    // (op_sample_play applies velocities[v] per voice). Default 1.0.
    float velocities[MAX_VALUES_PER_EVENT];
    // Per-voice MIDI note, parallel to values[]. Populated only for chord
    // events (num_values > 1) where each voice has a distinct pitch — chord
    // expansion knows the per-voice note. Single-voice events use the scalar
    // `midi_note` instead. Default 0.0f.
    float notes[MAX_VALUES_PER_EVENT];
    // Custom property values (Phase 2.1, PRD §11). Slot indices are assigned
    // at compile time by SequenceCompiler; semantics are pattern-local.
    // Default 0.0f means "not set".
    float prop_vals[MAX_PROPS_PER_EVENT];
    // Per-event presence mask for prop_vals (Phase 3, prd-poly-callback-event-
    // record). Bit S set means this event explicitly declared prop slot S.
    // Distinguishes "set to 0.0" from "absent" so a poly-callback destructure
    // default (`{cutoff = 0.5}`) applies only to notes that omit the field.
    std::uint8_t prop_set_mask;

    Event() : time(0.0f), duration(1.0f), chance(1.0f), velocity(1.0f),
              midi_note(0.0f),
              type(EventType::DATA), num_values(0), type_id(0),
              source_offset(0), source_length(0), prop_set_mask(0) {
        values[0] = 0.0f;
        for (std::size_t i = 0; i < MAX_VALUES_PER_EVENT; ++i) {
            velocities[i] = 1.0f;
            notes[i] = 0.0f;
        }
        for (std::size_t i = 0; i < MAX_PROPS_PER_EVENT; ++i) {
            prop_vals[i] = 0.0f;
        }
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

struct Sequence {
    Event* events = nullptr;          // Pointer to arena-allocated events
    std::uint32_t num_events = 0;
    std::uint32_t capacity = 0;       // Allocated event count
    float duration = 1.0f;            // Total duration in beats (cycle = beat)
    std::uint32_t step = 0;           // Current step (for ALTERNATE mode)
    SequenceMode mode = SequenceMode::NORMAL;

    // Add event (only during compilation when capacity allows)
    void add_event(const Event& e) {
        if (num_events < capacity) {
            events[num_events++] = e;
        }
    }
};

// ============================================================================
// OutputEvents - Collection of events produced by query
// ============================================================================

struct OutputEvents {
    struct OutputEvent {
        float time;
        float duration;
        float velocity;
        float chance;                 // Mirrors Event::chance for %.chance access.
        float midi_note;              // Mirrors Event::midi_note for %.note access.
        float values[MAX_VALUES_PER_EVENT];
        // Per-voice velocity, parallel to values[]. Mirrors Event::velocities.
        float velocities[MAX_VALUES_PER_EVENT];
        // Per-voice MIDI note, parallel to values[]. Mirrors Event::notes.
        // Populated for chord events; single-voice events use midi_note.
        float notes[MAX_VALUES_PER_EVENT];
        std::uint8_t num_values;
        std::uint16_t type_id;        // Type identifier for routing
        std::uint16_t source_offset;
        std::uint16_t source_length;
        // Custom property values (Phase 2.1, PRD §11). Mirrors Event::prop_vals.
        float prop_vals[MAX_PROPS_PER_EVENT];
        // Per-event prop presence mask (Phase 3). Mirrors Event::prop_set_mask.
        std::uint8_t prop_set_mask;
    };

    OutputEvent* events = nullptr;    // Pointer to arena-allocated output events
    std::uint32_t num_events = 0;
    std::uint32_t capacity = 0;       // Allocated event count

    void add(float time, float duration, const float* vals, std::uint8_t count,
             float velocity = 1.0f, std::uint16_t type_id = 0,
             std::uint16_t src_off = 0, std::uint16_t src_len = 0,
             float chance = 1.0f, float midi_note = 0.0f) {
        if (num_events < capacity) {
            auto& e = events[num_events++];
            e.time = time;
            e.duration = duration;
            e.velocity = velocity;
            e.chance = chance;
            e.midi_note = midi_note;
            e.num_values = std::min(count, static_cast<std::uint8_t>(MAX_VALUES_PER_EVENT));
            e.type_id = type_id;
            e.source_offset = src_off;
            e.source_length = src_len;
            for (std::uint8_t i = 0; i < e.num_values; ++i) {
                e.values[i] = vals[i];
            }
            // Default per-voice velocity to the event-wide value; callers that
            // want per-voice control overwrite e.velocities[] after add().
            // Default per-voice note to the event-wide midi_note; chord-aware
            // callers (process_event) overwrite e.notes[] after add().
            for (std::size_t i = 0; i < MAX_VALUES_PER_EVENT; ++i) {
                e.velocities[i] = velocity;
                e.notes[i] = midi_note;
            }
            // Phase 3: prop presence defaults to "none set"; process_event
            // overwrites prop_vals + mask for pattern events. Non-pattern
            // callers (e.g. MIDI queue) leave the event prop-free.
            e.prop_set_mask = 0;
            for (std::size_t i = 0; i < MAX_PROPS_PER_EVENT; ++i) {
                e.prop_vals[i] = 0.0f;
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

struct SequenceState {
    // Compiled sequences (set at init time)
    Sequence* sequences = nullptr;    // Pointer to arena-allocated sequences
    std::uint32_t num_sequences = 0;
    std::uint32_t seq_capacity = 0;   // Allocated sequence count

    // Pattern parameters
    float cycle_length = 1.0f;
    std::uint64_t pattern_seed = 0;
    bool is_sample_pattern = false;

    // Query results
    OutputEvents output;

    // Playback state
    std::uint32_t current_index = 0;
    float last_beat_pos = -1.0f;
    float last_queried_cycle = -1.0f;

    // SEQPAT_GATE retrigger tracking. Separate from `last_beat_pos` because
    // SEQPAT_STEP and SEQPAT_GATE may run in different orders relative to
    // other opcodes that read `last_beat_pos`. Sentinel -1.0f means "no
    // prior sample observed yet"; in that case the gate uses the natural
    // any-active state without a retrigger drop.
    float last_beat_pos_gate = -1.0f;

    // Cycle index for the current playback cycle. Set per block from
    // (global_sample_counter / spc / cycle_length); used by EVENT_REORDER(ITER)
    // and by the legacy iter rotation (REMOVED in Phase 4 Commit C; field
    // stays because it's still computed in SEQPAT_QUERY for diagnostic /
    // hot-swap continuity purposes).
    std::uint32_t cycle_index = 0;

    // Phase 4 (prd-runtime-event-transforms) — EVENT_REORDER / EVENT_FANOUT
    // transform state. EVENT_MAP / EVENT_FILTER reuse SequenceState as their
    // output holder; EVENT_REORDER / EVENT_FANOUT do the same and additionally
    // need these fields:
    //   - last_reorder_cycle: ITER cache invalidation (rebuild rotation when
    //     cycle index advances).
    //   - reorder_original_cycle_length / reorder_captured: first-block capture
    //     of the upstream cycle_length so ITER's rotation math and LINGER's
    //     downstream cycle_length scaling have a stable basis.
    std::uint32_t last_reorder_cycle = static_cast<std::uint32_t>(-1);
    float reorder_original_cycle_length = 0.0f;
    bool  reorder_captured = false;

    // Active event for UI highlighting
    std::uint16_t active_source_offset = 0;
    std::uint16_t active_source_length = 0;

    // Add a sequence and return its ID (only during initialization when capacity allows)
    std::uint16_t add_sequence(const Sequence& seq) {
        if (num_sequences < seq_capacity) {
            sequences[num_sequences] = seq;
            return static_cast<std::uint16_t>(num_sequences++);
        }
        return 0; // Overflow - return root
    }
};

// SequenceState is now small - just pointers and scalars
static_assert(sizeof(SequenceState) < 256, "SequenceState too large");

// ============================================================================
// RateScaleState — EVENT_RATE_SCALE per-block cycle_length modulator
// ============================================================================
//
// PRD prd-runtime-event-transforms Phase 3. Lives in sequence.hpp so the
// dsp_state.hpp DSPState variant can reference it without pulling in
// event_transforms.hpp (which depends on dsp_state.hpp). Op body lives in
// event_transforms.hpp::op_event_rate_scale.
//
// The opcode mutates an upstream SequenceState's `cycle_length` field each
// block: `upstream.cycle_length = original_cycle_length / rate`. Every SEQPAT_*
// opcode reads cycle_length when computing cycle_pos / event scaling, so a
// single-field write covers QUERY, STEP, GATE, FIELD, PHASE, TYPE, PROP, and
// VALUES uniformly — no need to patch each opcode's external-clock slot or
// feed a phase signal.
//
// `original_cycle_length` is captured on the first block (after the program
// loader has initialised the upstream SequenceState from its
// SequenceProgram StateInitData, which is the authoritative source for the
// pattern's compiled cycle_length, including any compile-time slow/fast
// mutations inside compile_pattern_for_transform's recursive path).
struct RateScaleState {
    float original_cycle_length = 1.0f;  // captured on first block
    bool  initialized = false;
};

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
                e.values, e.num_values, e.velocity, e.type_id,
                e.source_offset, e.source_length,
                e.chance, e.midi_note);
        // Copy custom property values onto the just-added OutputEvent
        // (PRD §11.1 / §11.2 — surfaced via SEQPAT_PROP).
        // Also copy per-voice velocities so op_sample_play can apply them.
        if (out.num_events > 0) {
            auto& last = out.events[out.num_events - 1];
            for (std::size_t i = 0; i < MAX_VALUES_PER_EVENT; ++i) {
                last.velocities[i] = e.velocities[i];
                last.notes[i] = e.notes[i];
            }
            for (std::size_t i = 0; i < MAX_PROPS_PER_EVENT; ++i) {
                last.prop_vals[i] = e.prop_vals[i];
            }
            last.prop_set_mask = e.prop_set_mask;
        }
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
