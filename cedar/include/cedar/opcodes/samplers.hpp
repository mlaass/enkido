#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../vm/sample_bank.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
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
//
// Polyphonic sampler with up to 16 simultaneous voices.
// Trigger detection on rising edge (0 -> positive).
// Uses linear interpolation for pitch shifting.
// Outputs stereo (mono samples are duplicated to both channels).
[[gnu::always_inline]]
inline void op_sample_play(ExecutionContext& ctx, const Instruction& inst, SampleBank* sample_bank) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* trigger = ctx.buffers->get(inst.inputs[0]);
    const float* pitch = ctx.buffers->get(inst.inputs[1]);
    const float* sample_id_buf = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<SamplerState>(inst.state_id);

    // Get sample ID (assumed constant per block)
    std::uint32_t sample_id = static_cast<std::uint32_t>(sample_id_buf[0]);
    
    // Get sample data
    const SampleData* sample = nullptr;
    if (sample_bank) {
        sample = sample_bank->get_sample(sample_id);
    }
    
    // If no sample, no sample bank, or invalid sample rate, output silence
    if (!sample || sample->frames == 0 || ctx.sample_rate <= 0.0f) {
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            out[i] = 0.0f;
        }
        return;
    }

    // Process block
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float current_trigger = trigger[i];
        float current_pitch = std::max(0.01f, pitch[i]);  // Prevent negative/zero pitch
        
        // Detect rising edge trigger
        bool trigger_on = (current_trigger > 0.0f && state.prev_trigger <= 0.0f);
        state.prev_trigger = current_trigger;
        
        // Trigger new voice
        if (trigger_on) {
            SamplerVoice* voice = state.allocate_voice();
            voice->position = 0.0f;
            voice->speed = current_pitch;
            voice->sample_id = sample_id;
            voice->active = true;
        }
        
        // Mix all active voices
        float output = 0.0f;
        
        for (std::size_t v = 0; v < SamplerState::MAX_VOICES; ++v) {
            SamplerVoice& voice = state.voices[v];
            
            if (!voice.active || voice.sample_id != sample_id) {
                continue;
            }
            
            // Read sample with interpolation (mix down to mono for now)
            float sample_value = 0.0f;
            for (std::uint32_t ch = 0; ch < sample->channels; ++ch) {
                sample_value += sample->get_interpolated(voice.position, ch);
            }
            sample_value /= static_cast<float>(sample->channels);
            
            output += sample_value;
            
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
        out[i] = std::clamp(output, -2.0f, 2.0f);
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
[[gnu::always_inline]]
inline void op_sample_play_loop(ExecutionContext& ctx, const Instruction& inst, SampleBank* sample_bank) {
    float* out = ctx.buffers->get(inst.out_buffer);
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
    
    // If no sample or invalid sample rate, output silence
    if (!sample || sample->frames == 0 || ctx.sample_rate <= 0.0f) {
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            out[i] = 0.0f;
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
        
        // Start playback on gate on
        if (gate_on) {
            SamplerVoice* voice = state.allocate_voice();
            voice->position = 0.0f;
            voice->speed = current_pitch;
            voice->sample_id = sample_id;
            voice->active = true;
        }
        
        // Stop all voices on gate off
        if (gate_off) {
            for (std::size_t v = 0; v < SamplerState::MAX_VOICES; ++v) {
                if (state.voices[v].sample_id == sample_id) {
                    state.voices[v].active = false;
                }
            }
        }
        
        // Mix active voices
        float output = 0.0f;
        
        for (std::size_t v = 0; v < SamplerState::MAX_VOICES; ++v) {
            SamplerVoice& voice = state.voices[v];
            
            if (!voice.active || voice.sample_id != sample_id) {
                continue;
            }
            
            // Read sample with interpolation
            float sample_value = 0.0f;
            for (std::uint32_t ch = 0; ch < sample->channels; ++ch) {
                sample_value += sample->get_interpolated(voice.position, ch);
            }
            sample_value /= static_cast<float>(sample->channels);
            
            output += sample_value;
            
            // Advance with looping
            float speed_factor = voice.speed * (sample->sample_rate / ctx.sample_rate);
            voice.position += speed_factor;
            
            // Loop back to start
            if (voice.position >= static_cast<float>(sample->frames)) {
                voice.position = std::fmod(voice.position, static_cast<float>(sample->frames));
            }
        }
        
        out[i] = std::clamp(output, -2.0f, 2.0f);
    }
}

}  // namespace cedar
