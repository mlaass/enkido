#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../vm/sample_bank.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
#include "sequence.hpp"
#include <cmath>
#include <algorithm>

namespace cedar {

// Forward declaration
class SampleBank;

// ============================================================================
// SAMPLE_PLAY: Polyphonic sample playback with pitch control
// ============================================================================
// in0: trigger signal (rising edge triggers new voice)
// in1: pitch/speed (1.0 = original pitch, 2.0 = octave up, 0.5 = octave down)
// in2: sample ID (constant, which sample to play)
// in3/in4: (optional) linked SequenceState state_id, split low/high 16 bits.
//   When both are 0xFFFF (unused), the sampler uses the scalar sample_id from
//   in2 (existing behavior). Otherwise the two halves are reassembled into a
//   32-bit state_id, and on every trigger the sampler fetches the most-
//   recently-crossed event from that SequenceState and spawns one voice per
//   non-zero entry in event.values[]. This is how sample polyrhythms such as
//   [bd, hh] play multiple simultaneous samples: codegen merges them into a
//   single event with num_values > 1 and the sampler picks up each voice.
//
// Polyphonic sampler with up to 32 simultaneous voices.
// Trigger detection on rising edge (0 -> positive).
// Uses linear interpolation for pitch shifting.
//
// Stereo-native (prd-stereo-native-opcodes Phase 3): writes out_buffer (L) and
// out_buffer+1 (R) in one call. Mono sample files broadcast (L=R=ch[0]);
// stereo files preserve channels (L=ch[0], R=ch[1]); ≥3-channel files take the
// first two channels and drop the rest.
[[gnu::always_inline]]
inline void op_sample_play(ExecutionContext& ctx, const Instruction& inst, SampleBank* sample_bank) {
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* trigger = ctx.buffers->get(inst.inputs[0]);
    const float* pitch = ctx.buffers->get(inst.inputs[1]);
    const float* sample_id_buf = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<SamplerState>(inst.state_id);

    // Optional link to an upstream SequenceState for polyphonic trigger events.
    // Both halves == BUFFER_UNUSED means no link → scalar mode below.
    SequenceState* linked_seq = nullptr;
    if (inst.inputs[3] != BUFFER_UNUSED || inst.inputs[4] != BUFFER_UNUSED) {
        std::uint32_t seq_state_id =
            static_cast<std::uint32_t>(inst.inputs[3]) |
            (static_cast<std::uint32_t>(inst.inputs[4]) << 16);
        linked_seq = ctx.states->get_if<SequenceState>(seq_state_id);
    }

    // Anti-click envelope constants
    constexpr std::uint8_t ATTACK_SAMPLES = 5;  // ~0.1ms at 48kHz

    // Spawn a single voice. Shared by the scalar and polyphonic trigger paths
    // so both honor identical voice-allocation and micro-fade behavior.
    // voice_velocity is baked at trigger time (per-voice, from
    // evt.velocities[v] in the polyphonic path; 1.0 in the scalar path).
    auto spawn_voice = [&](std::uint32_t sample_id, float voice_pitch, float voice_velocity) {
        if (sample_id == 0 || !sample_bank) return;
        const SampleData* sample = sample_bank->get_sample(sample_id);
        if (!sample || sample->frames == 0) return;
        SamplerVoice* voice = state.allocate_voice();
        if (!voice) return;
        voice->position = 0.0f;
        voice->speed = voice_pitch;
        voice->velocity = voice_velocity;
        voice->sample_id = sample_id;
        voice->active = true;
        voice->fading_out = false;
        float first_sample = sample->get_interpolated(0.0f, 0);
        voice->attack_counter = (std::abs(first_sample) > 0.01f) ? 0 : ATTACK_SAMPLES;
    };

    // Process block
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float current_trigger = trigger[i];
        float current_pitch = std::max(0.01f, pitch[i]);  // Prevent negative/zero pitch

        // Read sample_id per-sample (important for sequenced patterns!)
        std::uint32_t current_sample_id = static_cast<std::uint32_t>(sample_id_buf[i]);

        // Detect rising edge trigger
        bool trigger_on = (current_trigger > 0.0f && state.prev_trigger <= 0.0f);
        state.prev_trigger = current_trigger;

        // Trigger new voice(s) on rising edge.
        if (trigger_on) {
            if (linked_seq && linked_seq->output.num_events > 0) {
                // Polyphonic path: spawn one voice per values[] entry of the
                // event SEQPAT_STEP just crossed. Each voice gets its own
                // velocity from evt.velocities[v], so [cp, bd{vel:0.05}]
                // plays cp at 1.0 and bd at 0.05 independently.
                std::uint32_t event_index = (linked_seq->current_index > 0)
                    ? linked_seq->current_index - 1
                    : linked_seq->output.num_events - 1;
                const auto& evt = linked_seq->output.events[event_index];
                for (std::uint8_t v = 0; v < evt.num_values; ++v) {
                    spawn_voice(static_cast<std::uint32_t>(evt.values[v]),
                                current_pitch, evt.velocities[v]);
                }
            } else {
                // Scalar path (user-facing sample() builtin, or unlinked pat()).
                // Velocity scaling is handled by the codegen post-MUL on the
                // sampler output (legacy behavior); spawn at 1.0.
                spawn_voice(current_sample_id, current_pitch, 1.0f);
            }
        }

        // Mix all active voices (each voice plays its own sample_id).
        // Stereo split per-voice: mono file → L=R=ch[0]; stereo file →
        // L=ch[0], R=ch[1]; 3+ channels → first two channels, drop rest.
        float output_l = 0.0f;
        float output_r = 0.0f;

        for (std::size_t v = 0; v < SamplerState::MAX_VOICES; ++v) {
            SamplerVoice& voice = state.voices[v];

            if (!voice.active) {
                continue;
            }

            // Get sample for this voice
            const SampleData* sample = sample_bank ? sample_bank->get_sample(voice.sample_id) : nullptr;
            if (!sample || sample->frames == 0) {
                voice.active = false;
                continue;
            }

            // Read per-channel samples for stereo split.
            float sample_l;
            float sample_r;
            if (sample->channels == 1) {
                sample_l = sample->get_interpolated(voice.position, 0);
                sample_r = sample_l;
            } else {
                sample_l = sample->get_interpolated(voice.position, 0);
                sample_r = sample->get_interpolated(voice.position, 1);
            }

            // Apply micro-fade attack envelope (prevents DC click on start)
            float attack_env = (voice.attack_counter < ATTACK_SAMPLES)
                ? static_cast<float>(voice.attack_counter) / static_cast<float>(ATTACK_SAMPLES)
                : 1.0f;
            if (voice.attack_counter < ATTACK_SAMPLES) {
                voice.attack_counter++;
            }

            float voice_gain = attack_env * voice.velocity;
            output_l += sample_l * voice_gain;
            output_r += sample_r * voice_gain;

            // Advance playback position
            // Account for sample rate difference
            float speed_factor = voice.speed * (sample->sample_rate / ctx.sample_rate);
            voice.position += speed_factor;

            // Check if sample finished
            if (voice.position >= static_cast<float>(sample->frames)) {
                voice.active = false;
            }
        }

        // Clamp output to prevent clipping with many voices
        out_l[i] = std::clamp(output_l, -2.0f, 2.0f);
        out_r[i] = std::clamp(output_r, -2.0f, 2.0f);
    }
}

