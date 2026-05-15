#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
#include <bit>
#include <cmath>
#include <algorithm>

namespace cedar {

// LFO shape types (encoded in inst.rate field)
enum class LFOShape : std::uint8_t {
    SIN = 0,
    TRI = 1,
    SAW = 2,
    RAMP = 3,
    SQR = 4,
    PWM = 5,
    SAH = 6
};

// ============================================================================
// CLOCK - Beat/bar/cycle phase output
// ============================================================================
// Rate field selects phase type:
//   0 = beat_phase (0-1 per beat)
//   1 = bar_phase (0-1 per 4 beats)
//   2 = cycle_offset (same as bar_phase)
[[gnu::always_inline]]
inline void op_clock(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);

    const float spb = ctx.samples_per_beat();
    const float spbar = ctx.samples_per_bar();

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float sample = static_cast<float>(ctx.global_sample_counter + i);

        switch (inst.rate) {
            case 0:  // beat_phase
                out[i] = std::fmod(sample, spb) / spb;
                break;
            case 1:  // bar_phase
            case 2:  // cycle_offset (alias)
            default:
                out[i] = std::fmod(sample, spbar) / spbar;
                break;
        }
    }
}

// ============================================================================
// LFO - Beat-synced low frequency oscillator
// ============================================================================
// in0: frequency multiplier (cycles per beat, e.g., 1.0 = one cycle per beat)
// in1: duty cycle (for PWM shape only, 0-1)
// rate field: LFOShape (0-6)
[[gnu::always_inline]]
inline void op_lfo(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq_mult = ctx.buffers->get(inst.inputs[0]);
    auto& state = ctx.states->get_or_create<LFOState>(inst.state_id);

    const float spb = ctx.samples_per_beat();
    const LFOShape shape = static_cast<LFOShape>(inst.rate);

    // For PWM, get duty cycle buffer
    const float* duty = nullptr;
    if (shape == LFOShape::PWM && inst.inputs[1] != BUFFER_UNUSED) {
        duty = ctx.buffers->get(inst.inputs[1]);
    }

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Direct phase calculation from global sample counter
        std::uint64_t sample = ctx.global_sample_counter + i;
        float cycles = static_cast<float>(sample) * freq_mult[i] / spb;
        float phase = cycles - std::floor(cycles);  // fmod equivalent for 0-1 range

        float value = 0.0f;

        switch (shape) {
            case LFOShape::SIN:
                value = std::sin(phase * TWO_PI);
                break;

            case LFOShape::TRI:
                value = 4.0f * std::abs(phase - 0.5f) - 1.0f;
                break;

            case LFOShape::SAW:
                value = 2.0f * phase - 1.0f;
                break;

            case LFOShape::RAMP:
                value = 1.0f - 2.0f * phase;
                break;

            case LFOShape::SQR:
                value = (phase < 0.5f) ? 1.0f : -1.0f;
                break;

            case LFOShape::PWM: {
                float d = duty ? duty[i] : 0.5f;
                value = (phase < d) ? 1.0f : -1.0f;
                break;
            }

            case LFOShape::SAH:
                // Sample new random value when phase wraps
                if (phase < state.prev_phase && state.prev_phase > 0.5f) {
                    // Generate deterministic pseudo-random value
                    std::uint32_t h = static_cast<std::uint32_t>(ctx.global_sample_counter + i);
                    h ^= inst.state_id;
                    h = (h ^ 61) ^ (h >> 16);
                    h *= 9;
                    h ^= h >> 4;
                    h *= 0x27d4eb2d;
                    h ^= h >> 15;
                    state.prev_value = static_cast<float>(static_cast<std::int32_t>(h)) / 2147483648.0f;
                }
                value = state.prev_value;
                break;
        }

        out[i] = value;
        state.prev_phase = phase;
    }
}

// ============================================================================
// EUCLID - Euclidean rhythm trigger generator
// ============================================================================
// Helper: Compute Euclidean pattern as bitmask using Bjorklund algorithm
inline std::uint32_t compute_euclidean_pattern(std::uint32_t hits, std::uint32_t steps, std::uint32_t rotation) {
    if (steps == 0 || hits == 0) return 0;
    if (hits >= steps) return (1u << steps) - 1;  // All hits

    std::uint32_t pattern = 0;
    float bucket = 0.0f;
    float increment = static_cast<float>(hits) / static_cast<float>(steps);

    for (std::uint32_t i = 0; i < steps; ++i) {
        bucket += increment;
        if (bucket >= 1.0f) {
            pattern |= (1u << i);
            bucket -= 1.0f;
        }
    }

    // Apply rotation (shift pattern left — each hit moves to the next step)
    if (rotation > 0 && steps > 0) {
        rotation = rotation % steps;
        std::uint32_t mask = (1u << steps) - 1;
        pattern = ((pattern << rotation) | (pattern >> (steps - rotation))) & mask;
    }

    return pattern;
}

