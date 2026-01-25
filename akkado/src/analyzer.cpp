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

    // Pass 2.5: Update function body nodes to point to transformed AST
    update_function_body_nodes();

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
        // Check if RHS is a pattern expression (MiniLiteral)
        NodeIndex rhs = n.first_child;

        // Immutability check: error if variable already defined in current scope
        if (symbols_.is_defined_in_current_scope(name)) {
            error("E150", "Cannot reassign immutable variable '" + name + "'", n.location);
            // Continue processing to collect all errors
        }

        if (rhs != NULL_NODE && (*input_ast_).arena[rhs].type == NodeType::MiniLiteral) {
            // Pattern assignment
            PatternInfo pat_info{};
            pat_info.pattern_node = rhs;  // Will be updated after AST transform
            pat_info.is_sample_pattern = false;
            symbols_.define_pattern(name, pat_info);
        } else if (rhs != NULL_NODE && (*input_ast_).arena[rhs].type == NodeType::ArrayLit) {
            // Array assignment - count elements and register as Array symbol
            ArrayInfo arr_info{};
            arr_info.source_node = rhs;
            arr_info.element_count = 0;
            NodeIndex elem = (*input_ast_).arena[rhs].first_child;
            while (elem != NULL_NODE) {
                arr_info.element_count++;
                elem = (*input_ast_).arena[elem].next_sibling;
            }
            symbols_.define_array(name, arr_info);
        } else if (rhs != NULL_NODE && (*input_ast_).arena[rhs].type == NodeType::Closure) {
            // Lambda assignment - register as FunctionValue
            FunctionRef func_ref{};
            func_ref.closure_node = rhs;
            func_ref.is_user_function = false;
            // Extract parameters from closure
            NodeIndex child = (*input_ast_).arena[rhs].first_child;
            while (child != NULL_NODE) {
                const Node& child_node = (*input_ast_).arena[child];
                if (child_node.type == NodeType::Identifier) {
                    FunctionParamInfo param;
                    if (std::holds_alternative<Node::ClosureParamData>(child_node.data)) {
                        const auto& cp = child_node.as_closure_param();
                        param.name = cp.name;
                        param.default_value = cp.default_value;
                    } else if (std::holds_alternative<Node::IdentifierData>(child_node.data)) {
                        param.name = child_node.as_identifier();
                        param.default_value = std::nullopt;
                    } else {
                        break;  // Not a parameter, must be body
                    }
                    func_ref.params.push_back(std::move(param));
                } else {
                    break;  // Body node
                }
                child = (*input_ast_).arena[child].next_sibling;
            }
            symbols_.define_function_value(name, func_ref);
        } else {
            // Regular variable assignment
            symbols_.define_variable(name, 0xFFFF);
        }
    }

    if (n.type == NodeType::FunctionDef) {
        // Register the user-defined function
        const auto& fn_data = n.as_function_def();

        if (symbols_.is_defined_in_current_scope(fn_data.name)) {
            warning("Function '" + fn_data.name + "' redefined", n.location);
        }

        // Collect parameters from Identifier children (before body)
        UserFunctionInfo func_info;
        func_info.name = fn_data.name;
        func_info.def_node = node;

        NodeIndex child = n.first_child;
        std::size_t param_idx = 0;
        while (child != NULL_NODE && param_idx < fn_data.param_count) {
            const Node& child_node = (*input_ast_).arena[child];
            FunctionParamInfo param;
            if (std::holds_alternative<Node::ClosureParamData>(child_node.data)) {
                const auto& cp = child_node.as_closure_param();
                param.name = cp.name;
                param.default_value = cp.default_value;
            } else if (std::holds_alternative<Node::IdentifierData>(child_node.data)) {
                param.name = child_node.as_identifier();
                param.default_value = std::nullopt;
            }
            func_info.params.push_back(std::move(param));
            param_idx++;
            child = (*input_ast_).arena[child].next_sibling;
        }

        // Body is the next child after params
        func_info.body_node = child;

        symbols_.define_function(func_info);
    }

    // Recurse to children
    NodeIndex child = n.first_child;
    while (child != NULL_NODE) {
        collect_definitions(child);
        child = (*input_ast_).arena[child].next_sibling;
    }
}

