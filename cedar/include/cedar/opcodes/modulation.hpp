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

// ============================================================================
// EFFECT_FLANGER: Flanger Effect
// ============================================================================
// in0: input signal
// in1: LFO rate (Hz, 0.1-10)
// in2: depth (0.0-1.0)
// in3: min_delay - minimum sweep point in ms (default 0.1)
// in4: max_delay - maximum sweep point in ms (default 10.0)
// rate: feedback (high 4 bits 0-15 -> -0.99 to 0.99)
//
// Short modulated delay (0.1-10ms) with feedback. Creates metallic,
// jet-plane-like sweeping effect. Classic for guitars and synths.
// Outputs 100% wet signal - user can mix dry/wet manually if needed.

[[gnu::always_inline]]
inline void op_effect_flanger(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* rate = ctx.buffers->get(inst.inputs[1]);
    const float* depth = ctx.buffers->get(inst.inputs[2]);
    const float* min_delay_in = ctx.buffers->get(inst.inputs[3]);
    const float* max_delay_in = ctx.buffers->get(inst.inputs[4]);
    auto& state = ctx.states->get_or_create<FlangerState>(inst.state_id);

    // Decode feedback from high 4 bits
    float feedback = (static_cast<float>((inst.rate >> 4) & 0x0F) / 7.5f) - 1.0f;
    feedback = std::clamp(feedback, -0.99f, 0.99f);

    // Ensure buffer is allocated from arena
    state.ensure_buffer(ctx.arena);

    float inv_sample_rate = 1.0f / ctx.sample_rate;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Runtime tunable parameters (use defaults if zero/negative)
        float min_delay_ms = min_delay_in[i] > 0.0f ? min_delay_in[i] : FLANGER_MIN_DELAY_DEFAULT;
        float max_delay_ms = max_delay_in[i] > 0.0f ? max_delay_in[i] : FLANGER_MAX_DELAY_DEFAULT;
        float center_delay_ms = (min_delay_ms + max_delay_ms) * 0.5f;
        float depth_range_ms = (max_delay_ms - min_delay_ms) * 0.5f;

        // Update LFO phase
        float lfo_rate = std::clamp(rate[i], 0.1f, 10.0f);
        state.lfo_phase += lfo_rate * inv_sample_rate;
        if (state.lfo_phase >= 1.0f) state.lfo_phase -= 1.0f;

        // Calculate modulated delay time
        float lfo = std::sin(state.lfo_phase * TWO_PI);
        float d = std::clamp(depth[i], 0.0f, 1.0f);
        float delay_ms = center_delay_ms + lfo * d * depth_range_ms;
        float delay_samples = delay_ms * 0.001f * ctx.sample_rate;
        delay_samples = std::min(delay_samples, static_cast<float>(FlangerState::MAX_FLANGER_SAMPLES - 1));

        // Read from delay line
        float delayed = delay_read_linear(state.buffer, FlangerState::MAX_FLANGER_SAMPLES,
                                          state.write_pos, delay_samples);

        // Write with feedback
        state.buffer[state.write_pos] = input[i] + feedback * delayed;
        state.write_pos = (state.write_pos + 1) % FlangerState::MAX_FLANGER_SAMPLES;

        // Output 100% wet (user can mix dry/wet manually if needed)
        out[i] = delayed;
    }
}

// Default constants for chorus
constexpr float CHORUS_BASE_DELAY_DEFAULT = 20.0f;  // ms
constexpr float CHORUS_DEPTH_RANGE_DEFAULT = 10.0f; // ms

// ============================================================================
// EFFECT_CHORUS: Multi-Voice Chorus
// ============================================================================
// in0: input signal
// in1: LFO rate (Hz, 0.1-5)
// in2: depth (0.0-1.0)
// in3: base_delay - base chorus delay in ms (default 20)
// in4: depth_range - modulation depth in ms (default 10)
//
// Multiple detuned delay lines create a rich, thick sound.
// Uses 3 voices with slightly offset LFO phases for maximum width.
// Outputs 100% wet signal - user can mix dry/wet manually if needed.

