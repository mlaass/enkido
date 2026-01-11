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
// DYNAMICS_COMP: Feedforward Compressor
// ============================================================================
// in0: input signal
// in1: threshold (dB, -60 to 0)
// in2: ratio (1.0 to 20.0, where 20 = ~inf:1)
// rate: attack (high 4 bits, 0-15 -> 0.1-100ms), release (low 4 bits, 0-15 -> 10-1000ms)
//
// Classic feedforward compressor with RMS envelope detection.
// Reduces dynamic range by attenuating signals above threshold.

[[gnu::always_inline]]
inline void op_dynamics_comp(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* threshold_db = ctx.buffers->get(inst.inputs[1]);
    const float* ratio = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<CompressorState>(inst.state_id);

    // Decode attack/release times from rate field (4 bits each)
    float attack_ms = 0.1f + static_cast<float>((inst.rate >> 4) & 0x0F) * (100.0f - 0.1f) / 15.0f;
    float release_ms = 10.0f + static_cast<float>(inst.rate & 0x0F) * (1000.0f - 10.0f) / 15.0f;

    // Update coefficients if parameters changed
    if (attack_ms != state.last_attack || release_ms != state.last_release) {
        state.last_attack = attack_ms;
        state.last_release = release_ms;
        state.attack_coeff = time_to_coeff(attack_ms * 0.001f, ctx.sample_rate);
        state.release_coeff = time_to_coeff(release_ms * 0.001f, ctx.sample_rate);
    }

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float x = input[i];

        // Envelope follower (peak detection)
        float abs_x = std::abs(x);
        if (abs_x > state.envelope) {
            state.envelope += state.attack_coeff * (abs_x - state.envelope);
        } else {
            state.envelope += state.release_coeff * (abs_x - state.envelope);
        }

        // Convert to dB
        float env_db = linear_to_db(state.envelope + 1e-10f);

        // Calculate gain reduction
        float thresh = std::clamp(threshold_db[i], -60.0f, 0.0f);
        float r = std::clamp(ratio[i], 1.0f, 20.0f);

        float gain_db = 0.0f;
        if (env_db > thresh) {
            // Compression: reduce by (1 - 1/ratio) of the amount over threshold
            float over_db = env_db - thresh;
            float compressed_db = thresh + over_db / r;
            gain_db = compressed_db - env_db;
        }

        // Convert gain to linear and apply
        float gain = db_to_linear(gain_db);
        state.gain_reduction = gain;  // Store for metering

        out[i] = x * gain;
    }
}

// ============================================================================
// DYNAMICS_LIMITER: Brick-Wall Limiter with Lookahead
// ============================================================================
// in0: input signal
// in1: ceiling (dB, -12 to 0)
// in2: release (ms, 10-500)
// rate: lookahead (0 = off, non-zero = 1ms lookahead)
//
// True peak limiter that prevents signal from exceeding ceiling.
// Optional lookahead allows smoother limiting with no overshoot.

[[gnu::always_inline]]
inline void op_dynamics_limiter(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* ceiling_db = ctx.buffers->get(inst.inputs[1]);
    const float* release_ms = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<LimiterState>(inst.state_id);

    bool use_lookahead = inst.rate != 0;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float x = input[i];

        // Write to lookahead buffer
        state.lookahead_buffer[state.write_pos] = x;

        // Get signal to analyze (current or lookahead)
        float analyze_sample;
        if (use_lookahead) {
            // Read ahead in buffer
            std::size_t read_pos = (state.write_pos + 1) % LimiterState::LOOKAHEAD_SAMPLES;
            analyze_sample = state.lookahead_buffer[read_pos];
            x = analyze_sample;  // Output the delayed signal
        } else {
            analyze_sample = x;
        }

        state.write_pos = (state.write_pos + 1) % LimiterState::LOOKAHEAD_SAMPLES;

        // Calculate release coefficient
        float rel_ms = std::clamp(release_ms[i], 10.0f, 500.0f);
        float release_coeff = time_to_coeff(rel_ms * 0.001f, ctx.sample_rate);

        // Calculate required gain
        float ceiling = db_to_linear(std::clamp(ceiling_db[i], -12.0f, 0.0f));
        float abs_x = std::abs(analyze_sample);

        float target_gain = 1.0f;
        if (abs_x > ceiling) {
            target_gain = ceiling / abs_x;
        }

        // Smooth gain changes (instant attack, smooth release)
        if (target_gain < state.gain) {
            state.gain = target_gain;  // Instant attack
        } else {
            state.gain += release_coeff * (target_gain - state.gain);  // Smooth release
        }

        out[i] = x * state.gain;
    }
}

