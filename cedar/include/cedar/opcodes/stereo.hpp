#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
#include "drywet.hpp"
#include <cmath>
#include <algorithm>

namespace cedar {

// ============================================================================
// Stereo Utility Opcodes
// ============================================================================

// PAN: Pan mono signal to stereo position
// in0: mono input signal
// in1: pan position (-1.0 = left, 0.0 = center, 1.0 = right)
// out_buffer: left channel output
// out_buffer+1: right channel output (must be pre-allocated by compiler)
//
// Uses constant-power (equal-power) panning for natural stereo image.
// Formula: L = mono * cos(pan_angle), R = mono * sin(pan_angle)
// where pan_angle = (pan + 1) * PI/4 maps [-1,1] to [0, PI/2]
[[gnu::always_inline]]
inline void op_pan(ExecutionContext& ctx, const Instruction& inst) {
    float* out_left = ctx.buffers->get(inst.out_buffer);
    float* out_right = ctx.buffers->get(inst.out_buffer + 1);
    const float* mono = ctx.buffers->get(inst.inputs[0]);
    const float* pan = ctx.buffers->get(inst.inputs[1]);

    constexpr float PI_OVER_4 = 0.7853981633974483f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Clamp pan to [-1, 1] range
        float p = std::clamp(pan[i], -1.0f, 1.0f);

        // Convert pan to angle (0 to PI/2)
        float angle = (p + 1.0f) * PI_OVER_4;

        // Constant-power panning
        out_left[i] = mono[i] * std::cos(angle);
        out_right[i] = mono[i] * std::sin(angle);
    }
}

// WIDTH: Adjust stereo width
// in0: left input
// in1: right input
// in2: width amount (0.0 = mono, 1.0 = normal, 2.0 = extra wide)
// out_buffer: left output
// out_buffer+1: right output (must be pre-allocated by compiler)
//
// Uses mid/side processing internally:
// M = (L + R) / 2, S = (L - R) / 2
// L' = M + S * width, R' = M - S * width
[[gnu::always_inline]]
inline void op_width(ExecutionContext& ctx, const Instruction& inst) {
    float* out_left = ctx.buffers->get(inst.out_buffer);
    float* out_right = ctx.buffers->get(inst.out_buffer + 1);
    const float* in_left = ctx.buffers->get(inst.inputs[0]);
    const float* in_right = ctx.buffers->get(inst.inputs[1]);
    const float* width = ctx.buffers->get(inst.inputs[2]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Calculate mid and side
        float mid = (in_left[i] + in_right[i]) * 0.5f;
        float side = (in_left[i] - in_right[i]) * 0.5f;

        // Apply width (0 = mono, 1 = original, >1 = wider)
        float w = std::max(0.0f, width[i]);
        out_left[i] = mid + side * w;
        out_right[i] = mid - side * w;
    }
}

// MS_ENCODE: Convert stereo to mid/side
// in0: left input
// in1: right input
// out_buffer: mid output (mono sum)
// out_buffer+1: side output (stereo difference)
//
// M = (L + R) / 2, S = (L - R) / 2
[[gnu::always_inline]]
inline void op_ms_encode(ExecutionContext& ctx, const Instruction& inst) {
    float* out_mid = ctx.buffers->get(inst.out_buffer);
    float* out_side = ctx.buffers->get(inst.out_buffer + 1);
    const float* in_left = ctx.buffers->get(inst.inputs[0]);
    const float* in_right = ctx.buffers->get(inst.inputs[1]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out_mid[i] = (in_left[i] + in_right[i]) * 0.5f;
        out_side[i] = (in_left[i] - in_right[i]) * 0.5f;
    }
}

// MS_DECODE: Convert mid/side back to stereo
// in0: mid input
// in1: side input
// out_buffer: left output
// out_buffer+1: right output
//
// L = M + S, R = M - S
[[gnu::always_inline]]
inline void op_ms_decode(ExecutionContext& ctx, const Instruction& inst) {
    float* out_left = ctx.buffers->get(inst.out_buffer);
    float* out_right = ctx.buffers->get(inst.out_buffer + 1);
    const float* mid = ctx.buffers->get(inst.inputs[0]);
    const float* side = ctx.buffers->get(inst.inputs[1]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out_left[i] = mid[i] + side[i];
        out_right[i] = mid[i] - side[i];
    }
}

