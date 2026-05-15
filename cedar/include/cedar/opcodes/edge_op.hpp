#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"

namespace cedar {

// EDGE_OP: edge-detection / sample-and-hold / counter, mode-dispatched on inst.rate
//   rate=0 sah(in, trig)            — classic sample-and-hold (was op_sah)
//   rate=1 gateup(sig)              — 1.0 on rising edge of inputs[0], else 0.0
//   rate=2 gatedown(sig)            — 1.0 on falling edge of inputs[0], else 0.0
//   rate=3 counter(trig, reset?, start?) — increment on trig rising edge,
//                                     reset to start (or 0) on reset rising edge.
//                                     reset wins if both fire on the same sample.
//
// Stereo-native (prd-stereo-native-opcodes Phase 5): handles both channels in
// one dispatch with one EdgeState. inputs[0] is the stereo-capable primary
// (mono auto-broadcasts to L=R; stereo via STEREO_INPUT reads inputs[0]+1 for
// R). The remaining slots (sah's trig, counter's reset/start) are shared
// control signals applied identically to both lanes.
[[gnu::always_inline]]
inline void op_edge(ExecutionContext& ctx, const Instruction& inst) {
    auto& state = ctx.states->get_or_create<EdgeState>(inst.state_id);
    float* out_l = ctx.buffers->get(inst.out_buffer);
    const bool stereo_out = (inst.flags & InstructionFlag::STEREO_OUTPUT) != 0;
    float* out_r = stereo_out
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1))
        : nullptr;
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const std::size_t n_channels = stereo_out ? 2u : 1u;

    switch (inst.rate) {
        case 0: {
            // sah(in, trig) — `in` is the stereo-capable primary; `trig` shared
            const float* input = ctx.buffers->get(inst.inputs[0]);
            const float* input_r = stereo_in
                ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
                : input;
            const float* trig = ctx.buffers->get(inst.inputs[1]);
            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
                for (std::size_t ch = 0; ch < n_channels; ++ch) {
                    const float* in_ch = (ch == 0) ? input : input_r;
                    if (state.prev_trigger[ch] <= 0.0f && trig[i] > 0.0f) {
                        state.held_value[ch] = in_ch[i];
                    }
                    state.prev_trigger[ch] = trig[i];
                    (ch == 0 ? out_l : out_r)[i] = state.held_value[ch];
                }
            }
            break;
        }
        case 1: {
            // gateup(sig)
            const float* sig = ctx.buffers->get(inst.inputs[0]);
            const float* sig_r = stereo_in
                ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
                : sig;
            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
                for (std::size_t ch = 0; ch < n_channels; ++ch) {
                    const float* s = (ch == 0) ? sig : sig_r;
                    (ch == 0 ? out_l : out_r)[i] =
                        (state.prev_trigger[ch] <= 0.0f && s[i] > 0.0f) ? 1.0f : 0.0f;
                    state.prev_trigger[ch] = s[i];
                }
            }
            break;
        }
        case 2: {
            // gatedown(sig)
            const float* sig = ctx.buffers->get(inst.inputs[0]);
            const float* sig_r = stereo_in
                ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
                : sig;
            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
                for (std::size_t ch = 0; ch < n_channels; ++ch) {
                    const float* s = (ch == 0) ? sig : sig_r;
                    (ch == 0 ? out_l : out_r)[i] =
                        (state.prev_trigger[ch] > 0.0f && s[i] <= 0.0f) ? 1.0f : 0.0f;
                    state.prev_trigger[ch] = s[i];
                }
            }
            break;
        }
        case 3: {
            // counter(trig, reset?, start?) — `trig` is the stereo-capable
            // primary; `reset`/`start` are shared control signals.
            const float* trig = ctx.buffers->get(inst.inputs[0]);
            const float* trig_r = stereo_in
                ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
                : trig;
            const float* reset = (inst.inputs[1] != BUFFER_UNUSED)
                ? ctx.buffers->get(inst.inputs[1]) : nullptr;
            const float* start = (inst.inputs[2] != BUFFER_UNUSED)
                ? ctx.buffers->get(inst.inputs[2]) : nullptr;
            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
                for (std::size_t ch = 0; ch < n_channels; ++ch) {
                    const float* tr = (ch == 0) ? trig : trig_r;
                    if (reset && state.prev_reset_trigger[ch] <= 0.0f && reset[i] > 0.0f) {
                        state.held_value[ch] = start ? start[i] : 0.0f;
                    } else if (state.prev_trigger[ch] <= 0.0f && tr[i] > 0.0f) {
                        state.held_value[ch] = state.held_value[ch] + 1.0f;
                    }
                    state.prev_trigger[ch] = tr[i];
                    if (reset) state.prev_reset_trigger[ch] = reset[i];
                    (ch == 0 ? out_l : out_r)[i] = state.held_value[ch];
                }
            }
            break;
        }
        default:
            // Unknown mode: emit silence so we don't propagate uninitialized memory
            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
                out_l[i] = 0.0f;
                if (stereo_out) out_r[i] = 0.0f;
            }
            break;
    }
}

}  // namespace cedar