// in0: hits (number of triggers in pattern)
// in1: steps (total steps in pattern)
// in2: rotation (optional, shifts pattern)
// Outputs: 1.0 on trigger, 0.0 otherwise
[[gnu::always_inline]]
inline void op_euclid(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* hits_buf = ctx.buffers->get(inst.inputs[0]);
    const float* steps_buf = ctx.buffers->get(inst.inputs[1]);
    auto& state = ctx.states->get_or_create<EuclidState>(inst.state_id);

    // Sample parameters at start of block (control rate)
    std::uint32_t hits = static_cast<std::uint32_t>(std::max(0.0f, hits_buf[0]));
    std::uint32_t steps = static_cast<std::uint32_t>(std::max(1.0f, steps_buf[0]));
    std::uint32_t rotation = 0;

    if (inst.inputs[2] != BUFFER_UNUSED) {
        const float* rot_buf = ctx.buffers->get(inst.inputs[2]);
        rotation = static_cast<std::uint32_t>(std::max(0.0f, rot_buf[0]));
    }

    // Recompute pattern only if parameters changed
    if (hits != state.last_hits || steps != state.last_steps || rotation != state.last_rotation) {
        state.pattern = compute_euclidean_pattern(hits, steps, rotation);
        state.last_hits = hits;
        state.last_steps = steps;
        state.last_rotation = rotation;
        // Reset prev_step on pattern change for consistent behavior
        state.prev_step = UINT32_MAX;
    }

    // One bar = 4 beats
    const float spb = ctx.samples_per_beat();
    const float samples_per_bar = spb * 4.0f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Direct calculation: which step are we in?
        std::uint64_t sample = ctx.global_sample_counter + i;
        float bar_phase = std::fmod(static_cast<float>(sample), samples_per_bar) / samples_per_bar;
        std::uint32_t current_step = static_cast<std::uint32_t>(bar_phase * static_cast<float>(steps)) % steps;

        // Detect step boundary
        bool step_changed = (current_step != state.prev_step);
        state.prev_step = current_step;

        // Trigger if step changed AND this step is a hit
        bool is_hit = (state.pattern >> current_step) & 1;
        out[i] = (step_changed && is_hit) ? 1.0f : 0.0f;
    }
}

// ============================================================================
// TRIGGER - Beat-division impulse generator
// ============================================================================
// in0: division (triggers per beat, e.g., 1=quarter, 2=eighth, 4=16th)
// Outputs: 1.0 on trigger, 0.0 otherwise
[[gnu::always_inline]]
inline void op_trigger(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* division = ctx.buffers->get(inst.inputs[0]);
    auto& state = ctx.states->get_or_create<TriggerState>(inst.state_id);

    const float spb = ctx.samples_per_beat();

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Direct phase calculation from global sample counter
        std::uint64_t sample = ctx.global_sample_counter + i;
        float samples_per_trigger = spb / division[i];

        // Current phase within trigger period
        float phase = std::fmod(static_cast<float>(sample), samples_per_trigger) / samples_per_trigger;

        // Detect trigger: phase wrapped (current phase < previous phase with significant drop)
        bool trigger = (phase < state.prev_phase && state.prev_phase > 0.5f);

        out[i] = trigger ? 1.0f : 0.0f;
        state.prev_phase = phase;
    }
}

