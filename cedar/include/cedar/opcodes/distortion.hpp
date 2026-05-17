#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
#include "dsp_utils.hpp"
#include "drywet.hpp"
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
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* drive = ctx.buffers->get(inst.inputs[1]);
    const float* dry_level = (inst.inputs[2] != 0xFFFF) ? ctx.buffers->get(inst.inputs[2]) : nullptr;
    const float* wet_level = (inst.inputs[3] != 0xFFFF) ? ctx.buffers->get(inst.inputs[3]) : nullptr;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float d = std::max(0.1f, drive[i]);
        float dry = drywet::coeff(dry_level, i, 0.0f);
        float wet = drywet::coeff(wet_level, i, 1.0f);
        float x_l = input[i];
        float x_r = stereo_in ? input_r[i] : x_l;
        out_l[i] = drywet::mix(x_l, std::tanh(x_l * d), dry, wet);
        out_r[i] = drywet::mix(x_r, std::tanh(x_r * d), dry, wet);
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
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* threshold = ctx.buffers->get(inst.inputs[1]);
    const float* dry_level = (inst.inputs[2] != 0xFFFF) ? ctx.buffers->get(inst.inputs[2]) : nullptr;
    const float* wet_level = (inst.inputs[3] != 0xFFFF) ? ctx.buffers->get(inst.inputs[3]) : nullptr;

    auto soft = [](float x) {
        if (x > 3.0f) return 1.0f;
        if (x < -3.0f) return -1.0f;
        float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    };

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float t = std::clamp(threshold[i], 0.1f, 2.0f);
        float dry = drywet::coeff(dry_level, i, 0.0f);
        float wet = drywet::coeff(wet_level, i, 1.0f);
        float x_l = input[i];
        float x_r = stereo_in ? input_r[i] : x_l;
        out_l[i] = drywet::mix(x_l, soft(x_l / t) * t, dry, wet);
        out_r[i] = drywet::mix(x_r, soft(x_r / t) * t, dry, wet);
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
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* bits = ctx.buffers->get(inst.inputs[1]);
    const float* rate = ctx.buffers->get(inst.inputs[2]);
    const float* dry_level = (inst.inputs[3] != 0xFFFF) ? ctx.buffers->get(inst.inputs[3]) : nullptr;
    const float* wet_level = (inst.inputs[4] != 0xFFFF) ? ctx.buffers->get(inst.inputs[4]) : nullptr;
    auto& state = ctx.states->get_or_create<BitcrushState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Mono control inputs (shared across channels)
        float rate_factor = std::clamp(rate[i], 0.01f, 1.0f);
        float depth = std::clamp(bits[i], 1.0f, 16.0f);
        float levels = std::pow(2.0f, depth);
        float dry = drywet::coeff(dry_level, i, 0.0f);
        float wet = drywet::coeff(wet_level, i, 1.0f);

        for (std::size_t ch = 0; ch < 2; ++ch) {
            float x = (ch == 0) ? input[i] : (stereo_in ? input_r[i] : input[i]);
            state.phase[ch] += rate_factor;
            if (state.phase[ch] >= 1.0f) {
                state.phase[ch] -= 1.0f;
                state.held_sample[ch] = std::round(x * levels) / levels;
            }
            (ch == 0 ? out_l : out_r)[i] = drywet::mix(x, state.held_sample[ch], dry, wet);
        }
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
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* drive_in = ctx.buffers->get(inst.inputs[1]);
    const float* symmetry = (inst.inputs[2] != 0xFFFF) ? ctx.buffers->get(inst.inputs[2]) : nullptr;
    auto& state = ctx.states->get_or_create<FoldADAAState>(inst.state_id);

    // ExtendedParams<2>: ext[0] = dry (default 0.0), ext[1] = wet (default 1.0)
    const auto* ext = ctx.states->get_if<ExtendedParams<2>>(ext_params_state_id(inst.state_id));
    const float* dry_buf = nullptr; float dry_const = 0.0f;
    const float* wet_buf = nullptr; float wet_const = 1.0f;
    if (ext) {
        const auto& d = ext->params[0];
        if (d.is_constant()) dry_const = d.constant; else dry_buf = ctx.buffers->get(d.buffer_idx);
        const auto& w = ext->params[1];
        if (w.is_constant()) wet_const = w.constant; else wet_buf = ctx.buffers->get(w.buffer_idx);
    }

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Mono control inputs (shared across channels)
        float drive = std::clamp(drive_in[i], 1.0f, 10.0f);
        float sym = symmetry ? std::clamp(symmetry[i], 0.0f, 1.0f) : 0.5f;
        float dry = drywet::coeff(dry_buf, i, dry_const);
        float wet = drywet::coeff(wet_buf, i, wet_const);

        for (std::size_t ch = 0; ch < 2; ++ch) {
            float xin = (ch == 0) ? input[i] : (stereo_in ? input_r[i] : input[i]);
            float x = xin + (sym - 0.5f) * 0.5f;
            float x_scaled = x * drive;
            float ad = -std::cos(x_scaled) / drive;

            float diff = x_scaled - state.x_prev[ch];
            float y;
            if (std::abs(diff) < 1e-5f) {
                float mid = (x_scaled + state.x_prev[ch]) * 0.5f;
                y = std::sin(mid);
            } else {
                y = (ad - state.ad_prev[ch]) / (diff / drive);
            }

            state.x_prev[ch] = x_scaled;
            state.ad_prev[ch] = ad;
            float processed = std::clamp(y, -1.0f, 1.0f);
            (ch == 0 ? out_l : out_r)[i] = drywet::mix(xin, processed, dry, wet);
        }
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
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* drive = ctx.buffers->get(inst.inputs[1]);
    const float* bias = ctx.buffers->get(inst.inputs[2]);
    const float* dry_level = (inst.inputs[3] != 0xFFFF) ? ctx.buffers->get(inst.inputs[3]) : nullptr;
    const float* wet_level = (inst.inputs[4] != 0xFFFF) ? ctx.buffers->get(inst.inputs[4]) : nullptr;
    auto& state = ctx.states->get_or_create<TubeState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float d = std::clamp(drive[i], 1.0f, 20.0f);
        float b = std::clamp(bias[i], 0.0f, 0.3f);
        float dry_m = drywet::coeff(dry_level, i, 0.0f);
        float wet_m = drywet::coeff(wet_level, i, 1.0f);

        auto tube_core = [d, b](float s) {
            float driven = s * d + b;
            float y;
            if (driven >= 0.0f) {
                y = 1.0f - std::exp(-driven);
            } else {
                y = std::tanh(driven * 1.2f);
            }
            return std::clamp(y, -1.0f, 1.0f);
        };

        for (std::size_t ch = 0; ch < 2; ++ch) {
            float x = (ch == 0) ? input[i] : (stereo_in ? input_r[i] : input[i]);

            // 2x oversampling using per-channel delay line
            state.os_delay[ch][state.os_idx[ch]] = x;
            float x0 = x;
            float x1 = (x + state.os_delay[ch][(state.os_idx[ch] + 3) & 3]) * 0.5f;

            float y0 = tube_core(x0);
            float y1 = tube_core(x1);
            (ch == 0 ? out_l : out_r)[i] = drywet::mix(x, (y0 + y1) * 0.5f, dry_m, wet_m);
            state.os_idx[ch] = (state.os_idx[ch] + 1) & 3;
        }
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
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* drive = ctx.buffers->get(inst.inputs[1]);
    const float* dry_level = (inst.inputs[2] != 0xFFFF) ? ctx.buffers->get(inst.inputs[2]) : nullptr;
    const float* wet_level = (inst.inputs[3] != 0xFFFF) ? ctx.buffers->get(inst.inputs[3]) : nullptr;
    auto& state = ctx.states->get_or_create<SmoothSatState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float d = std::clamp(drive[i], 1.0f, 20.0f);
        float dry_m = drywet::coeff(dry_level, i, 0.0f);
        float wet_m = drywet::coeff(wet_level, i, 1.0f);

        for (std::size_t ch = 0; ch < 2; ++ch) {
            float xin = (ch == 0) ? input[i] : (stereo_in ? input_r[i] : input[i]);
            float x = xin * d;

            float abs_x = std::abs(x);
            float ad = abs_x + std::log1p(std::exp(-2.0f * abs_x)) - 0.693147f;

            float y;
            if (!state.initialized[ch]) {
                y = std::tanh(x);
                state.initialized[ch] = true;
            } else {
                float diff = x - state.x_prev[ch];
                if (std::abs(diff) < 1e-4f) {
                    y = std::tanh((x + state.x_prev[ch]) * 0.5f);
                } else {
                    float ad_diff;
                    float abs_xp = std::abs(state.x_prev[ch]);
                    if (abs_x > 0.5f && abs_xp > 0.5f && (x > 0) == (state.x_prev[ch] > 0)) {
                        float abs_diff = (x > 0) ? diff : -diff;
                        float log_diff = std::log1p(std::exp(-2.0f * abs_x))
                                       - std::log1p(std::exp(-2.0f * abs_xp));
                        ad_diff = abs_diff + log_diff;
                    } else {
                        ad_diff = ad - state.ad_prev[ch];
                    }
                    y = ad_diff / diff;
                }
            }

            state.x_prev[ch] = x;
            state.ad_prev[ch] = ad;
            (ch == 0 ? out_l : out_r)[i] = drywet::mix(xin, y, dry_m, wet_m);
        }
    }
}

