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
// DYNAMICS_COMP: Feedforward Compressor
// ============================================================================
// in0: input signal
// in1: threshold (dB, -60 to 0)
// in2: ratio (1.0 to 20.0, where 20 = ~inf:1)
// rate: attack (high 4 bits, 0-15 -> 0.1-100ms), release (low 4 bits, 0-15 -> 10-1000ms)
//
// Classic feedforward compressor with RMS envelope detection.
// Reduces dynamic range by attenuating signals above threshold.

// Stereo-native (prd-stereo-native-opcodes Phase 4d): per-channel envelope
// and gain-reduction state. Each channel processes its own dynamics independently
// (matches today's auto-lift behavior).
[[gnu::always_inline]]
inline void op_dynamics_comp(ExecutionContext& ctx, const Instruction& inst) {
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* threshold_db = ctx.buffers->get(inst.inputs[1]);
    const float* ratio = ctx.buffers->get(inst.inputs[2]);
    const float* dry_level = (inst.inputs[3] != 0xFFFF) ? ctx.buffers->get(inst.inputs[3]) : nullptr;
    const float* wet_level = (inst.inputs[4] != 0xFFFF) ? ctx.buffers->get(inst.inputs[4]) : nullptr;
    auto& state = ctx.states->get_or_create<CompressorState>(inst.state_id);

    // Decode attack/release times from rate field (4 bits each, shared)
    float attack_ms = 0.1f + static_cast<float>((inst.rate >> 4) & 0x0F) * (100.0f - 0.1f) / 15.0f;
    float release_ms = 10.0f + static_cast<float>(inst.rate & 0x0F) * (1000.0f - 10.0f) / 15.0f;

    if (attack_ms != state.last_attack || release_ms != state.last_release) {
        state.last_attack = attack_ms;
        state.last_release = release_ms;
        state.attack_coeff = time_to_coeff(attack_ms * 0.001f, ctx.sample_rate);
        state.release_coeff = time_to_coeff(release_ms * 0.001f, ctx.sample_rate);
    }

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float thresh = std::clamp(threshold_db[i], -60.0f, 0.0f);
        float r = std::clamp(ratio[i], 1.0f, 20.0f);
        float dry = drywet::coeff(dry_level, i, 0.0f);
        float wet = drywet::coeff(wet_level, i, 1.0f);

        for (std::size_t ch = 0; ch < 2; ++ch) {
            float x = (ch == 0) ? input[i] : (stereo_in ? input_r[i] : input[i]);

            float abs_x = std::abs(x);
            if (abs_x > state.envelope[ch]) {
                state.envelope[ch] += state.attack_coeff * (abs_x - state.envelope[ch]);
            } else {
                state.envelope[ch] += state.release_coeff * (abs_x - state.envelope[ch]);
            }

            float env_db = linear_to_db(state.envelope[ch] + 1e-10f);

            float gain_db = 0.0f;
            if (env_db > thresh) {
                float over_db = env_db - thresh;
                float compressed_db = thresh + over_db / r;
                gain_db = compressed_db - env_db;
            }

            float gain = db_to_linear(gain_db);
            state.gain_reduction[ch] = gain;

            (ch == 0 ? out_l : out_r)[i] = drywet::mix(x, x * gain, dry, wet);
        }
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

// Stereo-native (prd-stereo-native-opcodes Phase 4d): per-channel lookahead
// buffer, write_pos, and gain. Independent per-channel limiting (matches
// today's auto-lift behavior).
[[gnu::always_inline]]
inline void op_dynamics_limiter(ExecutionContext& ctx, const Instruction& inst) {
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* ceiling_db = ctx.buffers->get(inst.inputs[1]);
    const float* release_ms = ctx.buffers->get(inst.inputs[2]);
    const float* dry_level = (inst.inputs[3] != 0xFFFF) ? ctx.buffers->get(inst.inputs[3]) : nullptr;
    const float* wet_level = (inst.inputs[4] != 0xFFFF) ? ctx.buffers->get(inst.inputs[4]) : nullptr;
    auto& state = ctx.states->get_or_create<LimiterState>(inst.state_id);

    bool use_lookahead = inst.rate != 0;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float rel_ms = std::clamp(release_ms[i], 10.0f, 500.0f);
        float release_coeff = time_to_coeff(rel_ms * 0.001f, ctx.sample_rate);
        float ceiling = db_to_linear(std::clamp(ceiling_db[i], -12.0f, 0.0f));
        float dry = drywet::coeff(dry_level, i, 0.0f);
        float wet = drywet::coeff(wet_level, i, 1.0f);

        for (std::size_t ch = 0; ch < 2; ++ch) {
            float x_in = (ch == 0) ? input[i] : (stereo_in ? input_r[i] : input[i]);
            float x = x_in;

            state.lookahead_buffer[ch][state.write_pos[ch]] = x;

            float analyze_sample;
            if (use_lookahead) {
                std::size_t read_pos = (state.write_pos[ch] + 1) % LimiterState::LOOKAHEAD_SAMPLES;
                analyze_sample = state.lookahead_buffer[ch][read_pos];
                x = analyze_sample;
            } else {
                analyze_sample = x;
            }

            state.write_pos[ch] = (state.write_pos[ch] + 1) % LimiterState::LOOKAHEAD_SAMPLES;

            float abs_x = std::abs(analyze_sample);
            float target_gain = 1.0f;
            if (abs_x > ceiling) {
                target_gain = ceiling / abs_x;
            }

            if (target_gain < state.gain[ch]) {
                state.gain[ch] = target_gain;
            } else {
                state.gain[ch] += release_coeff * (target_gain - state.gain[ch]);
            }

            // Note: lookahead-mode dry is the lookahead-delayed sample (the same
            // signal the limiter is gain-shaping), preserving phase alignment
            // between dry and wet under parallel limiting.
            (ch == 0 ? out_l : out_r)[i] = drywet::mix(x, x * state.gain[ch], dry, wet);
        }
    }
}

