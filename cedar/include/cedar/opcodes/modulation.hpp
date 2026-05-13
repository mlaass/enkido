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
// EFFECT_COMB: Feedback Comb Filter with Damping
// ============================================================================
// in0: input signal
// in1: delay time (ms, 0.1-100)
// in2: feedback (-0.99 to 0.99)
// rate: damping (0-255 -> 0.0-1.0)
//
// Fundamental building block for many effects. Creates resonances at
// multiples of the fundamental frequency (1000/delay_ms Hz).

[[gnu::always_inline]]
inline void op_effect_comb(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* delay_ms = ctx.buffers->get(inst.inputs[1]);
    const float* feedback = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<CombFilterState>(inst.state_id);

    float damp = static_cast<float>(inst.rate) / 255.0f;

    // Ensure buffer is allocated from arena
    state.ensure_buffer(ctx.arena);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Calculate delay in samples
        float delay_samples = std::clamp(delay_ms[i], 0.1f, 100.0f) * 0.001f * ctx.sample_rate;
        delay_samples = std::min(delay_samples, static_cast<float>(CombFilterState::MAX_COMB_SAMPLES - 1));

        // Read from delay line with interpolation
        float delayed = delay_read_linear(state.buffer, CombFilterState::MAX_COMB_SAMPLES,
                                          state.write_pos, delay_samples);

        // Apply damping (lowpass in feedback path)
        float fb = std::clamp(feedback[i], -0.99f, 0.99f);
        state.filter_state = delayed * (1.0f - damp) + state.filter_state * damp;

        // Write to delay line
        state.buffer[state.write_pos] = input[i] + fb * state.filter_state;
        state.write_pos = (state.write_pos + 1) % CombFilterState::MAX_COMB_SAMPLES;

        out[i] = delayed;
    }
}

// Default constants for flanger
constexpr float FLANGER_MIN_DELAY_DEFAULT = 0.1f;   // ms
constexpr float FLANGER_MAX_DELAY_DEFAULT = 10.0f;  // ms

// Default per-channel LFO phase offset for stereo decorrelation (90° =
// quarter-cycle). The actual offset is supplied by an ExtendedParams<N>
// slot keyed on the opcode's state_id, populated by codegen from the
// user-facing `lfo_phase` parameter (turns, default 0.25). See
// docs/extended-params-mechanism.md.
constexpr float STEREO_LFO_OFFSET_DEFAULT_TURNS = 0.25f;

// ============================================================================
// EFFECT_FLANGER: Stereo-Native Flanger
// ============================================================================
// in0: input signal (mono auto-broadcasts to L=R; stereo via STEREO_INPUT)
// in1: LFO rate (Hz, 0.1-10)
// in2: depth (0.0-1.0)
// in3: min_delay - minimum sweep point in ms (default 0.1)
// in4: max_delay - maximum sweep point in ms (default 10.0)
// rate: feedback (high 4 bits 0-15 -> -0.99 to 0.99)
//
// Short modulated delay (0.1-10ms) with feedback. Stereo-native: per-channel
// delay lines + write_pos. Shared master LFO phase; R lane reads LFO at base +
// 90° for audible L/R decorrelation on mono input. With STEREO_INPUT, L and R
// process their respective input channels.