// Default constants for tape saturation
constexpr float TAPE_SOFT_THRESHOLD_DEFAULT = 0.5f;
constexpr float TAPE_WARMTH_SCALE_DEFAULT = 0.7f;

// ============================================================================
// DISTORT_TAPE: Tape-Style Saturation
// ============================================================================
// in0: input signal
// in1: drive (1-10, tape saturation amount)
// in2: warmth (0-1, high frequency rolloff)
// in3: soft_threshold - saturation onset (default 0.5)
// in4: warmth_scale - HF rolloff amount (default 0.7)
//
// Emulates magnetic tape saturation characteristics:
// - Soft, symmetric compression with wide linear region
// - Subtle high-frequency rolloff for warmth
// - Smooth limiting behavior at extremes

[[gnu::always_inline]]
inline void op_distort_tape(ExecutionContext& ctx, const Instruction& inst) {
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* drive = ctx.buffers->get(inst.inputs[1]);
    const float* warmth = ctx.buffers->get(inst.inputs[2]);
    const float* soft_threshold_in = ctx.buffers->get(inst.inputs[3]);
    const float* warmth_scale_in = ctx.buffers->get(inst.inputs[4]);
    auto& state = ctx.states->get_or_create<TapeState>(inst.state_id);

    // ExtendedParams<2>: ext[0] = dry (default 0.0), ext[1] = wet (default 1.0)
    const auto* ext = ctx.states->get_if<ExtendedParams<2>>(ext_params_state_id(inst.state_id));
    const float* dry_buf = nullptr; float dry_const = 0.0f;
    const float* wet_buf = nullptr; float wet_const = 1.0f;
    if (ext) {
        const auto& ed = ext->params[0];
        if (ed.is_constant()) dry_const = ed.constant; else dry_buf = ctx.buffers->get(ed.buffer_idx);
        const auto& ew = ext->params[1];
        if (ew.is_constant()) wet_const = ew.constant; else wet_buf = ctx.buffers->get(ew.buffer_idx);
    }

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float d = std::clamp(drive[i], 1.0f, 10.0f);
        float w = std::clamp(warmth[i], 0.0f, 1.0f);
        float soft_threshold = soft_threshold_in[i] > 0.0f ? soft_threshold_in[i] : TAPE_SOFT_THRESHOLD_DEFAULT;
        float warmth_scale = warmth_scale_in[i] > 0.0f ? warmth_scale_in[i] : TAPE_WARMTH_SCALE_DEFAULT;
        float dry_m = drywet::coeff(dry_buf, i, dry_const);
        float wet_m = drywet::coeff(wet_buf, i, wet_const);

        auto tape_core = [d, soft_threshold](float s) {
            float driven = s * d;
            float abs_d = std::abs(driven);
            float y;
            if (abs_d < soft_threshold) {
                y = driven;
            } else if (abs_d < 2.0f) {
                float t = (abs_d - soft_threshold) / (2.0f - soft_threshold);
                float knee = 1.0f - t * t * 0.3f;
                y = driven * knee;
            } else {
                float sign = driven >= 0.0f ? 1.0f : -1.0f;
                y = sign * (0.85f + 0.15f * std::tanh((abs_d - 2.0f) * 0.5f));
            }
            return y;
        };

        for (std::size_t ch = 0; ch < 2; ++ch) {
            float x = (ch == 0) ? input[i] : (stereo_in ? input_r[i] : input[i]);

            // 2x oversampling using per-channel delay line
            state.os_delay[ch][state.os_idx[ch]] = x;
            float x0 = x;
            float x1 = (x + state.os_delay[ch][(state.os_idx[ch] + 3) & 3]) * 0.5f;

            float y0 = tape_core(x0);
            float y1 = tape_core(x1);
            float y = (y0 + y1) * 0.5f;

            // Per-channel high-shelf warmth filter
            float hf = y - state.hs_z1[ch];
            state.hs_z1[ch] = state.hs_z1[ch] + hf * (1.0f - w * warmth_scale);
            y = state.hs_z1[ch] + hf * (1.0f - w);

            (ch == 0 ? out_l : out_r)[i] = drywet::mix(x, std::clamp(y, -1.0f, 1.0f), dry_m, wet_m);
            state.os_idx[ch] = (state.os_idx[ch] + 1) & 3;
        }
    }
}