// ============================================================================
// TIMELINE - Breakpoint automation with interpolation
// ============================================================================
// Uses TimelineState for breakpoint data (must be initialized before use)
// Outputs interpolated value based on current beat position
[[gnu::always_inline]]
inline void op_timeline(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    auto& state = ctx.states->get_or_create<TimelineState>(inst.state_id);

    if (state.num_points == 0) {
        std::fill_n(out, BLOCK_SIZE, 0.0f);
        return;
    }

    const float spb = ctx.samples_per_beat();

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Calculate current time in beats
        float time_beats = static_cast<float>(ctx.global_sample_counter + i) / spb;

        // Handle looping
        if (state.loop && state.loop_length > 0.0f) {
            time_beats = std::fmod(time_beats, state.loop_length);
        }

        // Find surrounding breakpoints
        std::uint32_t idx = 0;
        while (idx < state.num_points - 1 && state.points[idx + 1].time <= time_beats) {
            idx++;
        }

        const auto& p0 = state.points[idx];

        // If at or past last point, or curve is hold, output current value
        if (idx >= state.num_points - 1 || p0.curve == 2) {
            out[i] = p0.value;
            continue;
        }

        const auto& p1 = state.points[idx + 1];

        // Interpolate between p0 and p1
        float t = (time_beats - p0.time) / (p1.time - p0.time);
        t = std::clamp(t, 0.0f, 1.0f);

        if (p0.curve == 0) {
            // Linear interpolation
            out[i] = p0.value + t * (p1.value - p0.value);
        } else if (p0.curve == 1) {
            // Exponential (quadratic ease-in)
            t = t * t;
            out[i] = p0.value + t * (p1.value - p0.value);
        } else {
            // Default to linear
            out[i] = p0.value + t * (p1.value - p0.value);
        }
    }
}

// ============================================================================
// SEQPAT_TRANSPORT - Trigger-driven clock for pattern playback
// ============================================================================
// out_buffer: beat_pos output (audio-rate)
// inputs[0]: trigger signal (rising edge advances position)
// inputs[1]: step size (default 1.0; negative = reverse)
// inputs[2]: reset trigger (rising edge resets to 0; 0xFFFF = unused)
// inputs[3]: low 16 bits of bit_cast<uint32_t>(cycle_length)
// inputs[4]: high 16 bits of bit_cast<uint32_t>(cycle_length)
[[gnu::always_inline]]
inline void op_seqpat_transport(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* trig = ctx.buffers->get(inst.inputs[0]);
    const float* step = (inst.inputs[1] != BUFFER_UNUSED)
        ? ctx.buffers->get(inst.inputs[1]) : nullptr;
    const float* reset = (inst.inputs[2] != BUFFER_UNUSED)
        ? ctx.buffers->get(inst.inputs[2]) : nullptr;

    auto& state = ctx.states->get_or_create<TransportState>(inst.state_id);

    // Reconstruct cycle_length from two uint16_t halves
    std::uint32_t cl_bits = static_cast<std::uint32_t>(inst.inputs[3])
                          | (static_cast<std::uint32_t>(inst.inputs[4]) << 16);
    float cycle_length = std::bit_cast<float>(cl_bits);
    state.cycle_length = cycle_length;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float step_val = step ? step[i] : 1.0f;

        // Reset on rising edge
        if (reset) {
            float r = reset[i];
            if (r > 0.0f && state.last_reset <= 0.0f) {
                state.beat_pos = 0.0f;
            }
            state.last_reset = r;
        }

        // Advance on trigger rising edge
        float t = trig[i];
        if (t > 0.0f && state.last_trig <= 0.0f) {
            state.beat_pos += step_val;
            // Wrap at cycle_length
            if (cycle_length > 0.0f) {
                state.beat_pos = std::fmod(state.beat_pos, cycle_length);
                if (state.beat_pos < 0.0f) {
                    state.beat_pos += cycle_length;
                }
            }
        }
        state.last_trig = t;

        out[i] = state.beat_pos;
    }
}

