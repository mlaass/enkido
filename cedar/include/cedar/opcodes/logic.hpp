#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include <cmath>

namespace cedar {

// Small epsilon for floating-point equality comparison
constexpr float LOGIC_EPSILON = 1e-6f;

// ============================================================================
// Signal Selection
// ============================================================================

// SELECT: out = (cond > 0) ? a : b
// Sample-by-sample signal multiplexing based on condition signal
[[gnu::always_inline]]
inline void op_select(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* cond = ctx.buffers->get(inst.inputs[0]);
    const float* a = ctx.buffers->get(inst.inputs[1]);
    const float* b = ctx.buffers->get(inst.inputs[2]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = (cond[i] > 0.0f) ? a[i] : b[i];
    }
}

// ============================================================================
// Comparison Operations (output 0.0 or 1.0)
// ============================================================================

// CMP_GT: out = (a > b) ? 1.0 : 0.0
[[gnu::always_inline]]
inline void op_cmp_gt(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);
    const float* b = ctx.buffers->get(inst.inputs[1]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = (a[i] > b[i]) ? 1.0f : 0.0f;
    }
}

// CMP_LT: out = (a < b) ? 1.0 : 0.0
[[gnu::always_inline]]
inline void op_cmp_lt(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);
    const float* b = ctx.buffers->get(inst.inputs[1]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = (a[i] < b[i]) ? 1.0f : 0.0f;
    }
}

// CMP_GTE: out = (a >= b) ? 1.0 : 0.0
[[gnu::always_inline]]
inline void op_cmp_gte(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);
    const float* b = ctx.buffers->get(inst.inputs[1]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = (a[i] >= b[i]) ? 1.0f : 0.0f;
    }
}

// CMP_LTE: out = (a <= b) ? 1.0 : 0.0
[[gnu::always_inline]]
inline void op_cmp_lte(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);
    const float* b = ctx.buffers->get(inst.inputs[1]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = (a[i] <= b[i]) ? 1.0f : 0.0f;
    }
}

// CMP_EQ: out = (|a - b| < epsilon) ? 1.0 : 0.0
[[gnu::always_inline]]
inline void op_cmp_eq(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);
    const float* b = ctx.buffers->get(inst.inputs[1]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = (std::abs(a[i] - b[i]) < LOGIC_EPSILON) ? 1.0f : 0.0f;
    }
}

// CMP_NEQ: out = (|a - b| >= epsilon) ? 1.0 : 0.0
[[gnu::always_inline]]
inline void op_cmp_neq(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);
    const float* b = ctx.buffers->get(inst.inputs[1]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = (std::abs(a[i] - b[i]) >= LOGIC_EPSILON) ? 1.0f : 0.0f;
    }
}

// ============================================================================
// Logical Operations (treat > 0 as true)
// ============================================================================

// LOGIC_AND: out = ((a > 0) && (b > 0)) ? 1.0 : 0.0
[[gnu::always_inline]]
inline void op_logic_and(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);
    const float* b = ctx.buffers->get(inst.inputs[1]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = ((a[i] > 0.0f) && (b[i] > 0.0f)) ? 1.0f : 0.0f;
    }
}

// LOGIC_OR: out = ((a > 0) || (b > 0)) ? 1.0 : 0.0
[[gnu::always_inline]]
inline void op_logic_or(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);
    const float* b = ctx.buffers->get(inst.inputs[1]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = ((a[i] > 0.0f) || (b[i] > 0.0f)) ? 1.0f : 0.0f;
    }
}

// LOGIC_NOT: out = (a > 0) ? 0.0 : 1.0
[[gnu::always_inline]]
inline void op_logic_not(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = (a[i] > 0.0f) ? 0.0f : 1.0f;
    }
}

}  // namespace cedar
