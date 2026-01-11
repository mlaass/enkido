#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
#include <cmath>
#include <algorithm>

namespace cedar {

// ============================================================================
// Delay Opcodes
// ============================================================================

// DELAY: Stereo delay line with feedback and wet/dry mix
// in0: input signal
// in1: delay time (milliseconds, 0-2000ms)
// in2: feedback amount (0.0-1.0, clamped to 0.99 to prevent runaway)
// reserved field low byte: wet/dry mix (0-255 -> 0.0-1.0, 255 = fully wet)
//
// Uses linear interpolation for sub-sample delay accuracy
[[gnu::always_inline]]
inline void op_delay(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* delay_ms = ctx.buffers->get(inst.inputs[1]);
    const float* feedback = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<DelayState>(inst.state_id);

    // Extract wet/dry mix from reserved field (0-255 -> 0.0-1.0)
    float mix = static_cast<float>(inst.rate) / 255.0f;

    // Calculate maximum delay in samples and ensure buffer is allocated
    float max_delay_ms = 2000.0f;  // 2 second max delay
    std::size_t max_samples = static_cast<std::size_t>(max_delay_ms * 0.001f * ctx.sample_rate) + 1;
    max_samples = std::min(max_samples, DelayState::MAX_DELAY_SAMPLES);
    state.ensure_buffer(max_samples, ctx.arena);

    if (!state.buffer) {
        // Buffer allocation failed, output silence
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            out[i] = 0.0f;
        }
        return;
    }

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Convert delay time to samples
        float delay_samples = delay_ms[i] * 0.001f * ctx.sample_rate;
        delay_samples = std::clamp(delay_samples, 0.0f, static_cast<float>(state.buffer_size - 2));

        // Integer and fractional parts for linear interpolation
        std::size_t delay_int = static_cast<std::size_t>(delay_samples);
        float delay_frac = delay_samples - static_cast<float>(delay_int);

        // Calculate read positions (circular buffer)
        std::size_t read_pos1 = (state.write_pos + state.buffer_size - delay_int) % state.buffer_size;
        std::size_t read_pos2 = (read_pos1 + state.buffer_size - 1) % state.buffer_size;

        // Linear interpolation for fractional delay
        float delayed = state.buffer[read_pos1] * (1.0f - delay_frac) +
                       state.buffer[read_pos2] * delay_frac;

        // Clamp feedback to prevent runaway oscillation
        float fb = std::clamp(feedback[i], 0.0f, 0.99f);

        // Write input + feedback to buffer
        state.buffer[state.write_pos] = input[i] + delayed * fb;

        // Advance write position
        state.write_pos = (state.write_pos + 1) % state.buffer_size;

        // Mix wet/dry
        out[i] = input[i] * (1.0f - mix) + delayed * mix;
    }
}

// DELAY_SYNC: Beat-synchronized delay (delay time in beats/subdivisions)
// in0: input signal
// in1: delay time in beats (e.g., 0.25 = 1/16th note at 4/4)
// in2: feedback amount (0.0-1.0)
// reserved field low byte: wet/dry mix (0-255 -> 0.0-1.0)
//
// Delay time is calculated from BPM: delay_ms = (delay_beats / bpm) * 60000
[[gnu::always_inline]]
inline void op_delay_sync(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* delay_beats = ctx.buffers->get(inst.inputs[1]);
    const float* feedback = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<DelayState>(inst.state_id);

    float mix = static_cast<float>(inst.rate) / 255.0f;

    // Calculate samples per beat
    float samples_per_beat = (60.0f / ctx.bpm) * ctx.sample_rate;

    // Maximum 4 beats delay
    float max_beats = 4.0f;
    std::size_t max_samples = static_cast<std::size_t>(max_beats * samples_per_beat) + 1;
    max_samples = std::min(max_samples, DelayState::MAX_DELAY_SAMPLES);
    state.ensure_buffer(max_samples, ctx.arena);

    if (!state.buffer) {
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            out[i] = 0.0f;
        }
        return;
    }

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Convert beats to samples
        float delay_samples = delay_beats[i] * samples_per_beat;
        delay_samples = std::clamp(delay_samples, 0.0f, static_cast<float>(state.buffer_size - 2));

        std::size_t delay_int = static_cast<std::size_t>(delay_samples);
        float delay_frac = delay_samples - static_cast<float>(delay_int);

        std::size_t read_pos1 = (state.write_pos + state.buffer_size - delay_int) % state.buffer_size;
        std::size_t read_pos2 = (read_pos1 + state.buffer_size - 1) % state.buffer_size;

        float delayed = state.buffer[read_pos1] * (1.0f - delay_frac) +
                       state.buffer[read_pos2] * delay_frac;

        float fb = std::clamp(feedback[i], 0.0f, 0.99f);
        state.buffer[state.write_pos] = input[i] + delayed * fb;
        state.write_pos = (state.write_pos + 1) % state.buffer_size;

        out[i] = input[i] * (1.0f - mix) + delayed * mix;
    }
}

}  // namespace cedar
