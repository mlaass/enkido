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

// NOTE: The free `emit_zero(buffers, instructions)` helper was removed in
// PRD prd-parser-codegen-correctness.md Phase 3 (F2). Use
// `CodeGenerator::emit_zero()` instead — it routes through emit() so
// source_locations_ stays in sync.

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

// NOTE: The free `finalize_array_result(node, ..., node_types, buffers,
// instructions)` helper was removed in PRD prd-parser-codegen-correctness.md
// Phase 3 (F2). Use `CodeGenerator::finalize_array_result(node, buffers)`
// instead — it routes empty-array zero emission through emit() so
// source_locations_ stays in sync.

// Get input buffers from a node (handles both single and multi-buffer sources)
// Checks the node_types map for Array typed values
[[gnu::always_inline]]
inline std::vector<std::uint16_t> get_input_buffers(
    NodeIndex array_node,
    std::uint16_t single_buf,
    const std::unordered_map<NodeIndex, TypedValue>& node_types
) {
    auto it = node_types.find(array_node);
    if (it != node_types.end() && it->second.type == ValueType::Array && it->second.array) {
        std::vector<std::uint16_t> bufs;
        bufs.reserve(it->second.array->elements.size());
        for (const auto& elem : it->second.array->elements) {
            bufs.push_back(elem.buffer);
        }
        if (bufs.size() > 1) return bufs;
    }
    return {single_buf};
}

} // namespace codegen
} // namespace akkado
