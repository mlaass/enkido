#pragma once

#include <string>
#include <vector>
#include "ast.hpp"
#include <cedar/opcodes/sequence.hpp>

namespace akkado {

/// Serialize a mini-notation AST node and its children to JSON
/// @param root The root node index of the mini-notation subtree
/// @param arena The AST arena containing all nodes
/// @return JSON string representing the AST structure
std::string serialize_mini_ast_json(NodeIndex root, const AstArena& arena);

/// Serialize compiled sequences and their events to JSON
/// @param sequences Vector of compiled Sequence structs
/// @param sequence_events Vector of event vectors (one per sequence)
/// @return JSON string with sequence hierarchy and events
std::string serialize_sequences_json(
    const std::vector<cedar::Sequence>& sequences,
    const std::vector<std::vector<cedar::Event>>& sequence_events);

} // namespace akkado
