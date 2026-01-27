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

// FunctionValue (lambda variable) call handler - inlines closure bodies at call sites
std::uint16_t CodeGenerator::handle_function_value_call(
    NodeIndex node, const Node& n, const FunctionRef& func) {

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

    // Visit arguments BEFORE pushing scope to evaluate them in caller's context
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

    // Push scope for parameters and bind them
    symbols_->push_scope();
    for (std::size_t i = 0; i < func.params.size(); ++i) {
        symbols_->define_variable(func.params[i].name, param_bufs[i]);
    }

    // Find the body node from the closure
    // The closure structure is: [param1, param2, ..., body]
    const Node& closure_node = ast_->arena[func.closure_node];
    NodeIndex body = NULL_NODE;
    NodeIndex child = closure_node.first_child;
    std::size_t param_count = 0;

    while (child != NULL_NODE) {
        const Node& child_node = ast_->arena[child];
        if (child_node.type == NodeType::Identifier) {
            // This is a parameter
            param_count++;
        } else {
            // This is the body
            body = child;
            break;
        }
        child = ast_->arena[child].next_sibling;
    }

    // Save node_buffers_ state before visiting body
    auto saved_node_buffers = std::move(node_buffers_);
    node_buffers_.clear();

    // Visit closure body (inline expansion)
    std::uint16_t result = BufferAllocator::BUFFER_UNUSED;
    if (body != NULL_NODE) {
        result = visit(body);
    }

    // Restore node_buffers_
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

// Check if a match expression can be resolved at compile time
bool CodeGenerator::is_compile_time_match(NodeIndex node, const Node& n) const {
    // Get match expression data
    if (!std::holds_alternative<Node::MatchExprData>(n.data)) {
        return false;
    }
    const auto& match_data = n.as_match_expr();

    // For guard-only form without scrutinee, we need runtime evaluation
    // unless all guards are compile-time constants
    if (!match_data.has_scrutinee) {
        // Guard-only: check if all guards are compile-time evaluable
        NodeIndex arm = n.first_child;
        while (arm != NULL_NODE) {
            const Node& arm_node = ast_->arena[arm];
            if (arm_node.type == NodeType::MatchArm) {
                const auto& arm_data = arm_node.as_match_arm();
                if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
                    const Node& guard_node = ast_->arena[arm_data.guard_node];
                    // Only simple literals are compile-time evaluable
                    if (guard_node.type != NodeType::BoolLit &&
                        guard_node.type != NodeType::NumberLit) {
                        return false;  // Non-const guard -> runtime
                    }
                }
            }
            arm = ast_->arena[arm].next_sibling;
        }
        return true;
    }

    // Scrutinee form: check if scrutinee resolves to a literal
    NodeIndex scrutinee = n.first_child;
    if (scrutinee == NULL_NODE) return false;

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
                scrutinee_ptr = &ast_->arena[it->second];
            } else {
                return false;  // Variable without known literal -> runtime
            }
        }
    }

    // Check if scrutinee is a literal
    if (scrutinee_ptr->type != NodeType::StringLit &&
        scrutinee_ptr->type != NodeType::NumberLit &&
        scrutinee_ptr->type != NodeType::BoolLit) {
        return false;  // Non-literal scrutinee -> runtime
    }

    // Check all guards for const-evaluability
    NodeIndex arm = ast_->arena[scrutinee].next_sibling;
    while (arm != NULL_NODE) {
        const Node& arm_node = ast_->arena[arm];
        if (arm_node.type == NodeType::MatchArm) {
            const auto& arm_data = arm_node.as_match_arm();
            if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
                const Node& guard_node = ast_->arena[arm_data.guard_node];
                if (guard_node.type != NodeType::BoolLit &&
                    guard_node.type != NodeType::NumberLit) {
                    return false;  // Non-const guard -> runtime
                }
            }
        }
        arm = ast_->arena[arm].next_sibling;
    }

    return true;
}

