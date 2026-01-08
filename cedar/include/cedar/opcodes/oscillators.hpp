#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
#include <cmath>

namespace cedar {

// SIN oscillator: out = sin(phase * 2pi), frequency from in0
[[gnu::always_inline]]
inline void op_osc_sin(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = std::sin(state.phase * TWO_PI);

        // Advance phase
        state.phase += freq[i] * ctx.inv_sample_rate;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
        } else if (state.phase < 0.0f) {
            state.phase += 1.0f;
        }
    }
}

// TRI oscillator: triangle wave, frequency from in0
// Output: -1 to +1, linear rise then fall
[[gnu::always_inline]]
inline void op_osc_tri(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Triangle: 4 * |phase - 0.5| - 1
        // At phase 0: 4*0.5-1 = 1
        // At phase 0.25: 4*0.25-1 = 0
        // At phase 0.5: 4*0-1 = -1
        // At phase 0.75: 4*0.25-1 = 0
        out[i] = 4.0f * std::abs(state.phase - 0.5f) - 1.0f;

        state.phase += freq[i] * ctx.inv_sample_rate;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
        } else if (state.phase < 0.0f) {
            state.phase += 1.0f;
        }
    }
}

// SAW oscillator: sawtooth wave, frequency from in0
// Output: -1 to +1, linear ramp up then instant reset
[[gnu::always_inline]]
inline void op_osc_saw(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Sawtooth: 2 * phase - 1
        out[i] = 2.0f * state.phase - 1.0f;

        state.phase += freq[i] * ctx.inv_sample_rate;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
        } else if (state.phase < 0.0f) {
            state.phase += 1.0f;
        }
    }
}

// SQR oscillator: square wave, frequency from in0
// Output: +1 for first half of cycle, -1 for second half
[[gnu::always_inline]]
inline void op_osc_sqr(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Square: +1 if phase < 0.5, else -1
        out[i] = (state.phase < 0.5f) ? 1.0f : -1.0f;

        state.phase += freq[i] * ctx.inv_sample_rate;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
        } else if (state.phase < 0.0f) {
            state.phase += 1.0f;
        }
    }
}

// RAMP oscillator: inverted sawtooth (descending ramp)
// Output: +1 to -1, linear ramp down then instant reset
[[gnu::always_inline]]
inline void op_osc_ramp(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Ramp (inverted saw): 1 - 2 * phase
        out[i] = 1.0f - 2.0f * state.phase;

        state.phase += freq[i] * ctx.inv_sample_rate;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
        } else if (state.phase < 0.0f) {
            state.phase += 1.0f;
        }
    }
}

// PHASOR: raw phase output (0 to 1)
// Useful as modulation source or for custom waveshaping
[[gnu::always_inline]]
inline void op_osc_phasor(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = state.phase;

        state.phase += freq[i] * ctx.inv_sample_rate;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
        } else if (state.phase < 0.0f) {
            state.phase += 1.0f;
        }
    }
}

}  // namespace cedar
