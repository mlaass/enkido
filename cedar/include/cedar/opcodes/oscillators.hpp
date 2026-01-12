#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
#include "minblep.hpp"
#include <cmath>
#include <algorithm>

namespace cedar {

// ============================================================================
// PolyBLEP Anti-Aliasing Functions
// ============================================================================
// PolyBLEP (Polynomial Band-Limited Step) reduces aliasing by applying
// polynomial correction near waveform discontinuities.

// PolyBLEP residual function
// t: current phase (0 to 1)
// dt: phase increment (normalized frequency)
// Returns correction value to subtract from naive waveform
[[gnu::always_inline]]
inline float poly_blep(float t, float dt) {
    // dt should be positive and less than 0.5
    dt = std::abs(dt);
    if (dt < 1e-8f) return 0.0f;  // Avoid division by zero

    if (t < dt) {
        // Just after discontinuity (phase near 0)
        t /= dt;
        return t + t - t * t - 1.0f;
    } else if (t > 1.0f - dt) {
        // Just before discontinuity (phase near 1)
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

// Symmetric PolyBLEP using signed distance to discontinuity
// This ensures identical treatment of rising and falling edges
// distance: signed distance to discontinuity (negative = before, positive = after)
// dt: phase increment (normalized frequency)
[[gnu::always_inline]]
inline float poly_blep_distance(float distance, float dt) {
    if (dt < 1e-8f) return 0.0f;

    if (distance >= 0.0f && distance < dt) {
        // Just after discontinuity
        float t = distance / dt;  // [0, 1)
        return t + t - t * t - 1.0f;
    } else if (distance < 0.0f && distance > -dt) {
        // Just before discontinuity
        float t = distance / dt;  // (-1, 0]
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

// PolyBLAMP (Polynomial Band-Limited Ramp) for triangle waves
// Integrated version of PolyBLEP for ramp discontinuities (slope changes)
[[gnu::always_inline]]
inline float poly_blamp(float t, float dt) {
    dt = std::abs(dt);
    if (dt < 1e-8f) return 0.0f;

    if (t < dt) {
        t = t / dt - 1.0f;
        return -1.0f / 3.0f * t * t * t;
    } else if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt + 1.0f;
        return 1.0f / 3.0f * t * t * t;
    }
    return 0.0f;
}

// SIN oscillator: out = sin(phase * 2pi), frequency from in0
// Note: Sine has no discontinuities so no anti-aliasing needed
[[gnu::always_inline]]
inline void op_osc_sin(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = std::sin(state.phase * TWO_PI);

        // Advance phase
        state.prev_phase = state.phase;
        state.phase += freq[i] * ctx.inv_sample_rate;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
        } else if (state.phase < 0.0f) {
            state.phase += 1.0f;
        }
    }
}

// TRI oscillator: triangle wave with PolyBLAMP anti-aliasing
// Output: -1 to +1, linear rise then fall
[[gnu::always_inline]]
inline void op_osc_tri(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float dt = freq[i] * ctx.inv_sample_rate;

        // Naive triangle: 4 * |phase - 0.5| - 1
        // At phase 0: 4*0.5-1 = 1 (peak)
        // At phase 0.25: 4*0.25-1 = 0
        // At phase 0.5: 4*0-1 = -1 (trough)
        // At phase 0.75: 4*0.25-1 = 0
        float value = 4.0f * std::abs(state.phase - 0.5f) - 1.0f;

        // Apply PolyBLAMP correction at slope discontinuities (skip on first sample)
        if (state.initialized) {
            // Corner at phase = 0 (slope changes from -4 to +4)
            float blamp = poly_blamp(state.phase, dt);

            // Corner at phase = 0.5 (slope changes from +4 to -4)
            float phase_half = state.phase + 0.5f;
            if (phase_half >= 1.0f) phase_half -= 1.0f;
            blamp -= poly_blamp(phase_half, dt);

            // Scale by 4*dt (slope magnitude * phase increment)
            // The factor of 4 comes from the triangle slope magnitude
            value += 4.0f * dt * blamp;
        }

        out[i] = value;

        // Advance phase
        state.prev_phase = state.phase;
        state.phase += dt;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
        } else if (state.phase < 0.0f) {
            state.phase += 1.0f;
        }
        state.initialized = true;
    }
}

