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
// DISTORT_FOLD: Wavefolder with ADAA (Antiderivative Antialiasing)
// ============================================================================
// in0: input signal
// in1: drive (1.0-10.0, fold intensity)
// in2: symmetry (0.0-1.0, 0.5 = symmetric, other = asymmetric harmonics)
//
// Alias-free sine wavefolder using first-order ADAA.
// Classic West Coast synthesis technique with mathematical anti-aliasing.
// f(x) = sin(drive * x)
// F₁(x) = -cos(drive * x) / drive  (antiderivative)
// ADAA: y[n] = (F₁(x[n]) - F₁(x[n-1])) / (x[n] - x[n-1])

[[gnu::always_inline]]
inline void op_distort_fold(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* drive_in = ctx.buffers->get(inst.inputs[1]);
    const float* symmetry = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<FoldADAAState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float drive = std::clamp(drive_in[i], 1.0f, 10.0f);
        float sym = std::clamp(symmetry[i], 0.0f, 1.0f);

        // Apply asymmetry bias (shifts the fold point)
        float x = input[i] + (sym - 0.5f) * 0.5f;

        // Scale by drive
        float x_scaled = x * drive;

        // Antiderivative of sin(x) is -cos(x)
        // For sin(drive * x), antiderivative is -cos(drive * x) / drive
        float ad = -std::cos(x_scaled) / drive;

        // ADAA formula: y[n] = (F₁(x[n]) - F₁(x[n-1])) / (x[n] - x[n-1])
        float diff = x_scaled - state.x_prev;
        float y;

        if (std::abs(diff) < 1e-5f) {
            // When samples are very close, use Taylor expansion fallback
            // sin(x) ≈ x - x³/6 near the singularity point
            // Actually just evaluate the function at the midpoint
            float mid = (x_scaled + state.x_prev) * 0.5f;
            y = std::sin(mid);
        } else {
            // Normal ADAA calculation
            y = (ad - state.ad_prev) / (diff / drive);
        }

        // Update state for next sample
        state.x_prev = x_scaled;
        state.ad_prev = ad;

        // Output with soft limiting
        out[i] = std::clamp(y, -1.0f, 1.0f);
    }
}

// ============================================================================
// DISTORT_TUBE: Asymmetric Tube-Style Saturation
// ============================================================================
// in0: input signal
// in1: drive (1-20, higher = more saturation)
// in2: bias (0.0-0.3, controls even harmonic content)
//
// Emulates triode tube saturation with asymmetric transfer function.
// Produces even harmonics (especially 2nd) for warm, vintage character.
// Uses 2x oversampling by default to reduce aliasing.

[[gnu::always_inline]]
inline void op_distort_tube(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* drive = ctx.buffers->get(inst.inputs[1]);
    const float* bias = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<TubeState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float d = std::clamp(drive[i], 1.0f, 20.0f);
        float b = std::clamp(bias[i], 0.0f, 0.3f);
        float x = input[i];

        // 2x oversampling: upsample
        state.os_delay[state.os_idx] = x;
        float x0 = x;
        float x1 = (x + state.os_delay[(state.os_idx + 3) & 3]) * 0.5f;

        auto tube_core = [d, b](float s) {
            // Apply drive and bias (bias creates asymmetry -> even harmonics)
            float driven = s * d + b;

            // Asymmetric transfer function
            // Positive: softer compression (tube-like)
            // Negative: slightly harder compression
            float y;
            if (driven >= 0.0f) {
                // Soft knee positive saturation: 1 - exp(-x)
                y = 1.0f - std::exp(-driven);
            } else {
                // Slightly harder negative saturation: tanh-based
                y = std::tanh(driven * 1.2f);
            }

            // Soft clip to prevent overshoot
            return std::clamp(y, -1.0f, 1.0f);
        };

        // Process at 2x rate and average back down
        float y0 = tube_core(x0);
        float y1 = tube_core(x1);
        out[i] = (y0 + y1) * 0.5f;

        state.os_idx = (state.os_idx + 1) & 3;
    }
}

