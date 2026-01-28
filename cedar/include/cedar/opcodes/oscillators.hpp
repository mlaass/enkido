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

// ============================================================================
// Phase Reset Helper for Trigger-based Phase Control
// ============================================================================

// Get buffer pointer, falling back to BUFFER_ZERO for unused inputs
[[gnu::always_inline]]
inline const float* get_input_or_zero(ExecutionContext& ctx, std::uint16_t buffer_id) {
    if (buffer_id == BUFFER_UNUSED) {
        return ctx.buffers->get(BUFFER_ZERO);
    }
    return ctx.buffers->get(buffer_id);
}

// Check for rising edge trigger and reset phase to offset if triggered
// Returns true if phase was reset (useful for MinBLEP buffer clearing)
[[gnu::always_inline]]
inline bool check_phase_reset(float& phase, float& prev_trigger, bool& initialized,
                               float trigger, float phase_offset) {
    // Detect rising edge (previous <= 0, current > 0)
    bool triggered = (trigger > 0.0f && prev_trigger <= 0.0f);
    prev_trigger = trigger;

    if (triggered) {
        // Normalize phase offset to [0, 1)
        phase = std::fmod(phase_offset, 1.0f);
        if (phase < 0.0f) phase += 1.0f;
        // Reset initialized to avoid PolyBLEP artifacts at reset point
        initialized = false;
        return true;
    }
    return false;
}

