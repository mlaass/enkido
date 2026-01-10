#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
#include "dsp_utils.hpp"
#include <cmath>
#include <algorithm>

namespace cedar {

// ============================================================================
// DISTORT_TANH: Tanh Saturation
// ============================================================================
// in0: input signal
// in1: drive (1.0 = unity, higher = more saturation)
//
// Classic warm saturation using hyperbolic tangent.
// Higher drive values push the signal into saturation more aggressively.

[[gnu::always_inline]]
inline void op_distort_tanh(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* drive = ctx.buffers->get(inst.inputs[1]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float d = std::max(0.1f, drive[i]);
        float x = input[i] * d;
        out[i] = std::tanh(x);
    }
}

// ============================================================================
// DISTORT_SOFT: Polynomial Soft Clipping
// ============================================================================
// in0: input signal
// in1: threshold (0.1-2.0, lower = more clipping)
//
// Smooth polynomial soft clipper that rounds off peaks gradually.
// Uses cubic polynomial for continuous first derivative.

[[gnu::always_inline]]
inline void op_distort_soft(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* threshold = ctx.buffers->get(inst.inputs[1]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float t = std::clamp(threshold[i], 0.1f, 2.0f);
        float x = input[i] / t;

        // Polynomial soft clip (same as Moog filter uses)
        float y;
        if (x > 3.0f) {
            y = 1.0f;
        } else if (x < -3.0f) {
            y = -1.0f;
        } else {
            float x2 = x * x;
            y = x * (27.0f + x2) / (27.0f + 9.0f * x2);
        }

        out[i] = y * t;
    }
}

// ============================================================================
// DISTORT_BITCRUSH: Bit Crusher / Sample Rate Reducer
// ============================================================================
// in0: input signal
// in1: bit depth (1-16, fractional allowed for smooth transitions)
// in2: sample rate reduction factor (0.0-1.0, where 1.0 = full rate, 0.1 = 1/10th rate)
//
// Lo-fi effect that reduces bit depth and/or sample rate.
// Creates classic 8-bit and retro digital sounds.

[[gnu::always_inline]]
inline void op_distort_bitcrush(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* bits = ctx.buffers->get(inst.inputs[1]);
    const float* rate = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<BitcrushState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Sample rate reduction: only sample when phase wraps
        float rate_factor = std::clamp(rate[i], 0.01f, 1.0f);
        state.phase += rate_factor;

        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;

            // Bit depth reduction
            float depth = std::clamp(bits[i], 1.0f, 16.0f);
            float levels = std::pow(2.0f, depth);

            // Quantize to discrete levels
            state.held_sample = std::round(input[i] * levels) / levels;
        }

        out[i] = state.held_sample;
    }
}

// ============================================================================
// DISTORT_FOLD: Wavefolder
// ============================================================================
// in0: input signal
// in1: threshold (0.1-2.0, lower = more folding)
// in2: symmetry (0.0-1.0, 0.5 = symmetric, other = asymmetric harmonics)
//
// Folds the waveform back when it exceeds the threshold, creating
// complex harmonics. Classic West Coast synthesis technique.

[[gnu::always_inline]]
inline void op_distort_fold(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* threshold = ctx.buffers->get(inst.inputs[1]);
    const float* symmetry = ctx.buffers->get(inst.inputs[2]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float t = std::clamp(threshold[i], 0.1f, 2.0f);
        float sym = std::clamp(symmetry[i], 0.0f, 1.0f);

        // Apply asymmetry bias
        float x = input[i] + (sym - 0.5f) * t;

        // Wavefold: reflect at boundaries
        // Use modular arithmetic for multiple folds
        float folded = x / t;

        // Map to 0-4 range, then fold
        folded = std::fmod(folded + 1.0f, 4.0f);
        if (folded < 0.0f) folded += 4.0f;

        // Triangular fold pattern: 0->1, 1->2->0, 2->3->-1, 3->4->0
        if (folded > 3.0f) {
            folded = folded - 4.0f;
        } else if (folded > 2.0f) {
            folded = -(folded - 2.0f);
        } else if (folded > 1.0f) {
            folded = 2.0f - folded;
        }

        out[i] = folded * t;
    }
}

}  // namespace cedar