// ============================================================================
// DISTORT_SMOOTH: ADAA (Antiderivative Antialiasing) Saturation
// ============================================================================
// in0: input signal
// in1: drive (1-20, higher = more saturation)
//
// Alias-free tanh saturation using first-order antiderivative antialiasing.
// Produces clean, high-quality saturation without harsh aliasing artifacts.
// No oversampling needed - ADAA handles antialiasing mathematically.

[[gnu::always_inline]]
inline void op_distort_smooth(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* drive = ctx.buffers->get(inst.inputs[1]);
    auto& state = ctx.states->get_or_create<SmoothSatState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float d = std::clamp(drive[i], 1.0f, 20.0f);
        float x = input[i] * d;

        // Antiderivative of tanh(x) is log(cosh(x))
        // For numerical stability, use: log(cosh(x)) = |x| + log(1 + exp(-2|x|)) - log(2)
        float abs_x = std::abs(x);
        float ad;
        if (abs_x > 10.0f) {
            // For large values, log(cosh(x)) ≈ |x| - log(2)
            ad = abs_x - 0.693147f;
        } else {
            ad = std::log(std::cosh(x));
        }

        // ADAA formula: y[n] = (F₁(x[n]) - F₁(x[n-1])) / (x[n] - x[n-1])
        float diff = x - state.x_prev;
        float y;

        if (std::abs(diff) < 1e-5f) {
            // When samples are very close, use fallback: f((x + x_prev) / 2)
            y = std::tanh((x + state.x_prev) * 0.5f);
        } else {
            y = (ad - state.ad_prev) / diff;
        }

        // Update state for next sample
        state.x_prev = x;
        state.ad_prev = ad;

        // Normalize output (ADAA can affect amplitude slightly)
        out[i] = std::clamp(y, -1.0f, 1.0f);
    }
}

// ============================================================================
// DISTORT_TAPE: Tape-Style Saturation
// ============================================================================
// in0: input signal
// in1: drive (1-10, tape saturation amount)
// in2: warmth (0-1, high frequency rolloff)
//
// Emulates magnetic tape saturation characteristics:
// - Soft, symmetric compression with wide linear region
// - Subtle high-frequency rolloff for warmth
// - Smooth limiting behavior at extremes

[[gnu::always_inline]]
inline void op_distort_tape(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* drive = ctx.buffers->get(inst.inputs[1]);
    const float* warmth = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<TapeState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float d = std::clamp(drive[i], 1.0f, 10.0f);
        float w = std::clamp(warmth[i], 0.0f, 1.0f);
        float x = input[i];

        // 2x oversampling
        state.os_delay[state.os_idx] = x;
        float x0 = x;
        float x1 = (x + state.os_delay[(state.os_idx + 3) & 3]) * 0.5f;

        auto tape_core = [d](float s) {
            float driven = s * d;
            float abs_d = std::abs(driven);

            // Tape-style soft saturation with wide linear region
            // Uses smooth polynomial knee transitioning to tanh limiting
            float y;
            if (abs_d < 0.5f) {
                // Linear region (unity gain below threshold)
                y = driven;
            } else if (abs_d < 2.0f) {
                // Soft knee: polynomial transition
                float t = (abs_d - 0.5f) / 1.5f;  // 0 to 1 in transition region
                float knee = 1.0f - t * t * 0.3f;  // Gentle compression curve
                y = driven * knee;
            } else {
                // Hard saturation region: tanh limiting
                float sign = driven >= 0.0f ? 1.0f : -1.0f;
                y = sign * (0.85f + 0.15f * std::tanh((abs_d - 2.0f) * 0.5f));
            }

            return y;
        };

        // Process at 2x rate
        float y0 = tape_core(x0);
        float y1 = tape_core(x1);
        float y = (y0 + y1) * 0.5f;

        // High-shelf filter for warmth (subtle HF rolloff)
        // One-pole lowpass on the difference signal
        float hf = y - state.hs_z1;
        state.hs_z1 = state.hs_z1 + hf * (1.0f - w * 0.7f);
        y = state.hs_z1 + hf * (1.0f - w);

        out[i] = std::clamp(y, -1.0f, 1.0f);
        state.os_idx = (state.os_idx + 1) & 3;
    }
}

