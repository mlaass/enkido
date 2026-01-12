#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
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
        // Phase increment: freq_mult cycles per beat
        float cycles_per_sample = freq_mult[i] / spb;
        float prev_phase = state.phase;
        state.phase += cycles_per_sample;

        // Wrap phase
        if (state.phase >= 1.0f) {
            state.phase -= std::floor(state.phase);
        }

        float value = 0.0f;
        float phase = state.phase;

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
                if (phase < prev_phase) {
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
    }
}

// ============================================================================
// SEQ_STEP - Time-based event sequencer
// ============================================================================
// out_buffer: value output (sample ID, pitch, etc.)
// inputs[0]: velocity output buffer
// inputs[1]: trigger output buffer
// State contains event times, values, velocities, and cycle_length
[[gnu::always_inline]]
inline void op_seq_step(ExecutionContext& ctx, const Instruction& inst) {
    float* out_value = ctx.buffers->get(inst.out_buffer);
    float* out_velocity = ctx.buffers->get(inst.inputs[0]);
    float* out_trigger = ctx.buffers->get(inst.inputs[1]);
    auto& state = ctx.states->get_or_create<SeqStepState>(inst.state_id);

    if (state.num_events == 0) {
        std::fill_n(out_value, BLOCK_SIZE, 0.0f);
        std::fill_n(out_velocity, BLOCK_SIZE, 0.0f);
        std::fill_n(out_trigger, BLOCK_SIZE, 0.0f);
        return;
    }

    const float spb = ctx.samples_per_beat();

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Current beat position within cycle
        float beat_pos = std::fmod(
            static_cast<float>(ctx.global_sample_counter + i) / spb,
            state.cycle_length
        );

        // Detect cycle wrap
        bool wrapped = (state.last_beat_pos >= 0.0f && beat_pos < state.last_beat_pos);
        if (wrapped) {
            state.current_index = 0;
        }

        // Check if we crossed an event time (for trigger)
        out_trigger[i] = 0.0f;
        while (state.current_index < state.num_events &&
               beat_pos >= state.times[state.current_index]) {
            out_trigger[i] = 1.0f;  // Fire trigger when crossing event time
            state.current_index++;
        }

        // Handle wrap: also trigger if we wrapped and crossed first event
        if (wrapped && state.num_events > 0 && beat_pos >= state.times[0]) {
            out_trigger[i] = 1.0f;
        }

        // Output current value and velocity (from current event)
        std::uint32_t event_index = (state.current_index > 0)
            ? state.current_index - 1
            : state.num_events - 1;
        out_value[i] = state.values[event_index];
        out_velocity[i] = state.velocities[event_index];

        state.last_beat_pos = beat_pos;
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

    // Apply rotation (shift pattern right)
    if (rotation > 0 && steps > 0) {
        rotation = rotation % steps;
        std::uint32_t mask = (1u << steps) - 1;
        pattern = ((pattern >> rotation) | (pattern << (steps - rotation))) & mask;
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
        // Reset step on pattern change for consistent behavior
        state.current_step = 0;
        state.phase = 0.0f;
    }

    // One bar divided by number of steps
    const float spb = ctx.samples_per_beat();
    const float samples_per_step = (spb * 4.0f) / static_cast<float>(steps);  // 1 bar = 4 beats

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        state.phase += 1.0f / samples_per_step;

        bool phase_wrapped = false;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
            state.current_step = (state.current_step + 1) % steps;
            phase_wrapped = true;
        }

        // Trigger on step boundary if this step is a hit
        bool is_hit = (state.pattern >> state.current_step) & 1;
        out[i] = (phase_wrapped && is_hit) ? 1.0f : 0.0f;
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
        float samples_per_trigger = spb / division[i];
        state.phase += 1.0f / samples_per_trigger;

        if (state.phase >= 1.0f) {
            state.phase -= std::floor(state.phase);
            out[i] = 1.0f;  // Trigger pulse
        } else {
            out[i] = 0.0f;
        }
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

}  // namespace cedar