// SIN oscillator: out = sin(phase * 2pi), frequency from in0
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Note: Sine has no discontinuities so no anti-aliasing needed
[[gnu::always_inline]]
inline void op_osc_sin(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.phase, state.prev_trigger, state.initialized,
                          trigger[i], phase_offset[i]);

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
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Output: -1 to +1, linear rise then fall
[[gnu::always_inline]]
inline void op_osc_tri(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.phase, state.prev_trigger, state.initialized,
                          trigger[i], phase_offset[i]);

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
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Output: -1 to +1, linear ramp up then instant reset
[[gnu::always_inline]]
inline void op_osc_saw(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.phase, state.prev_trigger, state.initialized,
                          trigger[i], phase_offset[i]);

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
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Output: +1 for first half of cycle, -1 for second half
[[gnu::always_inline]]
inline void op_osc_sqr(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.phase, state.prev_trigger, state.initialized,
                          trigger[i], phase_offset[i]);

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
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Output: +1 to -1, linear ramp down then instant reset
[[gnu::always_inline]]
inline void op_osc_ramp(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.phase, state.prev_trigger, state.initialized,
                          trigger[i], phase_offset[i]);

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
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Useful as modulation source or for custom waveshaping
// Note: Discontinuity at phase wrap is intentional for phasor use
[[gnu::always_inline]]
inline void op_osc_phasor(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.phase, state.prev_trigger, state.initialized,
                          trigger[i], phase_offset[i]);

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
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Perfect harmonic purity - no even harmonics, ideal for PWM and distortion
[[gnu::always_inline]]
inline void op_osc_sqr_minblep(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<MinBLEPOscState>(inst.state_id);

    const auto& minblep_table = get_minblep_table();

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        if (trigger[i] > 0.0f && state.prev_trigger <= 0.0f) {
            // Reset phase to offset
            state.phase = std::fmod(phase_offset[i], 1.0f);
            if (state.phase < 0.0f) state.phase += 1.0f;
            state.initialized = false;
            // Clear MinBLEP buffer on reset to avoid artifacts
            state.reset();
        }
        state.prev_trigger = trigger[i];

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

// ============================================================================
// PWM Oscillators - Pulse Width Modulation
// ============================================================================

// SQR_PWM oscillator: square wave with variable pulse width
// in0: frequency (Hz)
// in1: PWM (-1 to +1, where 0 = 50% duty cycle)
// in2: phase offset (0-1, optional)
// in3: trigger (reset phase on rising edge, optional)
// PWM mapping: duty = 0.5 + pwm * 0.5, so [-1,+1] maps to [0,1]
[[gnu::always_inline]]
inline void op_osc_sqr_pwm(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* pwm = ctx.buffers->get(inst.inputs[1]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[2]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[3]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.phase, state.prev_trigger, state.initialized,
                          trigger[i], phase_offset[i]);

        float dt = freq[i] * ctx.inv_sample_rate;

        // Calculate duty cycle from PWM input
        float duty = 0.5f + std::clamp(pwm[i], -1.0f, 1.0f) * 0.5f;
        duty = std::clamp(duty, 0.001f, 0.999f);  // Prevent degenerate cases

        // Naive square: +1 if phase < duty, else -1
        float value = (state.phase < duty) ? 1.0f : -1.0f;

        if (state.initialized) {
            // Rising edge at phase = 0 (fixed position)
            value += poly_blep(state.phase, dt);

            // Falling edge at phase = duty (variable position)
            // Calculate signed distance to falling edge
            float dist_to_fall = state.phase - duty;
            if (dist_to_fall > 0.5f) dist_to_fall -= 1.0f;
            if (dist_to_fall < -0.5f) dist_to_fall += 1.0f;
            value -= poly_blep_distance(dist_to_fall, dt);
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

// SAW_PWM oscillator: variable-slope sawtooth (morphs saw to tri to ramp)
// in0: frequency (Hz)
// in1: PWM (-1 to +1)
// in2: phase offset (0-1, optional)
// in3: trigger (reset phase on rising edge, optional)
// PWM = -1: Rising ramp (standard saw)
// PWM = 0: Triangle
// PWM = +1: Falling ramp (inverted saw)
[[gnu::always_inline]]
inline void op_osc_saw_pwm(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* pwm = ctx.buffers->get(inst.inputs[1]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[2]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[3]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.phase, state.prev_trigger, state.initialized,
                          trigger[i], phase_offset[i]);

        float dt = freq[i] * ctx.inv_sample_rate;

        // Map PWM to midpoint position: [-1,+1] -> [0.01, 0.99]
        float mid = (1.0f + std::clamp(pwm[i], -1.0f, 1.0f)) * 0.5f;
        mid = std::clamp(mid, 0.01f, 0.99f);

        // Piecewise linear waveform:
        // Phase [0, mid]: rise from -1 to +1
        // Phase [mid, 1]: fall from +1 to -1
        float value;
        if (state.phase < mid) {
            value = 2.0f * state.phase / mid - 1.0f;
        } else {
            value = 1.0f - 2.0f * (state.phase - mid) / (1.0f - mid);
        }

        if (state.initialized) {
            // PolyBLAMP at slope discontinuities
            // Corner at phase = 0 (slope change from falling to rising)
            float blamp = poly_blamp(state.phase, dt);

            // Corner at phase = mid (slope change from rising to falling)
            float phase_at_mid = state.phase - mid;
            if (phase_at_mid < 0.0f) phase_at_mid += 1.0f;
            blamp -= poly_blamp(phase_at_mid, dt);

            // Scale by slope difference
            float slope_rise = 2.0f / mid;
            float slope_fall = -2.0f / (1.0f - mid);
            float slope_diff = slope_rise - slope_fall;  // Total slope change at corners

            value += slope_diff * dt * blamp;
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

// SQR_PWM_MINBLEP oscillator: highest quality PWM square wave
// in0: frequency (Hz)
// in1: PWM (-1 to +1, where 0 = 50% duty cycle)
// in2: phase offset (0-1, optional)
// in3: trigger (reset phase on rising edge, optional)
// Uses MinBLEP for sub-sample accurate edge placement
[[gnu::always_inline]]
inline void op_osc_sqr_pwm_minblep(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* pwm = ctx.buffers->get(inst.inputs[1]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[2]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[3]);
    auto& state = ctx.states->get_or_create<MinBLEPOscState>(inst.state_id);

    const auto& minblep_table = get_minblep_table();

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        if (trigger[i] > 0.0f && state.prev_trigger <= 0.0f) {
            // Reset phase to offset
            state.phase = std::fmod(phase_offset[i], 1.0f);
            if (state.phase < 0.0f) state.phase += 1.0f;
            state.initialized = false;
            // Clear MinBLEP buffer on reset to avoid artifacts
            state.reset();
        }
        state.prev_trigger = trigger[i];

        float dt = freq[i] * ctx.inv_sample_rate;

        // Calculate duty cycle from PWM
        float duty = 0.5f + std::clamp(pwm[i], -1.0f, 1.0f) * 0.5f;
        duty = std::clamp(duty, 0.001f, 0.999f);

        float naive_value = (state.phase < duty) ? 1.0f : -1.0f;
        float next_phase = state.phase + dt;

        if (state.initialized) {
            // Rising edge at phase wrap (0 crossing)
            if (next_phase >= 1.0f) {
                float frac = (dt > 1e-8f) ? (next_phase - 1.0f) / dt : 0.0f;
                state.add_step(2.0f, frac, minblep_table.data(), MINBLEP_PHASES, MINBLEP_SAMPLES);
                naive_value = 1.0f;
            }

            // Falling edge at duty crossing
            if (state.phase < duty && next_phase >= duty) {
                float frac = (dt > 1e-8f) ? (next_phase - duty) / dt : 0.0f;
                state.add_step(-2.0f, frac, minblep_table.data(), MINBLEP_PHASES, MINBLEP_SAMPLES);
                naive_value = -1.0f;
            }
        }

        out[i] = naive_value + state.get_and_advance();

        state.phase = next_phase;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
        }
        state.initialized = true;
    }
}

// ============================================================================
// Oversampled Oscillators - For alias-free FM synthesis
// ============================================================================

// SIN_2X: 2x oversampled sine oscillator
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Interpolates frequency input for true sample-accurate FM
[[gnu::always_inline]]
inline void op_osc_sin_2x(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<OscState2x>(inst.state_id);

    float inv_sr_2x = ctx.inv_sample_rate * 0.5f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.osc.phase, state.osc.prev_trigger, state.osc.initialized,
                          trigger[i], phase_offset[i]);

        // Interpolate frequency between this sample and next
        float freq_curr = freq[i];
        float freq_next = (i + 1 < BLOCK_SIZE) ? freq[i + 1] : freq[i];

        float samples[2];

        // Generate 2 samples at 2x rate with interpolated frequency
        for (int j = 0; j < 2; ++j) {
            // Linear interpolation: j=0 -> freq_curr, j=1 -> midpoint to next
            float t = static_cast<float>(j) * 0.5f;
            float freq_interp = freq_curr + t * (freq_next - freq_curr);
            float dt = freq_interp * inv_sr_2x;

            samples[j] = std::sin(state.osc.phase * TWO_PI);
            state.osc.phase += dt;
            if (state.osc.phase >= 1.0f) state.osc.phase -= 1.0f;
            else if (state.osc.phase < 0.0f) state.osc.phase += 1.0f;
        }

        out[i] = state.downsample(samples[0], samples[1]);
    }
}

// SIN_4X: 4x oversampled sine oscillator
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Interpolates frequency input for true sample-accurate FM
[[gnu::always_inline]]
inline void op_osc_sin_4x(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<OscState4x>(inst.state_id);

    float inv_sr_4x = ctx.inv_sample_rate * 0.25f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.osc.phase, state.osc.prev_trigger, state.osc.initialized,
                          trigger[i], phase_offset[i]);

        // Interpolate frequency between this sample and next
        float freq_curr = freq[i];
        float freq_next = (i + 1 < BLOCK_SIZE) ? freq[i + 1] : freq[i];

        float samples[4];

        // Generate 4 samples at 4x rate with interpolated frequency
        for (int j = 0; j < 4; ++j) {
            // Linear interpolation across the 4 sub-samples
            float t = static_cast<float>(j) * 0.25f;
            float freq_interp = freq_curr + t * (freq_next - freq_curr);
            float dt = freq_interp * inv_sr_4x;

            samples[j] = std::sin(state.osc.phase * TWO_PI);
            state.osc.phase += dt;
            if (state.osc.phase >= 1.0f) state.osc.phase -= 1.0f;
            else if (state.osc.phase < 0.0f) state.osc.phase += 1.0f;
        }

        out[i] = state.downsample(samples[0], samples[1], samples[2], samples[3]);
    }
}

