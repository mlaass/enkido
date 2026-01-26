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
// Pattern Query Helpers - Deterministic Randomness
// ============================================================================

// Splitmix64-style mixer for deterministic pseudo-random values
[[gnu::always_inline]]
inline std::uint64_t splitmix64(std::uint64_t x) {
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Mix pattern seed with time position for deterministic randomness
// Same seed + time always produces same result (important for seek/scrub)
[[gnu::always_inline]]
inline float deterministic_random(std::uint64_t pattern_seed, float time_position) {
    // Quantize time to avoid floating point issues (10000 quanta per beat)
    std::uint64_t time_quant = static_cast<std::uint64_t>(time_position * 10000.0f);
    std::uint64_t h = splitmix64(pattern_seed ^ time_quant);
    return static_cast<float>(h & 0xFFFFFFFFull) / 4294967296.0f;
}

// ============================================================================
// Pattern Query Context - Passed through recursive evaluation
// ============================================================================

struct PatternQueryContext {
    float arc_start;      // Query start time (in beats, relative to cycle)
    float arc_end;        // Query end time
    float time_scale;     // Current time scale (for FAST/SLOW)
    float time_offset;    // Current time offset
    std::uint64_t rng_seed;
    PatternQueryState* state;

    // Emit an event if it falls within the query arc
    void add_event(float time, float duration, float value, float velocity = 1.0f) {
        // Transform time by current scale and offset
        float event_time = time * time_scale + time_offset;

        // Check if event overlaps query arc
        if (event_time < arc_end && event_time + duration * time_scale > arc_start) {
            if (state->num_events < PatternQueryState::MAX_QUERY_EVENTS) {
                auto& e = state->events[state->num_events++];
                e.time = event_time;
                e.duration = duration * time_scale;
                e.value = value;
                e.velocity = velocity;
            }
        }
    }

    // Create a subdivided context for CAT children
    [[nodiscard]] PatternQueryContext subdivide(std::size_t child_idx, std::size_t total_children,
                                                 float child_weight = 1.0f, float total_weight = 0.0f) const {
        if (total_weight <= 0.0f) total_weight = static_cast<float>(total_children);
        float child_duration = time_scale / total_weight * child_weight;
        float accumulated = 0.0f;
        for (std::size_t i = 0; i < child_idx; ++i) {
            accumulated += 1.0f;  // Simplified: assume equal weight for now
        }
        if (total_weight > 0.0f && total_weight != static_cast<float>(total_children)) {
            accumulated = static_cast<float>(child_idx);
        }
        float child_offset = time_offset + (accumulated / total_weight) * time_scale;

        return {
            .arc_start = arc_start,
            .arc_end = arc_end,
            .time_scale = child_duration,
            .time_offset = child_offset,
            .rng_seed = rng_seed ^ (child_idx + 1),
            .state = state
        };
    }

    // Create a context with modified time scale (for FAST/SLOW)
    [[nodiscard]] PatternQueryContext with_scale(float factor) const {
        return {
            .arc_start = arc_start,
            .arc_end = arc_end,
            .time_scale = time_scale / factor,
            .time_offset = time_offset,
            .rng_seed = rng_seed,
            .state = state
        };
    }

    // Create a context with modified time offset (for EARLY/LATE)
    [[nodiscard]] PatternQueryContext with_offset(float offset) const {
        return {
            .arc_start = arc_start,
            .arc_end = arc_end,
            .time_scale = time_scale,
            .time_offset = time_offset + offset,
            .rng_seed = rng_seed,
            .state = state
        };
    }
};

// Forward declaration for recursive evaluation
void evaluate_pattern_node(const PatternQueryState* prog, std::uint32_t node_idx,
                           PatternQueryContext& ctx);

// Recursive pattern evaluation
inline void evaluate_pattern_node(const PatternQueryState* prog, std::uint32_t node_idx,
                                   PatternQueryContext& ctx) {
    if (node_idx >= prog->num_nodes) return;

    const PatternNode& node = prog->nodes[node_idx];

    switch (node.op) {
        case PatternOp::ATOM:
            // Emit event for this atom
            ctx.add_event(0.0f, 1.0f, node.data.float_val);
            break;

        case PatternOp::SILENCE:
            // No event - rest
            break;

        case PatternOp::CAT: {
            // Sequential concatenation - subdivide time among children
            for (std::uint8_t i = 0; i < node.num_children; ++i) {
                PatternQueryContext child_ctx = ctx.subdivide(i, node.num_children);
                evaluate_pattern_node(prog, node.first_child_idx + i, child_ctx);
            }
            break;
        }

        case PatternOp::STACK: {
            // Parallel - all children share the same time span
            for (std::uint8_t i = 0; i < node.num_children; ++i) {
                PatternQueryContext child_ctx = ctx;
                child_ctx.rng_seed ^= static_cast<std::uint64_t>(i + 1);
                evaluate_pattern_node(prog, node.first_child_idx + i, child_ctx);
            }
            break;
        }

        case PatternOp::SLOWCAT: {
            // Alternation - pick child based on cycle number
            // Calculate which cycle we're in based on arc_start
            float cycle_pos = ctx.arc_start / prog->cycle_length;
            std::uint32_t cycle = static_cast<std::uint32_t>(cycle_pos);
            std::uint8_t choice = static_cast<std::uint8_t>(cycle % node.num_children);
            evaluate_pattern_node(prog, node.first_child_idx + choice, ctx);
            break;
        }

        case PatternOp::FAST: {
            // Speed up by factor
            PatternQueryContext scaled_ctx = ctx.with_scale(node.data.float_val);
            if (node.num_children > 0) {
                evaluate_pattern_node(prog, node.first_child_idx, scaled_ctx);
            }
            break;
        }

        case PatternOp::SLOW: {
            // Slow down by factor
            PatternQueryContext scaled_ctx = ctx.with_scale(1.0f / node.data.float_val);
            if (node.num_children > 0) {
                evaluate_pattern_node(prog, node.first_child_idx, scaled_ctx);
            }
            break;
        }

        case PatternOp::EARLY: {
            // Shift earlier in time
            PatternQueryContext offset_ctx = ctx.with_offset(-node.time_offset);
            if (node.num_children > 0) {
                evaluate_pattern_node(prog, node.first_child_idx, offset_ctx);
            }
            break;
        }

        case PatternOp::LATE: {
            // Shift later in time
            PatternQueryContext offset_ctx = ctx.with_offset(node.time_offset);
            if (node.num_children > 0) {
                evaluate_pattern_node(prog, node.first_child_idx, offset_ctx);
            }
            break;
        }

        case PatternOp::REV: {
            // Reverse is complex - for now, just pass through
            // TODO: Implement proper time reversal
            if (node.num_children > 0) {
                evaluate_pattern_node(prog, node.first_child_idx, ctx);
            }
            break;
        }

        case PatternOp::DEGRADE: {
            // Chance-based filtering
            float rnd = deterministic_random(ctx.rng_seed, ctx.time_offset);
            if (rnd < node.data.float_val && node.num_children > 0) {
                evaluate_pattern_node(prog, node.first_child_idx, ctx);
            }
            break;
        }

        case PatternOp::CHOOSE: {
            // Random element from children
            if (node.num_children > 0) {
                float rnd = deterministic_random(ctx.rng_seed, ctx.time_offset);
                std::uint8_t choice = static_cast<std::uint8_t>(
                    rnd * static_cast<float>(node.num_children)) % node.num_children;
                evaluate_pattern_node(prog, node.first_child_idx + choice, ctx);
            }
            break;
        }

        case PatternOp::EUCLID: {
            // Euclidean rhythm - generate pattern and emit events
            std::uint32_t hits = node.data.euclid.hits;
            std::uint32_t steps = node.data.euclid.steps;
            std::uint32_t rotation = node.data.euclid.rotation;

            if (steps > 0 && hits > 0 && node.num_children > 0) {
                std::uint32_t pattern = compute_euclidean_pattern(hits, steps, rotation);
                for (std::uint32_t i = 0; i < steps; ++i) {
                    if ((pattern >> i) & 1) {
                        PatternQueryContext step_ctx = ctx.subdivide(i, steps);
                        evaluate_pattern_node(prog, node.first_child_idx, step_ctx);
                    }
                }
            }
            break;
        }

        case PatternOp::REPLICATE: {
            // Repeat n times (but don't subdivide time - that's handled by !n modifier)
            std::uint32_t count = static_cast<std::uint32_t>(node.data.float_val);
            if (count > 0 && node.num_children > 0) {
                for (std::uint32_t i = 0; i < count; ++i) {
                    PatternQueryContext rep_ctx = ctx.subdivide(i, count);
                    evaluate_pattern_node(prog, node.first_child_idx, rep_ctx);
                }
            }
            break;
        }

        case PatternOp::WEIGHT: {
            // Weight modifier - adjust time scale
            // The weight is stored in float_val, but actual time subdivision
            // is handled by the parent CAT node
            if (node.num_children > 0) {
                evaluate_pattern_node(prog, node.first_child_idx, ctx);
            }
            break;
        }
    }
}

// Sort events by time (simple insertion sort for small arrays)
inline void sort_query_events(PatternQueryState& state) {
    for (std::uint32_t i = 1; i < state.num_events; ++i) {
        QueryEvent key = state.events[i];
        std::int32_t j = static_cast<std::int32_t>(i) - 1;
        while (j >= 0 && state.events[j].time > key.time) {
            state.events[j + 1] = state.events[j];
            --j;
        }
        state.events[j + 1] = key;
    }
}

// ============================================================================
// PAT_QUERY - Query pattern at block boundaries (control rate)
// ============================================================================
// Fills PatternQueryState.events[] for current beat range
// Called once per block to prepare events for PAT_STEP
[[gnu::always_inline]]
inline void op_pat_query(ExecutionContext& ctx, const Instruction& inst) {
    auto& state = ctx.states->get_or_create<PatternQueryState>(inst.state_id);

    const float spb = ctx.samples_per_beat();

    // Calculate current beat position (start of block)
    float beat_start = static_cast<float>(ctx.global_sample_counter) / spb;

    // Calculate beat position at end of block (lookahead)
    float beat_end = static_cast<float>(ctx.global_sample_counter + BLOCK_SIZE) / spb;

    // Normalize to cycle position
    float cycle_start = std::fmod(beat_start, state.cycle_length);
    float cycle_end = std::fmod(beat_end, state.cycle_length);

    // Handle cycle wrap
    if (cycle_end < cycle_start) {
        cycle_end += state.cycle_length;
    }

    // Only re-query if we've moved to a new time window
    // (optimization to avoid redundant work)
    if (std::abs(cycle_start - state.query_start) < 0.0001f &&
        std::abs(cycle_end - state.query_end) < 0.0001f) {
        return;  // Already have events for this window
    }

    // Clear previous events
    state.num_events = 0;
    state.query_start = cycle_start;
    state.query_end = cycle_end;

    // Set up query context
    PatternQueryContext query_ctx{
        .arc_start = cycle_start,
        .arc_end = cycle_end,
        .time_scale = state.cycle_length,
        .time_offset = 0.0f,
        .rng_seed = state.pattern_seed,
        .state = &state
    };

    // Evaluate pattern from root (node 0)
    if (state.num_nodes > 0) {
        evaluate_pattern_node(&state, 0, query_ctx);
    }

    // Sort events by time
    sort_query_events(state);

    // Reset playback index
    state.current_index = 0;

    // Find first event after current position (handle wrap)
    while (state.current_index < state.num_events &&
           state.events[state.current_index].time < cycle_start) {
        state.current_index++;
    }
}

// ============================================================================
// PAT_STEP - Step through query results
// ============================================================================
// out_buffer: value output (frequency or sample ID)
// inputs[0]: velocity output buffer
// inputs[1]: trigger output buffer
// Outputs current event value, velocity, and trigger when events fire
[[gnu::always_inline]]
inline void op_pat_step(ExecutionContext& ctx, const Instruction& inst) {
    float* out_value = ctx.buffers->get(inst.out_buffer);
    float* out_velocity = ctx.buffers->get(inst.inputs[0]);
    float* out_trigger = ctx.buffers->get(inst.inputs[1]);
    auto& state = ctx.states->get_or_create<PatternQueryState>(inst.state_id);

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
        bool wrapped = (state.last_beat_pos >= 0.0f && beat_pos < state.last_beat_pos - 0.5f);
        if (wrapped) {
            state.current_index = 0;
        }

        // Check if we crossed an event time (for trigger)
        out_trigger[i] = 0.0f;
        while (state.current_index < state.num_events &&
               beat_pos >= state.events[state.current_index].time) {
            out_trigger[i] = 1.0f;
            state.current_index++;
        }

        // Handle wrap: also trigger if we wrapped and crossed first event
        if (wrapped && state.num_events > 0 && beat_pos >= state.events[0].time) {
            out_trigger[i] = 1.0f;
        }

        // Output current value and velocity (from current event)
        std::uint32_t event_index = (state.current_index > 0)
            ? state.current_index - 1
            : state.num_events - 1;
        out_value[i] = state.events[event_index].value;
        out_velocity[i] = state.events[event_index].velocity;

        state.last_beat_pos = beat_pos;
    }
}

}  // namespace cedar
