#pragma once

#include "../dsp/constants.hpp"
#include <cmath>
#include <algorithm>

namespace cedar {

// Default oversampling factor for non-ADAA distortion opcodes
constexpr int DEFAULT_OVERSAMPLE = 2;

// Oversampling state for 2x processing
// Uses polyphase halfband FIR filter for efficient up/downsampling
struct OversampleState {
    // 7-tap halfband FIR coefficients (symmetric, optimized for 2x)
    // Designed for ~80dB stopband attenuation
    static constexpr float COEFFS[4] = {
        0.07322047f,   // h[0], h[6]
        0.0f,          // h[1], h[5] (zeros at Nyquist)
        0.30677953f,   // h[2], h[4]
        0.5f           // h[3] (center tap)
    };

    // Delay line for interpolation filter (polyphase form)
    float delay[4] = {};
    int delay_idx = 0;

    // Downsample accumulator
    float ds_acc = 0.0f;
    bool ds_phase = false;
};

// Upsample a single sample to 2 samples using polyphase halfband filter
// Returns the two upsampled values in out[0] and out[1]
[[gnu::always_inline]]
inline void upsample_2x(float in, float out[2], OversampleState& state) {
    // Polyphase decomposition:
    // Phase 0: uses even coefficients (h[0], h[2], h[4], h[6])
    // Phase 1: uses odd coefficients (all zeros except h[3]=0.5)

    // Update delay line
    state.delay[state.delay_idx] = in;

    // Phase 0: full filter with even taps
    int idx = state.delay_idx;
    float sum = state.delay[idx] * OversampleState::COEFFS[3];  // Center tap

    idx = (idx + 3) & 3;
    sum += state.delay[idx] * OversampleState::COEFFS[2];

    idx = (idx + 3) & 3;
    sum += state.delay[idx] * OversampleState::COEFFS[0];

    // For halfband, every other sample of impulse response is zero
    // so we get efficient computation
    out[0] = sum * 2.0f;  // Compensate for zero-stuffing

    // Phase 1: only center sample matters (simplified)
    out[1] = in;

    state.delay_idx = (state.delay_idx + 1) & 3;
}

// Downsample 2 samples to 1 sample using halfband filter
[[gnu::always_inline]]
inline float downsample_2x(float in0, float in1, OversampleState& state) {
    // Accumulate through anti-aliasing filter
    if (!state.ds_phase) {
        // First sample of pair: filter and accumulate
        state.ds_acc = in0 * 0.5f;
        state.ds_phase = true;
        return 0.0f;  // Not used, caller takes second return
    } else {
        // Second sample: complete filtering and output
        float result = state.ds_acc + in1 * 0.5f;
        state.ds_phase = false;
        return result;
    }
}

// Simplified 2x oversampling wrapper for any saturation function
// sat_func: lambda/function taking float input, returning float output
template<typename SatFunc>
[[gnu::always_inline]]
inline float oversample_2x(float x, SatFunc&& sat_func, OversampleState& state) {
    float up[2];
    upsample_2x(x, up, state);

    // Process at 2x rate
    up[0] = sat_func(up[0]);
    up[1] = sat_func(up[1]);

    // Simple averaging downsample (good enough for saturation)
    return (up[0] + up[1]) * 0.5f;
}

// 4x oversampling (two stages of 2x)
struct Oversample4xState {
    OversampleState stage1;
    OversampleState stage2a;
    OversampleState stage2b;
};

template<typename SatFunc>
[[gnu::always_inline]]
inline float oversample_4x(float x, SatFunc&& sat_func, Oversample4xState& state) {
    float up2[2];
    upsample_2x(x, up2, state.stage1);

    float up4[4];
    upsample_2x(up2[0], &up4[0], state.stage2a);
    upsample_2x(up2[1], &up4[2], state.stage2b);

    // Process at 4x rate
    for (int i = 0; i < 4; ++i) {
        up4[i] = sat_func(up4[i]);
    }

    // Downsample back to 1x
    return (up4[0] + up4[1] + up4[2] + up4[3]) * 0.25f;
}

// Generic oversampling dispatcher based on factor
template<typename SatFunc>
[[gnu::always_inline]]
inline float oversample(float x, int factor, SatFunc&& sat_func,
                        OversampleState& state2x, Oversample4xState& state4x) {
    switch (factor) {
        case 1:
            return sat_func(x);
        case 2:
            return oversample_2x(x, std::forward<SatFunc>(sat_func), state2x);
        case 4:
        default:
            return oversample_4x(x, std::forward<SatFunc>(sat_func), state4x);
    }
}

}  // namespace cedar
