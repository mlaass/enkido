#pragma once

// Helper functions for code generation
// These are commonly used patterns extracted for reuse and readability

#include "akkado/ast.hpp"
#include "akkado/codegen.hpp"
#include <cedar/vm/instruction.hpp>
#include <cstring>
#include <vector>
#include <string>

namespace akkado {
namespace codegen {

// ============================================================================
// Instruction Encoding Helpers
// ============================================================================

/// Encode a float constant in a PUSH_CONST instruction.
/// The float is stored directly in state_id (32 bits).
[[gnu::always_inline]]
inline void encode_const_value(cedar::Instruction& inst, float value) {
    std::memcpy(&inst.state_id, &value, sizeof(float));
    inst.inputs[4] = 0xFFFF;  // BUFFER_UNUSED
}

/// Create and emit a PUSH_CONST instruction, returning the output buffer index.
/// Returns BufferAllocator::BUFFER_UNUSED if buffer pool exhausted.
[[gnu::always_inline]]
inline std::uint16_t emit_push_const(
    BufferAllocator& buffers,
    std::vector<cedar::Instruction>& instructions,
    float value
) {
    std::uint16_t out = buffers.allocate();
    if (out == BufferAllocator::BUFFER_UNUSED) {
        return BufferAllocator::BUFFER_UNUSED;
    }

    cedar::Instruction inst{};
    inst.opcode = cedar::Opcode::PUSH_CONST;
    inst.out_buffer = out;
    inst.inputs[0] = 0xFFFF;
    inst.inputs[1] = 0xFFFF;
    inst.inputs[2] = 0xFFFF;
    inst.inputs[3] = 0xFFFF;
    encode_const_value(inst, value);

    instructions.push_back(inst);
    return out;
}

// ============================================================================
// AST Navigation Helpers
// ============================================================================

/// Unwrap an Argument node to get the inner value node.
/// If the node is not an Argument, returns the node itself.
[[gnu::always_inline]]
inline NodeIndex unwrap_argument(const AstArena& arena, NodeIndex arg) {
    if (arg == NULL_NODE) return NULL_NODE;
    const Node& n = arena[arg];
    if (n.type == NodeType::Argument) {
        return n.first_child;
    }
    return arg;
}

/// Count the number of arguments in a Call node
[[gnu::always_inline]]
inline std::size_t count_call_args(const AstArena& arena, NodeIndex first_arg) {
    std::size_t count = 0;
    NodeIndex arg = first_arg;
    while (arg != NULL_NODE) {
        ++count;
        arg = arena[arg].next_sibling;
    }
    return count;
}

/// Information extracted from a Closure node
struct ClosureInfo {
    std::vector<std::string> params;
    NodeIndex body;
};

/// Extract closure parameters and body from a Closure node.
/// Parameters are stored as Identifier nodes with ClosureParamData or IdentifierData.
/// The body is the last child that is not an identifier.
[[gnu::always_inline]]
inline ClosureInfo extract_closure_info(const AstArena& arena, NodeIndex closure_node) {
    ClosureInfo info{};
    info.body = NULL_NODE;

    if (closure_node == NULL_NODE) return info;
    const Node& closure = arena[closure_node];
    if (closure.type != NodeType::Closure) return info;

    NodeIndex child = closure.first_child;
    while (child != NULL_NODE) {
        const Node& child_node = arena[child];
        if (child_node.type == NodeType::Identifier) {
            if (std::holds_alternative<Node::ClosureParamData>(child_node.data)) {
                info.params.push_back(child_node.as_closure_param().name);
            } else if (std::holds_alternative<Node::IdentifierData>(child_node.data)) {
                info.params.push_back(child_node.as_identifier());
            } else {
                // Not a parameter, must be body
                info.body = child;
                break;
            }
        } else {
            // Non-identifier child is the body
            info.body = child;
            break;
        }
        child = child_node.next_sibling;
    }

    return info;
}

// ============================================================================
// Buffer Allocation Helpers
// ============================================================================

/// Allocate multiple buffers at once.
/// Returns true if all allocations succeeded, false otherwise.
/// On failure, the allocated buffers are still valid (no rollback).
template<typename... Args>
[[gnu::always_inline]]
inline bool allocate_buffers(BufferAllocator& alloc, std::uint16_t* first, Args*... rest) {
    *first = alloc.allocate();
    if (*first == BufferAllocator::BUFFER_UNUSED) {
        return false;
    }
    if constexpr (sizeof...(rest) > 0) {
        return allocate_buffers(alloc, rest...);
    }
    return true;
}

/// Initialize unused inputs in an instruction
[[gnu::always_inline]]
inline void set_unused_inputs(cedar::Instruction& inst) {
    inst.inputs[0] = 0xFFFF;
    inst.inputs[1] = 0xFFFF;
    inst.inputs[2] = 0xFFFF;
    inst.inputs[3] = 0xFFFF;
    inst.inputs[4] = 0xFFFF;
}

// ============================================================================
// SAMPLE_PLAY emission (single source of truth)
// ============================================================================
// All SAMPLE_PLAY emission for sample patterns and the scalar sample() builtin
// goes through emit_sample_chain. See docs/prd-sample-emission-unification.md.
struct SamplePatternEmitCtx {
    enum class Kind {
        Pattern,  // pat / chord / velocity / bank / variant / transport-clock
        Scalar,   // builtin sample(trig, pitch, "bd")
    };
    Kind            kind            = Kind::Pattern;
    // Pattern mode: linked SequenceState id. The emitted SAMPLE_PLAY's own
    // state_id is `seq_state_id + 1` so the sampler can read its upstream
    // SEQPAT events without colliding state ids.
    // Scalar mode: caller passes the value compute_state_id() produces from
    // the active semantic-path stack so hot-swap behavior matches the
    // generic-builtin-emission path.
    std::uint32_t   seq_state_id    = 0;
    std::uint16_t   value_buf       = BufferAllocator::BUFFER_UNUSED; // sample-id buffer
    std::uint16_t   trigger_buf     = BufferAllocator::BUFFER_UNUSED;
    // BUFFER_UNUSED → emit no MUL (used by Scalar mode today, which has no
    // velocity input on the sample() builtin).
    std::uint16_t   velocity_buf    = BufferAllocator::BUFFER_UNUSED;
    // BUFFER_UNUSED → helper allocates and emits its own PUSH_CONST 1.0.
    // Scalar callers can pass the user-supplied pitch arg buffer.
    std::uint16_t   pitch_buf       = BufferAllocator::BUFFER_UNUSED;
    // Scalar callers pass builtin->inst_rate to preserve the rate-field
    // bytecode the generic-builtin-emission path sets. Pattern callers leave
    // it at 0 (current pattern path emits SAMPLE_PLAY with rate=0).
    std::uint8_t    rate            = 0;
    SourceLocation  loc             = {};
};

// Free helper used by both class methods (via a thunk that calls this->emit
// to keep source_locations_ in sync) and the static emit_pattern_with_state
// (via a thunk wrapping its existing emit_fn function pointer).
//
// Returns the final audio output buffer, or BufferAllocator::BUFFER_UNUSED on
// allocation failure (caller is responsible for diagnostic emission — both
// existing call sites already short-circuit on this sentinel).
template <typename EmitFn>
inline std::uint16_t emit_sample_chain(
    BufferAllocator& buffers,
    EmitFn&& emit,
    const SamplePatternEmitCtx& ctx) {

    // 1. Pitch input. Scalar callers may already have a pitch buffer; Pattern
    //    callers always allocate a fresh PUSH_CONST 1.0 (sample patterns play
    //    at original pitch — pitch shifting is reserved for sample()).
    std::uint16_t pitch_buf = ctx.pitch_buf;
    if (pitch_buf == BufferAllocator::BUFFER_UNUSED) {
        pitch_buf = buffers.allocate();
        if (pitch_buf == BufferAllocator::BUFFER_UNUSED) {
            return BufferAllocator::BUFFER_UNUSED;
        }
        cedar::Instruction pitch_inst{};
        pitch_inst.opcode = cedar::Opcode::PUSH_CONST;
        pitch_inst.out_buffer = pitch_buf;
        pitch_inst.inputs[0] = 0xFFFF;
        pitch_inst.inputs[1] = 0xFFFF;
        pitch_inst.inputs[2] = 0xFFFF;
        pitch_inst.inputs[3] = 0xFFFF;
        encode_const_value(pitch_inst, 1.0f);
        emit(pitch_inst);
    }

    // 2. SAMPLE_PLAY. Stereo-native (prd-stereo-native-opcodes Phase 3):
    //    allocate an adjacent L/R output pair and set the STEREO_OUTPUT flag
    //    so the opcode writes both channels in one call. inputs[3]/[4] split
    //    the linked SequenceState id in half (Pattern mode) or BUFFER_UNUSED
    //    (Scalar mode, falls back to the scalar sample_id buffer in
    //    op_sample_play).
    std::uint16_t output_buf = buffers.allocate();
    std::uint16_t output_buf_r = buffers.allocate();
    if (output_buf == BufferAllocator::BUFFER_UNUSED ||
        output_buf_r == BufferAllocator::BUFFER_UNUSED ||
        output_buf_r != output_buf + 1) {
        return BufferAllocator::BUFFER_UNUSED;
    }
    cedar::Instruction sample_inst{};
    sample_inst.opcode = cedar::Opcode::SAMPLE_PLAY;
    sample_inst.out_buffer = output_buf;
    sample_inst.inputs[0] = ctx.trigger_buf;
    sample_inst.inputs[1] = pitch_buf;
    sample_inst.inputs[2] = ctx.value_buf;
    sample_inst.rate      = ctx.rate;
    sample_inst.flags     = cedar::InstructionFlag::STEREO_OUTPUT;
    if (ctx.kind == SamplePatternEmitCtx::Kind::Pattern) {
        sample_inst.inputs[3] = static_cast<std::uint16_t>(ctx.seq_state_id & 0xFFFFu);
        sample_inst.inputs[4] = static_cast<std::uint16_t>((ctx.seq_state_id >> 16) & 0xFFFFu);
        sample_inst.state_id  = ctx.seq_state_id + 1;
    } else {
        sample_inst.inputs[3] = 0xFFFF;
        sample_inst.inputs[4] = 0xFFFF;
        sample_inst.state_id  = ctx.seq_state_id;
    }
    emit(sample_inst);

    // 3. Velocity post-multiply. Skipped if velocity_buf is BUFFER_UNUSED
    //    (Scalar mode today). Per-voice velocity is already applied inside
    //    op_sample_play via evt.velocities[v]; the post-MUL exists only to
    //    let `velocity(pat, runtime_expr)` scale the sampler output at
    //    runtime via velocity_buf.
    //
    //    Stereo-native sampler emits a stereo pair (out, out+1). The post-MUL
    //    scales both channels by the same velocity buffer (mono control). The
    //    scaling produces a second stereo pair (scaled, scaled+1).
    if (ctx.velocity_buf == BufferAllocator::BUFFER_UNUSED) {
        return output_buf;
    }
    std::uint16_t scaled_buf = buffers.allocate();
    std::uint16_t scaled_buf_r = buffers.allocate();
    if (scaled_buf == BufferAllocator::BUFFER_UNUSED ||
        scaled_buf_r == BufferAllocator::BUFFER_UNUSED ||
        scaled_buf_r != scaled_buf + 1) {
        return BufferAllocator::BUFFER_UNUSED;
    }
    // Scale L channel
    cedar::Instruction mul_l{};
    mul_l.opcode = cedar::Opcode::MUL;
    mul_l.out_buffer = scaled_buf;
    mul_l.inputs[0] = output_buf;
    mul_l.inputs[1] = ctx.velocity_buf;
    mul_l.inputs[2] = 0xFFFF;
    mul_l.inputs[3] = 0xFFFF;
    mul_l.inputs[4] = 0xFFFF;
    emit(mul_l);
    // Scale R channel
    cedar::Instruction mul_r{};
    mul_r.opcode = cedar::Opcode::MUL;
    mul_r.out_buffer = scaled_buf_r;
    mul_r.inputs[0] = output_buf_r;
    mul_r.inputs[1] = ctx.velocity_buf;
    mul_r.inputs[2] = 0xFFFF;
    mul_r.inputs[3] = 0xFFFF;
    mul_r.inputs[4] = 0xFFFF;
    emit(mul_r);
    return scaled_buf;
}

} // namespace codegen
} // namespace akkado
