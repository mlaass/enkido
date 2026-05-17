#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
#include <cmath>
#include <algorithm>

namespace cedar {

// ============================================================================
// Delay Opcodes
// ============================================================================

// DELAY: Delay line with feedback
// in0: input signal
// in1: delay time (unit depends on rate field)
// in2: feedback amount (0.0-1.0, clamped to 0.99 to prevent runaway)
// in3: dry level (0xFFFF = default 0.0)
// in4: wet level (0xFFFF = default 1.0)
// rate field: time unit (0=seconds, 1=milliseconds, 2=samples)
//
// Uses linear interpolation for sub-sample delay accuracy.
// Stereo-native (prd-stereo-native-opcodes Phase 4c): per-channel ring
// buffers and smoothed-delay state. Mono input auto-broadcasts;
// stereo input drives per-channel delay lines.
[[gnu::always_inline]]
inline void op_delay(ExecutionContext& ctx, const Instruction& inst) {
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* delay_time = ctx.buffers->get(inst.inputs[1]);
    const float* feedback = ctx.buffers->get(inst.inputs[2]);
    const float* dry_level = (inst.inputs[3] != 0xFFFF) ? ctx.buffers->get(inst.inputs[3]) : nullptr;
    const float* wet_level = (inst.inputs[4] != 0xFFFF) ? ctx.buffers->get(inst.inputs[4]) : nullptr;
    auto& state = ctx.states->get_or_create<DelayState>(inst.state_id);

    // Calculate maximum delay in samples and ensure both per-channel buffers
    // are allocated. Max 10 seconds regardless of unit.
    float max_delay_sec = 10.0f;
    std::size_t max_samples = static_cast<std::size_t>(max_delay_sec * ctx.sample_rate) + 1;
    max_samples = std::min(max_samples, DelayState::MAX_DELAY_SAMPLES);
    state.ensure_buffer(max_samples, ctx.arena);

    if (!state.buffer[0] || !state.buffer[1]) {
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            out_l[i] = 0.0f;
            out_r[i] = 0.0f;
        }
        return;
    }

    // Time unit conversion factor based on rate field
    float unit_factor;
    switch (inst.rate) {
        case 0:  unit_factor = ctx.sample_rate; break;           // seconds → samples
        case 1:  unit_factor = 0.001f * ctx.sample_rate; break;  // ms → samples
        case 2:  unit_factor = 1.0f; break;                      // samples → samples
        default: unit_factor = ctx.sample_rate; break;
    }

    constexpr float smooth_coeff = 0.9995f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Mono control inputs (shared across channels)
        float target_delay = delay_time[i] * unit_factor;
        target_delay = std::clamp(target_delay, 1.0f, static_cast<float>(state.buffer_size - 2));
        float fb = std::clamp(feedback[i], 0.0f, 0.99f);
        float dry = dry_level ? dry_level[i] : 1.0f;
        float wet = wet_level ? wet_level[i] : 0.5f;

        for (std::size_t ch = 0; ch < 2; ++ch) {
            float x = (ch == 0) ? input[i] : (stereo_in ? input_r[i] : input[i]);

            // Per-channel smoothed delay
            if (!state.delay_initialized[ch]) {
                state.smoothed_delay[ch] = target_delay;
                state.delay_initialized[ch] = true;
            } else {
                state.smoothed_delay[ch] = state.smoothed_delay[ch] * smooth_coeff +
                                           target_delay * (1.0f - smooth_coeff);
            }

            float delay_samples = state.smoothed_delay[ch];
            std::size_t delay_int = static_cast<std::size_t>(delay_samples);
            float delay_frac = delay_samples - static_cast<float>(delay_int);

            std::size_t read_pos1 = (state.write_pos[ch] + state.buffer_size - delay_int) % state.buffer_size;
            std::size_t read_pos2 = (read_pos1 + state.buffer_size - 1) % state.buffer_size;

            float delayed = state.buffer[ch][read_pos1] * (1.0f - delay_frac) +
                            state.buffer[ch][read_pos2] * delay_frac;

            state.buffer[ch][state.write_pos[ch]] = x + delayed * fb;
            state.write_pos[ch] = (state.write_pos[ch] + 1) % state.buffer_size;

            (ch == 0 ? out_l : out_r)[i] = x * dry + delayed * wet;
        }
    }
}

