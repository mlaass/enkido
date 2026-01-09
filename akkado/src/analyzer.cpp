#include "akkado/analyzer.hpp"
#include "akkado/builtins.hpp"

namespace akkado {

AnalysisResult SemanticAnalyzer::analyze(const Ast& ast, std::string_view filename) {
    input_ast_ = &ast;
    output_arena_ = AstArena{};
    symbols_ = SymbolTable{};
    diagnostics_.clear();
    node_map_.clear();
    filename_ = std::string(filename);

    if (!ast.valid()) {
        error("E001", "Invalid AST: no root node", {});
        return {std::move(symbols_), {}, std::move(diagnostics_), false};
    }

    // Pass 1: Collect variable definitions
    collect_definitions(ast.root);

    // Pass 2: Rewrite pipes (builds new AST)
    NodeIndex new_root = rewrite_pipes(ast.root);

    // Pass 3: Resolve and validate function calls
    resolve_and_validate(new_root);

    bool success = !has_errors(diagnostics_);

    AnalysisResult result;
    result.symbols = std::move(symbols_);
    result.transformed_ast.arena = std::move(output_arena_);
    result.transformed_ast.root = new_root;
    result.diagnostics = std::move(diagnostics_);
    result.success = success;

    return result;
}

void SemanticAnalyzer::collect_definitions(NodeIndex node) {
    if (node == NULL_NODE) return;

    const Node& n = (*input_ast_).arena[node];

    if (n.type == NodeType::Assignment) {
        // Variable name is stored in the node's data (as IdentifierData)
        const std::string& name = n.as_identifier();
        // We'll allocate actual buffers during code generation
        // For now, use a placeholder buffer index
        if (symbols_.is_defined_in_current_scope(name)) {
            warning("Variable '" + name + "' redefined", n.location);
        }
        symbols_.define_variable(name, 0xFFFF);
    }

    // Recurse to children
    NodeIndex child = n.first_child;
    while (child != NULL_NODE) {
        collect_definitions(child);
        child = (*input_ast_).arena[child].next_sibling;
    }
}

NodeIndex SemanticAnalyzer::rewrite_pipes(NodeIndex node) {
    if (node == NULL_NODE) return NULL_NODE;

    const Node& n = (*input_ast_).arena[node];

    if (n.type == NodeType::Pipe) {
        // Pipe: LHS |> RHS
        // Get LHS (first child) and RHS (second child)
        NodeIndex lhs_idx = n.first_child;
        NodeIndex rhs_idx = (lhs_idx != NULL_NODE) ?
                            (*input_ast_).arena[lhs_idx].next_sibling : NULL_NODE;

        if (lhs_idx == NULL_NODE || rhs_idx == NULL_NODE) {
            error("E002", "Invalid pipe expression", n.location);
            return NULL_NODE;
        }

        // First, recursively rewrite the LHS (may contain nested pipes)
        NodeIndex new_lhs = rewrite_pipes(lhs_idx);

        // Now substitute all holes in RHS with the LHS value
        // This performs: a |> f(%) -> f(a)
        // substitute_holes also handles any nested pipes in the RHS
        NodeIndex new_rhs = substitute_holes(rhs_idx, new_lhs);

        // The pipe is eliminated - return the transformed RHS
        return new_rhs;
    }

    // For non-pipe nodes, clone and recurse
    return clone_subtree(node);
}

NodeIndex SemanticAnalyzer::clone_node(NodeIndex src_idx) {
    if (src_idx == NULL_NODE) return NULL_NODE;

    const Node& src = (*input_ast_).arena[src_idx];

    // Allocate in output arena
    NodeIndex dst_idx = output_arena_.alloc(src.type, src.location);
    Node& dst = output_arena_[dst_idx];

    // Copy data
    dst.data = src.data;

    // Track mapping for substitute_holes
    node_map_[src_idx] = dst_idx;

    return dst_idx;
}

NodeIndex SemanticAnalyzer::clone_subtree(NodeIndex src_idx) {
    if (src_idx == NULL_NODE) return NULL_NODE;

    // Check if already cloned
    auto it = node_map_.find(src_idx);
    if (it != node_map_.end()) {
        return it->second;
    }

    const Node& src = (*input_ast_).arena[src_idx];

    // Handle pipe nodes specially during cloning
    if (src.type == NodeType::Pipe) {
        return rewrite_pipes(src_idx);
    }

    // Clone this node
    NodeIndex dst_idx = clone_node(src_idx);

    // Clone children
    NodeIndex src_child = src.first_child;
    NodeIndex prev_dst_child = NULL_NODE;

    while (src_child != NULL_NODE) {
        NodeIndex dst_child = clone_subtree(src_child);

        if (dst_child != NULL_NODE) {
            if (prev_dst_child == NULL_NODE) {
                output_arena_[dst_idx].first_child = dst_child;
            } else {
                output_arena_[prev_dst_child].next_sibling = dst_child;
            }
            prev_dst_child = dst_child;
        }

        src_child = (*input_ast_).arena[src_child].next_sibling;
    }

    return dst_idx;
}

NodeIndex SemanticAnalyzer::substitute_holes(NodeIndex node, NodeIndex replacement) {
    if (node == NULL_NODE) return NULL_NODE;

    const Node& n = (*input_ast_).arena[node];

    // If this is a hole, return the replacement node
    // (For MVP, multiple holes share the same replacement node)
    if (n.type == NodeType::Hole) {
        return replacement;
    }

    // Handle nested pipes - they need to be rewritten
    if (n.type == NodeType::Pipe) {
        // Get LHS and RHS of the nested pipe
        NodeIndex src_lhs = n.first_child;
        NodeIndex src_rhs = (src_lhs != NULL_NODE) ?
                            (*input_ast_).arena[src_lhs].next_sibling : NULL_NODE;

        // First, substitute holes in the LHS using the outer replacement
        NodeIndex new_lhs = substitute_holes(src_lhs, replacement);

        // Then, substitute holes in the RHS using the new LHS as replacement
        // This eliminates the nested pipe
        NodeIndex new_rhs = substitute_holes(src_rhs, new_lhs);

        // Return the transformed RHS (pipe is eliminated)
        return new_rhs;
    }

    // For other nodes, clone and recurse on children
    NodeIndex new_node = clone_node(node);

    NodeIndex src_child = n.first_child;
    NodeIndex prev_dst_child = NULL_NODE;

    while (src_child != NULL_NODE) {
        NodeIndex dst_child = substitute_holes(src_child, replacement);

        if (dst_child != NULL_NODE) {
            if (prev_dst_child == NULL_NODE) {
                output_arena_[new_node].first_child = dst_child;
            } else {
                output_arena_[prev_dst_child].next_sibling = dst_child;
            }
            prev_dst_child = dst_child;
        }

        src_child = (*input_ast_).arena[src_child].next_sibling;
    }

    return new_node;
}

bool SemanticAnalyzer::contains_hole(NodeIndex node) const {
    if (node == NULL_NODE) return false;

    const Node& n = (*input_ast_).arena[node];

    if (n.type == NodeType::Hole) return true;

    NodeIndex child = n.first_child;
    while (child != NULL_NODE) {
        if (contains_hole(child)) return true;
        child = (*input_ast_).arena[child].next_sibling;
    }

    return false;
}

void SemanticAnalyzer::resolve_and_validate(NodeIndex node) {
    if (node == NULL_NODE) return;

    const Node& n = output_arena_[node];

    if (n.type == NodeType::Hole) {
        // Holes should have been substituted - if we see one, it's an error
        error("E003", "Hole '%' used outside of pipe expression", n.location);
    }

    if (n.type == NodeType::Call) {
        // Function name is stored in the node's data (as IdentifierData)
        const std::string& func_name = n.as_identifier();

        // Look up in symbol table
        auto sym = symbols_.lookup(func_name);
        if (!sym) {
            error("E004", "Unknown function: '" + func_name + "'", n.location);
        } else if (sym->kind == SymbolKind::Builtin) {
            // Count arguments (children of the Call node)
            std::size_t arg_count = 0;
            NodeIndex arg = n.first_child;
            while (arg != NULL_NODE) {
                arg_count++;
                arg = output_arena_[arg].next_sibling;
            }

            validate_arguments(func_name, sym->builtin, arg_count, n.location);
        }
    }

    if (n.type == NodeType::Identifier) {
        // Check if identifier is defined
        const std::string& name = n.as_identifier();
        auto sym = symbols_.lookup(name);
        if (!sym) {
            error("E005", "Undefined identifier: '" + name + "'", n.location);
        }
    }

    // Recurse to children
    NodeIndex child = n.first_child;
    while (child != NULL_NODE) {
        resolve_and_validate(child);
        child = output_arena_[child].next_sibling;
    }
}

void SemanticAnalyzer::validate_arguments(const std::string& func_name,
                                          const BuiltinInfo& builtin,
                                          std::size_t arg_count,
                                          SourceLocation loc) {
    std::size_t min_args = builtin.input_count;
    std::size_t max_args = builtin.input_count + builtin.optional_count;

    if (arg_count < min_args) {
        error("E006", "Function '" + func_name + "' expects at least " +
              std::to_string(min_args) + " argument(s), got " +
              std::to_string(arg_count), loc);
    } else if (arg_count > max_args) {
        error("E007", "Function '" + func_name + "' expects at most " +
              std::to_string(max_args) + " argument(s), got " +
              std::to_string(arg_count), loc);
    }
}

void SemanticAnalyzer::error(const std::string& message, SourceLocation loc) {
    error("E000", message, loc);
}

void SemanticAnalyzer::error(const std::string& code, const std::string& message,
                             SourceLocation loc) {
    Diagnostic diag;
    diag.severity = Severity::Error;
    diag.code = code;
    diag.message = message;
    diag.filename = filename_;
    diag.location = loc;
    diagnostics_.push_back(std::move(diag));
}

void SemanticAnalyzer::warning(const std::string& message, SourceLocation loc) {
    Diagnostic diag;
    diag.severity = Severity::Warning;
    diag.code = "W000";
    diag.message = message;
    diag.filename = filename_;
    diag.location = loc;
    diagnostics_.push_back(std::move(diag));
}

} // namespace akkado
