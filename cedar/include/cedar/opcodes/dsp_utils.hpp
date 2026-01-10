#pragma once

#include "../dsp/constants.hpp"
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace cedar {

// ============================================================================
// Gain Utilities
// ============================================================================

// Convert decibels to linear gain
[[gnu::always_inline]]
inline float db_to_linear(float db) {
    return std::pow(10.0f, db * 0.05f);
}

// Convert linear gain to decibels
[[gnu::always_inline]]
inline float linear_to_db(float linear) {
    return 20.0f * std::log10(std::max(linear, 1e-10f));
}

// ============================================================================
// Delay Line Utilities
// ============================================================================

// Read from delay line with linear interpolation
// buffer: circular buffer
// size: buffer size
// write_pos: current write position
// delay_samples: fractional delay in samples
[[gnu::always_inline]]
inline float delay_read_linear(const float* buffer, std::size_t size,
                               std::size_t write_pos, float delay_samples) {
    delay_samples = std::clamp(delay_samples, 0.0f, static_cast<float>(size - 1));

    // Calculate read positions
    float read_pos_f = static_cast<float>(write_pos) - delay_samples;
    if (read_pos_f < 0.0f) {
        read_pos_f += static_cast<float>(size);
    }

    std::size_t pos0 = static_cast<std::size_t>(read_pos_f);
    std::size_t pos1 = (pos0 + 1) % size;
    float frac = read_pos_f - static_cast<float>(pos0);

    // Linear interpolation
    return buffer[pos0] * (1.0f - frac) + buffer[pos1] * frac;
}

// Write to delay line and advance position
[[gnu::always_inline]]
inline void delay_write(float* buffer, std::size_t size, std::size_t& write_pos, float sample) {
    buffer[write_pos] = sample;
    write_pos = (write_pos + 1) % size;
}

// ============================================================================
// Allpass Filter (Schroeder allpass)
// ============================================================================

// Process single sample through allpass filter
// y[n] = -g * x[n] + x[n-d] + g * y[n-d]
// Simplified: y[n] = x[n-d] + g * (x[n] - y[n-d])
[[gnu::always_inline]]
inline float allpass_process(float* buffer, std::size_t size, std::size_t& write_pos,
                             float input, float gain) {
    // Read delayed sample
    float delayed = buffer[write_pos];

    // Allpass calculation
    float output = delayed - gain * input;
    float feedback = input + gain * output;

    // Write to buffer
    buffer[write_pos] = feedback;
    write_pos = (write_pos + 1) % size;

    return output;
}

// ============================================================================
// Comb Filters
// ============================================================================

// Feedback comb filter
// y[n] = x[n] + g * y[n-d]
[[gnu::always_inline]]
inline float comb_fb_process(float* buffer, std::size_t size, std::size_t& write_pos,
                             float input, float feedback) {
    // Read delayed sample
    float delayed = buffer[write_pos];

    // Feedback comb
    float output = delayed;
    buffer[write_pos] = input + feedback * delayed;
    write_pos = (write_pos + 1) % size;

    return output;
}

// Feedback comb filter with lowpass damping (Freeverb-style)
// Includes a one-pole lowpass in the feedback path
[[gnu::always_inline]]
inline float comb_lp_process(float* buffer, std::size_t size, std::size_t& write_pos,
                             float input, float feedback, float damp, float& filter_state) {
    // Read delayed sample
    float delayed = buffer[write_pos];

    // One-pole lowpass on delayed signal (damping)
    filter_state = delayed * (1.0f - damp) + filter_state * damp;

    // Feedback comb with damped signal
    float output = delayed;
    buffer[write_pos] = input + feedback * filter_state;
    write_pos = (write_pos + 1) % size;

    return output;
}

// ============================================================================
// Envelope Follower
// ============================================================================

// Attack/release envelope follower for dynamics processing
// Returns smoothed amplitude envelope
[[gnu::always_inline]]
inline float env_follower(float& envelope, float input, float attack_coeff, float release_coeff) {
    float abs_input = std::abs(input);

    if (abs_input > envelope) {
        // Attack phase
        envelope += attack_coeff * (abs_input - envelope);
    } else {
        // Release phase
        envelope += release_coeff * (abs_input - envelope);
    }

    return envelope;
}

// Calculate attack/release coefficients from time in seconds
[[gnu::always_inline]]
inline float time_to_coeff(float time_seconds, float sample_rate) {
    if (time_seconds <= 0.0f) return 1.0f;
    // Coefficient for exponential decay reaching ~63% in given time
    return 1.0f - std::exp(-1.0f / (time_seconds * sample_rate));
}

// ============================================================================
// Fast Math Approximations
// ============================================================================

// Fast tanh approximation (Pade approximant)
// Already exists as soft_clip in filters.hpp, but provide here for reference
[[gnu::always_inline]]
inline float fast_tanh(float x) {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// Fast sine approximation using parabolic curve
// Input: -1 to 1 (representing -PI to PI)
[[gnu::always_inline]]
inline float fast_sin(float x) {
    // Wrap to -1..1
    x = x - static_cast<int>(x);
    if (x < -0.5f) x += 1.0f;
    else if (x > 0.5f) x -= 1.0f;

    // Parabolic approximation
    float y = 4.0f * x * (1.0f - std::abs(x));
    // Refinement for better accuracy
    return y * (0.775f + 0.225f * std::abs(y));
}

// ============================================================================
// LFO Shapes (for modulation effects)
// ============================================================================

// Triangle wave from phase (0-1)
[[gnu::always_inline]]
inline float lfo_triangle(float phase) {
    return 4.0f * std::abs(phase - 0.5f) - 1.0f;
}

// Sine wave from phase (0-1)
[[gnu::always_inline]]
inline float lfo_sine(float phase) {
    return std::sin(phase * TWO_PI);
}

}  // namespace cedar