// Default constants for noise gate
constexpr float GATE_HYSTERESIS_DEFAULT = 6.0f;    // dB
constexpr float GATE_CLOSE_TIME_DEFAULT = 5.0f;    // ms

// ============================================================================
// DYNAMICS_GATE: Noise Gate with Hysteresis
// ============================================================================
// in0: input signal
// in1: threshold (dB, -80 to 0)
// in2: range (dB, 0 to -80, how much to attenuate when closed)
// in3: hysteresis - open/close difference in dB (default 6)
// in4: close_time - fade-out time in ms (default 5)
// rate: attack (bits 6-7), hold (bits 4-5), release (bits 0-3)
//
// Attenuates signal when it falls below threshold.
// Hysteresis prevents chatter at the threshold.

// Stereo-native (prd-stereo-native-opcodes Phase 4d): per-channel gate
// envelope/gain/open-state/hold-counter; coefficients shared.
[[gnu::always_inline]]
inline void op_dynamics_gate(ExecutionContext& ctx, const Instruction& inst) {
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* threshold_db = ctx.buffers->get(inst.inputs[1]);
    const float* range_db = ctx.buffers->get(inst.inputs[2]);
    const float* hysteresis_in = ctx.buffers->get(inst.inputs[3]);
    const float* close_time_in = ctx.buffers->get(inst.inputs[4]);
    auto& state = ctx.states->get_or_create<GateState>(inst.state_id);

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

    float attack_ms = 0.1f + static_cast<float>((inst.rate >> 6) & 0x3) * (10.0f - 0.1f) / 3.0f;
    float hold_ms = static_cast<float>((inst.rate >> 4) & 0x3) * 200.0f / 3.0f;
    float release_ms = 10.0f + static_cast<float>(inst.rate & 0x0F) * (500.0f - 10.0f) / 15.0f;

    if (attack_ms != state.last_attack || release_ms != state.last_release) {
        state.last_attack = attack_ms;
        state.last_release = release_ms;
        state.attack_coeff = time_to_coeff(attack_ms * 0.001f, ctx.sample_rate);
        state.release_coeff = time_to_coeff(release_ms * 0.001f, ctx.sample_rate);
    }

    float hold_samples = hold_ms * 0.001f * ctx.sample_rate;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float hysteresis_db = hysteresis_in[i] > 0.0f ? hysteresis_in[i] : GATE_HYSTERESIS_DEFAULT;
        float close_time_ms = close_time_in[i] > 0.0f ? close_time_in[i] : GATE_CLOSE_TIME_DEFAULT;
        float thresh = std::clamp(threshold_db[i], -80.0f, 0.0f);
        float range = std::clamp(range_db[i], -80.0f, 0.0f);
        float close_coeff = 1.0f - std::exp(-1.0f / (close_time_ms * 0.001f * ctx.sample_rate));
        float dry = drywet::coeff(dry_buf, i, dry_const);
        float wet = drywet::coeff(wet_buf, i, wet_const);

        for (std::size_t ch = 0; ch < 2; ++ch) {
            float x = (ch == 0) ? input[i] : (stereo_in ? input_r[i] : input[i]);

            float abs_x = std::abs(x);
            float coeff = abs_x > state.envelope[ch] ? state.attack_coeff * 10.0f : state.release_coeff * 10.0f;
            state.envelope[ch] += coeff * (abs_x - state.envelope[ch]);

            float env_db = linear_to_db(state.envelope[ch] + 1e-10f);

            if (state.is_open[ch]) {
                if (env_db < thresh - hysteresis_db) {
                    state.hold_counter[ch] += 1.0f;
                    if (state.hold_counter[ch] > hold_samples) {
                        state.is_open[ch] = false;
                        state.hold_counter[ch] = 0.0f;
                    }
                } else {
                    state.hold_counter[ch] = 0.0f;
                }
            } else {
                if (env_db > thresh) {
                    state.is_open[ch] = true;
                    state.hold_counter[ch] = 0.0f;
                }
            }

            float target_gain = state.is_open[ch] ? 1.0f : db_to_linear(range);

            float gain_coeff;
            if (target_gain > state.gain[ch]) {
                gain_coeff = state.attack_coeff;
            } else {
                gain_coeff = close_coeff;
            }
            state.gain[ch] += gain_coeff * (target_gain - state.gain[ch]);

            (ch == 0 ? out_l : out_r)[i] = drywet::mix(x, x * state.gain[ch], dry, wet);
        }
    }
}

}  // namespace cedar