// ============================================================================
// SEQPAT_QUERY - Query new sequence system at block boundaries
// ============================================================================
// Fills SequenceState.output[] for current cycle
// Called once per block to prepare events for SEQPAT_STEP
//
// Uses the new simplified sequence model with query_pattern()
[[gnu::always_inline]]
inline void op_seqpat_query(ExecutionContext& ctx, const Instruction& inst) {
    auto& state = ctx.states->get_or_create<SequenceState>(inst.state_id);

    float beat_start;
    bool external_clock = (inst.inputs[0] != BUFFER_UNUSED);

    if (external_clock) {
        // External clock: read beat_pos from transport buffer (start of block)
        const float* ext_beat_pos = ctx.buffers->get(inst.inputs[0]);
        beat_start = ext_beat_pos[0];
    } else {
        const float spb = ctx.samples_per_beat();
        // Calculate current beat position (start of block)
        beat_start = static_cast<float>(ctx.global_sample_counter) / spb;
    }

    // Determine which cycle we're currently in
    float current_cycle = std::floor(beat_start / state.cycle_length);

    // For external clock, always re-query (non-monotonic clock)
    if (!external_clock && current_cycle == state.last_queried_cycle && state.output.num_events > 0) {
        return;  // Use cached results from this cycle
    }

    // Update cycle tracking
    state.last_queried_cycle = current_cycle;
    // cycle_index drives iter()/iterBack() rotation. Tracking the global
    // cycle number directly (not a wrap-detect counter) means seek/scrub
    // produces the correct rotation for any cycle.
    state.cycle_index = static_cast<std::uint32_t>(current_cycle);

    // Query the root sequence for this cycle
    query_pattern(state, static_cast<std::uint64_t>(current_cycle), state.cycle_length);

    // iter()/iterBack(): rotate query results so that an event at original
    // time t plays at effective time ((t - dir * k / n) mod 1), where
    // k = cycle_index mod iter_n. Per PRD §5.2.
    if (state.iter_n > 0 && state.cycle_length > 0.0f) {
        std::uint32_t k = state.cycle_index % static_cast<std::uint32_t>(state.iter_n);
        float shift = -static_cast<float>(state.iter_dir) *
                      static_cast<float>(k) * state.cycle_length /
                      static_cast<float>(state.iter_n);
        for (std::uint32_t i = 0; i < state.output.num_events; ++i) {
            auto& e = state.output.events[i];
            float t = e.time + shift;
            t = std::fmod(t, state.cycle_length);
            if (t < 0.0f) t += state.cycle_length;
            e.time = t;
        }
        state.output.sort_by_time();
    }

    // Find first event at or after current beat position within cycle
    float cycle_pos = std::fmod(beat_start, state.cycle_length);
    state.current_index = 0;
    while (state.current_index < state.output.num_events &&
           state.output.events[state.current_index].time < cycle_pos) {
        state.current_index++;
    }
}

// ============================================================================
// SEQPAT_STEP - Step through sequence query results
// ============================================================================
// out_buffer: value output (frequency or sample ID)
// inputs[0]: velocity output buffer (0xFFFF to skip writing)
// inputs[1]: trigger output buffer (0xFFFF to skip writing)
// inputs[2]: voice index for polyphonic patterns (0-7, default 0 if 0xFFFF)
//            Selects which value from evt.values[] to output for chords
// Outputs current event value and trigger when events fire
[[gnu::always_inline]]
inline void op_seqpat_step(ExecutionContext& ctx, const Instruction& inst) {
    float* out_value = ctx.buffers->get(inst.out_buffer);
    // Velocity and trigger output are optional (use 0xFFFF to skip for secondary voices)
    float* out_velocity = (inst.inputs[0] != BUFFER_UNUSED)
        ? ctx.buffers->get(inst.inputs[0]) : nullptr;
    float* out_trigger = (inst.inputs[1] != BUFFER_UNUSED)
        ? ctx.buffers->get(inst.inputs[1]) : nullptr;
    auto& state = ctx.states->get_or_create<SequenceState>(inst.state_id);

    // Voice index for polyphonic patterns (selects which chord note to output)
    std::uint8_t voice_index = (inst.inputs[2] != BUFFER_UNUSED)
        ? static_cast<std::uint8_t>(inst.inputs[2]) : 0;

    if (state.output.num_events == 0) {
        std::fill_n(out_value, BLOCK_SIZE, 0.0f);
        if (out_velocity) std::fill_n(out_velocity, BLOCK_SIZE, 0.0f);
        if (out_trigger) std::fill_n(out_trigger, BLOCK_SIZE, 0.0f);
        return;
    }

    // Initialize active step from first event if not yet set (only for voice 0)
    if (voice_index == 0 && state.active_source_length == 0 && state.output.num_events > 0) {
        state.active_source_offset = state.output.events[0].source_offset;
        state.active_source_length = state.output.events[0].source_length;
    }

    // External clock override: inputs[3] contains beat_pos buffer
    bool external_clock = (inst.inputs[3] != BUFFER_UNUSED);
    const float* ext_beat_pos = external_clock ? ctx.buffers->get(inst.inputs[3]) : nullptr;

    const float spb = external_clock ? 1.0f : ctx.samples_per_beat();

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Current beat position within cycle
        float beat_pos;
        if (external_clock) {
            beat_pos = std::fmod(ext_beat_pos[i], state.cycle_length);
            if (beat_pos < 0.0f) beat_pos += state.cycle_length;
        } else {
            beat_pos = std::fmod(
                static_cast<float>(ctx.global_sample_counter + i) / spb,
                state.cycle_length
            );
        }

        // Detect cycle wrap (or external clock direction change)
        bool wrapped = (state.last_beat_pos >= 0.0f && beat_pos < state.last_beat_pos - 0.5f);
        if (wrapped) {
            state.current_index = 0;
        }

        // Check if we crossed an event time (for trigger)
        float trigger_val = 0.0f;
        while (state.current_index < state.output.num_events &&
               beat_pos >= state.output.events[state.current_index].time) {
            const auto& crossed_evt = state.output.events[state.current_index];
            // Only fire trigger for non-rest events (rest = num_values 0)
            if (crossed_evt.num_values > 0) {
                trigger_val = 1.0f;
            }
            // Update active step for UI highlighting (including rests)
            if (voice_index == 0) {
                state.active_source_offset = crossed_evt.source_offset;
                state.active_source_length = crossed_evt.source_length;
            }
            state.current_index++;
        }

        // Handle wrap: also trigger if we wrapped and crossed first event
        if (wrapped && state.output.num_events > 0 && beat_pos >= state.output.events[0].time) {
            trigger_val = 1.0f;
        }

        if (out_trigger) out_trigger[i] = trigger_val;

        // Output current value and velocity (from current event)
        std::uint32_t event_index = (state.current_index > 0)
            ? state.current_index - 1
            : state.output.num_events - 1;
        const auto& evt = state.output.events[event_index];

        // Select value based on voice index (for polyphonic chord support)
        // If voice_index exceeds available values, output 0 (silence that voice)
        out_value[i] = (voice_index < evt.num_values) ? evt.values[voice_index] : 0.0f;
        if (out_velocity) out_velocity[i] = evt.velocity;

        // Keep active step in sync with current event for UI highlighting (only voice 0)
        if (voice_index == 0) {
            state.active_source_offset = evt.source_offset;
            state.active_source_length = evt.source_length;
        }

        state.last_beat_pos = beat_pos;
    }
}

