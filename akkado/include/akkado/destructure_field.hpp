#pragma once

#include <string>
#include <variant>
#include <vector>

#include "ast.hpp"

namespace akkado {

/// A destructure field binding: name + optional default-expression node.
/// This is the plumbing form used by parser results, symbol-table Params
/// and codegen binding. The AST itself stores only the names
/// (DestructureField in ast.hpp); default-expression nodes live in the
/// owning Node's extra_children (index-aligned with the fields).
struct DestructureBinding {
    std::string name;
    NodeIndex default_node = NULL_NODE;
};

/// Zip a DestructureAssignment / DestructureParam node's field names with
/// its extra_children defaults into the plumbing form.
inline std::vector<DestructureBinding> destructure_bindings(const Node& n) {
    const std::vector<DestructureField>* fields = nullptr;
    if (std::holds_alternative<Node::DestructureAssignmentData>(n.data)) {
        fields = &std::get<Node::DestructureAssignmentData>(n.data).fields;
    } else if (std::holds_alternative<Node::DestructureParamData>(n.data)) {
        fields = &std::get<Node::DestructureParamData>(n.data).fields;
    }
    std::vector<DestructureBinding> out;
    if (!fields) return out;
    out.reserve(fields->size());
    for (std::size_t i = 0; i < fields->size(); ++i) {
        out.push_back({(*fields)[i].name, n.extra_child(i)});
    }
    return out;
}

}  // namespace akkado
