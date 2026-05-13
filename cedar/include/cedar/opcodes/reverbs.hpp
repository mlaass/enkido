#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
#include "dsp_utils.hpp"
#include <cmath>
#include <algorithm>

namespace cedar {

// DC blocker coefficient: R ≈ 0.997 gives ~23Hz cutoff at 48kHz
// y[n] = x[n] - x[n-1] + R * y[n-1]
constexpr float DC_BLOCKER_R = 0.997f;

// Default constants for Freeverb
constexpr float FREEVERB_ROOM_SCALE_DEFAULT = 0.28f;
constexpr float FREEVERB_ROOM_OFFSET_DEFAULT = 0.7f;

// ============================================================================
// REVERB_FREEVERB: Schroeder-Moorer Reverb (Freeverb Algorithm)
// ============================================================================
// in0: input signal
// in1: room size (0.0-1.0)
// in2: damping (0.0-1.0)
// in3: room_scale - density factor (default 0.28)
// in4: room_offset - decay baseline (default 0.7)
// rate: wet/dry mix (0-255 -> 0.0-1.0)
//
// Classic algorithm: 8 parallel lowpass-feedback comb filters summed,
// then through 4 series allpass filters. Creates lush, dense reverb.

[[gnu::always_inline]]
inline void op_reverb_freeverb(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* room_size = ctx.buffers->get(inst.inputs[1]);
    const float* damping = ctx.buffers->get(inst.inputs[2]);
    const float* room_scale_in = ctx.buffers->get(inst.inputs[3]);
    const float* room_offset_in = ctx.buffers->get(inst.inputs[4]);
    auto& state = ctx.states->get_or_create<FreeverbState>(inst.state_id);

    // Ensure buffers are allocated from arena
    state.ensure_buffers(ctx.arena);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float x = input[i];
        float room = std::clamp(room_size[i], 0.0f, 1.0f);
        float damp = std::clamp(damping[i], 0.0f, 1.0f);

        // Runtime tunable parameters (use defaults if zero/negative)
        float room_scale = room_scale_in[i] > 0.0f ? room_scale_in[i] : FREEVERB_ROOM_SCALE_DEFAULT;
        float room_offset = room_offset_in[i] > 0.0f ? room_offset_in[i] : FREEVERB_ROOM_OFFSET_DEFAULT;

        // Feedback coefficient from room size
        float feedback = room * room_scale + room_offset;

        // Sum output from all 8 comb filters in parallel
        float comb_sum = 0.0f;
        for (std::size_t c = 0; c < FreeverbState::NUM_COMBS; ++c) {
            float* buffer = state.comb_buffers[c];
            std::size_t size = FreeverbState::COMB_SIZES[c];

            // Read from delay line
            float delayed = buffer[state.comb_pos[c]];

            // Lowpass filter in feedback path (damping)
            state.comb_filter_state[c] = delayed * (1.0f - damp) + state.comb_filter_state[c] * damp;

            // DC blocker in feedback path to prevent low-frequency buildup
            float dc_in = state.comb_filter_state[c];
            float dc_out = dc_in - state.dc_x1[c] + DC_BLOCKER_R * state.dc_y1[c];
            state.dc_x1[c] = dc_in;
            state.dc_y1[c] = dc_out;

            // Write with feedback
            buffer[state.comb_pos[c]] = x + feedback * dc_out;
            state.comb_pos[c] = (state.comb_pos[c] + 1) % size;

            comb_sum += delayed;
        }

        // Comb output — allpass chain provides ~16x peak reduction
        float y = comb_sum;

        // Series allpass filters for diffusion
        constexpr float ALLPASS_GAIN = 0.5f;
        for (std::size_t a = 0; a < FreeverbState::NUM_ALLPASSES; ++a) {
            float* buffer = state.allpass_buffers[a];
            std::size_t size = FreeverbState::ALLPASS_SIZES[a];

            float delayed = buffer[state.allpass_pos[a]];
            float output = delayed - ALLPASS_GAIN * y;
            buffer[state.allpass_pos[a]] = y + ALLPASS_GAIN * output;
            state.allpass_pos[a] = (state.allpass_pos[a] + 1) % size;

            y = output;
        }

        out[i] = y;
    }
}

