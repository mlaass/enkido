#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
#include "dsp_utils.hpp"
#include <cmath>
#include <algorithm>

namespace cedar {

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

    float mix = static_cast<float>(inst.rate) / 255.0f;

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

            // Write with feedback
            buffer[state.comb_pos[c]] = x + feedback * state.comb_filter_state[c];
            state.comb_pos[c] = (state.comb_pos[c] + 1) % size;

            comb_sum += delayed;
        }

        // Normalize comb output
        float y = comb_sum * 0.125f;  // 1/8

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

        // Wet/dry mix
        out[i] = x * (1.0f - mix) + y * mix;
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
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
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
        float x = input[i];
        float dec = std::clamp(decay[i], 0.0f, 0.99f);
        float pre_ms = std::clamp(predelay_ms[i], 0.0f, 100.0f);

        // Runtime tunable parameters (use defaults if zero/negative)
        float input_diffusion = input_diffusion_in[i] > 0.0f ? input_diffusion_in[i] : DATTORRO_INPUT_DIFFUSION_DEFAULT;
        float decay_diffusion = decay_diffusion_in[i] > 0.0f ? decay_diffusion_in[i] : DATTORRO_DECAY_DIFFUSION_DEFAULT;
        float lfo_rate = DATTORRO_LFO_RATE_DEFAULT;  // Fixed at default

        // Pre-delay
        float predelay_samples = pre_ms * 0.001f * ctx.sample_rate;
        predelay_samples = std::min(predelay_samples, static_cast<float>(DattorroState::PREDELAY_SIZE - 1));
        state.predelay_buffer[state.predelay_pos] = x;
        std::size_t read_pos = (state.predelay_pos + DattorroState::PREDELAY_SIZE -
                                static_cast<std::size_t>(predelay_samples)) % DattorroState::PREDELAY_SIZE;
        x = state.predelay_buffer[read_pos];
        state.predelay_pos = (state.predelay_pos + 1) % DattorroState::PREDELAY_SIZE;

        // Input diffusion: 4 allpass filters
        for (std::size_t d = 0; d < DattorroState::NUM_INPUT_DIFFUSERS; ++d) {
            float* buffer = state.input_diffusers[d];
            std::size_t size = DattorroState::INPUT_DIFFUSER_SIZES[d];

            float delayed = buffer[state.input_pos[d]];
            float output = delayed - input_diffusion * x;
            buffer[state.input_pos[d]] = x + input_diffusion * output;
            state.input_pos[d] = (state.input_pos[d] + 1) % size;
            x = output;
        }

        // Update modulation LFO
        state.mod_phase += lfo_rate * inv_sample_rate;
        if (state.mod_phase >= 1.0f) state.mod_phase -= 1.0f;

        // Tank processing (figure-8 topology)
        // Left branch: decay diffuser 1 -> delay 1 -> damp -> decay diffuser 2 -> delay 2
        // Right branch: same but feeds back to left

        // Get feedback from opposite branch
        float left_in = x + dec * state.tank_feedback[1];
        float right_in = x + dec * state.tank_feedback[0];

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

            state.tank_feedback[1] = right_in;
        }

        // Output is sum of taps from both tank branches
        out[i] = (state.tank_feedback[0] + state.tank_feedback[1]) * 0.5f;
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
            delayed[d] = state.damp_state[d];
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

        // Output is average of all delay taps
        out[i] = output_sum * 0.25f;
    }
}

}  // namespace cedar
