#pragma once

#include "ast.hpp"
#include "diagnostics.hpp"
#include "symbol_table.hpp"
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace akkado {

// Forward declarations
class SourceMap;

/// Namespace import info passed from compile() to analyzer
struct ModuleNamespace {
    std::string canonical_path;   // Module's canonical path
    std::string alias;            // The alias name
};

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
    /// @param source_map Optional source map for module origin tracking
    /// @param namespaces Namespace imports (import "x" as alias)
    /// @return Analysis result with transformed AST and diagnostics
    AnalysisResult analyze(const Ast& ast, std::string_view filename = "<input>",
                           const SourceMap* source_map = nullptr,
                           std::span<const ModuleNamespace> namespaces = {});

private:
    // Pass 1: Collect all variable definitions
    void collect_definitions(NodeIndex node);

    // Pass 2: Rewrite pipes - transforms AST
    // Returns the index of the rewritten node in the new arena
    //
    // closure_allowed: true when this pipe sits in a binding-RHS context
    // (Assignment/PipeBinding bound expr/FunctionDef body/Closure body/
    // default value). Only then may an unbound-identifier LHS like
    // `x |> f(%)` be silently rewritten into a closure `(x) -> f(x)`.
    // At expression-statement position (closure_allowed=false), we leave
    // the pipe alone and let resolve_and_validate emit E102 for the
    // undefined identifier — the correct, debuggable failure mode.
    NodeIndex rewrite_pipes(NodeIndex node, bool closure_allowed = false);

    // Pass 2.5: Update function body nodes to point to transformed AST
    void update_function_body_nodes();

    // Pass 2.7: Reject recursive `fn` definitions (PRD
    // prd-runtime-functions-control-flow L2). Builds a call graph over
    // user functions and emits E240 (or E244 if a member is #inline) on
    // any cycle. Recursion is rejected in v1 — it would otherwise
    // infinite-loop codegen's per-call-site body inlining.
    void detect_recursive_functions();

    // Pass 3: Resolve function calls and validate
    void resolve_and_validate(NodeIndex node);

    // Helper: Clone a node from input AST to output AST
    NodeIndex clone_node(NodeIndex src_idx);

    // Helper: Deep clone a subtree (see rewrite_pipes for closure_allowed)
    NodeIndex clone_subtree(NodeIndex src_idx, bool closure_allowed = false);

    // Options for unified node substitution
    struct SubstituteOpts {
        NodeIndex replacement = NULL_NODE;
        std::string binding_name;                    // empty = no binding match
        NodeIndex identifier_to_replace = NULL_NODE; // NULL_NODE = no identity match
        bool clone_on_hole = false;                  // true for as-binding (needs fresh copies)
        NodeIndex binding_replacement = NULL_NODE;   // separate replacement for binding_name (stays constant through pipe chain)
        std::vector<std::string> destructure_fields; // field names from `as {f1, f2}` destructuring
    };

    // Helper: Unified substitution of holes, binding names, and identifier nodes
    NodeIndex substitute_nodes(NodeIndex node, const SubstituteOpts& opts);

    // Helper: clone a LoopExpr (input arena) attaching `seed_out` (output
    // arena) as its seed child. The loop body is cloned WITHOUT hole
    // substitution — its `@` holes are the running accumulator, resolved at
    // codegen, not the enclosing pipe's value.
    NodeIndex clone_loop_seeded(NodeIndex loop_in, NodeIndex seed_out);

    // Helper: Check if a subtree contains a hole
    bool contains_hole(NodeIndex node) const;

    // Helper: Create a closure from pipe expression with unbound identifier LHS
    // Transforms: x |> f(%) |> g(%)  ->  (x) -> g(f(x))
    NodeIndex create_closure_from_pipe(NodeIndex param_node, NodeIndex body_node,
                                       SourceLocation loc);

    // Helper: Desugar method call to function call
    // Transforms: receiver.method(a, b) -> method(receiver, a, b)
    NodeIndex desugar_method_call(NodeIndex method_call_idx);


    // Helper: Validate argument count for builtin
    void validate_arguments(const std::string& func_name, const BuiltinInfo& builtin,
                           std::size_t arg_count, SourceLocation loc);

    // Helper: Reorder named arguments to match builtin signature
    // Returns true if reordering succeeded, false on error
    bool reorder_named_arguments(NodeIndex call_node, const BuiltinInfo& builtin,
                                 const std::string& func_name);

    // Helper: Reorder named arguments to match user function signature
    bool reorder_named_arguments(NodeIndex call_node,
                                 const std::vector<std::string>& param_names,
                                 const std::string& func_name);

    // Helper: Returns true if any child of the call node is a spread argument
    // (Argument with spread_source != NULL_NODE). When spread is present,
    // argument count and reordering are deferred to codegen.
    bool has_spread_arg(NodeIndex call_node) const;

    // Helper: Verify that an expression is pure (allowed in const context)
    void verify_const_purity(NodeIndex node, const std::string& context);

    // Helper: Check for variable captures in closure body
    void check_closure_captures(NodeIndex node, const std::set<std::string>& params,
                                SourceLocation closure_loc);

    // Helper: returns true if `idx` (in input_ast_) is a pattern-producing
    // expression — directly (MiniLiteral, or Call to a pattern-producer
    // builtin like pat/note/chord/seq/value), transitively through a chain
    // of method calls (.slow/.fast/.rev/etc.) ending in such a producer, or
    // through an Identifier already bound to a Pattern symbol. Used by
    // collect_definitions so that `notes = n"…".slow(2)` and friends
    // register the binding as Pattern (not Variable), letting downstream
    // `notes |> saw(@freq)` field access pass the E061 check.
    bool is_pattern_producing_expr(NodeIndex idx) const;

    // Error reporting helpers
    void error(const std::string& message, SourceLocation loc);
    void error(const std::string& code, const std::string& message, SourceLocation loc);
    void warning(const std::string& message, SourceLocation loc);

    // Hide definitions from namespaced modules (between pass 1 and pass 2)
    void hide_namespaced_definitions();

    // Extract the definition name from an Assignment/FunctionDef/ConstDecl node
    static std::string extract_definition_name(const Node& n);

    // Context
    const Ast* input_ast_ = nullptr;
    AstArena output_arena_;
    SymbolTable symbols_;
    std::vector<Diagnostic> diagnostics_;
    std::string filename_;
    const SourceMap* source_map_ = nullptr;
    std::vector<ModuleNamespace> namespaces_;

    // For pipe rewriting: track mapping from old indices to new indices
    std::unordered_map<NodeIndex, NodeIndex> node_map_;

    // >0 while resolve_and_validate is inside a loop() body, where surviving
    // `@` holes are the running accumulator rather than an E003 error.
    int loop_body_depth_ = 0;

    // PRD prd-runtime-functions-control-flow L3: closure-node → name of its
    // event-record parameter, for lambdas passed to each_voice/each/reduce.
    // The Closure handler defines that parameter as a Parameter (record-
    // capable) so `n.freq`-style field access passes the E061 check.
    std::unordered_map<NodeIndex, std::string> foreach_record_param_;
};

} // namespace akkado
