#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include <cmath>

namespace cedar {

// ADD: out = in0 + in1
[[gnu::always_inline]]
inline void op_add(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);
    const float* b = ctx.buffers->get(inst.inputs[1]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = a[i] + b[i];
    }
}

// SUB: out = in0 - in1
[[gnu::always_inline]]
inline void op_sub(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);
    const float* b = ctx.buffers->get(inst.inputs[1]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = a[i] - b[i];
    }
}

// MUL: out = in0 * in1
[[gnu::always_inline]]
inline void op_mul(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);
    const float* b = ctx.buffers->get(inst.inputs[1]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = a[i] * b[i];
    }
}

// DIV: out = in0 / in1 (safe division, returns 0 for 0/0)
[[gnu::always_inline]]
inline void op_div(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);
    const float* b = ctx.buffers->get(inst.inputs[1]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = (b[i] != 0.0f) ? (a[i] / b[i]) : 0.0f;
    }
}

// POW: out = in0 ^ in1
[[gnu::always_inline]]
inline void op_pow(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);
    const float* b = ctx.buffers->get(inst.inputs[1]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = std::pow(a[i], b[i]);
    }
}

// NEG: out = -in0
[[gnu::always_inline]]
inline void op_neg(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = -a[i];
    }
}

}  // namespace cedar