// ============================================================================
// SEQPAT_TYPE - Type ID signal for event routing
// ============================================================================
// out_buffer: type_id output (float representation of event's type_id)
// inputs[0]: voice index for polyphonic patterns (0-7, default 0 if 0xFFFF)
// state_id: must match the SEQPAT_QUERY that populated the SequenceState
//
// Outputs the type_id of the current event as a float.
// Used with match() for per-type routing (e.g., kick vs snare synthesis).
[[gnu::always_inline]]
inline void op_seqpat_type(ExecutionContext& ctx, const Instruction& inst) {
    float* out_type = ctx.buffers->get(inst.out_buffer);
    auto& state = ctx.states->get_or_create<SequenceState>(inst.state_id);

    // Voice index for polyphonic patterns
    std::uint8_t voice_index = (inst.inputs[0] != BUFFER_UNUSED)
        ? static_cast<std::uint8_t>(inst.inputs[0]) : 0;

    if (state.output.num_events == 0) {
        std::fill_n(out_type, BLOCK_SIZE, 0.0f);
        return;
    }

    // External clock override: inputs[1] contains beat_pos buffer
    bool external_clock = (inst.inputs[1] != BUFFER_UNUSED);
    const float* ext_beat_pos = external_clock ? ctx.buffers->get(inst.inputs[1]) : nullptr;

    const float spb = external_clock ? 1.0f : ctx.samples_per_beat();

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Current beat position within cycle
        float beat_pos;
        if (external_clock) {
            beat_pos = std::fmod(ext_beat_pos[i], state.cycle_length);
            if (beat_pos < 0.0f) beat_pos += state.cycle_length;
        } else {
            beat_pos = std::fmod(
                static_cast<float>(ctx.global_sample_counter + i) / spb,
                state.cycle_length
            );
        }

        // Find the current event (same logic as SEQPAT_STEP)
        std::uint32_t event_index = 0;
        for (std::uint32_t e = 0; e < state.output.num_events; ++e) {
            if (state.output.events[e].time <= beat_pos) {
                event_index = e;
            } else {
                break;
            }
        }

        // If beat_pos is before first event, use the last event (wrap-around)
        if (state.output.num_events > 0 && beat_pos < state.output.events[0].time) {
            event_index = state.output.num_events - 1;
        }

        const auto& evt = state.output.events[event_index];

        // Check if this voice has a value for this event
        if (voice_index < evt.num_values) {
            out_type[i] = static_cast<float>(evt.type_id);
        } else {
            out_type[i] = 0.0f;  // Voice is silent for this event
        }
    }
}

