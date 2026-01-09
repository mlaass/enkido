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

            // Visit arguments first (dependencies must be satisfied)
            // Arguments are children of the Call node
            std::vector<std::uint16_t> arg_buffers;
            NodeIndex arg = n.first_child;
            while (arg != NULL_NODE) {
                const Node& arg_node = ast_->arena[arg];
                // Arguments may be wrapped in Argument nodes
                NodeIndex arg_value = arg;
                if (arg_node.type == NodeType::Argument) {
                    arg_value = arg_node.first_child;
                }
                std::uint16_t buf = visit(arg_value);
                arg_buffers.push_back(buf);
                arg = ast_->arena[arg].next_sibling;
            }

            // Allocate output buffer
            std::uint16_t out = buffers_.allocate();
            if (out == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return BufferAllocator::BUFFER_UNUSED;
            }

            // Build instruction
            cedar::Instruction inst{};
            inst.opcode = builtin->opcode;
            inst.out_buffer = out;
            inst.inputs[0] = arg_buffers.size() > 0 ? arg_buffers[0] : 0xFFFF;
            inst.inputs[1] = arg_buffers.size() > 1 ? arg_buffers[1] : 0xFFFF;
            inst.inputs[2] = arg_buffers.size() > 2 ? arg_buffers[2] : 0xFFFF;

            // Generate state_id if needed
            if (builtin->requires_state) {
                push_path(func_name);
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

        case NodeType::Closure:
            error("E112", "Closures not supported in MVP", n.location);
            return BufferAllocator::BUFFER_UNUSED;

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
