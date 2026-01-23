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

// ============================================================================
// Diode Ladder Filter (TB-303 Acid)
// ============================================================================

// Diode nonlinearity: hyperbolic sine approximation
// True diode: I = I_s * (exp(V / V_t) - 1), but sinh provides symmetric behavior
[[gnu::always_inline]]
inline float diode_sinh(float x) {
    // Fast sinh approximation for |x| < 4
    // sinh(x) = x + x^3/6 + x^5/120 for small x
    // For larger values, use (exp(x) - exp(-x)) / 2
    if (x > 4.0f) return 27.29f + 0.5f * std::exp(x);  // exp(4)/2 ≈ 27.29
    if (x < -4.0f) return -27.29f - 0.5f * std::exp(-x);

    float x2 = x * x;
    return x * (1.0f + x2 * (0.166667f + x2 * 0.00833333f));
}

// Derivative of sinh for Newton-Raphson
[[gnu::always_inline]]
inline float diode_cosh(float x) {
    // cosh(x) = 1 + x^2/2 + x^4/24
    if (std::abs(x) > 4.0f) return 0.5f * (std::exp(std::abs(x)));

    float x2 = x * x;
    return 1.0f + x2 * (0.5f + x2 * 0.0416667f);
}

// Default constants for diode ladder filter (used when inputs provide 0 or negative values)
// VT: Thermal voltage - affects diode curve sharpness (real diode ~0.026V)
// FB_GAIN: Feedback multiplier to compensate for VT attenuation in feedback path
constexpr float DIODE_VT_DEFAULT = 0.026f;
constexpr float DIODE_FB_GAIN_DEFAULT = 10.0f;

// FILTER_DIODE: ZDF 4-pole diode ladder filter
// in0: input signal
// in1: cutoff frequency (Hz)
// in2: resonance (0.0-4.0, self-oscillates at ~3.5+)
// in3: vt - thermal voltage (affects diode curve sharpness, default 0.026)
// in4: fb_gain - feedback gain multiplier (compensates VT attenuation, default 10.0)
//
// Based on the Roland TB-303 filter topology with diode nonlinearity.
// Uses Newton-Raphson iteration for implicit integration.
// The diode nonlinearity creates the characteristic "squelchy" acid sound.
[[gnu::always_inline]]
inline void op_filter_diode(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* freq = ctx.buffers->get(inst.inputs[1]);
    const float* res = ctx.buffers->get(inst.inputs[2]);
    const float* vt_in = ctx.buffers->get(inst.inputs[3]);
    const float* fb_gain_in = ctx.buffers->get(inst.inputs[4]);
    auto& state = ctx.states->get_or_create<DiodeState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float cutoff = freq[i];
        float resonance = res[i];

        // Runtime tunable parameters (use defaults if zero/negative)
        float vt = vt_in[i] > 0.0f ? vt_in[i] : DIODE_VT_DEFAULT;
        float vt_inv = 1.0f / vt;
        float fb_gain = fb_gain_in[i] > 0.0f ? fb_gain_in[i] : DIODE_FB_GAIN_DEFAULT;

        // Update coefficients if parameters changed
        if (cutoff != state.last_freq || resonance != state.last_res) {
            state.last_freq = cutoff;
            state.last_res = resonance;

            // Frequency warping - clamp to safe range
            float f = std::clamp(cutoff / ctx.sample_rate, 0.0f, 0.45f);

            // g coefficient using tan for frequency warping
            state.g = std::tan(PI * f);

            // Resonance: diode ladder has different feedback topology
            // Range 0-4, self-oscillates around 3.5
            state.k = std::clamp(resonance, 0.0f, 4.0f);
        }

        // Get feedback from output with diode nonlinearity
        // The diode creates exponential response at extremes
        // fb_gain compensates for VT attenuation to enable proper self-oscillation
        float fb_voltage = state.cap[3] * vt_inv;
        float feedback = state.k * diode_sinh(fb_voltage) * vt * fb_gain;

        // Input with feedback
        float x = input[i] - feedback;

        // Apply soft saturation at input (prevents harsh clipping)
        x = std::tanh(x * 0.5f) * 2.0f;

        // Calculate G for trapezoidal integration
        float G = state.g / (1.0f + state.g);

        // Process 4 cascaded stages with diode nonlinearity
        // Each stage: v_out = G * (v_in + v_cap) / (1 + G) using Newton-Raphson
        for (int j = 0; j < 4; ++j) {
            float v_in = (j == 0) ? x : state.cap[j - 1];

            // Diode-coupled stage: the coupling is nonlinear
            // Approximate with one Newton-Raphson iteration
            float v_est = state.cap[j];
            float v_diff = v_in - v_est;

            // Nonlinear transfer through diode
            float diode_v = v_diff * vt_inv;
            float i_diode = diode_sinh(diode_v);
            float di_diode = diode_cosh(diode_v) * vt_inv;

            // Newton-Raphson update: v_new = v_old - f(v)/f'(v)
            // f(v) = v - G * i_diode - (1-G) * v_cap
            float f_v = v_est - G * i_diode * vt - (1.0f - G) * state.cap[j];
            float df_v = 1.0f + G * di_diode * vt;

            float v_new = v_est - f_v / df_v;

            // Clamp to prevent blowup
            state.cap[j] = clamp_audio(v_new);
        }

        // Output is the 4-pole lowpass
        out[i] = state.cap[3];
    }
}