// DELAY_SYNC: Beat-synchronized delay (delay time in beats/subdivisions)
// in0: input signal
// in1: delay time in beats (e.g., 0.25 = 1/16th note at 4/4)
// in2: feedback amount (0.0-1.0)
// reserved field low byte: wet/dry mix (0-255 -> 0.0-1.0)
//
// Delay time is calculated from BPM: delay_ms = (delay_beats / bpm) * 60000
// Stereo-native — uses per-channel DelayState buffers/write_pos (same as op_delay).
[[gnu::always_inline]]
inline void op_delay_sync(ExecutionContext& ctx, const Instruction& inst) {
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* delay_beats = ctx.buffers->get(inst.inputs[1]);
    const float* feedback = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<DelayState>(inst.state_id);

    float mix = 1.0f - static_cast<float>(inst.rate) / 255.0f;
    float samples_per_beat = (60.0f / ctx.bpm) * ctx.sample_rate;
    float max_beats = 4.0f;
    std::size_t max_samples = static_cast<std::size_t>(max_beats * samples_per_beat) + 1;
    max_samples = std::min(max_samples, DelayState::MAX_DELAY_SAMPLES);
    state.ensure_buffer(max_samples, ctx.arena);

    if (!state.buffer[0] || !state.buffer[1]) {
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            out_l[i] = 0.0f;
            out_r[i] = 0.0f;
        }
        return;
    }

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float delay_samples = delay_beats[i] * samples_per_beat;
        delay_samples = std::clamp(delay_samples, 0.0f, static_cast<float>(state.buffer_size - 2));
        float fb = std::clamp(feedback[i], 0.0f, 0.99f);

        std::size_t delay_int = static_cast<std::size_t>(delay_samples);
        float delay_frac = delay_samples - static_cast<float>(delay_int);

        for (std::size_t ch = 0; ch < 2; ++ch) {
            float x = (ch == 0) ? input[i] : (stereo_in ? input_r[i] : input[i]);

            std::size_t read_pos1 = (state.write_pos[ch] + state.buffer_size - delay_int) % state.buffer_size;
            std::size_t read_pos2 = (read_pos1 + state.buffer_size - 1) % state.buffer_size;

            float delayed = state.buffer[ch][read_pos1] * (1.0f - delay_frac) +
                            state.buffer[ch][read_pos2] * delay_frac;

            state.buffer[ch][state.write_pos[ch]] = x + delayed * fb;
            state.write_pos[ch] = (state.write_pos[ch] + 1) % state.buffer_size;

            (ch == 0 ? out_l : out_r)[i] = x * (1.0f - mix) + delayed * mix;
        }
    }
}

// ============================================================================
// Tap Delay Opcodes (Coordinated Pair)
// ============================================================================
// DELAY_TAP and DELAY_WRITE work together to enable configurable feedback
// processing. The pattern is:
//   1. DELAY_TAP reads from the delay buffer and caches in tap_cache
//   2. User's feedback chain processes the cached samples
//   3. DELAY_WRITE writes input + processed feedback to buffer, outputs delayed
//
// Both opcodes MUST share the same state_id to operate on the same delay buffer.