[[gnu::always_inline]]
inline void op_effect_chorus(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* rate = ctx.buffers->get(inst.inputs[1]);
    const float* depth = ctx.buffers->get(inst.inputs[2]);
    const float* base_delay_in = ctx.buffers->get(inst.inputs[3]);
    const float* depth_range_in = ctx.buffers->get(inst.inputs[4]);
    auto& state = ctx.states->get_or_create<ChorusState>(inst.state_id);

    // Ensure buffer is allocated from arena
    state.ensure_buffer(ctx.arena);

    // LFO phase offsets for each voice (spread across cycle)
    constexpr float PHASE_OFFSETS[ChorusState::NUM_VOICES] = {0.0f, 0.33f, 0.67f};

    float inv_sample_rate = 1.0f / ctx.sample_rate;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Runtime tunable parameters (use defaults if zero/negative)
        float base_delay_ms = base_delay_in[i] > 0.0f ? base_delay_in[i] : CHORUS_BASE_DELAY_DEFAULT;
        float depth_range_ms = depth_range_in[i] > 0.0f ? depth_range_in[i] : CHORUS_DEPTH_RANGE_DEFAULT;

        // Update master LFO phase
        float lfo_rate = std::clamp(rate[i], 0.1f, 5.0f);
        state.lfo_phase += lfo_rate * inv_sample_rate;
        if (state.lfo_phase >= 1.0f) state.lfo_phase -= 1.0f;

        float d = std::clamp(depth[i], 0.0f, 1.0f);

        // Sum contributions from all voices
        float wet = 0.0f;
        for (std::size_t v = 0; v < ChorusState::NUM_VOICES; ++v) {
            // Each voice has offset LFO phase
            float voice_phase = state.lfo_phase + PHASE_OFFSETS[v];
            if (voice_phase >= 1.0f) voice_phase -= 1.0f;

            float lfo = std::sin(voice_phase * TWO_PI);
            float delay_ms = base_delay_ms + lfo * d * depth_range_ms;
            float delay_samples = delay_ms * 0.001f * ctx.sample_rate;
            delay_samples = std::clamp(delay_samples, 1.0f, static_cast<float>(ChorusState::MAX_CHORUS_SAMPLES - 1));

            wet += delay_read_linear(state.buffer, ChorusState::MAX_CHORUS_SAMPLES,
                                     state.write_pos, delay_samples);
        }
        wet /= static_cast<float>(ChorusState::NUM_VOICES);

        // Write dry signal to delay line
        state.buffer[state.write_pos] = input[i];
        state.write_pos = (state.write_pos + 1) % ChorusState::MAX_CHORUS_SAMPLES;

        // Output 100% wet (user can mix dry/wet manually if needed)
        out[i] = wet;
    }
}

// Default constants for phaser
constexpr float PHASER_MIN_FREQ_DEFAULT = 200.0f;   // Hz
constexpr float PHASER_MAX_FREQ_DEFAULT = 4000.0f;  // Hz

// ============================================================================
// EFFECT_PHASER: All-Pass Phaser
// ============================================================================
// in0: input signal
// in1: LFO rate (Hz, 0.1-5)
// in2: depth (0.0-1.0)
// in3: min_freq - sweep range low in Hz (default 200)
// in4: max_freq - sweep range high in Hz (default 4000)
// rate: feedback (high 4 bits 0-15 -> 0.0-0.99), stages (low 4 bits, clamped 2-12)
//
// Cascaded first-order allpass filters with modulated center frequencies.
// Creates notches that sweep through the spectrum. Classic phaser sound.
// Outputs 100% wet signal - user can mix dry/wet manually if needed.

[[gnu::always_inline]]
inline void op_effect_phaser(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* rate = ctx.buffers->get(inst.inputs[1]);
    const float* depth = ctx.buffers->get(inst.inputs[2]);
    const float* min_freq_in = ctx.buffers->get(inst.inputs[3]);
    const float* max_freq_in = ctx.buffers->get(inst.inputs[4]);
    auto& state = ctx.states->get_or_create<PhaserState>(inst.state_id);

    // Decode rate field parameters (4 bits each)
    float feedback = static_cast<float>((inst.rate >> 4) & 0x0F) / 15.0f * 0.99f;
    std::size_t num_stages = std::clamp(static_cast<std::size_t>(inst.rate & 0x0F),
                                         std::size_t{2}, PhaserState::NUM_STAGES);

    float inv_sample_rate = 1.0f / ctx.sample_rate;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Runtime tunable parameters (use defaults if zero/negative)
        float min_freq = min_freq_in[i] > 0.0f ? min_freq_in[i] : PHASER_MIN_FREQ_DEFAULT;
        float max_freq = max_freq_in[i] > 0.0f ? max_freq_in[i] : PHASER_MAX_FREQ_DEFAULT;

        // Update LFO phase
        float lfo_rate = std::clamp(rate[i], 0.1f, 5.0f);
        state.lfo_phase += lfo_rate * inv_sample_rate;
        if (state.lfo_phase >= 1.0f) state.lfo_phase -= 1.0f;

        // Calculate center frequency for allpass filters
        float lfo = std::sin(state.lfo_phase * TWO_PI);
        float d = std::clamp(depth[i], 0.0f, 1.0f);

        // Logarithmic frequency sweep
        float freq_factor = std::exp(lfo * d * 2.0f);  // ~0.13 to ~7.4 range
        float center_freq = std::sqrt(min_freq * max_freq) * freq_factor;
        center_freq = std::clamp(center_freq, min_freq, max_freq);

        // Calculate allpass coefficient
        // First-order allpass: y[n] = a * x[n] + x[n-1] - a * y[n-1]
        // Where a = (tan(pi*f/fs) - 1) / (tan(pi*f/fs) + 1)
        float tan_val = std::tan(PI * center_freq * inv_sample_rate);
        float a = (tan_val - 1.0f) / (tan_val + 1.0f);

        // Apply feedback from last stage
        float x = input[i] + feedback * state.last_output;

        // Cascade allpass stages
        for (std::size_t s = 0; s < num_stages; ++s) {
            float y = a * x + state.allpass_state[s] - a * state.allpass_delay[s];
            state.allpass_state[s] = x;
            state.allpass_delay[s] = y;
            x = y;
        }

        state.last_output = x;

        // Output 100% wet (user can mix dry/wet manually if needed)
        out[i] = x;
    }
}

}  // namespace cedar
