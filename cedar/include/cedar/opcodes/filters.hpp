#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
#include <cmath>

namespace cedar {

// Calculate biquad coefficients for lowpass filter
// Uses RBJ Audio EQ Cookbook formulas
inline void calc_biquad_lp(BiquadState& state, float freq, float q, float sample_rate) {
    // Only recalculate if parameters changed
    if (freq == state.last_freq && q == state.last_q) {
        return;
    }
    state.last_freq = freq;
    state.last_q = q;

    // Clamp frequency to valid range
    freq = std::clamp(freq, 20.0f, sample_rate * 0.49f);
    q = std::max(0.1f, q);

    float w0 = TWO_PI * freq / sample_rate;
    float cos_w0 = std::cos(w0);
    float sin_w0 = std::sin(w0);
    float alpha = sin_w0 / (2.0f * q);

    float a0 = 1.0f + alpha;
    state.b0 = ((1.0f - cos_w0) / 2.0f) / a0;
    state.b1 = (1.0f - cos_w0) / a0;
    state.b2 = ((1.0f - cos_w0) / 2.0f) / a0;
    state.a1 = (-2.0f * cos_w0) / a0;
    state.a2 = (1.0f - alpha) / a0;
}

// Calculate biquad coefficients for highpass filter
inline void calc_biquad_hp(BiquadState& state, float freq, float q, float sample_rate) {
    if (freq == state.last_freq && q == state.last_q) {
        return;
    }
    state.last_freq = freq;
    state.last_q = q;

    freq = std::clamp(freq, 20.0f, sample_rate * 0.49f);
    q = std::max(0.1f, q);

    float w0 = TWO_PI * freq / sample_rate;
    float cos_w0 = std::cos(w0);
    float sin_w0 = std::sin(w0);
    float alpha = sin_w0 / (2.0f * q);

    float a0 = 1.0f + alpha;
    state.b0 = ((1.0f + cos_w0) / 2.0f) / a0;
    state.b1 = (-(1.0f + cos_w0)) / a0;
    state.b2 = ((1.0f + cos_w0) / 2.0f) / a0;
    state.a1 = (-2.0f * cos_w0) / a0;
    state.a2 = (1.0f - alpha) / a0;
}

// Calculate biquad coefficients for bandpass filter
inline void calc_biquad_bp(BiquadState& state, float freq, float q, float sample_rate) {
    if (freq == state.last_freq && q == state.last_q) {
        return;
    }
    state.last_freq = freq;
    state.last_q = q;

    freq = std::clamp(freq, 20.0f, sample_rate * 0.49f);
    q = std::max(0.1f, q);

    float w0 = TWO_PI * freq / sample_rate;
    float cos_w0 = std::cos(w0);
    float sin_w0 = std::sin(w0);
    float alpha = sin_w0 / (2.0f * q);

    float a0 = 1.0f + alpha;
    state.b0 = alpha / a0;
    state.b1 = 0.0f;
    state.b2 = -alpha / a0;
    state.a1 = (-2.0f * cos_w0) / a0;
    state.a2 = (1.0f - alpha) / a0;
}

// Process biquad filter (Direct Form 2 Transposed)
[[gnu::always_inline]]
inline float process_biquad(BiquadState& state, float input) {
    float output = state.b0 * input + state.b1 * state.x1 + state.b2 * state.x2
                 - state.a1 * state.y1 - state.a2 * state.y2;

    // Update history
    state.x2 = state.x1;
    state.x1 = input;
    state.y2 = state.y1;
    state.y1 = output;

    return output;
}

// FILTER_LP: Lowpass biquad filter
// in0 = input signal, in1 = cutoff frequency, in2 = Q
[[gnu::always_inline]]
inline void op_filter_lp(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* freq = ctx.buffers->get(inst.inputs[1]);
    const float* q = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<BiquadState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        calc_biquad_lp(state, freq[i], q[i], ctx.sample_rate);
        out[i] = process_biquad(state, input[i]);
    }
}

// FILTER_HP: Highpass biquad filter
[[gnu::always_inline]]
inline void op_filter_hp(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* freq = ctx.buffers->get(inst.inputs[1]);
    const float* q = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<BiquadState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        calc_biquad_hp(state, freq[i], q[i], ctx.sample_rate);
        out[i] = process_biquad(state, input[i]);
    }
}

// FILTER_BP: Bandpass biquad filter
[[gnu::always_inline]]
inline void op_filter_bp(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* freq = ctx.buffers->get(inst.inputs[1]);
    const float* q = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<BiquadState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        calc_biquad_bp(state, freq[i], q[i], ctx.sample_rate);
        out[i] = process_biquad(state, input[i]);
    }
}

// SVF (State Variable Filter) coefficient calculation
inline void calc_svf(SVFState& state, float freq, float q, float sample_rate) {
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

        float v3 = input[i] - state.ic2eq;
        float v1 = state.a1 * state.ic1eq + state.a2 * v3;
        float v2 = state.ic2eq + state.a2 * state.ic1eq + state.a3 * v3;
        state.ic1eq = 2.0f * v1 - state.ic1eq;
        state.ic2eq = 2.0f * v2 - state.ic2eq;

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

        float v3 = input[i] - state.ic2eq;
        float v1 = state.a1 * state.ic1eq + state.a2 * v3;
        float v2 = state.ic2eq + state.a2 * state.ic1eq + state.a3 * v3;
        state.ic1eq = 2.0f * v1 - state.ic1eq;
        state.ic2eq = 2.0f * v2 - state.ic2eq;

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

        float v3 = input[i] - state.ic2eq;
        float v1 = state.a1 * state.ic1eq + state.a2 * v3;
        float v2 = state.ic2eq + state.a2 * state.ic1eq + state.a3 * v3;
        state.ic1eq = 2.0f * v1 - state.ic1eq;
        state.ic2eq = 2.0f * v2 - state.ic2eq;

        out[i] = v1;  // Bandpass output
    }
}

}  // namespace cedar
