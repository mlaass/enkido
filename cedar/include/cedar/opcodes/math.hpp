#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include <cmath>
#include <algorithm>

namespace cedar {

// ABS: out = |in0|
[[gnu::always_inline]]
inline void op_abs(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = std::abs(a[i]);
    }
}

// SQRT: out = sqrt(in0)
[[gnu::always_inline]]
inline void op_sqrt(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = std::sqrt(std::max(0.0f, a[i]));  // Clamp negative to avoid NaN
    }
}

// LOG: out = log(in0) (natural logarithm)
[[gnu::always_inline]]
inline void op_log(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = std::log(std::max(1e-10f, a[i]));  // Avoid log(0)
    }
}

// EXP: out = e^in0
[[gnu::always_inline]]
inline void op_exp(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = std::exp(std::clamp(a[i], -87.0f, 87.0f));  // Avoid overflow
    }
}

// MIN: out = min(in0, in1)
[[gnu::always_inline]]
inline void op_min(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);
    const float* b = ctx.buffers->get(inst.inputs[1]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = std::min(a[i], b[i]);
    }
}

// MAX: out = max(in0, in1)
[[gnu::always_inline]]
inline void op_max(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);
    const float* b = ctx.buffers->get(inst.inputs[1]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = std::max(a[i], b[i]);
    }
}

// CLAMP: out = clamp(in0, in1, in2) where in1=min, in2=max
[[gnu::always_inline]]
inline void op_clamp(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* value = ctx.buffers->get(inst.inputs[0]);
    const float* lo = ctx.buffers->get(inst.inputs[1]);
    const float* hi = ctx.buffers->get(inst.inputs[2]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = std::clamp(value[i], lo[i], hi[i]);
    }
}

// WRAP: out = wrap(in0, in1, in2) where in1=min, in2=max
// Wraps value to range [min, max)
[[gnu::always_inline]]
inline void op_wrap(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* value = ctx.buffers->get(inst.inputs[0]);
    const float* lo = ctx.buffers->get(inst.inputs[1]);
    const float* hi = ctx.buffers->get(inst.inputs[2]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float range = hi[i] - lo[i];
        if (range > 0.0f) {
            float v = value[i] - lo[i];
            out[i] = lo[i] + v - range * std::floor(v / range);
        } else {
            out[i] = lo[i];
        }
    }
}

// FLOOR: out = floor(in0)
[[gnu::always_inline]]
inline void op_floor(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = std::floor(a[i]);
    }
}

// CEIL: out = ceil(in0)
[[gnu::always_inline]]
inline void op_ceil(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = std::ceil(a[i]);
    }
}

// ============================================================================
// Trigonometric Functions (radians)
// ============================================================================

// MATH_SIN: out = sin(in0) - NOT an oscillator, pure trig function
[[gnu::always_inline]]
inline void op_math_sin(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = std::sin(a[i]);
    }
}

// MATH_COS: out = cos(in0)
[[gnu::always_inline]]
inline void op_math_cos(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = std::cos(a[i]);
    }
}

// MATH_TAN: out = tan(in0)
[[gnu::always_inline]]
inline void op_math_tan(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = std::tan(a[i]);
    }
}

// MATH_ASIN: out = asin(in0)
[[gnu::always_inline]]
inline void op_math_asin(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = std::asin(std::clamp(a[i], -1.0f, 1.0f));  // Clamp to valid domain
    }
}

// MATH_ACOS: out = acos(in0)
[[gnu::always_inline]]
inline void op_math_acos(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = std::acos(std::clamp(a[i], -1.0f, 1.0f));  // Clamp to valid domain
    }
}

// MATH_ATAN: out = atan(in0)
[[gnu::always_inline]]
inline void op_math_atan(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = std::atan(a[i]);
    }
}

// MATH_ATAN2: out = atan2(in0, in1) where in0=y, in1=x
[[gnu::always_inline]]
inline void op_math_atan2(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* y = ctx.buffers->get(inst.inputs[0]);
    const float* x = ctx.buffers->get(inst.inputs[1]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = std::atan2(y[i], x[i]);
    }
}

// ============================================================================
// Hyperbolic Functions
// ============================================================================

// MATH_SINH: out = sinh(in0)
[[gnu::always_inline]]
inline void op_math_sinh(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = std::sinh(std::clamp(a[i], -87.0f, 87.0f));  // Avoid overflow
    }
}

// MATH_COSH: out = cosh(in0)
[[gnu::always_inline]]
inline void op_math_cosh(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = std::cosh(std::clamp(a[i], -87.0f, 87.0f));  // Avoid overflow
    }
}

// MATH_TANH: out = tanh(in0) - pure hyperbolic tangent
// Unlike DISTORT_TANH which has a drive parameter, this is the raw math function.
// Useful for waveshaping: tanh(signal * drive) where drive is computed separately.
[[gnu::always_inline]]
inline void op_math_tanh(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = std::tanh(a[i]);
    }
}

}  // namespace cedar