// SAW_2X: 2x oversampled sawtooth with PolyBLEP at higher rate
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Interpolates frequency input for true sample-accurate FM
[[gnu::always_inline]]
inline void op_osc_saw_2x(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<OscState2x>(inst.state_id);

    float inv_sr_2x = ctx.inv_sample_rate * 0.5f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.osc.phase, state.osc.prev_trigger, state.osc.initialized,
                          trigger[i], phase_offset[i]);

        float freq_curr = freq[i];
        float freq_next = (i + 1 < BLOCK_SIZE) ? freq[i + 1] : freq[i];

        float samples[2];

        for (int j = 0; j < 2; ++j) {
            float t = static_cast<float>(j) * 0.5f;
            float freq_interp = freq_curr + t * (freq_next - freq_curr);
            float dt = freq_interp * inv_sr_2x;

            float value = 2.0f * state.osc.phase - 1.0f;

            if (state.osc.initialized) {
                value -= poly_blep(state.osc.phase, dt);
            }

            samples[j] = value;

            state.osc.prev_phase = state.osc.phase;
            state.osc.phase += dt;
            if (state.osc.phase >= 1.0f) state.osc.phase -= 1.0f;
            else if (state.osc.phase < 0.0f) state.osc.phase += 1.0f;
            state.osc.initialized = true;
        }

        out[i] = state.downsample(samples[0], samples[1]);
    }
}

