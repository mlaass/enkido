#pragma once

// Helper functions for array higher-order function code generation
// These reduce boilerplate in map(), sum(), fold(), zipWith(), etc.

#include "akkado/ast.hpp"
#include "akkado/codegen.hpp"
#include "helpers.hpp"
#include <cedar/vm/instruction.hpp>
#include <vector>

namespace akkado {
namespace codegen {

// Emit a zero constant buffer (used for empty array results)
// Returns buffer index or BUFFER_UNUSED on failure
[[gnu::always_inline]]
inline std::uint16_t emit_zero(
    BufferAllocator& buffers,
    std::vector<cedar::Instruction>& instructions
) {
    std::uint16_t out = buffers.allocate();
    if (out == BufferAllocator::BUFFER_UNUSED) {
        return BufferAllocator::BUFFER_UNUSED;
    }

    cedar::Instruction inst{};
    inst.opcode = cedar::Opcode::PUSH_CONST;
    inst.out_buffer = out;
    set_unused_inputs(inst);
    encode_const_value(inst, 0.0f);
    instructions.push_back(inst);

    return out;
}

// Result of extracting call arguments
struct CallArgs {
    std::vector<NodeIndex> nodes;
    bool valid = true;
};

// Extract N arguments from a Call node, unwrapping Argument wrappers
// Returns empty nodes vector with valid=false if argument count doesn't match expected
[[gnu::always_inline]]
inline CallArgs extract_call_args(
    const AstArena& arena,
    NodeIndex first_arg,
    std::size_t expected_min,
    std::size_t expected_max = 0
) {
    if (expected_max == 0) expected_max = expected_min;

    CallArgs result;
    NodeIndex arg = first_arg;

    while (arg != NULL_NODE) {
        NodeIndex unwrapped = unwrap_argument(arena, arg);
        result.nodes.push_back(unwrapped);
        arg = arena[arg].next_sibling;
    }

    if (result.nodes.size() < expected_min || result.nodes.size() > expected_max) {
        result.valid = false;
    }

    return result;
}

// Finalize multi-buffer array result:
// - Empty vector: emit zero constant
// - Single element: return directly
// - Multiple elements: register as multi-buffer
// Returns the first buffer index for the result
[[gnu::always_inline]]
inline std::uint16_t finalize_array_result(
    CodeGenerator& cg,
    BufferAllocator& buffers,
    std::vector<cedar::Instruction>& instructions,
    NodeIndex node,
    std::vector<std::uint16_t> result_buffers,
    std::unordered_map<NodeIndex, std::uint16_t>& node_buffers,
    std::unordered_map<NodeIndex, std::vector<std::uint16_t>>& multi_buffers
) {
    if (result_buffers.empty()) {
        std::uint16_t zero = emit_zero(buffers, instructions);
        node_buffers[node] = zero;
        return zero;
    }

    if (result_buffers.size() == 1) {
        node_buffers[node] = result_buffers[0];
        return result_buffers[0];
    }

    // Register as multi-buffer and return first
    std::uint16_t first_buf = result_buffers[0];
    multi_buffers[node] = std::move(result_buffers);
    node_buffers[node] = first_buf;
    return first_buf;
}

// Get input buffers from a node (handles both single and multi-buffer sources)
[[gnu::always_inline]]
inline std::vector<std::uint16_t> get_input_buffers(
    NodeIndex array_node,
    std::uint16_t single_buf,
    const std::unordered_map<NodeIndex, std::vector<std::uint16_t>>& multi_buffers
) {
    auto it = multi_buffers.find(array_node);
    if (it != multi_buffers.end() && it->second.size() > 1) {
        return it->second;
    }
    return {single_buf};
}

} // namespace codegen
} // namespace akkado