[[gnu::always_inline]]
inline void op_effect_flanger(ExecutionContext& ctx, const Instruction& inst) {
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* rate = ctx.buffers->get(inst.inputs[1]);
    const float* depth = ctx.buffers->get(inst.inputs[2]);
    const float* min_delay_in = ctx.buffers->get(inst.inputs[3]);
    const float* max_delay_in = ctx.buffers->get(inst.inputs[4]);
    auto& state = ctx.states->get_or_create<FlangerState>(inst.state_id);

    // Decode feedback from high 4 bits (legacy phaser-style packing kept
    // until flanger feedback is migrated to ExtendedParams in a follow-up).
    float feedback = (static_cast<float>((inst.rate >> 4) & 0x0F) / 7.5f) - 1.0f;
    feedback = std::clamp(feedback, -0.99f, 0.99f);

    // Extended params: ext[0] = lfo_phase (turns, default 0.25). Resolve
    // the slot once — at audio rate the slot is a const buffer pointer,
    // at control rate it's a constant scalar.
    const auto* ext = ctx.states->get_if<ExtendedParams<1>>(ext_params_state_id(inst.state_id));
    const float* lfo_phase_buf = nullptr;
    float lfo_phase_const = STEREO_LFO_OFFSET_DEFAULT_TURNS;
    if (ext) {
        const auto& slot = ext->params[0];
        if (slot.is_constant()) {
            lfo_phase_const = slot.constant;
        } else {
            lfo_phase_buf = ctx.buffers->get(slot.buffer_idx);
        }
    }

    // Ensure both per-channel buffers are allocated from arena
    state.ensure_buffers(ctx.arena);

    float inv_sample_rate = 1.0f / ctx.sample_rate;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float x_l = input[i];
        float x_r = stereo_in ? input_r[i] : x_l;

        // Runtime tunable parameters (use defaults if zero/negative)
        float min_delay_ms = min_delay_in[i] > 0.0f ? min_delay_in[i] : FLANGER_MIN_DELAY_DEFAULT;
        float max_delay_ms = max_delay_in[i] > 0.0f ? max_delay_in[i] : FLANGER_MAX_DELAY_DEFAULT;
        float center_delay_ms = (min_delay_ms + max_delay_ms) * 0.5f;
        float depth_range_ms = (max_delay_ms - min_delay_ms) * 0.5f;

        // Update shared LFO phase (master timebase)
        float lfo_rate = std::clamp(rate[i], 0.1f, 10.0f);
        state.lfo_phase += lfo_rate * inv_sample_rate;
        if (state.lfo_phase >= 1.0f) state.lfo_phase -= 1.0f;

        float d = std::clamp(depth[i], 0.0f, 1.0f);

        // L lane: LFO at master phase. Bit-identical to legacy mono path
        // when input is mono.
        float lfo_l = std::sin(state.lfo_phase * TWO_PI);
        float delay_ms_l = center_delay_ms + lfo_l * d * depth_range_ms;
        float delay_smp_l = delay_ms_l * 0.001f * ctx.sample_rate;
        delay_smp_l = std::min(delay_smp_l, static_cast<float>(FlangerState::MAX_FLANGER_SAMPLES - 1));
        float delayed_l = delay_read_linear(state.buffer[0], FlangerState::MAX_FLANGER_SAMPLES,
                                            state.write_pos[0], delay_smp_l);
        state.buffer[0][state.write_pos[0]] = x_l + feedback * delayed_l;
        state.write_pos[0] = (state.write_pos[0] + 1) % FlangerState::MAX_FLANGER_SAMPLES;
        out_l[i] = delayed_l;

        // R lane: LFO offset by user-tunable lfo_phase turns (default 0.25
        // = 90°). Decorrelates L/R on mono input; on stereo input adds
        // spatial movement on top of per-channel processing.
        const float lfo_offset = lfo_phase_buf ? lfo_phase_buf[i] : lfo_phase_const;
        float r_phase = state.lfo_phase + lfo_offset;
        r_phase -= std::floor(r_phase);
        float lfo_r = std::sin(r_phase * TWO_PI);
        float delay_ms_r = center_delay_ms + lfo_r * d * depth_range_ms;
        float delay_smp_r = delay_ms_r * 0.001f * ctx.sample_rate;
        delay_smp_r = std::min(delay_smp_r, static_cast<float>(FlangerState::MAX_FLANGER_SAMPLES - 1));
        float delayed_r = delay_read_linear(state.buffer[1], FlangerState::MAX_FLANGER_SAMPLES,
                                            state.write_pos[1], delay_smp_r);
        state.buffer[1][state.write_pos[1]] = x_r + feedback * delayed_r;
        state.write_pos[1] = (state.write_pos[1] + 1) % FlangerState::MAX_FLANGER_SAMPLES;
        out_r[i] = delayed_r;
    }
}

// Default constants for chorus
constexpr float CHORUS_BASE_DELAY_DEFAULT = 20.0f;  // ms
constexpr float CHORUS_DEPTH_RANGE_DEFAULT = 10.0f; // ms

// ============================================================================
// EFFECT_CHORUS: Stereo-Native Multi-Voice Chorus
// ============================================================================
// in0: input signal (mono auto-broadcasts; stereo via STEREO_INPUT)
// in1: LFO rate (Hz, 0.1-5)
// in2: depth (0.0-1.0)
// in3: base_delay - base chorus delay in ms (default 20)
// in4: depth_range - modulation depth in ms (default 10)
//
// Three detuned delay lines per channel create rich, thick stereo. Stereo-
// native: per-channel delay lines + write_pos. Master LFO phase shared; R lane
// reads LFO at +90° so mono input widens to a true stereo image.

