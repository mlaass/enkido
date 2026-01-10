#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace akkado {

/// Type of pattern event
enum class PatternEventType : std::uint8_t {
    Pitch,      // Melodic note (MIDI note number)
    Sample,     // Sample trigger (sample name + variant)
    Rest,       // Silence (no output)
};

/// A single event in an expanded pattern timeline
///
/// Events are positioned within a single cycle (0.0 to 1.0).
/// The pattern evaluator expands the mini-notation AST into a flat
/// list of these events, which can then be compiled to bytecode.
struct PatternEvent {
    PatternEventType type = PatternEventType::Rest;

    // Timing (relative to cycle)
    float time = 0.0f;          // Start time (0.0 to 1.0 within cycle)
    float duration = 1.0f;      // Duration (fraction of cycle)

    // Dynamics
    float velocity = 1.0f;      // Velocity/amplitude (0.0 to 1.0)
    float chance = 1.0f;        // Probability of playing (0.0 to 1.0)

    // Pitch data (for Pitch type)
    std::uint8_t midi_note = 60; // MIDI note number (60 = C4)

    // Sample data (for Sample type)
    std::string sample_name;    // Sample identifier (e.g., "bd", "sd")
    std::uint8_t sample_variant = 0; // Sample variant (e.g., 0 for "bd", 2 for "bd:2")

    /// Check if this event should trigger (based on chance)
    /// @param random_value Random value in [0, 1)
    [[nodiscard]] bool should_trigger(float random_value) const {
        return random_value < chance;
    }

    /// Check if this is a rest event
    [[nodiscard]] bool is_rest() const {
        return type == PatternEventType::Rest;
    }

    /// Check if this is a pitch event
    [[nodiscard]] bool is_pitch() const {
        return type == PatternEventType::Pitch;
    }

    /// Check if this is a sample event
    [[nodiscard]] bool is_sample() const {
        return type == PatternEventType::Sample;
    }
};

/// A complete pattern expanded into a timeline of events
///
/// This represents one cycle of a pattern, with all mini-notation
/// constructs (groups, sequences, modifiers) resolved into concrete events.
struct PatternEventStream {
    std::vector<PatternEvent> events;

    /// Number of events in the stream
    [[nodiscard]] std::size_t size() const {
        return events.size();
    }

    /// Check if the stream is empty
    [[nodiscard]] bool empty() const {
        return events.empty();
    }

    /// Add an event to the stream
    void add(PatternEvent event) {
        events.push_back(std::move(event));
    }

    /// Sort events by time (for proper playback order)
    void sort_by_time();

    /// Get events within a time range (inclusive start, exclusive end)
    [[nodiscard]] std::vector<const PatternEvent*>
    events_in_range(float start, float end) const;

    /// Merge another stream into this one (for polyrhythms)
    void merge(const PatternEventStream& other);

    /// Scale all event times and durations by a factor
    void scale_time(float factor);

    /// Offset all event times by an amount
    void offset_time(float offset);

    /// Clear all events
    void clear() {
        events.clear();
    }
};

/// Context for pattern evaluation
///
/// Passed to the pattern evaluator to track the current time span
/// and accumulate modifiers as we traverse the AST.
struct PatternEvalContext {
    float start_time = 0.0f;    // Start of current time span
    float duration = 1.0f;      // Duration of current time span
    float velocity = 1.0f;      // Current velocity multiplier
    float chance = 1.0f;        // Current chance multiplier

    /// Create a child context for a subdivision
    /// @param child_index Index of child in subdivision
    /// @param child_count Total number of children
    [[nodiscard]] PatternEvalContext subdivide(std::size_t child_index,
                                                std::size_t child_count) const {
        float child_duration = duration / static_cast<float>(child_count);
        float child_start = start_time + child_duration * static_cast<float>(child_index);
        return {
            .start_time = child_start,
            .duration = child_duration,
            .velocity = velocity,
            .chance = chance
        };
    }

    /// Create a child context that inherits all properties (for polyrhythm)
    [[nodiscard]] PatternEvalContext inherit() const {
        return *this;
    }

    /// Apply a speed modifier (multiply time by factor)
    [[nodiscard]] PatternEvalContext with_speed(float factor) const {
        return {
            .start_time = start_time,
            .duration = duration / factor,
            .velocity = velocity,
            .chance = chance
        };
    }

    /// Apply a velocity modifier
    [[nodiscard]] PatternEvalContext with_velocity(float vel) const {
        return {
            .start_time = start_time,
            .duration = duration,
            .velocity = velocity * vel,
            .chance = chance
        };
    }

    /// Apply a chance modifier
    [[nodiscard]] PatternEvalContext with_chance(float ch) const {
        return {
            .start_time = start_time,
            .duration = duration,
            .velocity = velocity,
            .chance = chance * ch
        };
    }
};

} // namespace akkado