// SAW_4X: 4x oversampled sawtooth
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Interpolates frequency input for true sample-accurate FM
[[gnu::always_inline]]
inline void op_osc_saw_4x(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<OscState4x>(inst.state_id);

    float inv_sr_4x = ctx.inv_sample_rate * 0.25f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.osc.phase, state.osc.prev_trigger, state.osc.initialized,
                          trigger[i], phase_offset[i]);

        float freq_curr = freq[i];
        float freq_next = (i + 1 < BLOCK_SIZE) ? freq[i + 1] : freq[i];

        float samples[4];

        for (int j = 0; j < 4; ++j) {
            float t = static_cast<float>(j) * 0.25f;
            float freq_interp = freq_curr + t * (freq_next - freq_curr);
            float dt = freq_interp * inv_sr_4x;

            float value = 2.0f * state.osc.phase - 1.0f;

            if (state.osc.initialized) {
                value -= poly_blep(state.osc.phase, dt);
            }

            samples[j] = value;

            state.osc.prev_phase = state.osc.phase;
            state.osc.phase += dt;
            if (state.osc.phase >= 1.0f) state.osc.phase -= 1.0f;
            else if (state.osc.phase < 0.0f) state.osc.phase += 1.0f;
            state.osc.initialized = true;
        }

        out[i] = state.downsample(samples[0], samples[1], samples[2], samples[3]);
    }
}

// SQR_2X: 2x oversampled square with PolyBLEP
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Interpolates frequency input for true sample-accurate FM
[[gnu::always_inline]]
inline void op_osc_sqr_2x(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<OscState2x>(inst.state_id);

    float inv_sr_2x = ctx.inv_sample_rate * 0.5f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.osc.phase, state.osc.prev_trigger, state.osc.initialized,
                          trigger[i], phase_offset[i]);

        float freq_curr = freq[i];
        float freq_next = (i + 1 < BLOCK_SIZE) ? freq[i + 1] : freq[i];

        float samples[2];

        for (int j = 0; j < 2; ++j) {
            float t = static_cast<float>(j) * 0.5f;
            float freq_interp = freq_curr + t * (freq_next - freq_curr);
            float dt = freq_interp * inv_sr_2x;

            float value = (state.osc.phase < 0.5f) ? 1.0f : -1.0f;

            if (state.osc.initialized) {
                value += poly_blep(state.osc.phase, dt);
                float phase_half = state.osc.phase + 0.5f;
                if (phase_half >= 1.0f) phase_half -= 1.0f;
                value -= poly_blep(phase_half, dt);
            }

            samples[j] = value;

            state.osc.prev_phase = state.osc.phase;
            state.osc.phase += dt;
            if (state.osc.phase >= 1.0f) state.osc.phase -= 1.0f;
            else if (state.osc.phase < 0.0f) state.osc.phase += 1.0f;
            state.osc.initialized = true;
        }

        out[i] = state.downsample(samples[0], samples[1]);
    }
}

// SQR_4X: 4x oversampled square
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Interpolates frequency input for true sample-accurate FM
[[gnu::always_inline]]
inline void op_osc_sqr_4x(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<OscState4x>(inst.state_id);

    float inv_sr_4x = ctx.inv_sample_rate * 0.25f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.osc.phase, state.osc.prev_trigger, state.osc.initialized,
                          trigger[i], phase_offset[i]);

        float freq_curr = freq[i];
        float freq_next = (i + 1 < BLOCK_SIZE) ? freq[i + 1] : freq[i];

        float samples[4];

        for (int j = 0; j < 4; ++j) {
            float t = static_cast<float>(j) * 0.25f;
            float freq_interp = freq_curr + t * (freq_next - freq_curr);
            float dt = freq_interp * inv_sr_4x;

            float value = (state.osc.phase < 0.5f) ? 1.0f : -1.0f;

            if (state.osc.initialized) {
                value += poly_blep(state.osc.phase, dt);
                float phase_half = state.osc.phase + 0.5f;
                if (phase_half >= 1.0f) phase_half -= 1.0f;
                value -= poly_blep(phase_half, dt);
            }

            samples[j] = value;

            state.osc.prev_phase = state.osc.phase;
            state.osc.phase += dt;
            if (state.osc.phase >= 1.0f) state.osc.phase -= 1.0f;
            else if (state.osc.phase < 0.0f) state.osc.phase += 1.0f;
            state.osc.initialized = true;
        }

        out[i] = state.downsample(samples[0], samples[1], samples[2], samples[3]);
    }
}

