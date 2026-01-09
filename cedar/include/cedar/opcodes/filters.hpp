#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
#include <cmath>
#include <algorithm>

namespace cedar {

// Clamp filter state/output to prevent blowup
// Audio signals should never exceed ±10 in normal operation
[[gnu::always_inline]]
inline float clamp_audio(float val) {
    if (val > 10.0f) return 10.0f;
    if (val < -10.0f) return -10.0f;
    return val;
}

// Tiny DC offset to prevent denormal numbers (inaudible)
constexpr float DENORMAL_DC = 1e-18f;

// SVF (State Variable Filter) coefficient calculation
inline void calc_svf(SVFState& state, float freq, float q, float sample_rate) {
    freq = std::max(0.0f, freq);

    if (freq == state.last_freq && q == state.last_q) {
        return;
    }
    state.last_freq = freq;
    state.last_q = q;

    freq = std::clamp(freq, 20.0f, sample_rate * 0.49f);
    q = std::max(0.1f, q);

    state.g = std::tan(PI * freq / sample_rate);
    state.k = 1.0f / q;
    state.a1 = 1.0f / (1.0f + state.g * (state.g + state.k));
    state.a2 = state.g * state.a1;
    state.a3 = state.g * state.a2;
}

// SVF Lowpass
[[gnu::always_inline]]
inline void op_filter_svf_lp(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* freq = ctx.buffers->get(inst.inputs[1]);
    const float* q = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<SVFState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        calc_svf(state, freq[i], q[i], ctx.sample_rate);

        // Add tiny DC to prevent denormals
        float v3 = input[i] - (state.ic2eq + DENORMAL_DC);
        float v1 = state.a1 * (state.ic1eq + DENORMAL_DC) + state.a2 * v3;
        float v2 = (state.ic2eq + DENORMAL_DC) + state.a2 * (state.ic1eq + DENORMAL_DC) + state.a3 * v3;
        state.ic1eq = clamp_audio(2.0f * v1 - state.ic1eq);
        state.ic2eq = clamp_audio(2.0f * v2 - state.ic2eq);

        out[i] = v2;  // Lowpass output
    }
}

// SVF Highpass
[[gnu::always_inline]]
inline void op_filter_svf_hp(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* freq = ctx.buffers->get(inst.inputs[1]);
    const float* q = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<SVFState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        calc_svf(state, freq[i], q[i], ctx.sample_rate);

        // Add tiny DC to prevent denormals
        float v3 = input[i] - (state.ic2eq + DENORMAL_DC);
        float v1 = state.a1 * (state.ic1eq + DENORMAL_DC) + state.a2 * v3;
        float v2 = (state.ic2eq + DENORMAL_DC) + state.a2 * (state.ic1eq + DENORMAL_DC) + state.a3 * v3;
        state.ic1eq = clamp_audio(2.0f * v1 - state.ic1eq);
        state.ic2eq = clamp_audio(2.0f * v2 - state.ic2eq);

        // Highpass = input - k*bandpass - lowpass
        out[i] = input[i] - state.k * v1 - v2;
    }
}

// SVF Bandpass
[[gnu::always_inline]]
inline void op_filter_svf_bp(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* freq = ctx.buffers->get(inst.inputs[1]);
    const float* q = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<SVFState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        calc_svf(state, freq[i], q[i], ctx.sample_rate);

        // Add tiny DC to prevent denormals
        float v3 = input[i] - (state.ic2eq + DENORMAL_DC);
        float v1 = state.a1 * (state.ic1eq + DENORMAL_DC) + state.a2 * v3;
        float v2 = (state.ic2eq + DENORMAL_DC) + state.a2 * (state.ic1eq + DENORMAL_DC) + state.a3 * v3;
        state.ic1eq = clamp_audio(2.0f * v1 - state.ic1eq);
        state.ic2eq = clamp_audio(2.0f * v2 - state.ic2eq);

        out[i] = v1;  // Bandpass output
    }
}

}  // namespace cedar