// ============================================================================
// Formant (Vowel) Filter
// ============================================================================

// Vowel formant table (F1, F2, F3 in Hz)
// Based on average male voice formants
struct VowelFormants {
    float f1, f2, f3;
    float g1, g2, g3;  // Relative gains
};

constexpr VowelFormants VOWEL_TABLE[5] = {
    {650.0f, 1100.0f, 2860.0f, 1.0f, 0.5f, 0.25f},   // A (as in "father")
    {300.0f, 2300.0f, 3000.0f, 1.0f, 0.4f, 0.2f},    // I (as in "feet")
    {300.0f, 870.0f, 2240.0f, 1.0f, 0.6f, 0.3f},     // U (as in "boot")
    {400.0f, 2000.0f, 2550.0f, 1.0f, 0.45f, 0.25f},  // E (as in "bed")
    {400.0f, 800.0f, 2600.0f, 1.0f, 0.5f, 0.2f}      // O (as in "bought")
};

// FILTER_FORMANT: 3-band parallel vowel morphing filter
// in0: input signal
// in1: vowel_a (0-4, selects first vowel: A/I/U/E/O)
// in2: vowel_b (0-4, selects second vowel)
// in3: morph (0-1, interpolates between vowel_a and vowel_b)
// in4: q (resonance/bandwidth, 1-20)
//
// Creates vocal-like filter sweeps by morphing between vowel formants.
// Uses 3 parallel Chamberlin SVF bandpass filters.
[[gnu::always_inline]]
inline void op_filter_formant(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* vowel_a = ctx.buffers->get(inst.inputs[1]);
    const float* vowel_b = ctx.buffers->get(inst.inputs[2]);
    const float* morph = ctx.buffers->get(inst.inputs[3]);
    const float* q_in = ctx.buffers->get(inst.inputs[4]);
    auto& state = ctx.states->get_or_create<FormantState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float va = std::clamp(vowel_a[i], 0.0f, 4.0f);
        float vb = std::clamp(vowel_b[i], 0.0f, 4.0f);
        float m = std::clamp(morph[i], 0.0f, 1.0f);
        float q = std::clamp(q_in[i], 1.0f, 20.0f);

        // Check if we need to recalculate formant targets
        if (va != state.last_vowel_a || vb != state.last_vowel_b ||
            m != state.last_morph || q != state.last_q) {

            state.last_vowel_a = va;
            state.last_vowel_b = vb;
            state.last_morph = m;
            state.last_q = q;

            // Interpolate between vowel indices (with fractional support)
            int idx_a = static_cast<int>(va);
            int idx_b = static_cast<int>(vb);
            float frac_a = va - static_cast<float>(idx_a);
            float frac_b = vb - static_cast<float>(idx_b);

            idx_a = std::min(idx_a, 4);
            idx_b = std::min(idx_b, 4);
            int idx_a2 = std::min(idx_a + 1, 4);
            int idx_b2 = std::min(idx_b + 1, 4);

            // Get formants for vowel A (with fractional interpolation)
            float f1_a = VOWEL_TABLE[idx_a].f1 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].f1 * frac_a;
            float f2_a = VOWEL_TABLE[idx_a].f2 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].f2 * frac_a;
            float f3_a = VOWEL_TABLE[idx_a].f3 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].f3 * frac_a;
            float g1_a = VOWEL_TABLE[idx_a].g1 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].g1 * frac_a;
            float g2_a = VOWEL_TABLE[idx_a].g2 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].g2 * frac_a;
            float g3_a = VOWEL_TABLE[idx_a].g3 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].g3 * frac_a;

            // Get formants for vowel B
            float f1_b = VOWEL_TABLE[idx_b].f1 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].f1 * frac_b;
            float f2_b = VOWEL_TABLE[idx_b].f2 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].f2 * frac_b;
            float f3_b = VOWEL_TABLE[idx_b].f3 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].f3 * frac_b;
            float g1_b = VOWEL_TABLE[idx_b].g1 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].g1 * frac_b;
            float g2_b = VOWEL_TABLE[idx_b].g2 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].g2 * frac_b;
            float g3_b = VOWEL_TABLE[idx_b].g3 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].g3 * frac_b;

            // Morph between A and B
            state.f1 = f1_a * (1.0f - m) + f1_b * m;
            state.f2 = f2_a * (1.0f - m) + f2_b * m;
            state.f3 = f3_a * (1.0f - m) + f3_b * m;
            state.g1 = g1_a * (1.0f - m) + g1_b * m;
            state.g2 = g2_a * (1.0f - m) + g2_b * m;
            state.g3 = g3_a * (1.0f - m) + g3_b * m;
        }

        float x = input[i];

        // Chamberlin SVF coefficients
        // f_coef = 2 * sin(pi * freq / sample_rate)
        float f1_coef = 2.0f * std::sin(PI * state.f1 / ctx.sample_rate);
        float f2_coef = 2.0f * std::sin(PI * state.f2 / ctx.sample_rate);
        float f3_coef = 2.0f * std::sin(PI * state.f3 / ctx.sample_rate);
        float q_coef = 1.0f / q;

        // Bandpass 1 (F1 - first formant)
        float hp1 = x - state.bp1_z1 * q_coef - state.bp1_z2;
        float bp1 = state.bp1_z1 + f1_coef * hp1;
        float lp1 = state.bp1_z2 + f1_coef * state.bp1_z1;
        state.bp1_z1 = clamp_audio(bp1);
        state.bp1_z2 = clamp_audio(lp1);

        // Bandpass 2 (F2 - second formant)
        float hp2 = x - state.bp2_z1 * q_coef - state.bp2_z2;
        float bp2 = state.bp2_z1 + f2_coef * hp2;
        float lp2 = state.bp2_z2 + f2_coef * state.bp2_z1;
        state.bp2_z1 = clamp_audio(bp2);
        state.bp2_z2 = clamp_audio(lp2);

        // Bandpass 3 (F3 - third formant)
        float hp3 = x - state.bp3_z1 * q_coef - state.bp3_z2;
        float bp3 = state.bp3_z1 + f3_coef * hp3;
        float lp3 = state.bp3_z2 + f3_coef * state.bp3_z1;
        state.bp3_z1 = clamp_audio(bp3);
        state.bp3_z2 = clamp_audio(lp3);

        // Sum bandpasses with formant gains
        out[i] = bp1 * state.g1 + bp2 * state.g2 + bp3 * state.g3;
    }
}