// ============================================================================
// DISTORT_XFMR: Transformer Saturation
// ============================================================================
// in0: input signal
// in1: drive (1-10, overall saturation)
// in2: bass saturation (1-10, low frequency saturation emphasis)
//
// Emulates transformer saturation where bass frequencies saturate
// more heavily than highs (magnetic core saturation).
// Creates thick, punchy low-end with cleaner highs.

[[gnu::always_inline]]
inline void op_distort_xfmr(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* drive = ctx.buffers->get(inst.inputs[1]);
    const float* bass_sat = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<XfmrState>(inst.state_id);

    // Leaky integrator coefficient (~60Hz @ 48kHz)
    constexpr float LP_COEFF = 0.992f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float d = std::clamp(drive[i], 1.0f, 10.0f);
        float bs = std::clamp(bass_sat[i], 1.0f, 10.0f);
        float x = input[i];

        // 2x oversampling
        state.os_delay[state.os_idx] = x;
        float x0 = x;
        float x1 = (x + state.os_delay[(state.os_idx + 3) & 3]) * 0.5f;

        auto xfmr_core = [d, bs, &state, LP_COEFF](float s) {
            // Extract bass via leaky integrator
            state.integrator = state.integrator * LP_COEFF + s * (1.0f - LP_COEFF);
            float bass = state.integrator;
            float highs = s - bass;

            // Saturate bass more heavily (transformer core saturation)
            float sat_bass = std::tanh(bass * bs);

            // Lighter saturation on highs (winding saturation is gentler)
            float sat_highs = highs;
            if (std::abs(highs * d) > 0.7f) {
                float sign = highs >= 0.0f ? 1.0f : -1.0f;
                sat_highs = sign * 0.7f + std::tanh((highs * d - sign * 0.7f) * 0.5f) * 0.3f;
                sat_highs /= d;  // Normalize
            }

            // Recombine with overall drive
            float combined = sat_bass + sat_highs * 0.9f;
            return std::tanh(combined * d * 0.5f);
        };

        // Process at 2x rate
        float y0 = xfmr_core(x0);
        float y1 = xfmr_core(x1);
        out[i] = (y0 + y1) * 0.5f;

        state.os_idx = (state.os_idx + 1) & 3;
    }
}

// ============================================================================
// DISTORT_EXCITE: Harmonic Exciter
// ============================================================================
// in0: input signal
// in1: amount (0-1, exciter intensity)
// in2: frequency (1000-10000 Hz, high-pass corner for harmonics)
//
// Adds controlled harmonic content to high frequencies only.
// Similar to Aphex Aural Exciter - creates presence and sparkle
// without adding low-frequency mud.

[[gnu::always_inline]]
inline void op_distort_excite(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* amount = ctx.buffers->get(inst.inputs[1]);
    const float* freq = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<ExciterState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float amt = std::clamp(amount[i], 0.0f, 1.0f);
        float f = std::clamp(freq[i], 1000.0f, 10000.0f);
        float x = input[i];

        // High-pass filter coefficient (one-pole)
        // coeff = exp(-2 * pi * freq / sample_rate)
        float coeff = std::exp(-6.283185f * f / 48000.0f);

        // 2x oversampling for harmonic generation
        state.os_delay[state.os_idx] = x;
        float x0 = x;
        float x1 = (x + state.os_delay[(state.os_idx + 3) & 3]) * 0.5f;

        auto excite_core = [amt, coeff, &state](float s) {
            // High-pass filter to extract high frequencies
            float hp = s - state.hp_z1;
            state.hp_z1 = state.hp_z1 + hp * (1.0f - coeff);

            // Generate harmonics from highs only
            // Odd harmonics: cubic
            float odd = hp * hp * hp;
            // Even harmonics: quadratic (rectified)
            float even = hp * std::abs(hp);

            // Mix harmonics (2nd harmonic emphasis for musicality)
            float harmonics = odd * 0.4f + even * 0.6f;

            // Return original + harmonics
            return s + harmonics * amt * 1.5f;
        };

        // Process at 2x rate
        float y0 = excite_core(x0);
        float y1 = excite_core(x1);
        out[i] = std::clamp((y0 + y1) * 0.5f, -1.0f, 1.0f);

        state.os_idx = (state.os_idx + 1) & 3;
    }
}

}  // namespace cedar
