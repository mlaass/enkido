#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../vm/env_map.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
#include <cmath>
#include <cstring>

namespace cedar {

// PUSH_CONST: Fill output buffer with constant value
// The constant is stored directly in state_id (32 bits)
[[gnu::always_inline]]
inline void op_push_const(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);

    // Read 32-bit float directly from state_id
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
// inputs[0]: left channel (required)
// inputs[1]: right channel (optional, uses left if BUFFER_UNUSED)
[[gnu::always_inline]]
inline void op_output(ExecutionContext& ctx, const Instruction& inst) {
    const float* left = ctx.buffers->get(inst.inputs[0]);
    const float* right = (inst.inputs[1] != BUFFER_UNUSED)
        ? ctx.buffers->get(inst.inputs[1])
        : left;  // mono: use left for both

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        ctx.output_left[i] += left[i];
        ctx.output_right[i] += right[i];
    }
}

// NOISE: Noise generator (deterministic LCG for reproducibility)
// in0: freq - rate in Hz (0 = white noise, >0 = sample-and-hold at that frequency)
// in1: trig - reset RNG to start_seed on rising edge (optional)
// in2: seed - initial seed value (optional, default 12345)
[[gnu::always_inline]]
inline void op_noise(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);

    // Get inputs (fall back to BUFFER_ZERO for unused)
    const float* freq = (inst.inputs[0] != BUFFER_UNUSED)
        ? ctx.buffers->get(inst.inputs[0])
        : ctx.buffers->get(BUFFER_ZERO);
    const float* trigger = (inst.inputs[1] != BUFFER_UNUSED)
        ? ctx.buffers->get(inst.inputs[1])
        : ctx.buffers->get(BUFFER_ZERO);
    const float* seed_input = (inst.inputs[2] != BUFFER_UNUSED)
        ? ctx.buffers->get(inst.inputs[2])
        : nullptr;

    auto& state = ctx.states->get_or_create<NoiseState>(inst.state_id);

    // Helper: generate next random value using LCG
    auto generate = [&state]() -> float {
        state.seed = state.seed * 1103515245u + 12345u;
        return static_cast<float>(static_cast<std::int32_t>(state.seed)) / 2147483648.0f;
    };

    // Initialize on first run
    if (!state.initialized) {
        state.start_seed = seed_input ? static_cast<std::uint32_t>(seed_input[0]) : 12345u;
        state.seed = state.start_seed;
        state.current_value = generate();
        state.initialized = true;
    }

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check trigger - reset to start seed on rising edge
        if (trigger[i] > 0.0f && state.prev_trigger <= 0.0f) {
            state.seed = state.start_seed;
            state.phase = 0.0f;
            state.current_value = generate();
        }
        state.prev_trigger = trigger[i];

        float f = freq[i];
        if (f <= 0.0f) {
            // Every-sample mode: new value each sample (white noise)
            out[i] = generate();
        } else {
            // Sample-and-hold mode: new value at phase wrap
            float phase_inc = f / ctx.sample_rate;
            state.phase += phase_inc;
            if (state.phase >= 1.0f) {
                state.phase -= 1.0f;
                state.current_value = generate();
            }
            out[i] = state.current_value;
        }
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
// Constant stored directly in state_id (32 bits)
[[gnu::always_inline]]
inline void op_dc(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* in = ctx.buffers->get(inst.inputs[0]);

    // Read 32-bit float directly from state_id
    float offset;
    std::memcpy(&offset, &inst.state_id, sizeof(float));

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = in[i] + offset;
    }
}

// SLEW: Slew rate limiter (smooths sudden changes)
// in0: target signal
// in1: rate (units per second, e.g., rate=10 means 100ms to traverse 0→1)
[[gnu::always_inline]]
inline void op_slew(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* target = ctx.buffers->get(inst.inputs[0]);
    const float* rate_buf = ctx.buffers->get(inst.inputs[1]);
    auto& state = ctx.states->get_or_create<SlewState>(inst.state_id);

    // Initialize state to first input value (instant startup)
    if (!state.initialized) {
        state.current = target[0];
        state.initialized = true;
    }

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float rate = rate_buf[i];
        // Linear slew rate limiter: limit change to rate units per second
        float max_delta = (rate > 0.0f) ? rate / ctx.sample_rate : 1e10f;
        float delta = target[i] - state.current;

        if (std::abs(delta) <= max_delta) {
            state.current = target[i];
        } else if (delta > 0.0f) {
            state.current += max_delta;
        } else {
            state.current -= max_delta;
        }
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

// ENV_GET: Read external environment parameter with interpolation
// state_id contains FNV-1a hash of parameter name
// inputs[0]: optional fallback value buffer (BUFFER_UNUSED if none)
[[gnu::always_inline]]
inline void op_env_get(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);

    // Get fallback value if provided
    float fallback = 0.0f;
    if (inst.inputs[0] != BUFFER_UNUSED) {
        fallback = ctx.buffers->get(inst.inputs[0])[0];  // Control-rate sample
    }

    // Check if env_map is available
    if (!ctx.env_map) {
        std::fill_n(out, BLOCK_SIZE, fallback);
        return;
    }

    // Per-sample interpolation for smooth transitions
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        ctx.env_map->update_interpolation_sample();
        float value = ctx.env_map->get(inst.state_id);

        // Return fallback if parameter doesn't exist
        if (!ctx.env_map->has_param_hash(inst.state_id)) {
            out[i] = fallback;
        } else {
            out[i] = value;
        }
    }
}

// Helper: Create instruction with float constant stored in state_id
inline Instruction make_const_instruction(Opcode op, std::uint16_t out, float value) {
    Instruction inst{};
    inst.opcode = op;
    inst.out_buffer = out;
    inst.inputs[0] = BUFFER_UNUSED;
    inst.inputs[1] = BUFFER_UNUSED;
    inst.inputs[2] = BUFFER_UNUSED;
    inst.inputs[3] = BUFFER_UNUSED;
    inst.inputs[4] = BUFFER_UNUSED;
    // Store float directly in state_id (32 bits)
    std::memcpy(&inst.state_id, &value, sizeof(float));
    return inst;
}

}  // namespace cedar