// TRI_2X: 2x oversampled triangle with PolyBLAMP
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Interpolates frequency input for true sample-accurate FM
[[gnu::always_inline]]
inline void op_osc_tri_2x(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<OscState2x>(inst.state_id);

    float inv_sr_2x = ctx.inv_sample_rate * 0.5f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.osc.phase, state.osc.prev_trigger, state.osc.initialized,
                          trigger[i], phase_offset[i]);

        float freq_curr = freq[i];
        float freq_next = (i + 1 < BLOCK_SIZE) ? freq[i + 1] : freq[i];

        float samples[2];

        for (int j = 0; j < 2; ++j) {
            float t = static_cast<float>(j) * 0.5f;
            float freq_interp = freq_curr + t * (freq_next - freq_curr);
            float dt = freq_interp * inv_sr_2x;

            float value = 4.0f * std::abs(state.osc.phase - 0.5f) - 1.0f;

            if (state.osc.initialized) {
                float blamp = poly_blamp(state.osc.phase, dt);
                float phase_half = state.osc.phase + 0.5f;
                if (phase_half >= 1.0f) phase_half -= 1.0f;
                blamp -= poly_blamp(phase_half, dt);
                value += 4.0f * dt * blamp;
            }

            samples[j] = value;

            state.osc.prev_phase = state.osc.phase;
            state.osc.phase += dt;
            if (state.osc.phase >= 1.0f) state.osc.phase -= 1.0f;
            else if (state.osc.phase < 0.0f) state.osc.phase += 1.0f;
            state.osc.initialized = true;
        }

        out[i] = state.downsample(samples[0], samples[1]);
    }
}

// TRI_4X: 4x oversampled triangle
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Interpolates frequency input for true sample-accurate FM
[[gnu::always_inline]]
inline void op_osc_tri_4x(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<OscState4x>(inst.state_id);

    float inv_sr_4x = ctx.inv_sample_rate * 0.25f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.osc.phase, state.osc.prev_trigger, state.osc.initialized,
                          trigger[i], phase_offset[i]);

        float freq_curr = freq[i];
        float freq_next = (i + 1 < BLOCK_SIZE) ? freq[i + 1] : freq[i];

        float samples[4];

        for (int j = 0; j < 4; ++j) {
            float t = static_cast<float>(j) * 0.25f;
            float freq_interp = freq_curr + t * (freq_next - freq_curr);
            float dt = freq_interp * inv_sr_4x;

            float value = 4.0f * std::abs(state.osc.phase - 0.5f) - 1.0f;

            if (state.osc.initialized) {
                float blamp = poly_blamp(state.osc.phase, dt);
                float phase_half = state.osc.phase + 0.5f;
                if (phase_half >= 1.0f) phase_half -= 1.0f;
                blamp -= poly_blamp(phase_half, dt);
                value += 4.0f * dt * blamp;
            }

            samples[j] = value;

            state.osc.prev_phase = state.osc.phase;
            state.osc.phase += dt;
            if (state.osc.phase >= 1.0f) state.osc.phase -= 1.0f;
            else if (state.osc.phase < 0.0f) state.osc.phase += 1.0f;
            state.osc.initialized = true;
        }

        out[i] = state.downsample(samples[0], samples[1], samples[2], samples[3]);
    }
}

// ============================================================================
// 4x Oversampled PWM Oscillators - For alias-free FM synthesis with PWM
// ============================================================================

