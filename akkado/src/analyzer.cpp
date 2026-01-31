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
        } else if (rhs != NULL_NODE && (*input_ast_).arena[rhs].type == NodeType::RecordLit) {
            // Record assignment - collect field names and register as Record symbol
            auto record_type = std::make_shared<RecordTypeInfo>();
            record_type->source_node = rhs;

            // Collect field names from RecordLit children (each is an Argument with RecordFieldData)
            NodeIndex field_node = (*input_ast_).arena[rhs].first_child;
            while (field_node != NULL_NODE) {
                const Node& field = (*input_ast_).arena[field_node];
                if (field.type == NodeType::Argument &&
                    std::holds_alternative<Node::RecordFieldData>(field.data)) {
                    const auto& field_data = field.as_record_field();
                    RecordFieldInfo field_info;
                    field_info.name = field_data.name;
                    field_info.buffer_index = 0xFFFF;  // Will be set during codegen
                    field_info.field_kind = SymbolKind::Variable;  // Default, may be updated
                    record_type->fields.push_back(std::move(field_info));
                }
                field_node = (*input_ast_).arena[field_node].next_sibling;
            }

            symbols_.define_record(name, record_type);
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
        } else if (rhs != NULL_NODE && (*input_ast_).arena[rhs].type == NodeType::Pipe) {
            // Check for pipe-to-lambda: x |> f(%) |> g(%)
            // If innermost LHS of pipe chain is an unbound identifier, this becomes a closure
            // Walk down nested pipes to find the innermost LHS
            NodeIndex current = rhs;
            while (current != NULL_NODE && (*input_ast_).arena[current].type == NodeType::Pipe) {
                current = (*input_ast_).arena[current].first_child;
            }

            // current is now the innermost LHS (should be Identifier for pipe-to-lambda)
            if (current != NULL_NODE) {
                const Node& lhs_node = (*input_ast_).arena[current];
                if (lhs_node.type == NodeType::Identifier) {
                    std::string param_name;
                    if (std::holds_alternative<Node::IdentifierData>(lhs_node.data)) {
                        param_name = lhs_node.as_identifier();
                    }
                    // Check if identifier is unbound (not defined)
                    if (!param_name.empty() && !symbols_.lookup(param_name)) {
                        // This is pipe-to-lambda: register as FunctionValue
                        FunctionRef func_ref{};
                        func_ref.closure_node = rhs;  // Will be updated to the closure
                        func_ref.is_user_function = false;
                        FunctionParamInfo param;
                        param.name = param_name;
                        param.default_value = std::nullopt;
                        func_ref.params.push_back(std::move(param));
                        symbols_.define_function_value(name, func_ref);
                    } else {
                        // LHS is bound - regular variable
                        symbols_.define_variable(name, 0xFFFF);
                    }
                } else {
                    // LHS is not an identifier - regular variable
                    symbols_.define_variable(name, 0xFFFF);
                }
            } else {
                // Invalid pipe - regular variable
                symbols_.define_variable(name, 0xFFFF);
            }
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

        // Handle PipeBinding on LHS: expr as name |> ...
        // This creates a named binding visible in the RHS
        std::string binding_name;
        NodeIndex actual_lhs = lhs_idx;

        const Node& lhs_node = (*input_ast_).arena[lhs_idx];
        if (lhs_node.type == NodeType::PipeBinding) {
            const auto& binding_data = lhs_node.as_pipe_binding();
            binding_name = binding_data.binding_name;
            // Get the actual expression inside the binding
            actual_lhs = lhs_node.first_child;
        }

        // Check if innermost LHS is an unbound identifier -> create closure
        // Walk down nested pipes to find the innermost LHS
        NodeIndex innermost_lhs = actual_lhs;
        while (innermost_lhs != NULL_NODE &&
               (*input_ast_).arena[innermost_lhs].type == NodeType::Pipe) {
            innermost_lhs = (*input_ast_).arena[innermost_lhs].first_child;
        }

        if (innermost_lhs != NULL_NODE) {
            const Node& lhs = (*input_ast_).arena[innermost_lhs];
            if (lhs.type == NodeType::Identifier) {
                std::string name;
                if (std::holds_alternative<Node::IdentifierData>(lhs.data)) {
                    name = lhs.as_identifier();
                }
                if (!name.empty() && !symbols_.lookup(name)) {
                    // Unbound identifier - transform to closure: x |> f(%) -> (x) -> f(x)
                    // For the closure, we use the innermost identifier as param
                    // and the whole pipe chain (except innermost) as body
                    NodeIndex closure = create_closure_from_pipe(innermost_lhs, node, n.location);
                    node_map_[node] = closure;
                    return closure;
                }
            }
        }

        // First, recursively rewrite the actual LHS (may contain nested pipes)
        NodeIndex new_lhs = rewrite_pipes(actual_lhs);

        // Now substitute all holes in RHS with the LHS value
        // This performs: a |> f(%) -> f(a)
        // Also substitute binding name references with the LHS value
        NodeIndex new_rhs;
        if (!binding_name.empty()) {
            // Substitute both holes and binding name references
            new_rhs = substitute_holes_and_binding(rhs_idx, new_lhs, binding_name);
        } else {
            // substitute_holes also handles any nested pipes in the RHS
            new_rhs = substitute_holes(rhs_idx, new_lhs);
        }

        // Track mapping so function/closure bodies pointing to this pipe get updated
        node_map_[node] = new_rhs;

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

    // Desugar method calls: receiver.method(a, b) -> method(receiver, a, b)
    if (src.type == NodeType::MethodCall) {
        return desugar_method_call(src_idx);
    }

    // Clone this node
    NodeIndex dst_idx = clone_node(src_idx);

    // Handle MatchArm guard_node specially - it's not a child but stored in data
    if (src.type == NodeType::MatchArm) {
        const auto& arm_data = src.as_match_arm();
        if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
            // Clone the guard expression
            NodeIndex cloned_guard = clone_subtree(arm_data.guard_node);
            // Update the MatchArmData in the cloned node
            auto& dst_data = std::get<Node::MatchArmData>(output_arena_[dst_idx].data);
            dst_data.guard_node = cloned_guard;
        }
    }

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
        // Check for hole with field access: %.field
        if (std::holds_alternative<Node::HoleData>(n.data)) {
            const auto& hole_data = n.as_hole();
            if (hole_data.field_name.has_value()) {
                // Create FieldAccess node: replacement.field
                NodeIndex field_access = output_arena_.alloc(NodeType::FieldAccess, n.location);
                output_arena_[field_access].data = Node::FieldAccessData{hole_data.field_name.value()};
                output_arena_[field_access].first_child = replacement;  // Already in output arena
                return field_access;
            }
        }
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

        // Track mapping so function/closure bodies pointing to this pipe get updated
        node_map_[node] = new_rhs;

        // Return the transformed RHS (pipe is eliminated)
        return new_rhs;
    }

    // For other nodes, clone and recurse on children
    NodeIndex new_node = clone_node(node);

    // Handle MatchArm guard_node specially - it's not a child but stored in data
    if (n.type == NodeType::MatchArm) {
        const auto& arm_data = n.as_match_arm();
        if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
            // Substitute holes in the guard expression
            NodeIndex new_guard = substitute_holes(arm_data.guard_node, replacement);
            // Update the MatchArmData in the new node
            auto& dst_data = std::get<Node::MatchArmData>(output_arena_[new_node].data);
            dst_data.guard_node = new_guard;
        }
    }

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

