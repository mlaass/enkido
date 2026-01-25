// User-defined function and match expression codegen implementations
// Extracted from codegen.cpp for maintainability

#include "akkado/codegen.hpp"
#include "akkado/codegen/codegen.hpp"
#include <cstring>

namespace akkado {

using codegen::encode_const_value;

// User function call handler - inlines function bodies at call sites
std::uint16_t CodeGenerator::handle_user_function_call(
    NodeIndex node, const Node& n, const UserFunctionInfo& func) {

    // Collect call arguments
    std::vector<NodeIndex> args;
    NodeIndex arg = n.first_child;
    while (arg != NULL_NODE) {
        const Node& arg_node = ast_->arena[arg];
        NodeIndex arg_value = arg;
        if (arg_node.type == NodeType::Argument) {
            arg_value = arg_node.first_child;
        }
        args.push_back(arg_value);
        arg = ast_->arena[arg].next_sibling;
    }

    // Save param_literals for this scope
    auto saved_param_literals = std::move(param_literals_);
    param_literals_.clear();

    // IMPORTANT: Visit arguments BEFORE pushing scope to evaluate them in caller's context
    // This allows nested function calls like double(double(x)) to work correctly.
    std::vector<std::uint16_t> param_bufs;
    for (std::size_t i = 0; i < func.params.size(); ++i) {
        std::uint16_t param_buf;

        if (i < args.size()) {
            // Check if the argument is a literal - record for match resolution
            const Node& arg_node = ast_->arena[args[i]];
            if (arg_node.type == NodeType::StringLit ||
                arg_node.type == NodeType::NumberLit ||
                arg_node.type == NodeType::BoolLit) {
                std::uint32_t param_hash = fnv1a_hash(func.params[i].name);
                param_literals_[param_hash] = args[i];
            }

            // Visit argument in caller's scope
            param_buf = visit(args[i]);
        } else if (func.params[i].default_value.has_value()) {
            // Use default value
            param_buf = buffers_.allocate();
            if (param_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                param_literals_ = std::move(saved_param_literals);
                return BufferAllocator::BUFFER_UNUSED;
            }

            cedar::Instruction push_inst{};
            push_inst.opcode = cedar::Opcode::PUSH_CONST;
            push_inst.out_buffer = param_buf;
            push_inst.inputs[0] = 0xFFFF;
            push_inst.inputs[1] = 0xFFFF;
            push_inst.inputs[2] = 0xFFFF;
            push_inst.inputs[3] = 0xFFFF;

            float default_val = static_cast<float>(*func.params[i].default_value);
            encode_const_value(push_inst, default_val);
            emit(push_inst);
        } else {
            // Missing required argument - should have been caught by analyzer
            error("E105", "Missing required argument for parameter '" +
                  func.params[i].name + "'", n.location);
            param_literals_ = std::move(saved_param_literals);
            return BufferAllocator::BUFFER_UNUSED;
        }

        param_bufs.push_back(param_buf);
    }

    // NOW push scope for function parameters and bind them
    symbols_->push_scope();
    for (std::size_t i = 0; i < func.params.size(); ++i) {
        symbols_->define_variable(func.params[i].name, param_bufs[i]);
    }

    // Save node_buffers_ state before visiting body.
    // This is necessary because function bodies are shared AST nodes that may be
    // visited multiple times with different parameter bindings.
    auto saved_node_buffers = std::move(node_buffers_);
    node_buffers_.clear();

    // Visit function body (inline expansion)
    std::uint16_t result = BufferAllocator::BUFFER_UNUSED;
    if (func.body_node != NULL_NODE) {
        result = visit(func.body_node);
    }

    // Restore node_buffers_ (keep new entries but restore old ones)
    for (auto& [k, v] : saved_node_buffers) {
        if (node_buffers_.find(k) == node_buffers_.end()) {
            node_buffers_[k] = v;
        }
    }

    // Pop scope and restore param_literals
    symbols_->pop_scope();
    param_literals_ = std::move(saved_param_literals);

    node_buffers_[node] = result;
    return result;
}

// Handle Closure nodes - allocate buffers for parameters and generate body
std::uint16_t CodeGenerator::handle_closure(NodeIndex node, const Node& n) {
    // For simple closures: allocate buffers for parameters, then generate body
    // Find parameters and body
    // Parameters may be stored as IdentifierData or ClosureParamData
    std::vector<std::string> param_names;
    NodeIndex child = n.first_child;
    NodeIndex body = NULL_NODE;

    while (child != NULL_NODE) {
        const Node& child_node = ast_->arena[child];
        if (child_node.type == NodeType::Identifier) {
            // Check if it's IdentifierData or ClosureParamData
            if (std::holds_alternative<Node::ClosureParamData>(child_node.data)) {
                param_names.push_back(child_node.as_closure_param().name);
            } else if (std::holds_alternative<Node::IdentifierData>(child_node.data)) {
                param_names.push_back(child_node.as_identifier());
            } else {
                body = child;
                break;
            }
        } else {
            body = child;
            break;
        }
        child = ast_->arena[child].next_sibling;
    }

    if (body == NULL_NODE) {
        error("E112", "Closure has no body", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Allocate input buffers for parameters and bind them
    for (const auto& param : param_names) {
        std::uint16_t param_buf = buffers_.allocate();
        if (param_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return BufferAllocator::BUFFER_UNUSED;
        }
        // Update symbol table with actual buffer index
        symbols_->define_variable(param, param_buf);
    }

    // Generate code for body
    std::uint16_t body_buf = visit(body);
    node_buffers_[node] = body_buf;
    return body_buf;
}

// Handle MatchExpr nodes - compile-time match resolution
std::uint16_t CodeGenerator::handle_match_expr(NodeIndex node, const Node& n) {
    // Compile-time match resolution
    // First child is scrutinee, remaining children are MatchArm nodes

    NodeIndex scrutinee = n.first_child;
    if (scrutinee == NULL_NODE) {
        error("E120", "Match expression has no scrutinee", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Save the original scrutinee node to iterate over arms
    NodeIndex original_scrutinee = scrutinee;
    const Node* scrutinee_ptr = &ast_->arena[scrutinee];

    // If scrutinee is an Identifier, check if it maps to a literal argument
    if (scrutinee_ptr->type == NodeType::Identifier) {
        std::string param_name;
        if (std::holds_alternative<Node::ClosureParamData>(scrutinee_ptr->data)) {
            param_name = scrutinee_ptr->as_closure_param().name;
        } else if (std::holds_alternative<Node::IdentifierData>(scrutinee_ptr->data)) {
            param_name = scrutinee_ptr->as_identifier();
        }

        if (!param_name.empty()) {
            std::uint32_t param_hash = fnv1a_hash(param_name);
            auto it = param_literals_.find(param_hash);
            if (it != param_literals_.end()) {
                // Use the literal argument instead for value matching
                scrutinee_ptr = &ast_->arena[it->second];
            }
        }
    }

    // Get scrutinee value for matching
    std::string scrutinee_key;
    if (scrutinee_ptr->type == NodeType::StringLit) {
        scrutinee_key = "s:" + scrutinee_ptr->as_string();
    } else if (scrutinee_ptr->type == NodeType::NumberLit) {
        scrutinee_key = "n:" + std::to_string(scrutinee_ptr->as_number());
    } else if (scrutinee_ptr->type == NodeType::BoolLit) {
        scrutinee_key = "b:" + std::to_string(scrutinee_ptr->as_bool());
    } else {
        error("E120", "Match scrutinee must be a compile-time literal", scrutinee_ptr->location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Find matching arm - use original_scrutinee for traversing AST
    NodeIndex arm = ast_->arena[original_scrutinee].next_sibling;
    NodeIndex default_body = NULL_NODE;

    while (arm != NULL_NODE) {
        const Node& arm_node = ast_->arena[arm];
        if (arm_node.type == NodeType::MatchArm) {
            const auto& arm_data = arm_node.as_match_arm();

            // Get pattern (first child) and body (second child)
            NodeIndex pattern = arm_node.first_child;
            NodeIndex body = (pattern != NULL_NODE) ?
                            ast_->arena[pattern].next_sibling : NULL_NODE;

            if (arm_data.is_wildcard) {
                // Wildcard - remember as default
                default_body = body;
            } else if (pattern != NULL_NODE) {
                // Check if pattern matches scrutinee
                const Node& pattern_node = ast_->arena[pattern];
                std::string pattern_key;

                if (pattern_node.type == NodeType::StringLit) {
                    pattern_key = "s:" + pattern_node.as_string();
                } else if (pattern_node.type == NodeType::NumberLit) {
                    pattern_key = "n:" + std::to_string(pattern_node.as_number());
                } else if (pattern_node.type == NodeType::BoolLit) {
                    pattern_key = "b:" + std::to_string(pattern_node.as_bool());
                }

                if (pattern_key == scrutinee_key) {
                    // Match found - compile only this body
                    if (body != NULL_NODE) {
                        std::uint16_t result = visit(body);
                        node_buffers_[node] = result;
                        return result;
                    }
                }
            }
        }
        arm = ast_->arena[arm].next_sibling;
    }

    // No match found - use default if available
    if (default_body != NULL_NODE) {
        std::uint16_t result = visit(default_body);
        node_buffers_[node] = result;
        return result;
    }

    error("E121", "No matching pattern in match expression", n.location);
    return BufferAllocator::BUFFER_UNUSED;
}

} // namespace akkado
