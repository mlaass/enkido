#include "akkado/codegen.hpp"
#include "akkado/builtins.hpp"
#include <cedar/vm/state_pool.hpp>
#include <cstring>

namespace akkado {

std::uint16_t BufferAllocator::allocate() {
    if (next_ >= MAX_BUFFERS) {
        return BUFFER_UNUSED;
    }
    return next_++;
}

CodeGenResult CodeGenerator::generate(const Ast& ast, SymbolTable& symbols,
                                       std::string_view filename) {
    ast_ = &ast;
    symbols_ = &symbols;
    buffers_ = BufferAllocator{};
    instructions_.clear();
    diagnostics_.clear();
    filename_ = std::string(filename);
    path_stack_.clear();
    anonymous_counter_ = 0;
    node_buffers_.clear();
    call_counters_.clear();

    // Start with "main" path
    push_path("main");

    if (!ast.valid()) {
        error("E100", "Invalid AST", {});
        return {{}, std::move(diagnostics_), false};
    }

    // Visit root (Program node)
    visit(ast.root);

    pop_path();

    bool success = !has_errors(diagnostics_);

    return {std::move(instructions_), std::move(diagnostics_), success};
}

std::uint16_t CodeGenerator::visit(NodeIndex node) {
    if (node == NULL_NODE) return BufferAllocator::BUFFER_UNUSED;

    // Check if already visited
    auto it = node_buffers_.find(node);
    if (it != node_buffers_.end()) {
        return it->second;
    }

    const Node& n = ast_->arena[node];

    switch (n.type) {
        case NodeType::Program: {
            // Visit all statements
            NodeIndex child = n.first_child;
            std::uint16_t last_buffer = BufferAllocator::BUFFER_UNUSED;
            while (child != NULL_NODE) {
                last_buffer = visit(child);
                child = ast_->arena[child].next_sibling;
            }
            return last_buffer;
        }

        case NodeType::NumberLit: {
            // Emit PUSH_CONST
            std::uint16_t out = buffers_.allocate();
            if (out == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return BufferAllocator::BUFFER_UNUSED;
            }

            cedar::Instruction inst{};
            inst.opcode = cedar::Opcode::PUSH_CONST;
            inst.out_buffer = out;
            inst.inputs[0] = 0xFFFF;
            inst.inputs[1] = 0xFFFF;
            inst.inputs[2] = 0xFFFF;

            // Store float value in state_id field (reinterpret cast)
            float value = static_cast<float>(n.as_number());
            std::memcpy(&inst.state_id, &value, sizeof(float));

            emit(inst);
            node_buffers_[node] = out;
            return out;
        }

        case NodeType::BoolLit: {
            // Emit PUSH_CONST with 1.0 or 0.0
            std::uint16_t out = buffers_.allocate();
            if (out == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return BufferAllocator::BUFFER_UNUSED;
            }

            cedar::Instruction inst{};
            inst.opcode = cedar::Opcode::PUSH_CONST;
            inst.out_buffer = out;
            inst.inputs[0] = 0xFFFF;
            inst.inputs[1] = 0xFFFF;
            inst.inputs[2] = 0xFFFF;

            float value = n.as_bool() ? 1.0f : 0.0f;
            std::memcpy(&inst.state_id, &value, sizeof(float));

            emit(inst);
            node_buffers_[node] = out;
            return out;
        }

        case NodeType::PitchLit: {
            // Emit PUSH_CONST for MIDI note, then MTOF to convert to frequency
            std::uint16_t midi_buf = buffers_.allocate();
            if (midi_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return BufferAllocator::BUFFER_UNUSED;
            }

            // Push MIDI note value
            cedar::Instruction push_inst{};
            push_inst.opcode = cedar::Opcode::PUSH_CONST;
            push_inst.out_buffer = midi_buf;
            push_inst.inputs[0] = 0xFFFF;
            push_inst.inputs[1] = 0xFFFF;
            push_inst.inputs[2] = 0xFFFF;

            float midi_value = static_cast<float>(n.as_pitch());
            std::memcpy(&push_inst.state_id, &midi_value, sizeof(float));
            emit(push_inst);

            // Allocate output buffer for frequency
            std::uint16_t freq_buf = buffers_.allocate();
            if (freq_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return BufferAllocator::BUFFER_UNUSED;
            }

            // MTOF: convert MIDI note to frequency
            cedar::Instruction mtof_inst{};
            mtof_inst.opcode = cedar::Opcode::MTOF;
            mtof_inst.out_buffer = freq_buf;
            mtof_inst.inputs[0] = midi_buf;
            mtof_inst.inputs[1] = 0xFFFF;
            mtof_inst.inputs[2] = 0xFFFF;
            mtof_inst.state_id = 0;
            emit(mtof_inst);

            node_buffers_[node] = freq_buf;
            return freq_buf;
        }

        case NodeType::ChordLit: {
            // For MVP, emit first note (root) of chord
            // Full chord expansion would require array support
            const auto& chord = n.as_chord();
            std::uint8_t root_midi = chord.root_midi;

            // Emit PUSH_CONST for MIDI note, then MTOF
            std::uint16_t midi_buf = buffers_.allocate();
            if (midi_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return BufferAllocator::BUFFER_UNUSED;
            }

            cedar::Instruction push_inst{};
            push_inst.opcode = cedar::Opcode::PUSH_CONST;
            push_inst.out_buffer = midi_buf;
            push_inst.inputs[0] = 0xFFFF;
            push_inst.inputs[1] = 0xFFFF;
            push_inst.inputs[2] = 0xFFFF;

            float midi_value = static_cast<float>(root_midi);
            std::memcpy(&push_inst.state_id, &midi_value, sizeof(float));
            emit(push_inst);

            std::uint16_t freq_buf = buffers_.allocate();
            if (freq_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return BufferAllocator::BUFFER_UNUSED;
            }

            cedar::Instruction mtof_inst{};
            mtof_inst.opcode = cedar::Opcode::MTOF;
            mtof_inst.out_buffer = freq_buf;
            mtof_inst.inputs[0] = midi_buf;
            mtof_inst.inputs[1] = 0xFFFF;
            mtof_inst.inputs[2] = 0xFFFF;
            mtof_inst.state_id = 0;
            emit(mtof_inst);

            node_buffers_[node] = freq_buf;
            return freq_buf;
        }

        case NodeType::Identifier: {
            const std::string& name = n.as_identifier();
            auto sym = symbols_->lookup(name);

            if (!sym) {
                error("E102", "Undefined identifier: '" + name + "'", n.location);
                return BufferAllocator::BUFFER_UNUSED;
            }

            if (sym->kind == SymbolKind::Variable || sym->kind == SymbolKind::Parameter) {
                // Return the buffer index from the symbol table
                // (should have been set during Assignment processing)
                return sym->buffer_index;
            }

            // Builtins without args? Shouldn't happen for identifiers
            error("E103", "Cannot use builtin as value: '" + name + "'", n.location);
            return BufferAllocator::BUFFER_UNUSED;
        }

        case NodeType::Assignment: {
            // Variable name is stored in the node's data
            // First child is the value expression
            NodeIndex value_idx = n.first_child;

            if (value_idx == NULL_NODE) {
                error("E104", "Invalid assignment", n.location);
                return BufferAllocator::BUFFER_UNUSED;
            }

            const std::string& var_name = n.as_identifier();

            // Push variable name onto path for semantic IDs
            push_path(var_name);

            // Generate code for the value expression
            std::uint16_t value_buffer = visit(value_idx);

            pop_path();

            // Update symbol table with the buffer index
            auto sym = symbols_->lookup(var_name);
            if (sym && (sym->kind == SymbolKind::Variable || sym->kind == SymbolKind::Parameter)) {
                // Re-define with correct buffer index
                symbols_->define_variable(var_name, value_buffer);
            }

            node_buffers_[node] = value_buffer;
            return value_buffer;
        }

        case NodeType::Call: {
            // Function name is stored in the node's data, not as a child
            const std::string& func_name = n.as_identifier();
            const BuiltinInfo* builtin = lookup_builtin(func_name);

            if (!builtin) {
                error("E107", "Unknown function: '" + func_name + "'", n.location);
                return BufferAllocator::BUFFER_UNUSED;
            }

            // For stateful functions, push path BEFORE visiting children
            // so nested calls see their parent's context
            bool pushed_path = false;
            if (builtin->requires_state) {
                std::uint32_t count = call_counters_[func_name]++;
                std::string unique_name = func_name + "#" + std::to_string(count);
                push_path(unique_name);
                pushed_path = true;
            }

            // Visit arguments (dependencies must be satisfied)
            std::vector<std::uint16_t> arg_buffers;
            NodeIndex arg = n.first_child;
            while (arg != NULL_NODE) {
                const Node& arg_node = ast_->arena[arg];
                NodeIndex arg_value = arg;
                if (arg_node.type == NodeType::Argument) {
                    arg_value = arg_node.first_child;
                }
                std::uint16_t buf = visit(arg_value);
                arg_buffers.push_back(buf);
                arg = ast_->arena[arg].next_sibling;
            }

            // Special case: out() with single argument (mono to stereo)
            if (func_name == "out" && arg_buffers.size() == 1) {
                arg_buffers.push_back(arg_buffers[0]);  // Duplicate L to R
            }

            // Fill in missing optional arguments with defaults
            std::size_t total_params = builtin->total_params();
            for (std::size_t i = arg_buffers.size(); i < total_params; ++i) {
                if (builtin->has_default(i)) {
                    // Emit PUSH_CONST for the default value
                    std::uint16_t default_buf = buffers_.allocate();
                    if (default_buf == BufferAllocator::BUFFER_UNUSED) {
                        error("E101", "Buffer pool exhausted", n.location);
                        if (pushed_path) pop_path();
                        return BufferAllocator::BUFFER_UNUSED;
                    }

                    cedar::Instruction push_inst{};
                    push_inst.opcode = cedar::Opcode::PUSH_CONST;
                    push_inst.out_buffer = default_buf;
                    push_inst.inputs[0] = 0xFFFF;
                    push_inst.inputs[1] = 0xFFFF;
                    push_inst.inputs[2] = 0xFFFF;

                    float default_val = builtin->get_default(i);
                    std::memcpy(&push_inst.state_id, &default_val, sizeof(float));
                    emit(push_inst);

                    arg_buffers.push_back(default_buf);
                }
            }

            // Allocate output buffer
            std::uint16_t out = buffers_.allocate();
            if (out == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                if (pushed_path) pop_path();
                return BufferAllocator::BUFFER_UNUSED;
            }

            // Build instruction
            cedar::Instruction inst{};
            inst.opcode = builtin->opcode;
            inst.out_buffer = out;
            inst.inputs[0] = arg_buffers.size() > 0 ? arg_buffers[0] : 0xFFFF;
            inst.inputs[1] = arg_buffers.size() > 1 ? arg_buffers[1] : 0xFFFF;
            inst.inputs[2] = arg_buffers.size() > 2 ? arg_buffers[2] : 0xFFFF;

            // Generate state_id from current path (already pushed if stateful)
            if (pushed_path) {
                inst.state_id = compute_state_id();
                pop_path();
            } else {
                inst.state_id = 0;
            }

            emit(inst);
            node_buffers_[node] = out;
            return out;
        }

        case NodeType::BinaryOp: {
            // BinaryOp should have been desugared to Call by parser
            // But handle it anyway in case we get one
            NodeIndex lhs = n.first_child;
            NodeIndex rhs = (lhs != NULL_NODE) ?
                           ast_->arena[lhs].next_sibling : NULL_NODE;

            if (lhs == NULL_NODE || rhs == NULL_NODE) {
                error("E108", "Invalid binary operation", n.location);
                return BufferAllocator::BUFFER_UNUSED;
            }

            std::uint16_t lhs_buf = visit(lhs);
            std::uint16_t rhs_buf = visit(rhs);

            std::uint16_t out = buffers_.allocate();
            if (out == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return BufferAllocator::BUFFER_UNUSED;
            }

            // Map BinOp to opcode
            cedar::Opcode opcode;
            switch (n.as_binop()) {
                case BinOp::Add: opcode = cedar::Opcode::ADD; break;
                case BinOp::Sub: opcode = cedar::Opcode::SUB; break;
                case BinOp::Mul: opcode = cedar::Opcode::MUL; break;
                case BinOp::Div: opcode = cedar::Opcode::DIV; break;
                case BinOp::Pow: opcode = cedar::Opcode::POW; break;
                default:
                    error("E109", "Unknown binary operator", n.location);
                    return BufferAllocator::BUFFER_UNUSED;
            }

            emit(cedar::Instruction::make_binary(opcode, out, lhs_buf, rhs_buf));
            node_buffers_[node] = out;
            return out;
        }

        case NodeType::Hole: {
            // Holes should have been substituted by the analyzer
            error("E110", "Hole '%' in unexpected context", n.location);
            return BufferAllocator::BUFFER_UNUSED;
        }

        case NodeType::Block: {
            // Visit all statements in block
            NodeIndex child = n.first_child;
            std::uint16_t last_buffer = BufferAllocator::BUFFER_UNUSED;
            while (child != NULL_NODE) {
                last_buffer = visit(child);
                child = ast_->arena[child].next_sibling;
            }
            node_buffers_[node] = last_buffer;
            return last_buffer;
        }

        // Unsupported for MVP
        case NodeType::Pipe:
            error("E111", "Pipe should have been rewritten", n.location);
            return BufferAllocator::BUFFER_UNUSED;

        case NodeType::Closure: {
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

        case NodeType::MethodCall:
            error("E113", "Method calls not supported in MVP", n.location);
            return BufferAllocator::BUFFER_UNUSED;

        case NodeType::MiniLiteral:
            error("E114", "Mini-notation patterns not supported in MVP", n.location);
            return BufferAllocator::BUFFER_UNUSED;

        case NodeType::PostStmt:
            error("E115", "Post statements not supported in MVP", n.location);
            return BufferAllocator::BUFFER_UNUSED;

        default:
            error("E199", "Unsupported node type", n.location);
            return BufferAllocator::BUFFER_UNUSED;
    }
}

void CodeGenerator::emit(const cedar::Instruction& inst) {
    instructions_.push_back(inst);
}

std::uint32_t CodeGenerator::compute_state_id() const {
    // Build path string
    std::string path;
    for (size_t i = 0; i < path_stack_.size(); ++i) {
        if (i > 0) path += '/';
        path += path_stack_[i];
    }
    return cedar::fnv1a_hash_runtime(path.data(), path.size());
}

void CodeGenerator::push_path(std::string_view segment) {
    path_stack_.push_back(std::string(segment));
}

void CodeGenerator::pop_path() {
    if (!path_stack_.empty()) {
        path_stack_.pop_back();
    }
}

void CodeGenerator::error(const std::string& code, const std::string& message,
                          SourceLocation loc) {
    Diagnostic diag;
    diag.severity = Severity::Error;
    diag.code = code;
    diag.message = message;
    diag.filename = filename_;
    diag.location = loc;
    diagnostics_.push_back(std::move(diag));
}

} // namespace akkado