// SAW oscillator: sawtooth wave with PolyBLEP anti-aliasing
// Output: -1 to +1, linear ramp up then instant reset
[[gnu::always_inline]]
inline void op_osc_saw(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float dt = freq[i] * ctx.inv_sample_rate;

        // Naive sawtooth: 2 * phase - 1
        float value = 2.0f * state.phase - 1.0f;

        // Apply PolyBLEP correction at the falling edge (skip on first sample)
        if (state.initialized) {
            value -= poly_blep(state.phase, dt);
        }

        out[i] = value;

        // Advance phase
        state.prev_phase = state.phase;
        state.phase += dt;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
        } else if (state.phase < 0.0f) {
            state.phase += 1.0f;
        }
        state.initialized = true;
    }
}

// SQR oscillator: square wave with PolyBLEP anti-aliasing
// Output: +1 for first half of cycle, -1 for second half
[[gnu::always_inline]]
inline void op_osc_sqr(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float dt = freq[i] * ctx.inv_sample_rate;

        // Naive square: +1 if phase < 0.5, else -1
        float value = (state.phase < 0.5f) ? 1.0f : -1.0f;

        // Apply PolyBLEP correction at both edges (skip on first sample)
        if (state.initialized) {
            // Rising edge at phase = 0 (transition from -1 to +1)
            value += poly_blep(state.phase, dt);

            // Falling edge at phase = 0.5 (transition from +1 to -1)
            float t = state.phase + 0.5f;
            if (t >= 1.0f) t -= 1.0f;
            value -= poly_blep(t, dt);
        }

        out[i] = value;

        // Advance phase
        state.prev_phase = state.phase;
        state.phase += dt;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
        } else if (state.phase < 0.0f) {
            state.phase += 1.0f;
        }
        state.initialized = true;
    }
}

// RAMP oscillator: inverted sawtooth (descending ramp) with PolyBLEP
// Output: +1 to -1, linear ramp down then instant reset
[[gnu::always_inline]]
inline void op_osc_ramp(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float dt = freq[i] * ctx.inv_sample_rate;

        // Naive ramp (inverted saw): 1 - 2 * phase
        float value = 1.0f - 2.0f * state.phase;

        // Apply PolyBLEP correction at the rising edge (skip on first sample)
        if (state.initialized) {
            value += poly_blep(state.phase, dt);
        }

        out[i] = value;

        // Advance phase
        state.prev_phase = state.phase;
        state.phase += dt;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
        } else if (state.phase < 0.0f) {
            state.phase += 1.0f;
        }
        state.initialized = true;
    }
}

// PHASOR: raw phase output (0 to 1)
// Useful as modulation source or for custom waveshaping
// Note: Discontinuity at phase wrap is intentional for phasor use
[[gnu::always_inline]]
inline void op_osc_phasor(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = state.phase;

        state.prev_phase = state.phase;
        state.phase += freq[i] * ctx.inv_sample_rate;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
        } else if (state.phase < 0.0f) {
            state.phase += 1.0f;
        }
    }
}

// ============================================================================
// MinBLEP Oscillators - Perfect harmonic purity for PWM and distortion
// ============================================================================

// SQR_MINBLEP oscillator: square wave with MinBLEP anti-aliasing
// Perfect harmonic purity - no even harmonics, ideal for PWM and distortion
[[gnu::always_inline]]
inline void op_osc_sqr_minblep(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    auto& state = ctx.states->get_or_create<MinBLEPOscState>(inst.state_id);

    const auto& minblep_table = get_minblep_table();

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float dt = freq[i] * ctx.inv_sample_rate;

        // Naive square wave based on current phase (PRE-advance value)
        float naive_value = (state.phase < 0.5f) ? 1.0f : -1.0f;

        // Calculate next phase
        float next_phase = state.phase + dt;

        // Check for discontinuities and add residual corrections
        if (state.initialized) {
            // Rising edge at phase = 0 (wrapping from 1.0 to 0.0)
            // Step from -1 to +1, amplitude = 2
            if (next_phase >= 1.0f) {
                state.add_step(2.0f, 0.0f, minblep_table.data(), MINBLEP_PHASES, MINBLEP_SAMPLES);
                naive_value = 1.0f;  // Switch to post-transition value
            }

            // Falling edge at phase = 0.5
            // Step from +1 to -1, amplitude = -2
            if (state.phase < 0.5f && next_phase >= 0.5f) {
                state.add_step(-2.0f, 0.0f, minblep_table.data(), MINBLEP_PHASES, MINBLEP_SAMPLES);
                naive_value = -1.0f;  // Switch to post-transition value
            }
        }

        // Output = naive + residual correction
        out[i] = naive_value + state.get_and_advance();

        // Advance phase
        state.phase = next_phase;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
        }

        state.initialized = true;
    }
}

}  // namespace cedar