// ============================================================================
// Sallen-Key Filter (MS-20 Style)
// ============================================================================

// Diode clipper function for feedback path
[[gnu::always_inline]]
inline float diode_clip(float x, float& state) {
    // Asymmetric soft diode clipping
    // Positive: gentle soft knee
    // Negative: sharper knee (like silicon diode)
    constexpr float THRESHOLD = 0.7f;

    float clipped;
    if (x > THRESHOLD) {
        clipped = THRESHOLD + std::tanh((x - THRESHOLD) * 2.0f) * 0.3f;
    } else if (x < -THRESHOLD) {
        // Sharper negative clipping
        clipped = -THRESHOLD + std::tanh((x + THRESHOLD) * 3.0f) * 0.2f;
    } else {
        clipped = x;
    }

    // Slight hysteresis for character
    state = state * 0.1f + clipped * 0.9f;
    return state;
}

// FILTER_SALLENKEY: MS-20 style 12dB/oct filter with diode feedback
// in0: input signal
// in1: cutoff frequency (Hz)
// in2: resonance (0.0-4.0, aggressive self-oscillation)
// in3: mode (0.0 = lowpass, 1.0 = highpass)
//
// Based on the Korg MS-20 filter topology with diode clipping in the
// feedback path. Creates aggressive, fuzzy resonance character.
[[gnu::always_inline]]
inline void op_filter_sallenkey(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* freq = ctx.buffers->get(inst.inputs[1]);
    const float* res = ctx.buffers->get(inst.inputs[2]);
    const float* mode_in = ctx.buffers->get(inst.inputs[3]);
    auto& state = ctx.states->get_or_create<SallenkeyState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float cutoff = freq[i];
        float resonance = res[i];
        float mode = mode_in[i];

        // Update coefficients if needed
        if (cutoff != state.last_freq || resonance != state.last_res) {
            state.last_freq = cutoff;
            state.last_res = resonance;

            // Frequency warping
            float f = std::clamp(cutoff / ctx.sample_rate, 0.0f, 0.45f);
            state.g = std::tan(PI * f);

            // Resonance - MS-20 has very aggressive feedback
            state.k = std::clamp(resonance, 0.0f, 4.0f);
        }

        // Get feedback with diode clipping (the MS-20 "scream")
        float fb = state.cap2 * state.k;
        fb = diode_clip(fb, state.diode_state);

        // Input with feedback
        float x = input[i] - fb;

        // Sallen-Key topology: 2-pole filter
        // Using trapezoidal integration for stability
        float G = state.g / (1.0f + state.g);

        // First stage
        float v1 = G * (x - state.cap1) + state.cap1;

        // Second stage with resonance boost
        float v2 = G * (v1 - state.cap2) + state.cap2;

        // Update capacitor states
        state.cap1 = clamp_audio(2.0f * v1 - state.cap1);
        state.cap2 = clamp_audio(2.0f * v2 - state.cap2);

        // Mode selection: 0 = lowpass, 1 = highpass
        float lp = v2;
        float hp = x - v1 * (1.0f + state.k * 0.5f) - v2;

        // Crossfade between LP and HP based on mode
        float m = std::clamp(mode, 0.0f, 1.0f);
        out[i] = lp * (1.0f - m) + hp * m;
    }
}

}  // namespace cedar