// Default constants for Dattorro reverb
constexpr float DATTORRO_INPUT_DIFFUSION_DEFAULT = 0.75f;
constexpr float DATTORRO_DECAY_DIFFUSION_DEFAULT = 0.625f;
constexpr float DATTORRO_LFO_RATE_DEFAULT = 0.5f;

// ============================================================================
// REVERB_DATTORRO: Dattorro Plate Reverb
// ============================================================================
// in0: input signal
// in1: decay (0.0-0.99)
// in2: pre-delay (ms, 0-100)
// in3: input_diffusion - input smoothing (default 0.75)
// in4: decay_diffusion - tail smoothing (default 0.625)
// rate: damping (low 4 bits 0-15 -> 0.0-1.0), modulation depth (high 4 bits 0-15 -> 0.0-1.0)
//
// High-quality plate reverb algorithm with modulation for richness.
// Uses input diffusion network + figure-8 tank topology.

[[gnu::always_inline]]
inline void op_reverb_dattorro(ExecutionContext& ctx, const Instruction& inst) {
    // Stereo-native: writes out_buffer (L) and out_buffer+1 (R) in one call.
    // When STEREO_INPUT is set in inst.flags, the primary signal input is a
    // stereo pair (inputs[0] = L, inputs[0]+1 = R). When unset, the mono input
    // is auto-broadcast to both internal lanes. See
    // prd-stereo-native-opcodes.md for the flag truth table.
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(inst.out_buffer + 1);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* decay = ctx.buffers->get(inst.inputs[1]);
    const float* predelay_ms = ctx.buffers->get(inst.inputs[2]);
    const float* input_diffusion_in = ctx.buffers->get(inst.inputs[3]);
    const float* decay_diffusion_in = ctx.buffers->get(inst.inputs[4]);
    auto& state = ctx.states->get_or_create<DattorroState>(inst.state_id);

    float damping = static_cast<float>(inst.rate & 0x0F) / 15.0f;
    float mod_depth = static_cast<float>((inst.rate >> 4) & 0x0F) / 15.0f;

    // Ensure buffers are allocated from arena
    state.ensure_buffers(ctx.arena);

    float inv_sample_rate = 1.0f / ctx.sample_rate;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float x_in_l = input[i];
        float x_in_r = stereo_in ? input_r[i] : x_in_l;
        float dec = std::clamp(decay[i], 0.0f, 0.99f);
        float pre_ms = std::clamp(predelay_ms[i], 0.0f, 100.0f);

        // Runtime tunable parameters (use defaults if zero/negative)
        float input_diffusion = input_diffusion_in[i] > 0.0f ? input_diffusion_in[i] : DATTORRO_INPUT_DIFFUSION_DEFAULT;
        float decay_diffusion = decay_diffusion_in[i] > 0.0f ? decay_diffusion_in[i] : DATTORRO_DECAY_DIFFUSION_DEFAULT;
        float lfo_rate = DATTORRO_LFO_RATE_DEFAULT;  // Fixed at default

        // Pre-delay (per-channel duplicated path)
        float predelay_samples = pre_ms * 0.001f * ctx.sample_rate;
        predelay_samples = std::min(predelay_samples, static_cast<float>(DattorroState::PREDELAY_SIZE - 1));

        float x_l;
        float x_r;
        {
            state.predelay_buffer[0][state.predelay_pos[0]] = x_in_l;
            std::size_t read_pos = (state.predelay_pos[0] + DattorroState::PREDELAY_SIZE -
                                    static_cast<std::size_t>(predelay_samples)) % DattorroState::PREDELAY_SIZE;
            x_l = state.predelay_buffer[0][read_pos];
            state.predelay_pos[0] = (state.predelay_pos[0] + 1) % DattorroState::PREDELAY_SIZE;
        }
        {
            state.predelay_buffer[1][state.predelay_pos[1]] = x_in_r;
            std::size_t read_pos = (state.predelay_pos[1] + DattorroState::PREDELAY_SIZE -
                                    static_cast<std::size_t>(predelay_samples)) % DattorroState::PREDELAY_SIZE;
            x_r = state.predelay_buffer[1][read_pos];
            state.predelay_pos[1] = (state.predelay_pos[1] + 1) % DattorroState::PREDELAY_SIZE;
        }

        // Input diffusion: 4 allpass filters per channel
        for (std::size_t d = 0; d < DattorroState::NUM_INPUT_DIFFUSERS; ++d) {
            std::size_t size = DattorroState::INPUT_DIFFUSER_SIZES[d];

            float* buf_l = state.input_diffusers[0][d];
            float delayed_l = buf_l[state.input_pos[0][d]];
            float output_l = delayed_l - input_diffusion * x_l;
            buf_l[state.input_pos[0][d]] = x_l + input_diffusion * output_l;
            state.input_pos[0][d] = (state.input_pos[0][d] + 1) % size;
            x_l = output_l;

            float* buf_r = state.input_diffusers[1][d];
            float delayed_r = buf_r[state.input_pos[1][d]];
            float output_r = delayed_r - input_diffusion * x_r;
            buf_r[state.input_pos[1][d]] = x_r + input_diffusion * output_r;
            state.input_pos[1][d] = (state.input_pos[1][d] + 1) % size;
            x_r = output_r;
        }

        // Update modulation LFO
        state.mod_phase += lfo_rate * inv_sample_rate;
        if (state.mod_phase >= 1.0f) state.mod_phase -= 1.0f;

        // Tank processing (figure-8 topology)
        // Branch 0 ("left tank"): decay diffuser 0 -> delay 0 -> damp 0 -> DC blocker 0
        // Branch 1 ("right tank"): decay diffuser 1 -> delay 1 -> damp 1 -> DC blocker 1
        // Each branch's feedback drives the OTHER branch's input (figure-8).

        // Get feedback from opposite branch + per-channel diffused signal
        float left_in = x_l + dec * state.tank_feedback[1];
        float right_in = x_r + dec * state.tank_feedback[0];

        // Process left branch
        {
            // Decay diffuser 1
            float* buffer = state.decay_diffusers[0];
            std::size_t size = DattorroState::DECAY_DIFFUSER_SIZES[0];
            float delayed = buffer[state.decay_pos[0]];
            float output = delayed - decay_diffusion * left_in;
            buffer[state.decay_pos[0]] = left_in + decay_diffusion * output;
            state.decay_pos[0] = (state.decay_pos[0] + 1) % size;
            left_in = output;

            // Delay 1 with modulation
            float mod = std::sin(state.mod_phase * TWO_PI) * mod_depth * 8.0f;
            float delay_samples = static_cast<float>(DattorroState::DELAY_SIZES[0]) + mod;
            delay_samples = std::clamp(delay_samples, 1.0f, static_cast<float>(DattorroState::MAX_DELAY_SIZE - 1));

            left_in = delay_read_linear(state.delays[0], DattorroState::MAX_DELAY_SIZE,
                                         state.delay_pos[0], delay_samples);
            state.delays[0][state.delay_pos[0]] = output * dec;
            state.delay_pos[0] = (state.delay_pos[0] + 1) % DattorroState::MAX_DELAY_SIZE;

            // Damping filter
            state.damp_state[0] = left_in * (1.0f - damping) + state.damp_state[0] * damping;
            left_in = state.damp_state[0];

            // DC blocker in feedback path
            float dc_out_l = left_in - state.dc_x1[0] + DC_BLOCKER_R * state.dc_y1[0];
            state.dc_x1[0] = left_in;
            state.dc_y1[0] = dc_out_l;
            left_in = dc_out_l;

            state.tank_feedback[0] = left_in;
        }

        // Process right branch
        {
            // Decay diffuser 2
            float* buffer = state.decay_diffusers[1];
            std::size_t size = DattorroState::DECAY_DIFFUSER_SIZES[1];
            float delayed = buffer[state.decay_pos[1]];
            float output = delayed - decay_diffusion * right_in;
            buffer[state.decay_pos[1]] = right_in + decay_diffusion * output;
            state.decay_pos[1] = (state.decay_pos[1] + 1) % size;
            right_in = output;

            // Delay 2 with modulation (opposite phase)
            float mod = std::sin((state.mod_phase + 0.5f) * TWO_PI) * mod_depth * 8.0f;
            float delay_samples = static_cast<float>(DattorroState::DELAY_SIZES[1]) + mod;
            delay_samples = std::clamp(delay_samples, 1.0f, static_cast<float>(DattorroState::MAX_DELAY_SIZE - 1));

            right_in = delay_read_linear(state.delays[1], DattorroState::MAX_DELAY_SIZE,
                                          state.delay_pos[1], delay_samples);
            state.delays[1][state.delay_pos[1]] = output * dec;
            state.delay_pos[1] = (state.delay_pos[1] + 1) % DattorroState::MAX_DELAY_SIZE;

            // Damping filter
            state.damp_state[1] = right_in * (1.0f - damping) + state.damp_state[1] * damping;
            right_in = state.damp_state[1];

            // DC blocker in feedback path
            float dc_out_r = right_in - state.dc_x1[1] + DC_BLOCKER_R * state.dc_y1[1];
            state.dc_x1[1] = right_in;
            state.dc_y1[1] = dc_out_r;
            right_in = dc_out_r;

            state.tank_feedback[1] = right_in;
        }

        // Stereo output: L tap = branch-0 feedback, R tap = branch-1 feedback.
        // (Mono pre-PRD output was the 0.5*(L+R) average; recover with
        // (out_l + out_r) * 0.5 if needed.)
        out_l[i] = state.tank_feedback[0];
        out_r[i] = state.tank_feedback[1];
    }
}