// MONO_DOWNMIX: Sum stereo signal to mono with 0.5 gain
// in0: left input
// in1: right input
// out_buffer: mono output
//
// Formula: out[i] = (L[i] + R[i]) * 0.5
// Standard sum-to-mono convention — 0dB for correlated content, -3dB for
// uncorrelated content (e.g. a sine panned hard to both channels stays 0dB;
// independent noise in L and R sums at -3dB RMS).
[[gnu::always_inline]]
inline void op_mono_downmix(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* in_left = ctx.buffers->get(inst.inputs[0]);
    const float* in_right = ctx.buffers->get(inst.inputs[1]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = (in_left[i] + in_right[i]) * 0.5f;
    }
}

// PAN_STEREO: Equal-power stereo balance (the DAW "pan" knob on a stereo track)
// in0: left input
// in1: right input
// in2: pan position (-1 = hard left, 0 = center -3dB, +1 = hard right)
// out_buffer: left output
// out_buffer+1: right output
//
// L_out = L_in * cos((p+1) * PI/4), R_out = R_in * sin((p+1) * PI/4).
// This is balance, not re-panning: at p=-1, R_out=0 and L_out=L_in (not L+R).
[[gnu::always_inline]]
inline void op_pan_stereo(ExecutionContext& ctx, const Instruction& inst) {
    float* out_left = ctx.buffers->get(inst.out_buffer);
    float* out_right = ctx.buffers->get(inst.out_buffer + 1);
    const float* in_left = ctx.buffers->get(inst.inputs[0]);
    const float* in_right = ctx.buffers->get(inst.inputs[1]);
    const float* pos = ctx.buffers->get(inst.inputs[2]);

    constexpr float PI_OVER_4 = 0.7853981633974483f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float p = std::clamp(pos[i], -1.0f, 1.0f);
        float angle = (p + 1.0f) * PI_OVER_4;
        out_left[i] = in_left[i] * std::cos(angle);
        out_right[i] = in_right[i] * std::sin(angle);
    }
}

// ============================================================================
// Stereo Effects
// ============================================================================

