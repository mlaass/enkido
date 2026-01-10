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

// ============================================================================
// Moog-Style Ladder Filter
// ============================================================================

// Soft saturation function (fast tanh approximation)
// Provides analog-like nonlinearity in the feedback path
[[gnu::always_inline]]
inline float soft_clip(float x) {
    // Pade approximant of tanh, accurate for |x| < 3
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// FILTER_MOOG: 4-pole (24dB/oct) Moog-style ladder filter
// in0: input signal
// in1: cutoff frequency (Hz)
// in2: resonance (0.0-4.0, self-oscillates at ~4.0)
//
// Based on the Huovilainen improved model for digital Moog filters
// Features nonlinear saturation in the feedback path for analog character
[[gnu::always_inline]]
inline void op_filter_moog(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* freq = ctx.buffers->get(inst.inputs[1]);
    const float* res = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<MoogState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float cutoff = freq[i];
        float resonance = res[i];

        // Update coefficients if parameters changed
        if (cutoff != state.last_freq || resonance != state.last_res) {
            state.last_freq = cutoff;
            state.last_res = resonance;

            // Frequency warping for digital implementation
            // Clamp cutoff to prevent instability at very high frequencies
            float f = std::clamp(cutoff / ctx.sample_rate, 0.0f, 0.45f);

            // Compute g coefficient using tan for frequency warping
            // This provides better frequency accuracy at high cutoffs
            state.g = std::tan(PI * f);

            // Resonance coefficient (0-4 range, self-oscillates near 4)
            state.k = std::clamp(resonance, 0.0f, 4.0f);
        }

        // Get feedback from last stage output with nonlinear saturation
        // This creates the characteristic Moog "growl" at high resonance
        float feedback = state.k * soft_clip(state.stage[3]);

        // Input with feedback subtracted (negative feedback loop)
        float x = input[i] - feedback;

        // Soft clip the input to prevent harsh clipping at high input levels
        x = soft_clip(x * 0.5f) * 2.0f;

        // Calculate single-pole lowpass coefficient for trapezoidal integration
        // G = g / (1 + g) for each stage
        float G = state.g / (1.0f + state.g);

        // 4 cascaded 1-pole lowpass stages using trapezoidal integration
        // Each stage: y[n] = G * (x[n] - y[n-1]) + y[n-1]
        // With unit delays for stability
        for (int j = 0; j < 4; ++j) {
            float input_stage = (j == 0) ? x : state.stage[j - 1];

            // Trapezoidal integration (implicit Euler)
            float v = G * (input_stage - state.delay[j]);
            float y = v + state.delay[j];

            // Update delay and stage output
            state.delay[j] = y + v;
            state.stage[j] = y;

            // Apply soft saturation between stages for analog character
            if (j < 3) {
                state.stage[j] = soft_clip(state.stage[j]);
            }
        }

        // Output is the 4-pole lowpass
        out[i] = clamp_audio(state.stage[3]);
    }
}

}  // namespace cedar