// DELAY_TAP: Read from per-channel delay lines, cache for closure processing.
// Stereo-native (prd-stereo-native-opcodes Phase 4c): the closure body in
// tap_delay receives a stereo signal pair (out_buffer is the L of an
// adjacent pair) so per-channel state machinery is necessary.
// in0: input signal (passed through, not used for reading)
// in1: delay time (unit depends on rate field)
// out: delayed signal L (out_buffer); R lives at out_buffer+1
// rate field: time unit (0=seconds, 1=milliseconds, 2=samples)
[[gnu::always_inline]]
inline void op_delay_tap(ExecutionContext& ctx, const Instruction& inst) {
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* delay_time = ctx.buffers->get(inst.inputs[1]);
    auto& state = ctx.states->get_or_create<DelayState>(inst.state_id);

    float max_delay_sec = 10.0f;
    std::size_t max_samples = static_cast<std::size_t>(max_delay_sec * ctx.sample_rate) + 1;
    max_samples = std::min(max_samples, DelayState::MAX_DELAY_SAMPLES);
    state.ensure_buffer(max_samples, ctx.arena);

    if (!state.buffer[0] || !state.buffer[1]) {
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            out_l[i] = 0.0f;
            out_r[i] = 0.0f;
            state.tap_cache[0][i] = 0.0f;
            state.tap_cache[1][i] = 0.0f;
        }
        return;
    }

    float unit_factor;
    switch (inst.rate) {
        case 0:  unit_factor = ctx.sample_rate; break;
        case 1:  unit_factor = 0.001f * ctx.sample_rate; break;
        case 2:  unit_factor = 1.0f; break;
        default: unit_factor = ctx.sample_rate; break;
    }

    constexpr float smooth_coeff = 0.9995f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float target_delay = delay_time[i] * unit_factor;
        target_delay = std::clamp(target_delay, 1.0f, static_cast<float>(state.buffer_size - 2));

        for (std::size_t ch = 0; ch < 2; ++ch) {
            if (!state.delay_initialized[ch]) {
                state.smoothed_delay[ch] = target_delay;
                state.delay_initialized[ch] = true;
            } else {
                state.smoothed_delay[ch] = state.smoothed_delay[ch] * smooth_coeff +
                                            target_delay * (1.0f - smooth_coeff);
            }

            float delay_samples = state.smoothed_delay[ch];
            std::size_t delay_int = static_cast<std::size_t>(delay_samples);
            float delay_frac = delay_samples - static_cast<float>(delay_int);

            // Read relative to current write_pos which hasn't advanced yet
            std::size_t read_pos1 = (state.write_pos[ch] + i + state.buffer_size - delay_int) % state.buffer_size;
            std::size_t read_pos2 = (read_pos1 + state.buffer_size - 1) % state.buffer_size;

            float delayed = state.buffer[ch][read_pos1] * (1.0f - delay_frac) +
                            state.buffer[ch][read_pos2] * delay_frac;

            (ch == 0 ? out_l : out_r)[i] = delayed;
            state.tap_cache[ch][i] = delayed;
        }
    }
}

// DELAY_WRITE: Write feedback to per-channel delay line, output delayed signal.
// Stereo-native (prd-stereo-native-opcodes Phase 4c). The processed feedback
// arrives as a stereo pair from the tap_delay closure body; the dry/input
// signal is stereo (or mono auto-broadcast via STEREO_INPUT flag).
// in0: input signal (mono auto-broadcasts; stereo via STEREO_INPUT)
// in1: processed feedback (stereo pair; closure body output)
// in2: feedback amount (0.0-1.0, clamped to 0.99)
// in3: dry level (0xFFFF = default 0.0)
// in4: wet level (0xFFFF = default 1.0)
// out: delayed signal L from tap_cache; R at out_buffer+1
[[gnu::always_inline]]
inline void op_delay_write(ExecutionContext& ctx, const Instruction& inst) {
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* processed_l = ctx.buffers->get(inst.inputs[1]);
    // Processed feedback is always stereo (closure body emits stereo pair).
    const float* processed_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[1] + 1));
    const float* feedback = ctx.buffers->get(inst.inputs[2]);
    const float* dry_level = (inst.inputs[3] != 0xFFFF) ? ctx.buffers->get(inst.inputs[3]) : nullptr;
    const float* wet_level = (inst.inputs[4] != 0xFFFF) ? ctx.buffers->get(inst.inputs[4]) : nullptr;
    auto& state = ctx.states->get_or_create<DelayState>(inst.state_id);

    if (!state.buffer[0] || !state.buffer[1]) {
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            out_l[i] = 0.0f;
            out_r[i] = 0.0f;
        }
        return;
    }

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float fb = std::clamp(feedback[i], 0.0f, 0.99f);
        float dry = dry_level ? dry_level[i] : 1.0f;
        float wet = wet_level ? wet_level[i] : 0.5f;

        for (std::size_t ch = 0; ch < 2; ++ch) {
            float x = (ch == 0) ? input[i] : (stereo_in ? input_r[i] : input[i]);
            float proc = (ch == 0) ? processed_l[i] : processed_r[i];

            state.buffer[ch][state.write_pos[ch]] = x + proc * fb;
            state.write_pos[ch] = (state.write_pos[ch] + 1) % state.buffer_size;

            (ch == 0 ? out_l : out_r)[i] = x * dry + state.tap_cache[ch][i] * wet;
        }
    }
}

}  // namespace cedar