[[gnu::always_inline]]
inline void op_effect_chorus(ExecutionContext& ctx, const Instruction& inst) {
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* rate = ctx.buffers->get(inst.inputs[1]);
    const float* depth = ctx.buffers->get(inst.inputs[2]);
    const float* base_delay_in = ctx.buffers->get(inst.inputs[3]);
    const float* depth_range_in = ctx.buffers->get(inst.inputs[4]);
    auto& state = ctx.states->get_or_create<ChorusState>(inst.state_id);

    state.ensure_buffers(ctx.arena);

    // Extended params: ext[0] = lfo_phase (turns, default 0.25).
    const auto* ext = ctx.states->get_if<ExtendedParams<1>>(ext_params_state_id(inst.state_id));
    const float* lfo_phase_buf = nullptr;
    float lfo_phase_const = STEREO_LFO_OFFSET_DEFAULT_TURNS;
    if (ext) {
        const auto& slot = ext->params[0];
        if (slot.is_constant()) {
            lfo_phase_const = slot.constant;
        } else {
            lfo_phase_buf = ctx.buffers->get(slot.buffer_idx);
        }
    }

    // LFO phase offsets for each voice (spread across cycle)
    constexpr float PHASE_OFFSETS[ChorusState::NUM_VOICES] = {0.0f, 0.33f, 0.67f};

    float inv_sample_rate = 1.0f / ctx.sample_rate;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float x_l = input[i];
        float x_r = stereo_in ? input_r[i] : x_l;

        // Runtime tunable parameters (use defaults if zero/negative)
        float base_delay_ms = base_delay_in[i] > 0.0f ? base_delay_in[i] : CHORUS_BASE_DELAY_DEFAULT;
        float depth_range_ms = depth_range_in[i] > 0.0f ? depth_range_in[i] : CHORUS_DEPTH_RANGE_DEFAULT;

        // Update shared master LFO phase
        float lfo_rate = std::clamp(rate[i], 0.1f, 5.0f);
        state.lfo_phase += lfo_rate * inv_sample_rate;
        if (state.lfo_phase >= 1.0f) state.lfo_phase -= 1.0f;

        float d = std::clamp(depth[i], 0.0f, 1.0f);

        // Process each lane independently with per-channel buffer/write_pos.
        // L reads LFO at master phase; R reads at master + lfo_phase turns
        // (default 0.25 = 90°, user-tunable via extended params).
        const float r_offset = lfo_phase_buf ? lfo_phase_buf[i] : lfo_phase_const;
        float wet_lr[2] = {0.0f, 0.0f};
        for (std::size_t ch = 0; ch < 2; ++ch) {
            float x = (ch == 0) ? x_l : x_r;
            float ch_phase_offset = (ch == 0) ? 0.0f : r_offset;

            float wet = 0.0f;
            for (std::size_t v = 0; v < ChorusState::NUM_VOICES; ++v) {
                float voice_phase = state.lfo_phase + PHASE_OFFSETS[v] + ch_phase_offset;
                voice_phase -= std::floor(voice_phase);  // wrap to [0,1)

                float lfo = std::sin(voice_phase * TWO_PI);
                float delay_ms = base_delay_ms + lfo * d * depth_range_ms;
                float delay_samples = delay_ms * 0.001f * ctx.sample_rate;
                delay_samples = std::clamp(delay_samples, 1.0f,
                                           static_cast<float>(ChorusState::MAX_CHORUS_SAMPLES - 1));

                wet += delay_read_linear(state.buffer[ch], ChorusState::MAX_CHORUS_SAMPLES,
                                         state.write_pos[ch], delay_samples);
            }
            wet /= static_cast<float>(ChorusState::NUM_VOICES);

            // Write dry input to per-channel delay line
            state.buffer[ch][state.write_pos[ch]] = x;
            state.write_pos[ch] = (state.write_pos[ch] + 1) % ChorusState::MAX_CHORUS_SAMPLES;

            wet_lr[ch] = wet;
        }

        out_l[i] = wet_lr[0];
        out_r[i] = wet_lr[1];
    }
}

// Default constants for phaser
constexpr float PHASER_MIN_FREQ_DEFAULT = 200.0f;   // Hz
constexpr float PHASER_MAX_FREQ_DEFAULT = 4000.0f;  // Hz

// ============================================================================
// EFFECT_PHASER: Stereo-Native All-Pass Phaser
// ============================================================================
// in0: input signal (mono auto-broadcasts; stereo via STEREO_INPUT)
// in1: LFO rate (Hz, 0.1-5)
// in2: depth (0.0-1.0)
// in3: min_freq - sweep range low in Hz (default 200)
// in4: max_freq - sweep range high in Hz (default 4000)
// ext params (ExtendedParams<3>):
//   ext[0]: feedback (0.0-0.99, default 0.5)
//   ext[1]: stages   (2-12, default 4)
//   ext[2]: lfo_phase (turns, default 0.25 = 90°)
//
// Cascaded first-order allpass filters with modulated center frequencies.
// Stereo-native: per-channel allpass state arrays. Shared master LFO phase;
// R lane reads LFO at the user-tunable lfo_phase offset for stereo notch
// sweep. Phaser was the proof migration for the unified extended-param
// mechanism (PRD prd-extended-params §6b) — feedback and stages were
// previously bit-packed into inst.rate.