// ============================================================================
// DYNAMICS_GATE: Noise Gate with Hysteresis
// ============================================================================
// in0: input signal
// in1: threshold (dB, -80 to 0)
// in2: range (dB, 0 to -80, how much to attenuate when closed)
// rate: attack (bits 6-7), hold (bits 4-5), release (bits 0-3)
//
// Attenuates signal when it falls below threshold.
// Hysteresis prevents chatter at the threshold.

[[gnu::always_inline]]
inline void op_dynamics_gate(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* threshold_db = ctx.buffers->get(inst.inputs[1]);
    const float* range_db = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<GateState>(inst.state_id);

    // Decode timing parameters from rate field
    // Attack: 0.1-10ms (2 bits -> 4 values)
    float attack_ms = 0.1f + static_cast<float>((inst.rate >> 6) & 0x3) * (10.0f - 0.1f) / 3.0f;
    // Hold: 0-200ms (2 bits -> 4 values)
    float hold_ms = static_cast<float>((inst.rate >> 4) & 0x3) * 200.0f / 3.0f;
    // Release: 10-500ms (4 bits -> 16 values)
    float release_ms = 10.0f + static_cast<float>(inst.rate & 0x0F) * (500.0f - 10.0f) / 15.0f;

    // Update coefficients if needed
    if (attack_ms != state.last_attack || release_ms != state.last_release) {
        state.last_attack = attack_ms;
        state.last_release = release_ms;
        state.attack_coeff = time_to_coeff(attack_ms * 0.001f, ctx.sample_rate);
        state.release_coeff = time_to_coeff(release_ms * 0.001f, ctx.sample_rate);
    }

    float hold_samples = hold_ms * 0.001f * ctx.sample_rate;

    // Hysteresis: 6dB difference between open and close thresholds
    constexpr float HYSTERESIS_DB = 6.0f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float x = input[i];

        // Envelope follower
        float abs_x = std::abs(x);
        float coeff = abs_x > state.envelope ? state.attack_coeff * 4.0f : state.release_coeff;
        state.envelope += coeff * (abs_x - state.envelope);

        float env_db = linear_to_db(state.envelope + 1e-10f);
        float thresh = std::clamp(threshold_db[i], -80.0f, 0.0f);
        float range = std::clamp(range_db[i], -80.0f, 0.0f);

        // Gate state machine with hysteresis
        if (state.is_open) {
            // Gate is open - check if we should close
            if (env_db < thresh - HYSTERESIS_DB) {
                // Start hold period
                state.hold_counter += 1.0f;
                if (state.hold_counter > hold_samples) {
                    state.is_open = false;
                    state.hold_counter = 0.0f;
                }
            } else {
                state.hold_counter = 0.0f;
            }
        } else {
            // Gate is closed - check if we should open
            if (env_db > thresh) {
                state.is_open = true;
                state.hold_counter = 0.0f;
            }
        }

        // Calculate target gain
        float target_gain = state.is_open ? 1.0f : db_to_linear(range);

        // Smooth gain transitions
        float gain_coeff = target_gain > state.gain ? state.attack_coeff : state.release_coeff;
        state.gain += gain_coeff * (target_gain - state.gain);

        out[i] = x * state.gain;
    }
}

}  // namespace cedar