void SemanticAnalyzer::update_function_body_nodes() {
    // Update all user function definitions to point to the transformed AST
    symbols_.update_function_nodes(node_map_);
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
        } else if (sym->kind == SymbolKind::UserFunction) {
            // Validate user function call
            const auto& fn = sym->user_function;

            // Count arguments
            std::size_t arg_count = 0;
            NodeIndex arg = output_arena_[node].first_child;
            while (arg != NULL_NODE) {
                arg_count++;
                arg = output_arena_[arg].next_sibling;
            }

            // Count required args (params without defaults)
            std::size_t min_args = 0;
            for (const auto& param : fn.params) {
                if (!param.default_value.has_value()) {
                    min_args++;
                }
            }
            std::size_t max_args = fn.params.size();

            if (arg_count < min_args) {
                error("E006", "Function '" + func_name + "' expects at least " +
                      std::to_string(min_args) + " argument(s), got " +
                      std::to_string(arg_count), n.location);
            } else if (arg_count > max_args) {
                error("E007", "Function '" + func_name + "' expects at most " +
                      std::to_string(max_args) + " argument(s), got " +
                      std::to_string(arg_count), n.location);
            }
        } else if (sym->kind == SymbolKind::Builtin) {
            // Special handling for osc() - validation happens in codegen
            // because argument count depends on the type string
            if (func_name == "osc") {
                // Basic validation: at least 2 args (type + freq)
                std::size_t arg_count = 0;
                NodeIndex arg = output_arena_[node].first_child;
                while (arg != NULL_NODE) {
                    arg_count++;
                    arg = output_arena_[arg].next_sibling;
                }
                if (arg_count < 2) {
                    error("E006", "Function 'osc' expects at least 2 arguments: "
                          "osc(type, freq) or osc(type, freq, pwm)", n.location);
                }
            } else {
                // Reorder named arguments if any
                reorder_named_arguments(node, sym->builtin, func_name);

                // Count arguments (children of the Call node)
                std::size_t arg_count = 0;
                NodeIndex arg = output_arena_[node].first_child;
                while (arg != NULL_NODE) {
                    arg_count++;
                    arg = output_arena_[arg].next_sibling;
                }

                validate_arguments(func_name, sym->builtin, arg_count, n.location);
            }
        }
    }

    if (n.type == NodeType::MatchExpr) {
        // Validate match expression
        // First child is scrutinee, remaining children are MatchArm nodes
        // NOTE: Literal scrutinee check is done in codegen (after inline expansion)

        NodeIndex scrutinee = n.first_child;

        // Check for duplicate patterns and unreachable code
        std::set<std::string> seen_patterns;
        bool seen_wildcard = false;
        NodeIndex arm = (scrutinee != NULL_NODE) ? output_arena_[scrutinee].next_sibling : NULL_NODE;

        while (arm != NULL_NODE) {
            const Node& arm_node = output_arena_[arm];
            if (arm_node.type == NodeType::MatchArm) {
                const auto& arm_data = arm_node.as_match_arm();

                if (seen_wildcard) {
                    warning("Unreachable pattern after wildcard '_'", arm_node.location);
                }

                if (arm_data.is_wildcard) {
                    seen_wildcard = true;
                } else {
                    // Get pattern value for duplicate check
                    NodeIndex pattern = arm_node.first_child;
                    if (pattern != NULL_NODE) {
                        const Node& pattern_node = output_arena_[pattern];
                        std::string pattern_key;

                        if (pattern_node.type == NodeType::StringLit) {
                            pattern_key = "s:" + pattern_node.as_string();
                        } else if (pattern_node.type == NodeType::NumberLit) {
                            pattern_key = "n:" + std::to_string(pattern_node.as_number());
                        } else if (pattern_node.type == NodeType::BoolLit) {
                            pattern_key = "b:" + std::to_string(pattern_node.as_bool());
                        }

                        if (!pattern_key.empty()) {
                            if (seen_patterns.count(pattern_key)) {
                                warning("Duplicate pattern in match expression", pattern_node.location);
                            }
                            seen_patterns.insert(pattern_key);
                        }
                    }
                }
            }
            arm = output_arena_[arm].next_sibling;
        }
    }

    if (n.type == NodeType::FunctionDef) {
        // Validate function definition: check no outer variable captures
        const auto& fn_data = n.as_function_def();
        std::set<std::string> params;

        // Push a new scope for function parameters
        symbols_.push_scope();

        // Collect parameter names
        NodeIndex child = n.first_child;
        std::size_t param_idx = 0;

        while (child != NULL_NODE && param_idx < fn_data.param_count) {
            const Node& child_node = output_arena_[child];
            std::string param_name;
            if (std::holds_alternative<Node::ClosureParamData>(child_node.data)) {
                param_name = child_node.as_closure_param().name;
            } else if (std::holds_alternative<Node::IdentifierData>(child_node.data)) {
                param_name = child_node.as_identifier();
            }
            if (!param_name.empty()) {
                params.insert(param_name);
                symbols_.define_variable(param_name, 0xFFFF);
            }
            param_idx++;
            child = output_arena_[child].next_sibling;
        }

        // Check body for captured variables (body is after params)
        NodeIndex body = child;
        if (body != NULL_NODE) {
            check_closure_captures(body, params, n.location);
        }

        // Recurse to children while params are in scope
        child = n.first_child;
        while (child != NULL_NODE) {
            resolve_and_validate(child);
            child = output_arena_[child].next_sibling;
        }

        // Pop scope
        symbols_.pop_scope();

        return;  // Already recursed
    }

    if (n.type == NodeType::Identifier) {
        // Check if identifier is defined
        // Note: Identifier nodes may use IdentifierData or ClosureParamData (for params with defaults)
        std::string name;
        if (std::holds_alternative<Node::ClosureParamData>(n.data)) {
            name = n.as_closure_param().name;
        } else if (std::holds_alternative<Node::IdentifierData>(n.data)) {
            name = n.as_identifier();
        } else {
            // Shouldn't happen - unknown data type for Identifier node
            return;
        }
        auto sym = symbols_.lookup(name);
        if (!sym) {
            error("E005", "Undefined identifier: '" + name + "'", n.location);
        }
        // FunctionValue and UserFunction can be used as values
        // (already allowed by symbol table lookup)
    }

    if (n.type == NodeType::Closure) {
        // Validate closure: collect parameters, then check body for captures
        std::set<std::string> params;

        // Push a new scope for closure parameters
        symbols_.push_scope();

        // Collect parameter names (Identifier children before body)
        // Parameters may be stored as IdentifierData or ClosureParamData
        NodeIndex child = n.first_child;
        NodeIndex body = NULL_NODE;

        while (child != NULL_NODE) {
            const Node& child_node = output_arena_[child];
            if (child_node.type == NodeType::Identifier) {
                // Check if it's IdentifierData or ClosureParamData
                std::string param_name;
                if (std::holds_alternative<Node::ClosureParamData>(child_node.data)) {
                    param_name = child_node.as_closure_param().name;
                } else if (std::holds_alternative<Node::IdentifierData>(child_node.data)) {
                    param_name = child_node.as_identifier();
                } else {
                    // This is the body (not a parameter)
                    body = child;
                    break;
                }
                params.insert(param_name);
                // Define parameter in current scope
                symbols_.define_variable(param_name, 0xFFFF);
            } else {
                // This is the body
                body = child;
                break;
            }
            child = child_node.next_sibling;
        }

        // Check body for captured variables
        if (body != NULL_NODE) {
            check_closure_captures(body, params, n.location);
        }

        // Recurse to children (including body) while params are in scope
        child = n.first_child;
        while (child != NULL_NODE) {
            resolve_and_validate(child);
            child = output_arena_[child].next_sibling;
        }

        // Pop scope - parameters go out of scope
        symbols_.pop_scope();

        return;  // Already recursed, don't do it again below
    }

    // Special handling for MatchArm: skip pattern, only validate body
    if (n.type == NodeType::MatchArm) {
        NodeIndex pattern = n.first_child;
        if (pattern != NULL_NODE) {
            NodeIndex body = output_arena_[pattern].next_sibling;
            if (body != NULL_NODE) {
                resolve_and_validate(body);
            }
        }
        return;
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

void SemanticAnalyzer::check_closure_captures(NodeIndex node,
                                               const std::set<std::string>& params,
                                               SourceLocation closure_loc) {
    if (node == NULL_NODE) return;

    const Node& n = output_arena_[node];

    // Skip match arm patterns - they are not variable references
    if (n.type == NodeType::MatchArm) {
        // Only check the body (second child), not the pattern (first child)
        NodeIndex pattern = n.first_child;
        if (pattern != NULL_NODE) {
            NodeIndex body = output_arena_[pattern].next_sibling;
            if (body != NULL_NODE) {
                check_closure_captures(body, params, closure_loc);
            }
        }
        return;
    }

    if (n.type == NodeType::Identifier) {
        // Get name - may be IdentifierData or ClosureParamData (for params with defaults)
        std::string name;
        if (std::holds_alternative<Node::ClosureParamData>(n.data)) {
            name = n.as_closure_param().name;
        } else if (std::holds_alternative<Node::IdentifierData>(n.data)) {
            name = n.as_identifier();
        } else {
            return;  // Unknown data type
        }

        // Check if it's a parameter
        if (params.find(name) != params.end()) {
            return;  // OK - parameter reference
        }

        // Check if it's a builtin, user function, or function value
        auto sym = symbols_.lookup(name);
        if (sym && (sym->kind == SymbolKind::Builtin ||
                    sym->kind == SymbolKind::UserFunction ||
                    sym->kind == SymbolKind::FunctionValue)) {
            return;  // OK - builtin, user function, or function value
        }

        // It's a captured variable - allowed for closures (read-only capture)
        // Variables are immutable, so read-only capture is safe
        if (sym && (sym->kind == SymbolKind::Variable ||
                    sym->kind == SymbolKind::Parameter ||
                    sym->kind == SymbolKind::Array)) {
            return;  // OK - captured variable (will be bound at codegen time)
        }

        // Unknown identifier - will be caught elsewhere
        return;
    }

    // For Call nodes, the function name is in data, not as a child
    // So we don't need special handling - just check children

    // Recurse to children
    NodeIndex child = n.first_child;
    while (child != NULL_NODE) {
        check_closure_captures(child, params, closure_loc);
        child = output_arena_[child].next_sibling;
    }
}

bool SemanticAnalyzer::reorder_named_arguments(NodeIndex call_node,
                                                const BuiltinInfo& builtin,
                                                const std::string& func_name) {
    Node& call = output_arena_[call_node];

    // Collect all arguments
    struct ArgInfo {
        NodeIndex node;
        std::optional<std::string> name;
        int target_pos;  // Position in reordered list (-1 = unknown)
    };
    std::vector<ArgInfo> args;

    NodeIndex arg = call.first_child;
    while (arg != NULL_NODE) {
        const Node& arg_node = output_arena_[arg];
        std::optional<std::string> arg_name;
        if (arg_node.type == NodeType::Argument) {
            arg_name = arg_node.as_arg_name();
        }
        args.push_back({arg, arg_name, -1});
        arg = output_arena_[arg].next_sibling;
    }

    if (args.empty()) return true;

    // Check for named arguments and determine if reordering is needed
    bool has_named = false;
    bool seen_named = false;
    std::set<std::string> used_params;

    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i].name.has_value()) {
            has_named = true;
            seen_named = true;

            const std::string& name = *args[i].name;

            // Check for duplicate parameter
            if (used_params.count(name)) {
                error("E010", "Duplicate named argument '" + name + "' in call to '" +
                      func_name + "'", output_arena_[args[i].node].location);
                return false;
            }
            used_params.insert(name);

            // Find parameter index
            int param_idx = builtin.find_param(name);
            if (param_idx < 0) {
                error("E011", "Unknown parameter '" + name + "' for function '" +
                      func_name + "'", output_arena_[args[i].node].location);
                return false;
            }
            args[i].target_pos = param_idx;
        } else {
            // Positional argument
            if (seen_named) {
                error("E009", "Positional argument cannot follow named argument in call to '" +
                      func_name + "'", output_arena_[args[i].node].location);
                return false;
            }
            // Positional args fill slots in order
            args[i].target_pos = static_cast<int>(i);
        }
    }

    if (!has_named) {
        return true;  // No reordering needed
    }

    // Check that positional args don't conflict with named args
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (!args[i].name.has_value()) {
            // Check if this positional slot was also filled by a named arg
            for (std::size_t j = 0; j < args.size(); ++j) {
                if (args[j].name.has_value() && args[j].target_pos == static_cast<int>(i)) {
                    error("E012", "Parameter '" + *args[j].name + "' at position " +
                          std::to_string(i) + " conflicts with positional argument in call to '" +
                          func_name + "'", output_arena_[args[i].node].location);
                    return false;
                }
            }
        }
    }

    // Reorder arguments: create array indexed by target position
    std::size_t max_pos = 0;
    for (const auto& a : args) {
        if (a.target_pos >= 0) {
            max_pos = std::max(max_pos, static_cast<std::size_t>(a.target_pos));
        }
    }

    std::vector<NodeIndex> reordered(max_pos + 1, NULL_NODE);
    for (const auto& a : args) {
        if (a.target_pos >= 0) {
            reordered[a.target_pos] = a.node;
        }
    }

    // Clear argument names after reordering (they're now positional)
    for (auto& a : args) {
        if (a.name.has_value() && a.node != NULL_NODE) {
            Node& arg_node = output_arena_[a.node];
            if (arg_node.type == NodeType::Argument) {
                arg_node.data = Node::ArgumentData{std::nullopt};
            }
        }
    }

    // Rebuild child list in new order
    call.first_child = NULL_NODE;
    NodeIndex prev = NULL_NODE;
    for (NodeIndex idx : reordered) {
        if (idx == NULL_NODE) continue;

        output_arena_[idx].next_sibling = NULL_NODE;

        if (prev == NULL_NODE) {
            call.first_child = idx;
        } else {
            output_arena_[prev].next_sibling = idx;
        }
        prev = idx;
    }

    return true;
}

} // namespace akkado