// Default constants for transformer saturation
constexpr float XFMR_BASS_FREQ_DEFAULT = 60.0f;

// ============================================================================
// DISTORT_XFMR: Transformer Saturation
// ============================================================================
// in0: input signal
// in1: drive (1-10, overall saturation)
// in2: bass saturation (1-10, low frequency saturation emphasis)
// in3: bass_freq - bass extraction cutoff in Hz (default 60)
//
// Emulates transformer saturation where bass frequencies saturate
// more heavily than highs (magnetic core saturation).
// Creates thick, punchy low-end with cleaner highs.

[[gnu::always_inline]]
inline void op_distort_xfmr(ExecutionContext& ctx, const Instruction& inst) {
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* drive = ctx.buffers->get(inst.inputs[1]);
    const float* bass_sat = ctx.buffers->get(inst.inputs[2]);
    const float* bass_freq_in = ctx.buffers->get(inst.inputs[3]);
    auto& state = ctx.states->get_or_create<XfmrState>(inst.state_id);

    // ExtendedParams<2>: ext[0] = dry (default 0.0), ext[1] = wet (default 1.0)
    const auto* ext = ctx.states->get_if<ExtendedParams<2>>(ext_params_state_id(inst.state_id));
    const float* dry_buf = nullptr; float dry_const = 0.0f;
    const float* wet_buf = nullptr; float wet_const = 1.0f;
    if (ext) {
        const auto& ed = ext->params[0];
        if (ed.is_constant()) dry_const = ed.constant; else dry_buf = ctx.buffers->get(ed.buffer_idx);
        const auto& ew = ext->params[1];
        if (ew.is_constant()) wet_const = ew.constant; else wet_buf = ctx.buffers->get(ew.buffer_idx);
    }

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float d = std::clamp(drive[i], 1.0f, 10.0f);
        float bs = std::clamp(bass_sat[i], 1.0f, 10.0f);
        float bass_freq = bass_freq_in[i] > 0.0f ? bass_freq_in[i] : XFMR_BASS_FREQ_DEFAULT;
        float lp_coeff = std::exp(-6.283185f * bass_freq / ctx.sample_rate);
        float dry_m = drywet::coeff(dry_buf, i, dry_const);
        float wet_m = drywet::coeff(wet_buf, i, wet_const);

        for (std::size_t ch = 0; ch < 2; ++ch) {
            float x = (ch == 0) ? input[i] : (stereo_in ? input_r[i] : input[i]);

            // Per-channel 2x oversampling delay line
            state.os_delay[ch][state.os_idx[ch]] = x;
            float x0 = x;
            float x1 = (x + state.os_delay[ch][(state.os_idx[ch] + 3) & 3]) * 0.5f;

            auto xfmr_core = [d, bs, &state, ch, lp_coeff](float s) {
                state.integrator[ch] = state.integrator[ch] * lp_coeff + s * (1.0f - lp_coeff);
                float bass = state.integrator[ch];
                float highs = s - bass;

                float sat_bass = std::tanh(bass * bs);

                float sat_highs = highs;
                if (std::abs(highs * d) > 0.7f) {
                    float sign = highs >= 0.0f ? 1.0f : -1.0f;
                    sat_highs = sign * 0.7f + std::tanh((highs * d - sign * 0.7f) * 0.5f) * 0.3f;
                    sat_highs /= d;
                }

                float combined = sat_bass + sat_highs * 0.9f;
                return std::tanh(combined * d * 0.5f);
            };

            float y0 = xfmr_core(x0);
            float y1 = xfmr_core(x1);
            (ch == 0 ? out_l : out_r)[i] = drywet::mix(x, (y0 + y1) * 0.5f, dry_m, wet_m);
            state.os_idx[ch] = (state.os_idx[ch] + 1) & 3;
        }
    }
}

