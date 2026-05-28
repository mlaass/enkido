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

// Rewrites each `Identifier("_")` in `args` whose slot has a numeric
// default in `builtin` into a synthetic NumberLit node in `arena`, so that
// downstream "must be a number literal" checks in specialized handlers
// (poly's voices/release, tap_delay's dry/wet, etc.) see the resolved
// default instead of failing on the bare `_` identifier.
//
// Slots without a default are left untouched — the analyzer's
// `all_placeholders_have_defaults` pre-pass already converts those into
// closure partial-applications before codegen, so a residual `_` here is
// either a defensive miss or a future call site to harden.
//
// Predicate must stay in sync with `analyzer.cpp` `is_placeholder_node` and
// the generic CallSlot branch at `codegen.cpp` (`CallSlot::Underscore`).
//
// String defaults are out of scope — no specialized handler today reads a
// string-defaulted optional via `extract_call_args`.
inline void resolve_underscore_defaults(
    AstArena& arena,
    const StringInterner& interner,
    std::vector<NodeIndex>& args,
    const BuiltinInfo& builtin
) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        NodeIndex inner = unwrap_argument(arena, args[i]);
        if (inner == NULL_NODE) continue;
        const Node& n = arena[inner];
        if (n.type != NodeType::Identifier) continue;
        if (!std::holds_alternative<Node::IdentifierData>(n.data)) continue;
        if (interner.view(n.as_identifier()) != "_") continue;
        if (!builtin.has_default(i)) continue;

        float default_val = builtin.get_default(i);
        SourceLocation loc = n.location;
        NodeIndex synthetic = arena.alloc(NodeType::NumberLit, loc);
        arena[synthetic].data = Node::NumberData{
            static_cast<double>(default_val), false};
        args[i] = synthetic;
    }
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