[[gnu::always_inline]]
inline void op_effect_phaser(ExecutionContext& ctx, const Instruction& inst) {
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* rate = ctx.buffers->get(inst.inputs[1]);
    const float* depth = ctx.buffers->get(inst.inputs[2]);
    const float* min_freq_in = ctx.buffers->get(inst.inputs[3]);
    const float* max_freq_in = ctx.buffers->get(inst.inputs[4]);
    auto& state = ctx.states->get_or_create<PhaserState>(inst.state_id);

    // Extended params: ext[0]=feedback, ext[1]=stages, ext[2]=lfo_phase.
    // Resolve each slot once outside the sample loop; slots are either a
    // constant scalar or a buffer pointer.
    const auto* ext = ctx.states->get_if<ExtendedParams<3>>(ext_params_state_id(inst.state_id));
    auto resolve_slot = [&](std::size_t idx, float default_val,
                            const float*& out_buf, float& out_const) {
        out_buf = nullptr;
        out_const = default_val;
        if (!ext) return;
        const auto& slot = ext->params[idx];
        if (slot.is_constant()) {
            out_const = slot.constant;
        } else {
            out_buf = ctx.buffers->get(slot.buffer_idx);
        }
    };
    const float* feedback_buf = nullptr; float feedback_const = 0.5f;
    const float* stages_buf   = nullptr; float stages_const   = 4.0f;
    const float* lfo_phase_buf = nullptr; float lfo_phase_const = STEREO_LFO_OFFSET_DEFAULT_TURNS;
    resolve_slot(0, 0.5f,  feedback_buf,  feedback_const);
    resolve_slot(1, 4.0f,  stages_buf,    stages_const);
    resolve_slot(2, STEREO_LFO_OFFSET_DEFAULT_TURNS, lfo_phase_buf, lfo_phase_const);

    float inv_sample_rate = 1.0f / ctx.sample_rate;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float x_in_l = input[i];
        float x_in_r = stereo_in ? input_r[i] : x_in_l;

        // Runtime tunable parameters (use defaults if zero/negative)
        float min_freq = min_freq_in[i] > 0.0f ? min_freq_in[i] : PHASER_MIN_FREQ_DEFAULT;
        float max_freq = max_freq_in[i] > 0.0f ? max_freq_in[i] : PHASER_MAX_FREQ_DEFAULT;

        const float feedback = std::clamp(
            feedback_buf ? feedback_buf[i] : feedback_const, 0.0f, 0.99f);
        const float stages_f = stages_buf ? stages_buf[i] : stages_const;
        const std::size_t num_stages = std::clamp(
            static_cast<std::size_t>(stages_f + 0.5f),
            std::size_t{2}, PhaserState::NUM_STAGES);
        const float r_offset = lfo_phase_buf ? lfo_phase_buf[i] : lfo_phase_const;

        // Update shared LFO phase
        float lfo_rate = std::clamp(rate[i], 0.1f, 5.0f);
        state.lfo_phase += lfo_rate * inv_sample_rate;
        if (state.lfo_phase >= 1.0f) state.lfo_phase -= 1.0f;

        float d = std::clamp(depth[i], 0.0f, 1.0f);

        // Per-channel processing. L uses master phase; R uses master +
        // lfo_phase turns (default 0.25 = 90°, user-tunable).
        float y_lr[2] = {0.0f, 0.0f};
        for (std::size_t ch = 0; ch < 2; ++ch) {
            float ch_phase_offset = (ch == 0) ? 0.0f : r_offset;
            float ch_phase = state.lfo_phase + ch_phase_offset;
            ch_phase -= std::floor(ch_phase);

            float lfo = std::sin(ch_phase * TWO_PI);
            float freq_factor = std::exp(lfo * d * 2.0f);
            float center_freq = std::sqrt(min_freq * max_freq) * freq_factor;
            center_freq = std::clamp(center_freq, min_freq, max_freq);

            float tan_val = std::tan(PI * center_freq * inv_sample_rate);
            float a = (tan_val - 1.0f) / (tan_val + 1.0f);

            float x_in = (ch == 0) ? x_in_l : x_in_r;
            float x = x_in + feedback * state.last_output[ch];

            for (std::size_t s = 0; s < num_stages; ++s) {
                float y = a * x + state.allpass_state[ch][s] - a * state.allpass_delay[ch][s];
                state.allpass_state[ch][s] = x;
                state.allpass_delay[ch][s] = y;
                x = y;
            }

            state.last_output[ch] = x;
            y_lr[ch] = x_in + x;  // dry + allpass cascade
        }

        out_l[i] = y_lr[0];
        out_r[i] = y_lr[1];
    }
}

}  // namespace cedar