// Helper for as-binding: substitute holes AND identifiers matching binding_name
NodeIndex SemanticAnalyzer::substitute_holes_and_binding(
    NodeIndex node, NodeIndex replacement, const std::string& binding_name) {

    if (node == NULL_NODE) return NULL_NODE;

    const Node& n = (*input_ast_).arena[node];

    // If this is a hole, return the replacement node
    if (n.type == NodeType::Hole) {
        // Check for hole with field access: %.field
        if (std::holds_alternative<Node::HoleData>(n.data)) {
            const auto& hole_data = n.as_hole();
            if (hole_data.field_name.has_value()) {
                // Create FieldAccess node: replacement.field
                NodeIndex field_access = output_arena_.alloc(NodeType::FieldAccess, n.location);
                output_arena_[field_access].data = Node::FieldAccessData{hole_data.field_name.value()};
                output_arena_[field_access].first_child = replacement;  // Already in output arena
                return field_access;
            }
        }
        // Clone the replacement to create a fresh copy
        return clone_subtree(replacement);
    }

    // If this is an identifier matching the binding name, substitute it
    if (n.type == NodeType::Identifier) {
        std::string name;
        if (std::holds_alternative<Node::IdentifierData>(n.data)) {
            name = n.as_identifier();
        }
        if (name == binding_name) {
            // Clone the replacement to create a fresh copy
            return clone_subtree(replacement);
        }
    }

    // Handle nested pipes - they need to be rewritten
    if (n.type == NodeType::Pipe) {
        NodeIndex src_lhs = n.first_child;
        NodeIndex src_rhs = (src_lhs != NULL_NODE) ?
                            (*input_ast_).arena[src_lhs].next_sibling : NULL_NODE;

        // Substitute binding references in the LHS
        NodeIndex new_lhs = substitute_holes_and_binding(src_lhs, replacement, binding_name);

        // Substitute holes in the RHS using the new LHS as replacement
        // But also substitute binding name references
        NodeIndex new_rhs = substitute_holes_and_binding(src_rhs, new_lhs, binding_name);

        // Track mapping
        node_map_[node] = new_rhs;

        return new_rhs;
    }

    // For other nodes, clone and recurse on children
    NodeIndex new_node = clone_node(node);

    // Handle MatchArm guard_node specially
    if (n.type == NodeType::MatchArm) {
        const auto& arm_data = n.as_match_arm();
        if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
            NodeIndex new_guard = substitute_holes_and_binding(arm_data.guard_node, replacement, binding_name);
            auto& dst_data = std::get<Node::MatchArmData>(output_arena_[new_node].data);
            dst_data.guard_node = new_guard;
        }
    }

    NodeIndex src_child = n.first_child;
    NodeIndex prev_dst_child = NULL_NODE;

    while (src_child != NULL_NODE) {
        NodeIndex dst_child = substitute_holes_and_binding(src_child, replacement, binding_name);

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

// Helper for pipe-to-lambda: substitute holes AND a specific identifier
NodeIndex SemanticAnalyzer::substitute_holes_and_identifier(
    NodeIndex node, NodeIndex replacement, NodeIndex identifier_to_replace) {

    if (node == NULL_NODE) return NULL_NODE;

    const Node& n = (*input_ast_).arena[node];

    // If this is a hole, return the replacement node
    if (n.type == NodeType::Hole) {
        // Check for hole with field access: %.field
        if (std::holds_alternative<Node::HoleData>(n.data)) {
            const auto& hole_data = n.as_hole();
            if (hole_data.field_name.has_value()) {
                // Create FieldAccess node: replacement.field
                NodeIndex field_access = output_arena_.alloc(NodeType::FieldAccess, n.location);
                output_arena_[field_access].data = Node::FieldAccessData{hole_data.field_name.value()};
                output_arena_[field_access].first_child = replacement;  // Already in output arena
                return field_access;
            }
        }
        return replacement;
    }

    // If this is the identifier we're replacing, return the replacement
    if (node == identifier_to_replace) {
        return replacement;
    }

    // Handle nested pipes - they need to be rewritten
    if (n.type == NodeType::Pipe) {
        NodeIndex src_lhs = n.first_child;
        NodeIndex src_rhs = (src_lhs != NULL_NODE) ?
                            (*input_ast_).arena[src_lhs].next_sibling : NULL_NODE;

        // Substitute holes/identifier in the LHS
        NodeIndex new_lhs = substitute_holes_and_identifier(src_lhs, replacement, identifier_to_replace);

        // Substitute holes in the RHS using the new LHS as replacement
        // (normal pipe semantics: holes in RHS get the LHS value)
        NodeIndex new_rhs = substitute_holes(src_rhs, new_lhs);

        // Track mapping
        node_map_[node] = new_rhs;

        return new_rhs;
    }

    // For other nodes, clone and recurse on children
    NodeIndex new_node = clone_node(node);

    // Handle MatchArm guard_node specially
    if (n.type == NodeType::MatchArm) {
        const auto& arm_data = n.as_match_arm();
        if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
            NodeIndex new_guard = substitute_holes_and_identifier(arm_data.guard_node, replacement, identifier_to_replace);
            auto& dst_data = std::get<Node::MatchArmData>(output_arena_[new_node].data);
            dst_data.guard_node = new_guard;
        }
    }

    NodeIndex src_child = n.first_child;
    NodeIndex prev_dst_child = NULL_NODE;

    while (src_child != NULL_NODE) {
        NodeIndex dst_child = substitute_holes_and_identifier(src_child, replacement, identifier_to_replace);

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

NodeIndex SemanticAnalyzer::create_closure_from_pipe(
    NodeIndex param_node, NodeIndex body_node, SourceLocation loc) {

    const Node& param_src = (*input_ast_).arena[param_node];
    const std::string& param_name = param_src.as_identifier();

    // Create closure node
    NodeIndex closure = output_arena_.alloc(NodeType::Closure, loc);

    // Clone parameter as closure param
    NodeIndex param = clone_node(param_node);
    output_arena_[closure].first_child = param;

    // Push scope, define param
    symbols_.push_scope();
    symbols_.define_variable(param_name, 0xFFFF);

    // Create param reference for hole substitution
    NodeIndex param_ref = output_arena_.alloc(NodeType::Identifier, loc);
    output_arena_[param_ref].data = Node::IdentifierData{param_name};

    // Substitute holes AND the parameter identifier in body with param reference
    NodeIndex body = substitute_holes_and_identifier(body_node, param_ref, param_node);
    output_arena_[param].next_sibling = body;

    symbols_.pop_scope();

    return closure;
}

NodeIndex SemanticAnalyzer::desugar_method_call(NodeIndex method_call_idx) {
    // Desugar: receiver.method(a, b) -> method(receiver, a, b)
    const Node& src = (*input_ast_).arena[method_call_idx];

    // Create a Call node with the same method name
    NodeIndex call_idx = output_arena_.alloc(NodeType::Call, src.location);
    output_arena_[call_idx].data = src.data;  // Copy the method name (IdentifierData)

    // Track mapping
    node_map_[method_call_idx] = call_idx;

    // Get the receiver (first child of MethodCall)
    NodeIndex src_receiver = src.first_child;
    if (src_receiver == NULL_NODE) {
        error("E008", "Method call missing receiver", src.location);
        return call_idx;
    }

    // Clone the receiver and wrap it in an Argument node
    NodeIndex cloned_receiver = clone_subtree(src_receiver);
    NodeIndex receiver_arg = output_arena_.alloc(NodeType::Argument, src.location);
    output_arena_[receiver_arg].data = Node::ArgumentData{std::nullopt};  // Positional arg
    output_arena_[receiver_arg].first_child = cloned_receiver;

    // Set receiver as first child of the Call
    output_arena_[call_idx].first_child = receiver_arg;

    // Clone remaining arguments (after the receiver)
    NodeIndex src_arg = (*input_ast_).arena[src_receiver].next_sibling;
    NodeIndex prev_dst_arg = receiver_arg;

    while (src_arg != NULL_NODE) {
        NodeIndex dst_arg = clone_subtree(src_arg);
        if (dst_arg != NULL_NODE) {
            output_arena_[prev_dst_arg].next_sibling = dst_arg;
            prev_dst_arg = dst_arg;
        }
        src_arg = (*input_ast_).arena[src_arg].next_sibling;
    }

    return call_idx;
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
        // NOTE: Literal scrutinee check is done in codegen (after inline expansion)

        // Check if this is a scrutinee form or guard-only form
        bool has_scrutinee = false;
        if (std::holds_alternative<Node::MatchExprData>(n.data)) {
            has_scrutinee = n.as_match_expr().has_scrutinee;
        }

        // Determine first arm based on whether scrutinee exists
        NodeIndex first_arm = n.first_child;
        if (has_scrutinee && first_arm != NULL_NODE) {
            first_arm = output_arena_[first_arm].next_sibling;
        }

        // Check for duplicate patterns and unreachable code
        std::set<std::string> seen_patterns;
        bool seen_wildcard = false;
        NodeIndex arm = first_arm;

        while (arm != NULL_NODE) {
            const Node& arm_node = output_arena_[arm];
            if (arm_node.type == NodeType::MatchArm) {
                const auto& arm_data = arm_node.as_match_arm();

                if (seen_wildcard) {
                    warning("Unreachable pattern after wildcard '_'", arm_node.location);
                }

                if (arm_data.is_wildcard) {
                    seen_wildcard = true;
                } else if (!arm_data.has_guard) {
                    // Only check for duplicates on arms without guards
                    // Guards make the same pattern semantically different
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

    if (n.type == NodeType::FieldAccess) {
        // Validate field access: check that field exists on the record type
        const auto& field_data = n.as_field_access();
        const std::string& field_name = field_data.field_name;

        // Get the expression being accessed (first child)
        NodeIndex expr = n.first_child;
        if (expr != NULL_NODE) {
            // First, validate the expression
            resolve_and_validate(expr);

            // Try to determine the type of the expression and validate field access
            // For MVP, we only validate field access on direct identifier references to records
            const Node& expr_node = output_arena_[expr];
            if (expr_node.type == NodeType::Identifier) {
                std::string expr_name;
                if (std::holds_alternative<Node::IdentifierData>(expr_node.data)) {
                    expr_name = expr_node.as_identifier();
                }
                if (!expr_name.empty()) {
                    auto sym = symbols_.lookup(expr_name);
                    if (sym && sym->kind == SymbolKind::Record && sym->record_type) {
                        // Check if field exists
                        const auto* field = sym->record_type->find_field(field_name);
                        if (!field) {
                            // Build list of available fields for error message
                            std::string available;
                            auto field_names = sym->record_type->field_names();
                            for (size_t i = 0; i < field_names.size(); ++i) {
                                if (i > 0) available += ", ";
                                available += field_names[i];
                            }
                            error("E060", "Unknown field '" + field_name + "' on record. Available: " + available, n.location);
                        }
                    } else if (sym && sym->kind != SymbolKind::Record) {
                        error("E061", "Cannot access field '" + field_name + "' on non-record value", n.location);
                    }
                }
            }
            // For nested field access (e.g., a.b.c) or complex expressions,
            // we skip validation for MVP - codegen will catch errors
        }
        return;  // Already validated child
    }

    if (n.type == NodeType::PipeBinding) {
        // Pipe binding creates a scoped symbol for the bound name
        const auto& binding_data = n.as_pipe_binding();
        const std::string& binding_name = binding_data.binding_name;

        // Define the binding in current scope (it will be visible in subsequent pipe stages)
        // The actual type will be determined based on the expression being bound
        NodeIndex bound_expr = n.first_child;
        if (bound_expr != NULL_NODE) {
            resolve_and_validate(bound_expr);

            // Try to determine the type of the bound expression and propagate it
            const Node& expr_node = output_arena_[bound_expr];
            if (expr_node.type == NodeType::Identifier) {
                std::string expr_name;
                if (std::holds_alternative<Node::IdentifierData>(expr_node.data)) {
                    expr_name = expr_node.as_identifier();
                }
                auto sym = symbols_.lookup(expr_name);
                if (sym) {
                    // Create a new symbol for the binding with the same type
                    if (sym->kind == SymbolKind::Record && sym->record_type) {
                        symbols_.define_record(binding_name, sym->record_type);
                    } else {
                        symbols_.define_variable(binding_name, 0xFFFF);
                    }
                } else {
                    symbols_.define_variable(binding_name, 0xFFFF);
                }
            } else if (expr_node.type == NodeType::RecordLit) {
                // Inline record literal - create record type from it
                auto record_type = std::make_shared<RecordTypeInfo>();
                record_type->source_node = bound_expr;

                NodeIndex field_node = expr_node.first_child;
                while (field_node != NULL_NODE) {
                    const Node& field = output_arena_[field_node];
                    if (field.type == NodeType::Argument &&
                        std::holds_alternative<Node::RecordFieldData>(field.data)) {
                        const auto& field_data = field.as_record_field();
                        RecordFieldInfo field_info;
                        field_info.name = field_data.name;
                        field_info.buffer_index = 0xFFFF;
                        field_info.field_kind = SymbolKind::Variable;
                        record_type->fields.push_back(std::move(field_info));
                    }
                    field_node = output_arena_[field_node].next_sibling;
                }
                symbols_.define_record(binding_name, record_type);
            } else {
                // For other expression types, treat as a simple variable
                symbols_.define_variable(binding_name, 0xFFFF);
            }
        }
        return;  // Already validated child
    }

    if (n.type == NodeType::Hole) {
        // Check for hole with field access (%.field)
        if (std::holds_alternative<Node::HoleData>(n.data)) {
            const auto& hole_data = n.as_hole();
            if (hole_data.field_name.has_value()) {
                // This is %.field - validation happens at the pipe level
                // For MVP, we just note this and let codegen handle it
                // A proper implementation would check that we're in a pattern pipe context
            }
        }
        // Regular holes are validated elsewhere (E003 for holes outside pipes)
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

    // Special handling for MatchArm: skip pattern, validate guard and body
    if (n.type == NodeType::MatchArm) {
        const auto& arm_data = n.as_match_arm();

        // Validate guard expression if present
        if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
            resolve_and_validate(arm_data.guard_node);
        }

        // Validate body
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
        const auto& arm_data = n.as_match_arm();

        // Check guard expression if present
        if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
            check_closure_captures(arm_data.guard_node, params, closure_loc);
        }

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