// ============================================================================
// SAMPLE_PLAY_LOOP: Looping sample playback
// ============================================================================
// in0: gate signal (>0 = play, 0 = stop)
// in1: pitch/speed (1.0 = original pitch)
// in2: sample ID
//
// Similar to SAMPLE_PLAY but loops the sample while gate is high.
// Useful for sustained sounds, loops, and textures.
//
// Stereo-native (prd-stereo-native-opcodes Phase 3): same L/R split rules as
// op_sample_play. Mono files broadcast; stereo files preserve channels.
[[gnu::always_inline]]
inline void op_sample_play_loop(ExecutionContext& ctx, const Instruction& inst, SampleBank* sample_bank) {
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* gate = ctx.buffers->get(inst.inputs[0]);
    const float* pitch = ctx.buffers->get(inst.inputs[1]);
    const float* sample_id_buf = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<SamplerState>(inst.state_id);

    // Get sample ID
    std::uint32_t sample_id = static_cast<std::uint32_t>(sample_id_buf[0]);
    
    // Get sample data
    const SampleData* sample = nullptr;
    if (sample_bank) {
        sample = sample_bank->get_sample(sample_id);
    }
    
    // Anti-click envelope constants
    constexpr std::uint8_t ATTACK_SAMPLES = 5;  // ~0.1ms at 48kHz
    constexpr std::uint8_t FADEOUT_SAMPLES = 5;

    // If no sample or invalid sample rate, output silence
    // but still track gate state for proper edge detection
    if (!sample || sample->frames == 0 || ctx.sample_rate <= 0.0f) {
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            state.prev_trigger = gate[i];
            out_l[i] = 0.0f;
            out_r[i] = 0.0f;
        }
        return;
    }

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float current_gate = gate[i];
        float current_pitch = std::max(0.01f, pitch[i]);

        // Detect gate edges
        bool gate_on = (current_gate > 0.0f && state.prev_trigger <= 0.0f);
        bool gate_off = (current_gate <= 0.0f && state.prev_trigger > 0.0f);
        state.prev_trigger = current_gate;

        // Start playback on gate on (if voice available)
        if (gate_on) {
            SamplerVoice* voice = state.allocate_voice();
            if (voice) {
                voice->position = 0.0f;
                voice->speed = current_pitch;
                voice->sample_id = sample_id;
                voice->active = true;
                voice->fading_out = false;

                // Check if sample starts near zero - skip fade if so
                float first_sample = sample->get_interpolated(0.0f, 0);
                voice->attack_counter = (std::abs(first_sample) > 0.01f) ? 0 : ATTACK_SAMPLES;
            }
        }

        // Start fadeout on gate off (instead of hard stop)
        if (gate_off) {
            for (std::size_t v = 0; v < SamplerState::MAX_VOICES; ++v) {
                if (state.voices[v].sample_id == sample_id && state.voices[v].active) {
                    state.voices[v].fading_out = true;
                    state.voices[v].fadeout_counter = 0;
                }
            }
        }

        // Mix active voices. Stereo split per voice: mono → L=R, stereo → L/R.
        float output_l = 0.0f;
        float output_r = 0.0f;

        for (std::size_t v = 0; v < SamplerState::MAX_VOICES; ++v) {
            SamplerVoice& voice = state.voices[v];

            if (!voice.active || voice.sample_id != sample_id) {
                continue;
            }

            // Read per-channel samples (looped interpolation wraps at the
            // boundary for seamless playback). Mono files broadcast L=R;
            // 2+ channel files take channels 0 and 1.
            float sample_l;
            float sample_r;
            if (sample->channels == 1) {
                sample_l = sample->get_interpolated_looped(voice.position, 0);
                sample_r = sample_l;
            } else {
                sample_l = sample->get_interpolated_looped(voice.position, 0);
                sample_r = sample->get_interpolated_looped(voice.position, 1);
            }

            // Apply envelope
            float env = 1.0f;
            if (voice.fading_out) {
                // Fadeout envelope
                env = 1.0f - static_cast<float>(voice.fadeout_counter) / static_cast<float>(FADEOUT_SAMPLES);
                if (++voice.fadeout_counter >= FADEOUT_SAMPLES) {
                    voice.active = false;
                    voice.fading_out = false;
                }
            } else {
                // Attack envelope
                env = (voice.attack_counter < ATTACK_SAMPLES)
                    ? static_cast<float>(voice.attack_counter) / static_cast<float>(ATTACK_SAMPLES)
                    : 1.0f;
                if (voice.attack_counter < ATTACK_SAMPLES) {
                    voice.attack_counter++;
                }
            }

            output_l += sample_l * env;
            output_r += sample_r * env;

            // Advance with looping
            float speed_factor = voice.speed * (sample->sample_rate / ctx.sample_rate);
            voice.position += speed_factor;

            // Loop back to start
            if (voice.position >= static_cast<float>(sample->frames)) {
                voice.position = std::fmod(voice.position, static_cast<float>(sample->frames));
            }
        }

        out_l[i] = std::clamp(output_l, -2.0f, 2.0f);
        out_r[i] = std::clamp(output_r, -2.0f, 2.0f);
    }
}

}  // namespace cedar
