#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
#include <cmath>
#include <cstring>

namespace cedar {

// PUSH_CONST: Fill output buffer with constant value
// The constant is stored in the state_id field (reinterpreted as float)
[[gnu::always_inline]]
inline void op_push_const(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);

    // Reinterpret first 4 bytes of state_id as float constant
    float value;
    std::memcpy(&value, &inst.state_id, sizeof(float));

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = value;
    }
}

// COPY: Copy input buffer to output buffer
[[gnu::always_inline]]
inline void op_copy(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* in = ctx.buffers->get(inst.inputs[0]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = in[i];
    }
}

// OUTPUT: Add input buffer to stereo output (accumulates)
[[gnu::always_inline]]
inline void op_output(ExecutionContext& ctx, const Instruction& inst) {
    const float* in = ctx.buffers->get(inst.inputs[0]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        ctx.output_left[i] += in[i];
        ctx.output_right[i] += in[i];
    }
}

// NOISE: White noise generator (deterministic LCG for reproducibility)
[[gnu::always_inline]]
inline void op_noise(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    auto& state = ctx.states->get_or_create<NoiseState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // LCG: x_{n+1} = (a * x_n + c) mod m
        state.seed = state.seed * 1103515245u + 12345u;

        // Convert to float in range [-1, 1]
        // Use signed interpretation for symmetric distribution
        out[i] = static_cast<float>(static_cast<std::int32_t>(state.seed)) / 2147483648.0f;
    }
}

// MTOF: MIDI note number to frequency
// Formula: f = 440 * 2^((n-69)/12)
[[gnu::always_inline]]
inline void op_mtof(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* note = ctx.buffers->get(inst.inputs[0]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = 440.0f * std::pow(2.0f, (note[i] - 69.0f) / 12.0f);
    }
}

// DC: Add DC offset (in0 + constant)
// Constant stored in state_id field
[[gnu::always_inline]]
inline void op_dc(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* in = ctx.buffers->get(inst.inputs[0]);

    float offset;
    std::memcpy(&offset, &inst.state_id, sizeof(float));

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = in[i] + offset;
    }
}

// SLEW: Slew rate limiter (smooths sudden changes)
// Rate stored in state_id field (samples to reach target)
[[gnu::always_inline]]
inline void op_slew(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* target = ctx.buffers->get(inst.inputs[0]);
    auto& state = ctx.states->get_or_create<SlewState>(inst.state_id);

    float rate;
    std::memcpy(&rate, &inst.state_id, sizeof(float));
    float coeff = (rate > 0.0f) ? (1.0f / rate) : 1.0f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        state.current += (target[i] - state.current) * coeff;
        out[i] = state.current;
    }
}

// SAH: Sample and hold
// Samples input when trigger (in1) crosses zero upward
[[gnu::always_inline]]
inline void op_sah(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* trigger = ctx.buffers->get(inst.inputs[1]);
    auto& state = ctx.states->get_or_create<SAHState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Detect rising edge (previous <= 0, current > 0)
        if (state.prev_trigger <= 0.0f && trigger[i] > 0.0f) {
            state.held_value = input[i];
        }
        state.prev_trigger = trigger[i];
        out[i] = state.held_value;
    }
}

// Helper: Create instruction with float constant in state_id field
// Note: state_id is 32-bit, same size as float, so this is safe
inline Instruction make_const_instruction(Opcode op, std::uint16_t out, float value) {
    Instruction inst{};
    inst.opcode = op;
    inst.out_buffer = out;
    inst.inputs[0] = BUFFER_UNUSED;
    inst.inputs[1] = BUFFER_UNUSED;
    inst.inputs[2] = BUFFER_UNUSED;
    inst.reserved = 0;
    std::memcpy(&inst.state_id, &value, sizeof(float));
    return inst;
}

}  // namespace cedar
