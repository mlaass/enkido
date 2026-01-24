#pragma once

#include "ast.hpp"
#include "diagnostics.hpp"
#include "symbol_table.hpp"
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace akkado {

/// Result of semantic analysis
struct AnalysisResult {
    SymbolTable symbols;           // Symbol table after analysis
    Ast transformed_ast;           // AST after pipe rewriting
    std::vector<Diagnostic> diagnostics;
    bool success = false;
};

/// Semantic analyzer: validates AST and rewrites pipes
///
/// Three passes:
/// 1. Collect definitions: Walk AST, register all Assignment nodes
/// 2. Pipe rewriting: Transform `a |> f(%)` into `f(a)`
/// 3. Resolve & validate: Check function calls, argument counts
class SemanticAnalyzer {
public:
    /// Analyze and transform AST
    /// @param ast The parsed AST
    /// @param filename Filename for error reporting
    /// @return Analysis result with transformed AST and diagnostics
    AnalysisResult analyze(const Ast& ast, std::string_view filename = "<input>");

private:
    // Pass 1: Collect all variable definitions
    void collect_definitions(NodeIndex node);

    // Pass 2: Rewrite pipes - transforms AST
    // Returns the index of the rewritten node in the new arena
    NodeIndex rewrite_pipes(NodeIndex node);

    // Pass 2.5: Update function body nodes to point to transformed AST
    void update_function_body_nodes();

    // Pass 3: Resolve function calls and validate
    void resolve_and_validate(NodeIndex node);

    // Helper: Clone a node from input AST to output AST
    NodeIndex clone_node(NodeIndex src_idx);

    // Helper: Deep clone a subtree
    NodeIndex clone_subtree(NodeIndex src_idx);

    // Helper: Substitute all holes (%) with a replacement node
    NodeIndex substitute_holes(NodeIndex node, NodeIndex replacement);

    // Helper: Check if a subtree contains a hole
    bool contains_hole(NodeIndex node) const;

    // Helper: Validate argument count for builtin
    void validate_arguments(const std::string& func_name, const BuiltinInfo& builtin,
                           std::size_t arg_count, SourceLocation loc);

    // Helper: Reorder named arguments to match builtin signature
    // Returns true if reordering succeeded, false on error
    bool reorder_named_arguments(NodeIndex call_node, const BuiltinInfo& builtin,
                                 const std::string& func_name);

    // Helper: Check for variable captures in closure body
    void check_closure_captures(NodeIndex node, const std::set<std::string>& params,
                                SourceLocation closure_loc);

    // Error reporting helpers
    void error(const std::string& message, SourceLocation loc);
    void error(const std::string& code, const std::string& message, SourceLocation loc);
    void warning(const std::string& message, SourceLocation loc);

    // Context
    const Ast* input_ast_ = nullptr;
    AstArena output_arena_;
    SymbolTable symbols_;
    std::vector<Diagnostic> diagnostics_;
    std::string filename_;

    // For pipe rewriting: track mapping from old indices to new indices
    std::unordered_map<NodeIndex, NodeIndex> node_map_;
};

} // namespace akkado