// DELAY_PINGPONG: True stereo ping-pong delay
// in0: left input
// in1: right input
// in2: delay time (seconds)
// in3: feedback amount (0.0-1.0)
// in4: pan width (0.0 = center, 1.0 = full stereo ping-pong)
// out_buffer: left output
// out_buffer+1: right output
//
// Classic ping-pong delay where echoes alternate between channels.
// Each echo crosses to the opposite channel with optional width control.
[[gnu::always_inline]]
inline void op_delay_pingpong(ExecutionContext& ctx, const Instruction& inst) {
    float* out_left = ctx.buffers->get(inst.out_buffer);
    float* out_right = ctx.buffers->get(inst.out_buffer + 1);
    const float* in_left = ctx.buffers->get(inst.inputs[0]);
    const float* in_right = ctx.buffers->get(inst.inputs[1]);
    const float* delay_time = ctx.buffers->get(inst.inputs[2]);
    const float* feedback = ctx.buffers->get(inst.inputs[3]);
    const float* pan_width = (inst.inputs[4] != BUFFER_UNUSED)
        ? ctx.buffers->get(inst.inputs[4])
        : nullptr;

    auto& state = ctx.states->get_or_create<PingPongDelayState>(inst.state_id);

    // ExtendedParams<2>: ext[0] = dry (default 1.0), ext[1] = wet (default 0.5)
    const auto* ext = ctx.states->get_if<ExtendedParams<2>>(ext_params_state_id(inst.state_id));
    const float* dry_buf = nullptr; float dry_const = 1.0f;
    const float* wet_buf = nullptr; float wet_const = 0.5f;
    if (ext) {
        const auto& d = ext->params[0];
        if (d.is_constant()) dry_const = d.constant; else dry_buf = ctx.buffers->get(d.buffer_idx);
        const auto& w = ext->params[1];
        if (w.is_constant()) wet_const = w.constant; else wet_buf = ctx.buffers->get(w.buffer_idx);
    }

    // Allocate buffers for max 10 seconds
    float max_delay_sec = 10.0f;
    std::size_t max_samples = static_cast<std::size_t>(max_delay_sec * ctx.sample_rate) + 1;
    max_samples = std::min(max_samples, PingPongDelayState::MAX_DELAY_SAMPLES);
    state.ensure_buffers(max_samples, ctx.arena);

    if (!state.buffer_left || !state.buffer_right) {
        // Buffer allocation failed, pass through dry signal
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            out_left[i] = in_left[i];
            out_right[i] = in_right[i];
        }
        return;
    }

    // Smoothing coefficient for delay time changes (~20ms slew at 48kHz)
    constexpr float smooth_coeff = 0.9995f;

    // Damping coefficient for high-frequency rolloff in feedback path
    constexpr float damp_coeff = 0.3f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Convert delay time to samples
        float target_delay = delay_time[i] * ctx.sample_rate;
        target_delay = std::clamp(target_delay, 1.0f, static_cast<float>(state.buffer_size - 2));

        // Initialize or smooth delay time
        if (!state.initialized) {
            state.smoothed_delay = target_delay;
            state.initialized = true;
        } else {
            state.smoothed_delay = state.smoothed_delay * smooth_coeff +
                                  target_delay * (1.0f - smooth_coeff);
        }

        float delay_samples = state.smoothed_delay;

        // Linear interpolation for sub-sample accuracy
        std::size_t delay_int = static_cast<std::size_t>(delay_samples);
        float delay_frac = delay_samples - static_cast<float>(delay_int);

        // Read from delay lines
        std::size_t read_pos1 = (state.write_pos + state.buffer_size - delay_int) % state.buffer_size;
        std::size_t read_pos2 = (read_pos1 + state.buffer_size - 1) % state.buffer_size;

        float delayed_left = state.buffer_left[read_pos1] * (1.0f - delay_frac) +
                            state.buffer_left[read_pos2] * delay_frac;
        float delayed_right = state.buffer_right[read_pos1] * (1.0f - delay_frac) +
                             state.buffer_right[read_pos2] * delay_frac;

        // Apply damping (simple one-pole lowpass)
        state.damp_state_left = delayed_left * (1.0f - damp_coeff) +
                               state.damp_state_left * damp_coeff;
        state.damp_state_right = delayed_right * (1.0f - damp_coeff) +
                                state.damp_state_right * damp_coeff;

        delayed_left = state.damp_state_left;
        delayed_right = state.damp_state_right;

        // Clamp feedback
        float fb = std::clamp(feedback[i], 0.0f, 0.99f);

        // Pan width (default 1.0 for full ping-pong)
        float width = pan_width ? std::clamp(pan_width[i], 0.0f, 1.0f) : 1.0f;

        // Ping-pong crossover: L feedback goes to R, R feedback goes to L
        // With width control: interpolate between center (width=0) and full ping-pong (width=1)
        float cross_left = delayed_right * width + delayed_left * (1.0f - width);
        float cross_right = delayed_left * width + delayed_right * (1.0f - width);

        // Write to delay lines: input + cross-feedback
        state.buffer_left[state.write_pos] = in_left[i] + cross_left * fb;
        state.buffer_right[state.write_pos] = in_right[i] + cross_right * fb;

        // Advance write position
        state.write_pos = (state.write_pos + 1) % state.buffer_size;

        // Unified dry/wet mix (Category A defaults: dry=1.0, wet=0.5)
        float dry = drywet::coeff(dry_buf, i, dry_const);
        float wet = drywet::coeff(wet_buf, i, wet_const);
        out_left[i] = drywet::mix(in_left[i], delayed_left, dry, wet);
        out_right[i] = drywet::mix(in_right[i], delayed_right, dry, wet);
    }
}

}  // namespace cedar