// ============================================================================
// SEQPAT_GATE - Sustained gate signal for sequence events
// ============================================================================
// out_buffer: gate output (1.0 while inside event duration, 0.0 otherwise)
// inputs[0]: voice index for polyphonic patterns (0-7, default 0 if 0xFFFF)
// state_id: must match the SEQPAT_QUERY that populated the SequenceState
//
// Gate is high when beat_pos is in [event.time, event.time + event.duration),
// **except** that at each event onset we force gate=0 for that one sample to
// give retrigger-style envelopes (AR, ADSR) a rising edge per note. The drop
// is suppressed if another event is simultaneously sustaining at the onset
// sample — this is the legato escape hatch: overlap event durations (e.g.
// via `dur(2)`) and the gate stays continuously high across the overlap.
[[gnu::always_inline]]
inline void op_seqpat_gate(ExecutionContext& ctx, const Instruction& inst) {
    float* out_gate = ctx.buffers->get(inst.out_buffer);
    auto& state = ctx.states->get_or_create<SequenceState>(inst.state_id);

    // Voice index for polyphonic patterns
    std::uint8_t voice_index = (inst.inputs[0] != BUFFER_UNUSED)
        ? static_cast<std::uint8_t>(inst.inputs[0]) : 0;

    if (state.output.num_events == 0) {
        std::fill_n(out_gate, BLOCK_SIZE, 0.0f);
        return;
    }

    // External clock override: inputs[1] contains beat_pos buffer
    bool external_clock = (inst.inputs[1] != BUFFER_UNUSED);
    const float* ext_beat_pos = external_clock ? ctx.buffers->get(inst.inputs[1]) : nullptr;

    const float spb = external_clock ? 1.0f : ctx.samples_per_beat();
    const float cycle = state.cycle_length;

    // Helper: is event e currently sustaining at bp? Handles wrap-around.
    auto event_active = [cycle](const OutputEvents::OutputEvent& e, float bp) -> bool {
        float end = e.time + e.duration;
        if (end > cycle) {
            return bp >= e.time || bp < (end - cycle);
        }
        return bp >= e.time && bp < end;
    };

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Current beat position within cycle
        float beat_pos;
        if (external_clock) {
            beat_pos = std::fmod(ext_beat_pos[i], cycle);
            if (beat_pos < 0.0f) beat_pos += cycle;
        } else {
            beat_pos = std::fmod(
                static_cast<float>(ctx.global_sample_counter + i) / spb,
                cycle
            );
        }

        const float bp_prev = state.last_beat_pos_gate;
        const bool have_prev = (bp_prev >= 0.0f);
        // Cycle wrap: bp jumped backward by more than half a cycle. Treat as
        // crossing through cycle end into 0.
        const bool wrapped = have_prev && (beat_pos < bp_prev - cycle * 0.5f);

        // Onset detection: did beat_pos cross any event's start time on this
        // sample? An onset for event e occurs when e.time falls in
        // (bp_prev, beat_pos] (or, with wrap, in (bp_prev, cycle) ∪ [0, beat_pos]).
        std::uint32_t onset_idx = state.output.num_events;  // sentinel "none"
        if (have_prev) {
            for (std::uint32_t e = 0; e < state.output.num_events; ++e) {
                const auto& evt = state.output.events[e];
                if (voice_index >= evt.num_values) continue;
                bool crossed;
                if (wrapped) {
                    crossed = (evt.time > bp_prev && evt.time < cycle) ||
                              (evt.time >= 0.0f && evt.time <= beat_pos);
                } else {
                    crossed = (evt.time > bp_prev && evt.time <= beat_pos);
                }
                if (crossed) { onset_idx = e; break; }
            }
        }

        // If we have an onset, check whether another event is also sustaining
        // at beat_pos. If so, this is a legato handoff — don't drop.
        bool retrigger_drop = false;
        if (onset_idx < state.output.num_events) {
            bool other_sustaining = false;
            for (std::uint32_t e = 0; e < state.output.num_events; ++e) {
                if (e == onset_idx) continue;
                const auto& evt = state.output.events[e];
                if (voice_index >= evt.num_values) continue;
                if (event_active(evt, beat_pos)) { other_sustaining = true; break; }
            }
            retrigger_drop = !other_sustaining;
        }

        float gate_val = 0.0f;
        if (!retrigger_drop) {
            // Default: high if any event currently sustains
            for (std::uint32_t e = 0; e < state.output.num_events; ++e) {
                const auto& evt = state.output.events[e];
                if (voice_index >= evt.num_values) continue;
                if (event_active(evt, beat_pos)) { gate_val = 1.0f; break; }
            }
        }

        out_gate[i] = gate_val;
        state.last_beat_pos_gate = beat_pos;
    }
}