// Handle compile-time match - evaluate patterns and guards, emit only winning branch
std::uint16_t CodeGenerator::handle_compile_time_match(NodeIndex node, const Node& n) {
    const auto& match_data = n.as_match_expr();

    NodeIndex first_arm = n.first_child;
    const Node* scrutinee_ptr = nullptr;
    std::string scrutinee_key;

    if (match_data.has_scrutinee) {
        NodeIndex scrutinee = n.first_child;
        first_arm = ast_->arena[scrutinee].next_sibling;
        scrutinee_ptr = &ast_->arena[scrutinee];

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
                    scrutinee_ptr = &ast_->arena[it->second];
                }
            }
        }

        // Get scrutinee value for matching
        if (scrutinee_ptr->type == NodeType::StringLit) {
            scrutinee_key = "s:" + scrutinee_ptr->as_string();
        } else if (scrutinee_ptr->type == NodeType::NumberLit) {
            scrutinee_key = "n:" + std::to_string(scrutinee_ptr->as_number());
        } else if (scrutinee_ptr->type == NodeType::BoolLit) {
            scrutinee_key = "b:" + std::to_string(scrutinee_ptr->as_bool());
        }
    }

    // Find matching arm
    NodeIndex arm = first_arm;
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
                default_body = body;
            } else if (match_data.has_scrutinee) {
                // Scrutinee form: check pattern match
                if (pattern != NULL_NODE) {
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
                        // Pattern matches - check guard if present
                        bool guard_passes = true;
                        if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
                            const Node& guard_node = ast_->arena[arm_data.guard_node];
                            if (guard_node.type == NodeType::BoolLit) {
                                guard_passes = guard_node.as_bool();
                            } else if (guard_node.type == NodeType::NumberLit) {
                                guard_passes = guard_node.as_number() != 0.0;
                            }
                        }

                        if (guard_passes && body != NULL_NODE) {
                            std::uint16_t result = visit(body);
                            node_buffers_[node] = result;
                            return result;
                        }
                    }
                }
            } else {
                // Guard-only form: evaluate guard
                if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
                    const Node& guard_node = ast_->arena[arm_data.guard_node];
                    bool guard_passes = false;
                    if (guard_node.type == NodeType::BoolLit) {
                        guard_passes = guard_node.as_bool();
                    } else if (guard_node.type == NodeType::NumberLit) {
                        guard_passes = guard_node.as_number() != 0.0;
                    }

                    if (guard_passes && body != NULL_NODE) {
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

// Handle runtime match - emit all branches and build nested select chain
std::uint16_t CodeGenerator::handle_runtime_match(NodeIndex node, const Node& n) {
    const auto& match_data = n.as_match_expr();

    // Check for missing wildcard arm and warn
    bool has_wildcard = false;
    NodeIndex first_arm = n.first_child;

    if (match_data.has_scrutinee) {
        first_arm = ast_->arena[first_arm].next_sibling;
    }

    NodeIndex arm = first_arm;
    while (arm != NULL_NODE) {
        const Node& arm_node = ast_->arena[arm];
        if (arm_node.type == NodeType::MatchArm) {
            const auto& arm_data = arm_node.as_match_arm();
            if (arm_data.is_wildcard) {
                has_wildcard = true;
                break;
            }
        }
        arm = ast_->arena[arm].next_sibling;
    }

    if (!has_wildcard) {
        warn("W001", "Match expression missing default '_' arm; defaulting to 0.0", n.location);
    }

    // Visit scrutinee if present
    std::uint16_t scrutinee_buf = BufferAllocator::BUFFER_UNUSED;
    if (match_data.has_scrutinee) {
        scrutinee_buf = visit(n.first_child);
    }

    // Collect all arms: condition buffer, body buffer, is_wildcard
    struct ArmInfo {
        std::uint16_t cond_buf;
        std::uint16_t body_buf;
        bool is_wildcard;
    };
    std::vector<ArmInfo> arms;

    arm = first_arm;
    while (arm != NULL_NODE) {
        const Node& arm_node = ast_->arena[arm];
        if (arm_node.type == NodeType::MatchArm) {
            const auto& arm_data = arm_node.as_match_arm();

            NodeIndex pattern = arm_node.first_child;
            NodeIndex body = (pattern != NULL_NODE) ?
                            ast_->arena[pattern].next_sibling : NULL_NODE;

            // Visit body first (all branches compute in DSP)
            std::uint16_t body_buf = BufferAllocator::BUFFER_UNUSED;
            if (body != NULL_NODE) {
                body_buf = visit(body);
            } else {
                // Empty body -> emit 0.0
                body_buf = buffers_.allocate();
                cedar::Instruction push_inst{};
                push_inst.opcode = cedar::Opcode::PUSH_CONST;
                push_inst.out_buffer = body_buf;
                push_inst.inputs[0] = 0xFFFF;
                push_inst.inputs[1] = 0xFFFF;
                push_inst.inputs[2] = 0xFFFF;
                push_inst.inputs[3] = 0xFFFF;
                codegen::encode_const_value(push_inst, 0.0f);
                emit(push_inst);
            }

            if (arm_data.is_wildcard) {
                arms.push_back({BufferAllocator::BUFFER_UNUSED, body_buf, true});
            } else {
                // Build condition
                std::uint16_t cond_buf = BufferAllocator::BUFFER_UNUSED;

                if (match_data.has_scrutinee) {
                    // Scrutinee form: eq(scrutinee, pattern)
                    std::uint16_t pattern_buf = visit(pattern);

                    cond_buf = buffers_.allocate();
                    cedar::Instruction eq_inst{};
                    eq_inst.opcode = cedar::Opcode::CMP_EQ;
                    eq_inst.out_buffer = cond_buf;
                    eq_inst.inputs[0] = scrutinee_buf;
                    eq_inst.inputs[1] = pattern_buf;
                    eq_inst.inputs[2] = 0xFFFF;
                    eq_inst.inputs[3] = 0xFFFF;
                    emit(eq_inst);
                } else {
                    // Guard-only form: condition is the guard itself
                    if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
                        cond_buf = visit(arm_data.guard_node);
                    }
                }

                // If there's a guard, AND it with the pattern condition
                if (arm_data.has_guard && arm_data.guard_node != NULL_NODE && match_data.has_scrutinee) {
                    std::uint16_t guard_buf = visit(arm_data.guard_node);

                    std::uint16_t combined_buf = buffers_.allocate();
                    cedar::Instruction and_inst{};
                    and_inst.opcode = cedar::Opcode::LOGIC_AND;
                    and_inst.out_buffer = combined_buf;
                    and_inst.inputs[0] = cond_buf;
                    and_inst.inputs[1] = guard_buf;
                    and_inst.inputs[2] = 0xFFFF;
                    and_inst.inputs[3] = 0xFFFF;
                    emit(and_inst);
                    cond_buf = combined_buf;
                }

                arms.push_back({cond_buf, body_buf, false});
            }
        }
        arm = ast_->arena[arm].next_sibling;
    }

    if (arms.empty()) {
        error("E122", "Match expression has no arms", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Build nested select chain (reverse order)
    // Start with default (wildcard arm or 0.0)
    std::uint16_t result = BufferAllocator::BUFFER_UNUSED;

    // Find wildcard arm for default
    for (const auto& arm_info : arms) {
        if (arm_info.is_wildcard) {
            result = arm_info.body_buf;
            break;
        }
    }

    // If no wildcard, emit 0.0 as default
    if (result == BufferAllocator::BUFFER_UNUSED) {
        result = buffers_.allocate();
        cedar::Instruction push_inst{};
        push_inst.opcode = cedar::Opcode::PUSH_CONST;
        push_inst.out_buffer = result;
        push_inst.inputs[0] = 0xFFFF;
        push_inst.inputs[1] = 0xFFFF;
        push_inst.inputs[2] = 0xFFFF;
        push_inst.inputs[3] = 0xFFFF;
        codegen::encode_const_value(push_inst, 0.0f);
        emit(push_inst);
    }

    // Build select chain in reverse order (last non-wildcard arm first)
    for (auto it = arms.rbegin(); it != arms.rend(); ++it) {
        if (!it->is_wildcard && it->cond_buf != BufferAllocator::BUFFER_UNUSED) {
            std::uint16_t select_buf = buffers_.allocate();
            cedar::Instruction sel_inst{};
            sel_inst.opcode = cedar::Opcode::SELECT;
            sel_inst.out_buffer = select_buf;
            sel_inst.inputs[0] = it->cond_buf;   // condition
            sel_inst.inputs[1] = it->body_buf;   // true branch
            sel_inst.inputs[2] = result;         // false branch (previous result)
            sel_inst.inputs[3] = 0xFFFF;
            emit(sel_inst);
            result = select_buf;
        }
    }

    node_buffers_[node] = result;
    return result;
}

// Handle MatchExpr nodes - dispatch to compile-time or runtime handling
std::uint16_t CodeGenerator::handle_match_expr(NodeIndex node, const Node& n) {
    // Check if match expression data exists
    if (!std::holds_alternative<Node::MatchExprData>(n.data)) {
        // Legacy: no MatchExprData, assume scrutinee form without guards
        // Fall back to compile-time behavior for backwards compatibility
        return handle_compile_time_match(node, n);
    }

    if (is_compile_time_match(node, n)) {
        return handle_compile_time_match(node, n);
    } else {
        return handle_runtime_match(node, n);
    }
}

} // namespace akkado