// Default constants for harmonic exciter
constexpr float EXCITE_HARMONIC_ODD_DEFAULT = 0.4f;
constexpr float EXCITE_HARMONIC_EVEN_DEFAULT = 0.6f;

// ============================================================================
// DISTORT_EXCITE: Harmonic Exciter
// ============================================================================
// in0: input signal
// in1: amount (0-1, exciter intensity)
// in2: frequency (1000-10000 Hz, high-pass corner for harmonics)
// in3: harmonic_odd - odd harmonic mix (default 0.4)
// in4: harmonic_even - even harmonic mix (default 0.6)
//
// Adds controlled harmonic content to high frequencies only.
// Similar to Aphex Aural Exciter - creates presence and sparkle
// without adding low-frequency mud.

[[gnu::always_inline]]
inline void op_distort_excite(ExecutionContext& ctx, const Instruction& inst) {
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* amount = ctx.buffers->get(inst.inputs[1]);
    const float* freq = ctx.buffers->get(inst.inputs[2]);
    const float* harmonic_odd_in = ctx.buffers->get(inst.inputs[3]);
    const float* harmonic_even_in = ctx.buffers->get(inst.inputs[4]);
    auto& state = ctx.states->get_or_create<ExciterState>(inst.state_id);

    // ExtendedParams<2>: ext[0] = dry (default 0.0), ext[1] = wet (default 1.0)
    const auto* ext = ctx.states->get_if<ExtendedParams<2>>(ext_params_state_id(inst.state_id));
    const float* dry_buf = nullptr; float dry_const = 0.0f;
    const float* wet_buf = nullptr; float wet_const = 1.0f;
    if (ext) {
        const auto& ed = ext->params[0];
        if (ed.is_constant()) dry_const = ed.constant; else dry_buf = ctx.buffers->get(ed.buffer_idx);
        const auto& ew = ext->params[1];
        if (ew.is_constant()) wet_const = ew.constant; else wet_buf = ctx.buffers->get(ew.buffer_idx);
    }

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float amt = std::clamp(amount[i], 0.0f, 1.0f);
        float f = std::clamp(freq[i], 1000.0f, 10000.0f);
        float harmonic_odd = harmonic_odd_in[i] > 0.0f ? harmonic_odd_in[i] : EXCITE_HARMONIC_ODD_DEFAULT;
        float harmonic_even = harmonic_even_in[i] > 0.0f ? harmonic_even_in[i] : EXCITE_HARMONIC_EVEN_DEFAULT;
        float coeff = std::exp(-6.283185f * f / ctx.sample_rate);
        float dry_m = drywet::coeff(dry_buf, i, dry_const);
        float wet_m = drywet::coeff(wet_buf, i, wet_const);

        for (std::size_t ch = 0; ch < 2; ++ch) {
            float x = (ch == 0) ? input[i] : (stereo_in ? input_r[i] : input[i]);

            state.os_delay[ch][state.os_idx[ch]] = x;
            float x0 = x;
            float x1 = (x + state.os_delay[ch][(state.os_idx[ch] + 3) & 3]) * 0.5f;

            auto excite_core = [amt, coeff, harmonic_odd, harmonic_even, &state, ch](float s) {
                float hp = s - state.hp_z1[ch];
                state.hp_z1[ch] = state.hp_z1[ch] + hp * (1.0f - coeff);

                float odd = hp * hp * hp;
                float even = hp * std::abs(hp);

                float harmonics = odd * harmonic_odd + even * harmonic_even;
                return s + harmonics * amt * 1.5f;
            };

            float y0 = excite_core(x0);
            float y1 = excite_core(x1);
            float processed = std::clamp((y0 + y1) * 0.5f, -1.0f, 1.0f);
            (ch == 0 ? out_l : out_r)[i] = drywet::mix(x, processed, dry_m, wet_m);
            state.os_idx[ch] = (state.os_idx[ch] + 1) & 3;
        }
    }
}

}  // namespace cedar