// ============================================================================
// REVERB_FDN: Feedback Delay Network
// ============================================================================
// in0: input signal
// in1: decay (0.0-0.99)
// in2: damping (0.0-1.0)
// rate: room size modifier (0-255 scales delay times, 128 = 1.0x)
//
// 4x4 FDN with Hadamard mixing matrix. Provides dense, smooth reverb
// with controllable decay. Good for realistic room simulation.

[[gnu::always_inline]]
inline void op_reverb_fdn(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* decay = ctx.buffers->get(inst.inputs[1]);
    const float* damping = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<FDNState>(inst.state_id);

    float size_mod = 0.5f + static_cast<float>(inst.rate) / 255.0f;  // 0.5-1.5

    state.ensure_buffers(ctx.arena);

    // Hadamard matrix coefficients (normalized 4x4)
    // H = 0.5 * [[1,1,1,1], [1,-1,1,-1], [1,1,-1,-1], [1,-1,-1,1]]
    constexpr float H = 0.5f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float x = input[i];
        float dec = std::clamp(decay[i], 0.0f, 0.99f);
        float damp = std::clamp(damping[i], 0.0f, 1.0f);

        // Read from all delay lines
        float delayed[FDNState::NUM_DELAYS];
        for (std::size_t d = 0; d < FDNState::NUM_DELAYS; ++d) {
            std::size_t actual_size = static_cast<std::size_t>(
                static_cast<float>(FDNState::DELAY_SIZES[d]) * size_mod);
            actual_size = std::clamp(actual_size, std::size_t{1}, FDNState::MAX_DELAY_SIZE - 1);

            std::size_t read_pos = (state.write_pos[d] + FDNState::MAX_DELAY_SIZE - actual_size)
                                   % FDNState::MAX_DELAY_SIZE;
            delayed[d] = state.delay_buffers[d][read_pos];

            // Apply damping (lowpass)
            state.damp_state[d] = delayed[d] * (1.0f - damp) + state.damp_state[d] * damp;

            // DC blocker in feedback path
            float dc_in = state.damp_state[d];
            float dc_out = dc_in - state.dc_x1[d] + DC_BLOCKER_R * state.dc_y1[d];
            state.dc_x1[d] = dc_in;
            state.dc_y1[d] = dc_out;
            delayed[d] = dc_out;
        }

        // Hadamard mixing matrix
        float mixed[FDNState::NUM_DELAYS];
        mixed[0] = H * (delayed[0] + delayed[1] + delayed[2] + delayed[3]);
        mixed[1] = H * (delayed[0] - delayed[1] + delayed[2] - delayed[3]);
        mixed[2] = H * (delayed[0] + delayed[1] - delayed[2] - delayed[3]);
        mixed[3] = H * (delayed[0] - delayed[1] - delayed[2] + delayed[3]);

        // Write to delay lines with input injection and decay
        float output_sum = 0.0f;
        for (std::size_t d = 0; d < FDNState::NUM_DELAYS; ++d) {
            state.delay_buffers[d][state.write_pos[d]] = x + mixed[d] * dec;
            state.write_pos[d] = (state.write_pos[d] + 1) % FDNState::MAX_DELAY_SIZE;
            output_sum += delayed[d];
        }

        // Output is sum of all delay taps, normalized by number of delays
        out[i] = output_sum * 0.25f;
    }
}

}  // namespace cedar