// SQR_PWM_4X: 4x oversampled PWM square wave
// in0: frequency (Hz)
// in1: PWM (-1 to +1, where 0 = 50% duty cycle)
// in2: phase offset (0-1, optional)
// in3: trigger (reset phase on rising edge, optional)
[[gnu::always_inline]]
inline void op_osc_sqr_pwm_4x(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* pwm = ctx.buffers->get(inst.inputs[1]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[2]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[3]);
    auto& state = ctx.states->get_or_create<OscState4x>(inst.state_id);

    float inv_sr_4x = ctx.inv_sample_rate * 0.25f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.osc.phase, state.osc.prev_trigger, state.osc.initialized,
                          trigger[i], phase_offset[i]);

        float freq_curr = freq[i];
        float freq_next = (i + 1 < BLOCK_SIZE) ? freq[i + 1] : freq[i];
        float pwm_val = pwm[i];

        // Calculate duty cycle from PWM input
        float duty = 0.5f + std::clamp(pwm_val, -1.0f, 1.0f) * 0.5f;
        duty = std::clamp(duty, 0.001f, 0.999f);

        float samples[4];

        for (int j = 0; j < 4; ++j) {
            float t = static_cast<float>(j) * 0.25f;
            float freq_interp = freq_curr + t * (freq_next - freq_curr);
            float dt = freq_interp * inv_sr_4x;

            float value = (state.osc.phase < duty) ? 1.0f : -1.0f;

            if (state.osc.initialized) {
                // Rising edge at phase = 0
                value += poly_blep(state.osc.phase, dt);

                // Falling edge at phase = duty
                float dist_to_fall = state.osc.phase - duty;
                if (dist_to_fall > 0.5f) dist_to_fall -= 1.0f;
                if (dist_to_fall < -0.5f) dist_to_fall += 1.0f;
                value -= poly_blep_distance(dist_to_fall, dt);
            }

            samples[j] = value;

            state.osc.prev_phase = state.osc.phase;
            state.osc.phase += dt;
            if (state.osc.phase >= 1.0f) state.osc.phase -= 1.0f;
            else if (state.osc.phase < 0.0f) state.osc.phase += 1.0f;
            state.osc.initialized = true;
        }

        out[i] = state.downsample(samples[0], samples[1], samples[2], samples[3]);
    }
}

// SAW_PWM_4X: 4x oversampled variable-slope sawtooth
// in0: frequency (Hz)
// in1: PWM (-1 to +1)
// in2: phase offset (0-1, optional)
// in3: trigger (reset phase on rising edge, optional)
[[gnu::always_inline]]
inline void op_osc_saw_pwm_4x(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* pwm = ctx.buffers->get(inst.inputs[1]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[2]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[3]);
    auto& state = ctx.states->get_or_create<OscState4x>(inst.state_id);

    float inv_sr_4x = ctx.inv_sample_rate * 0.25f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.osc.phase, state.osc.prev_trigger, state.osc.initialized,
                          trigger[i], phase_offset[i]);

        float freq_curr = freq[i];
        float freq_next = (i + 1 < BLOCK_SIZE) ? freq[i + 1] : freq[i];
        float pwm_val = pwm[i];

        // Map PWM to midpoint position
        float mid = (1.0f + std::clamp(pwm_val, -1.0f, 1.0f)) * 0.5f;
        mid = std::clamp(mid, 0.01f, 0.99f);

        float samples[4];

        for (int j = 0; j < 4; ++j) {
            float t = static_cast<float>(j) * 0.25f;
            float freq_interp = freq_curr + t * (freq_next - freq_curr);
            float dt = freq_interp * inv_sr_4x;

            // Piecewise linear waveform
            float value;
            if (state.osc.phase < mid) {
                value = 2.0f * state.osc.phase / mid - 1.0f;
            } else {
                value = 1.0f - 2.0f * (state.osc.phase - mid) / (1.0f - mid);
            }

            if (state.osc.initialized) {
                // PolyBLAMP at slope discontinuities
                float blamp = poly_blamp(state.osc.phase, dt);
                float phase_at_mid = state.osc.phase - mid;
                if (phase_at_mid < 0.0f) phase_at_mid += 1.0f;
                blamp -= poly_blamp(phase_at_mid, dt);

                float slope_rise = 2.0f / mid;
                float slope_fall = -2.0f / (1.0f - mid);
                float slope_diff = slope_rise - slope_fall;

                value += slope_diff * dt * blamp;
            }

            samples[j] = value;

            state.osc.prev_phase = state.osc.phase;
            state.osc.phase += dt;
            if (state.osc.phase >= 1.0f) state.osc.phase -= 1.0f;
            else if (state.osc.phase < 0.0f) state.osc.phase += 1.0f;
            state.osc.initialized = true;
        }

        out[i] = state.downsample(samples[0], samples[1], samples[2], samples[3]);
    }
}

}  // namespace cedar
