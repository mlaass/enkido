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

} // namespace codegen
} // namespace akkado