// ============================================================================
// SEQPAT_PROP - Per-event custom property signal (Phase 2.1, PRD §11)
// ============================================================================
// out_buffer: float buffer receiving the slot value of the active event
// rate: property slot index (0..MAX_PROPS_PER_EVENT-1)
// inputs[0]: voice index for polyphonic patterns (0-7, default 0 if 0xFFFF)
// inputs[1]: external clock buffer (0xFFFF = use internal)
// state_id: must match the SEQPAT_QUERY that populated the SequenceState
//
// Reads `state.output.events[active_idx].prop_vals[slot]` per sample, where
// active_idx is determined by beat_pos against event start times (same logic
// as SEQPAT_TYPE).
[[gnu::always_inline]]
inline void op_seqpat_prop(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    auto& state = ctx.states->get_or_create<SequenceState>(inst.state_id);
    std::uint8_t slot = static_cast<std::uint8_t>(inst.rate);

    // Voice index for polyphonic patterns
    std::uint8_t voice_index = (inst.inputs[0] != BUFFER_UNUSED)
        ? static_cast<std::uint8_t>(inst.inputs[0]) : 0;

    if (state.output.num_events == 0 || slot >= MAX_PROPS_PER_EVENT) {
        std::fill_n(out, BLOCK_SIZE, 0.0f);
        return;
    }

    // External clock override: inputs[1] contains beat_pos buffer
    bool external_clock = (inst.inputs[1] != BUFFER_UNUSED);
    const float* ext_beat_pos = external_clock ? ctx.buffers->get(inst.inputs[1]) : nullptr;

    const float spb = external_clock ? 1.0f : ctx.samples_per_beat();

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Current beat position within cycle
        float beat_pos;
        if (external_clock) {
            beat_pos = std::fmod(ext_beat_pos[i], state.cycle_length);
            if (beat_pos < 0.0f) beat_pos += state.cycle_length;
        } else {
            beat_pos = std::fmod(
                static_cast<float>(ctx.global_sample_counter + i) / spb,
                state.cycle_length
            );
        }

        // Find the most recent event that started before or at beat_pos
        // (same logic as SEQPAT_TYPE)
        std::uint32_t event_index = 0;
        for (std::uint32_t e = 0; e < state.output.num_events; ++e) {
            if (state.output.events[e].time <= beat_pos) {
                event_index = e;
            } else {
                break;
            }
        }

        // If beat_pos is before first event, wrap to last event
        if (state.output.num_events > 0 && beat_pos < state.output.events[0].time) {
            event_index = state.output.num_events - 1;
        }

        const auto& evt = state.output.events[event_index];

        // Voice gate: silent for events that don't carry this voice
        if (voice_index < evt.num_values) {
            out[i] = evt.prop_vals[slot];
        } else {
            out[i] = 0.0f;
        }
    }
}

// ============================================================================
// SEQPAT_FIELD - Per-event built-in scalar field signal
// ============================================================================
// out_buffer: float buffer receiving the active event's field value
// rate: field selector
//   0 = duration (beats)
//   1 = chance   (0..1)
//   2 = time     (event start within cycle, beats)
//   3 = midi_note (0..127, may be fractional for microtonal)
//   4 = type_id  (sample id; 0 for pitch events)
// inputs[0]: voice index for polyphonic patterns (0..7, default 0 if 0xFFFF)
// inputs[1]: external clock buffer (0xFFFF = use internal)
// state_id: must match the SEQPAT_QUERY that populated the SequenceState
//
// Active-event scan logic mirrors op_seqpat_type / op_seqpat_prop. Voices
// without a value for the active event read 0.0 (consistent with SEQPAT_TYPE).
[[gnu::always_inline]]
inline void op_seqpat_field(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    auto& state = ctx.states->get_or_create<SequenceState>(inst.state_id);
    std::uint8_t field = static_cast<std::uint8_t>(inst.rate);

    std::uint8_t voice_index = (inst.inputs[0] != BUFFER_UNUSED)
        ? static_cast<std::uint8_t>(inst.inputs[0]) : 0;

    if (state.output.num_events == 0) {
        std::fill_n(out, BLOCK_SIZE, 0.0f);
        return;
    }

    bool external_clock = (inst.inputs[1] != BUFFER_UNUSED);
    const float* ext_beat_pos = external_clock ? ctx.buffers->get(inst.inputs[1]) : nullptr;
    const float spb = external_clock ? 1.0f : ctx.samples_per_beat();

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float beat_pos;
        if (external_clock) {
            beat_pos = std::fmod(ext_beat_pos[i], state.cycle_length);
            if (beat_pos < 0.0f) beat_pos += state.cycle_length;
        } else {
            beat_pos = std::fmod(
                static_cast<float>(ctx.global_sample_counter + i) / spb,
                state.cycle_length
            );
        }

        std::uint32_t event_index = 0;
        for (std::uint32_t e = 0; e < state.output.num_events; ++e) {
            if (state.output.events[e].time <= beat_pos) {
                event_index = e;
            } else {
                break;
            }
        }
        if (state.output.num_events > 0 && beat_pos < state.output.events[0].time) {
            event_index = state.output.num_events - 1;
        }

        const auto& evt = state.output.events[event_index];

        if (voice_index >= evt.num_values) {
            out[i] = 0.0f;
            continue;
        }

        switch (field) {
            case 0: out[i] = evt.duration; break;
            case 1: out[i] = evt.chance; break;
            case 2: out[i] = evt.time; break;
            case 3: out[i] = evt.midi_note; break;
            case 4: out[i] = static_cast<float>(evt.type_id); break;
            default: out[i] = 0.0f; break;
        }
    }
}

