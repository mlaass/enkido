#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
#include <cmath>
#include <cstring>

namespace cedar {

// ============================================================================
// Envelope Opcodes
// ============================================================================

// ENV_ADSR: Attack-Decay-Sustain-Release envelope generator
// in0: gate signal (>0 = on, triggers on rising edge, releases on falling edge)
// in1: attack time (seconds)
// in2: decay time (seconds)
// reserved field low byte: sustain level (0-255 -> 0.0-1.0)
// reserved field high byte: release time in tenths of seconds (0-255 -> 0.0-25.5s)
//
// Stage transitions:
//   Gate on  -> Attack (from current level to 1.0)
//   Attack done -> Decay (from 1.0 to sustain)
//   Decay done -> Sustain (hold at sustain level)
//   Gate off -> Release (from current level to 0.0)
//   Release done -> Idle
[[gnu::always_inline]]
inline void op_env_adsr(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* gate = ctx.buffers->get(inst.inputs[0]);
    const float* attack_buf = ctx.buffers->get(inst.inputs[1]);
    const float* decay_buf = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<EnvState>(inst.state_id);

    // Extract sustain (0-255 -> 0.0-1.0) and release (0-255 -> 0.0-25.5s) from reserved
    float sustain = static_cast<float>(inst.reserved & 0xFF) / 255.0f;
    float release_time = static_cast<float>((inst.reserved >> 8) & 0xFF) * 0.1f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float current_gate = gate[i];
        float attack_time = attack_buf[i];
        float decay_time = decay_buf[i];

        // Detect gate edges
        bool gate_on = (current_gate > 0.0f && state.prev_gate <= 0.0f);
        bool gate_off = (current_gate <= 0.0f && state.prev_gate > 0.0f);
        state.prev_gate = current_gate;

        // Gate on: start attack from current level
        if (gate_on) {
            state.stage = 1;  // attack
            state.time_in_stage = 0.0f;
        }

        // Gate off: start release from current level (from any stage except idle)
        if (gate_off && state.stage != 0) {
            state.stage = 4;  // release
            state.time_in_stage = 0.0f;
            state.release_level = state.level;  // Remember level at release start
        }

        // Update coefficients if parameters changed
        if (attack_time != state.last_attack) {
            state.last_attack = attack_time;
            float attack_samples = std::max(0.001f, attack_time) * ctx.sample_rate;
            // Exponential coefficient: reaches ~63% in time constant, ~99% in 5 time constants
            // We use a faster coefficient to reach 1.0 in roughly the attack time
            state.attack_coeff = 1.0f - std::exp(-4.6f / attack_samples);  // ~99% in attack_time
        }

        if (decay_time != state.last_decay) {
            state.last_decay = decay_time;
            float decay_samples = std::max(0.001f, decay_time) * ctx.sample_rate;
            state.decay_coeff = 1.0f - std::exp(-4.6f / decay_samples);
        }

        if (release_time != state.last_release) {
            state.last_release = release_time;
            float release_samples = std::max(0.001f, release_time) * ctx.sample_rate;
            state.release_coeff = 1.0f - std::exp(-4.6f / release_samples);
        }

        // Process current stage
        switch (state.stage) {
            case 0:  // Idle
                state.level = 0.0f;
                break;

            case 1:  // Attack (exponential rise toward 1.0)
                state.level += state.attack_coeff * (1.0f - state.level);
                if (state.level >= 0.999f) {
                    state.level = 1.0f;
                    state.stage = 2;  // Transition to decay
                    state.time_in_stage = 0.0f;
                }
                break;

            case 2:  // Decay (exponential fall toward sustain)
                state.level += state.decay_coeff * (sustain - state.level);
                if (std::abs(state.level - sustain) < 0.001f) {
                    state.level = sustain;
                    state.stage = 3;  // Transition to sustain
                }
                break;

            case 3:  // Sustain (hold at sustain level while gate is on)
                state.level = sustain;
                break;

            case 4:  // Release (exponential fall toward 0)
                state.level += state.release_coeff * (0.0f - state.level);
                if (state.level < 0.001f) {
                    state.level = 0.0f;
                    state.stage = 0;  // Return to idle
                }
                break;

            default:
                state.stage = 0;
                state.level = 0.0f;
                break;
        }

        out[i] = state.level;
    }
}

// ENV_AR: Attack-Release envelope (simplified ADSR without decay/sustain)
// in0: trigger signal (any value >0 triggers attack)
// in1: attack time (seconds)
// in2: release time (seconds)
//
// Unlike ADSR, this is a one-shot envelope that triggers on any positive input
// and runs attack then release automatically (useful for percussion)
[[gnu::always_inline]]
inline void op_env_ar(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* trigger = ctx.buffers->get(inst.inputs[0]);
    const float* attack_buf = ctx.buffers->get(inst.inputs[1]);
    const float* release_buf = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<EnvState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float current_trigger = trigger[i];
        float attack_time = attack_buf[i];
        float release_time = release_buf[i];

        // Detect rising edge (retrigger)
        bool trigger_on = (current_trigger > 0.0f && state.prev_gate <= 0.0f);
        state.prev_gate = current_trigger;

        // Trigger: start attack
        if (trigger_on) {
            state.stage = 1;  // attack
            state.time_in_stage = 0.0f;
        }

        // Update coefficients
        if (attack_time != state.last_attack) {
            state.last_attack = attack_time;
            float attack_samples = std::max(0.001f, attack_time) * ctx.sample_rate;
            state.attack_coeff = 1.0f - std::exp(-4.6f / attack_samples);
        }

        if (release_time != state.last_release) {
            state.last_release = release_time;
            float release_samples = std::max(0.001f, release_time) * ctx.sample_rate;
            state.release_coeff = 1.0f - std::exp(-4.6f / release_samples);
        }

        // Process current stage (using stage 1=attack, 4=release, 0=idle)
        switch (state.stage) {
            case 0:  // Idle
                state.level = 0.0f;
                break;

            case 1:  // Attack
                state.level += state.attack_coeff * (1.0f - state.level);
                if (state.level >= 0.999f) {
                    state.level = 1.0f;
                    state.stage = 4;  // Transition directly to release
                    state.time_in_stage = 0.0f;
                }
                break;

            case 4:  // Release
                state.level += state.release_coeff * (0.0f - state.level);
                if (state.level < 0.001f) {
                    state.level = 0.0f;
                    state.stage = 0;  // Return to idle
                }
                break;

            default:
                state.stage = 0;
                state.level = 0.0f;
                break;
        }

        out[i] = state.level;
    }
}

}  // namespace cedar