// ============================================================================
// SEQPAT_PHASE - Event-scoped 0..1 phasor (linear ramp)
// ============================================================================
// out_buffer: phase output, 0.0 at event start, ramping to 1.0 at event end
// inputs[0]: voice index (default 0 if 0xFFFF)
// inputs[1]: external clock buffer (0xFFFF = use internal)
// state_id: must match the SEQPAT_QUERY that populated the SequenceState
//
// phase = clamp((beat_pos - evt.time) / evt.duration, 0, 1). Wrap-around
// handled the same way as SEQPAT_GATE: events whose end overflows
// state.cycle_length re-anchor on the wrapped half.
[[gnu::always_inline]]
inline void op_seqpat_phase(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    auto& state = ctx.states->get_or_create<SequenceState>(inst.state_id);

    std::uint8_t voice_index = (inst.inputs[0] != BUFFER_UNUSED)
        ? static_cast<std::uint8_t>(inst.inputs[0]) : 0;

    if (state.output.num_events == 0) {
        std::fill_n(out, BLOCK_SIZE, 0.0f);
        return;
    }

    bool external_clock = (inst.inputs[1] != BUFFER_UNUSED);
    const float* ext_beat_pos = external_clock ? ctx.buffers->get(inst.inputs[1]) : nullptr;
    const float spb = external_clock ? 1.0f : ctx.samples_per_beat();

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float beat_pos;
        if (external_clock) {
            beat_pos = std::fmod(ext_beat_pos[i], state.cycle_length);
            if (beat_pos < 0.0f) beat_pos += state.cycle_length;
        } else {
            beat_pos = std::fmod(
                static_cast<float>(ctx.global_sample_counter + i) / spb,
                state.cycle_length
            );
        }

        // Find the active event that contains beat_pos in [time, time+duration).
        // Wrap-aware: if no normal event matches, fall back to the most-recent
        // event that started before beat_pos (matches SEQPAT_TYPE/PROP fallback).
        float phase_val = 0.0f;
        bool found = false;

        for (std::uint32_t e = 0; e < state.output.num_events; ++e) {
            const auto& evt = state.output.events[e];
            if (voice_index >= evt.num_values) continue;
            if (evt.duration <= 0.0f) continue;

            float event_start = evt.time;
            float event_end = evt.time + evt.duration;

            if (event_end > state.cycle_length) {
                // Event wraps around cycle boundary
                if (beat_pos >= event_start) {
                    phase_val = (beat_pos - event_start) / evt.duration;
                    found = true;
                    break;
                }
                if (beat_pos < (event_end - state.cycle_length)) {
                    phase_val = (beat_pos + state.cycle_length - event_start) / evt.duration;
                    found = true;
                    break;
                }
            } else {
                if (beat_pos >= event_start && beat_pos < event_end) {
                    phase_val = (beat_pos - event_start) / evt.duration;
                    found = true;
                    break;
                }
            }
        }

        if (!found) {
            phase_val = 0.0f;
        } else {
            if (phase_val < 0.0f) phase_val = 0.0f;
            else if (phase_val > 1.0f) phase_val = 1.0f;
        }
        out[i] = phase_val;
    }
}

}  // namespace cedar
