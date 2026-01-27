#include "akkado/codegen.hpp"
#include "akkado/codegen/codegen.hpp"  // Master include for all codegen helpers
#include "akkado/builtins.hpp"
#include "akkado/chord_parser.hpp"
#include "akkado/pattern_eval.hpp"
#include <cedar/vm/state_pool.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

namespace akkado {

// Use helpers from akkado::codegen namespace
using codegen::encode_const_value;
using codegen::unwrap_argument;
using codegen::is_audio_rate_producer;
using codegen::is_upgradeable_oscillator;
using codegen::upgrade_for_fm;

std::uint16_t BufferAllocator::allocate() {
    if (next_ >= MAX_BUFFERS) {
        return BUFFER_UNUSED;
    }
    return next_++;
}

CodeGenResult CodeGenerator::generate(const Ast& ast, SymbolTable& symbols,
                                       std::string_view filename,
                                       SampleRegistry* sample_registry) {
    ast_ = &ast;
    symbols_ = &symbols;
    sample_registry_ = sample_registry;
    buffers_ = BufferAllocator{};
    instructions_.clear();
    diagnostics_.clear();
    state_inits_.clear();
    required_samples_.clear();
    param_decls_.clear();
    filename_ = std::string(filename);
    path_stack_.clear();
    anonymous_counter_ = 0;
    node_buffers_.clear();
    call_counters_.clear();
    multi_buffers_.clear();

    // Start with "main" path
    push_path("main");

    if (!ast.valid()) {
        error("E100", "Invalid AST", {});
        return {{}, std::move(diagnostics_), {}, {}, {}, false};
    }

    // Visit root (Program node)
    visit(ast.root);

    pop_path();

    bool success = !has_errors(diagnostics_);

    // Convert required_samples set to vector
    std::vector<std::string> required_samples_vec(required_samples_.begin(), required_samples_.end());

    return {std::move(instructions_), std::move(diagnostics_), std::move(state_inits_),
            std::move(required_samples_vec), std::move(param_decls_), success};
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

        case NodeType::StringLit: {
            // String literals are compile-time only (used for match patterns, osc type, etc.)
            // They don't have a runtime representation - return BUFFER_UNUSED.
            // The actual string value is accessed via as_string() during compile-time resolution.
            node_buffers_[node] = BufferAllocator::BUFFER_UNUSED;
            return BufferAllocator::BUFFER_UNUSED;
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
            inst.inputs[3] = 0xFFFF;

            // Encode float value (split across inputs[4] and state_id)
            float value = static_cast<float>(n.as_number());
            encode_const_value(inst, value);

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
            inst.inputs[3] = 0xFFFF;

            float value = n.as_bool() ? 1.0f : 0.0f;
            encode_const_value(inst, value);

            emit(inst);
            node_buffers_[node] = out;
            return out;
        }

        case NodeType::PitchLit: {
            // Emit PUSH_CONST for MIDI note, then MTOF to convert to frequency
            float midi_value = static_cast<float>(n.as_pitch());
            std::uint16_t freq_buf = codegen::emit_midi_to_freq(
                buffers_, instructions_, midi_value);
            if (freq_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return BufferAllocator::BUFFER_UNUSED;
            }
            node_buffers_[node] = freq_buf;
            return freq_buf;
        }

        case NodeType::ChordLit: {
            // For MVP, emit first note (root) of chord
            // Full chord expansion would require array support
            const auto& chord = n.as_chord();
            float midi_value = static_cast<float>(chord.root_midi);
            std::uint16_t freq_buf = codegen::emit_midi_to_freq(
                buffers_, instructions_, midi_value);
            if (freq_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return BufferAllocator::BUFFER_UNUSED;
            }
            node_buffers_[node] = freq_buf;
            return freq_buf;
        }

        case NodeType::ArrayLit: {
            // Arrays: emit all elements as multi-buffer for polyphony support
            NodeIndex first_elem = n.first_child;
            if (first_elem == NULL_NODE) {
                // Empty array - emit 0
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
                inst.inputs[3] = 0xFFFF;
                encode_const_value(inst, 0.0f);
                emit(inst);
                node_buffers_[node] = out;
                return out;
            }

            // Visit all elements and collect buffers
            std::vector<std::uint16_t> element_buffers;
            NodeIndex elem = first_elem;
            while (elem != NULL_NODE) {
                std::uint16_t elem_buf = visit(elem);
                element_buffers.push_back(elem_buf);
                elem = ast_->arena[elem].next_sibling;
            }

            if (element_buffers.size() == 1) {
                // Single element - return directly
                node_buffers_[node] = element_buffers[0];
                return element_buffers[0];
            }

            // Multi-element array - register as multi-buffer
            std::uint16_t first_buf = register_multi_buffer(node, std::move(element_buffers));
            node_buffers_[node] = first_buf;
            return first_buf;
        }

        case NodeType::Index: {
            // Array indexing: arr[i]
            // For now, just return the array (first element) since we don't
            // have runtime array support yet
            NodeIndex arr = n.first_child;
            if (arr == NULL_NODE) {
                error("E111", "Invalid index expression", n.location);
                return BufferAllocator::BUFFER_UNUSED;
            }

            std::uint16_t arr_buf = visit(arr);
            // TODO: Implement actual indexing when we have runtime arrays
            node_buffers_[node] = arr_buf;
            return arr_buf;
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

            if (sym->kind == SymbolKind::Pattern) {
                // Pattern variable - emit SEQ_STEP for this pattern
                // Use the stored pattern_node to generate code
                return handle_pattern_reference(name, sym->pattern.pattern_node, n.location);
            }

            if (sym->kind == SymbolKind::Array) {
                // Array variable - visit the source node to generate array code
                std::uint16_t first_buf = visit(sym->array.source_node);

                // Propagate multi-buffer association from source to this identifier
                if (is_multi_buffer(sym->array.source_node)) {
                    auto buffers = get_multi_buffers(sym->array.source_node);
                    register_multi_buffer(node, buffers);
                }

                node_buffers_[node] = first_buf;
                return first_buf;
            }

            if (sym->kind == SymbolKind::FunctionValue || sym->kind == SymbolKind::UserFunction) {
                // Function values are handled specially in map() and other HOFs
                // Return BUFFER_UNUSED since functions don't have runtime values
                return BufferAllocator::BUFFER_UNUSED;
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

            // Check if this is a pattern assignment
            auto sym = symbols_->lookup(var_name);
            if (sym && sym->kind == SymbolKind::Pattern) {
                // Pattern assignments don't emit code here - the pattern is
                // evaluated when the variable is referenced
                node_buffers_[node] = BufferAllocator::BUFFER_UNUSED;
                return BufferAllocator::BUFFER_UNUSED;
            }

            // Push variable name onto path for semantic IDs
            push_path(var_name);

            // Generate code for the value expression
            std::uint16_t value_buffer = visit(value_idx);

            pop_path();

            // Update symbol table with the buffer index
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

            // Check user-defined functions FIRST (allows stdlib osc to work)
            // This enables the stdlib osc() to be defined in user-space and
            // also allows users to shadow stdlib definitions.
            auto sym = symbols_->lookup(func_name);
            if (sym && sym->kind == SymbolKind::UserFunction) {
                return handle_user_function_call(node, n, sym->user_function);
            }

            // Check for FunctionValue (lambda assigned to variable)
            if (sym && sym->kind == SymbolKind::FunctionValue) {
                return handle_function_value_call(node, n, sym->function_ref);
            }

            // Dispatch table for special function handlers
            using Handler = std::uint16_t (CodeGenerator::*)(NodeIndex, const Node&);
            static const std::unordered_map<std::string_view, Handler> special_handlers = {
                {"len",     &CodeGenerator::handle_len_call},
                {"chord",   &CodeGenerator::handle_chord_call},
                {"map",     &CodeGenerator::handle_map_call},
                {"sum",     &CodeGenerator::handle_sum_call},
                {"fold",    &CodeGenerator::handle_fold_call},
                {"zipWith", &CodeGenerator::handle_zipWith_call},
                {"zip",     &CodeGenerator::handle_zip_call},
                {"take",    &CodeGenerator::handle_take_call},
                {"drop",    &CodeGenerator::handle_drop_call},
                {"reverse", &CodeGenerator::handle_reverse_call},
                {"range",   &CodeGenerator::handle_range_call},
                {"repeat",  &CodeGenerator::handle_repeat_call},
                // Parameter exposure builtins
                {"param",   &CodeGenerator::handle_param_call},
                {"button",  &CodeGenerator::handle_button_call},
                {"toggle",  &CodeGenerator::handle_toggle_call},
                {"dropdown", &CodeGenerator::handle_select_call},
            };

            auto handler_it = special_handlers.find(func_name);
            if (handler_it != special_handlers.end()) {
                return (this->*(handler_it->second))(node, n);
            }

            // Special handling for mtof() - propagate multi-buffers
            if (func_name == "mtof") {
                NodeIndex arg = n.first_child;
                if (arg == NULL_NODE) {
                    error("E135", "mtof() requires 1 argument", n.location);
                    return BufferAllocator::BUFFER_UNUSED;
                }

                const Node& arg_node = ast_->arena[arg];
                NodeIndex midi_node = (arg_node.type == NodeType::Argument) ?
                                      arg_node.first_child : arg;

                // Visit to populate multi_buffers_ map
                (void)visit(midi_node);

                // Check if input is multi-buffer (e.g., from chord())
                if (is_multi_buffer(midi_node)) {
                    auto midi_buffers = get_multi_buffers(midi_node);
                    std::vector<std::uint16_t> freq_buffers;
                    freq_buffers.reserve(midi_buffers.size());

                    for (std::uint16_t mb : midi_buffers) {
                        std::uint16_t freq_buf = buffers_.allocate();
                        if (freq_buf == BufferAllocator::BUFFER_UNUSED) {
                            error("E101", "Buffer pool exhausted", n.location);
                            return BufferAllocator::BUFFER_UNUSED;
                        }

                        cedar::Instruction mtof_inst{};
                        mtof_inst.opcode = cedar::Opcode::MTOF;
                        mtof_inst.out_buffer = freq_buf;
                        mtof_inst.inputs[0] = mb;
                        mtof_inst.inputs[1] = 0xFFFF;
                        mtof_inst.inputs[2] = 0xFFFF;
                        mtof_inst.inputs[3] = 0xFFFF;
                        mtof_inst.state_id = 0;
                        emit(mtof_inst);

                        freq_buffers.push_back(freq_buf);
                    }

                    std::uint16_t first_buf = register_multi_buffer(node, std::move(freq_buffers));
                    node_buffers_[node] = first_buf;
                    return first_buf;
                }

                // Single buffer case - fall through to normal handling
            }

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
                    push_inst.inputs[3] = 0xFFFF;

                    float default_val = builtin->get_default(i);
                    encode_const_value(push_inst, default_val);
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
            inst.inputs[3] = arg_buffers.size() > 3 ? arg_buffers[3] : 0xFFFF;
            inst.inputs[4] = arg_buffers.size() > 4 ? arg_buffers[4] : 0xFFFF;
            inst.rate = 0;

            // Special handling for ADSR: pack release time (arg 4) into rate field
            // Release time in tenths of seconds (0-255 -> 0-25.5s)
            if (func_name == "adsr" && arg_buffers.size() >= 5) {
                // Find the release argument value from AST to extract literal
                NodeIndex adsr_arg = n.first_child;
                for (std::size_t idx = 0; adsr_arg != NULL_NODE && idx < 5; ++idx) {
                    if (idx == 4) {
                        const Node& arg_node = ast_->arena[adsr_arg];
                        NodeIndex arg_value = (arg_node.type == NodeType::Argument) ?
                                             arg_node.first_child : adsr_arg;
                        if (arg_value != NULL_NODE) {
                            const Node& val_node = ast_->arena[arg_value];
                            if (val_node.type == NodeType::NumberLit) {
                                float release_val = static_cast<float>(val_node.as_number());
                                inst.rate = static_cast<std::uint8_t>(
                                    std::clamp(release_val / 0.1f, 0.0f, 255.0f));
                            }
                        }
                        break;
                    }
                    adsr_arg = ast_->arena[adsr_arg].next_sibling;
                }
            }

            // Generate state_id from current path (already pushed if stateful)
            if (pushed_path) {
                inst.state_id = compute_state_id();
                pop_path();
            } else {
                inst.state_id = 0;
            }

            // FM Detection: Automatically upgrade oscillators to 4x when frequency
            // input comes from an audio-rate source (another oscillator, noise, etc.)
            if (is_upgradeable_oscillator(inst.opcode) && !arg_buffers.empty()) {
                std::uint16_t freq_buffer = arg_buffers[0];
                if (is_fm_modulated(freq_buffer)) {
                    inst.opcode = upgrade_for_fm(inst.opcode);
                }
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
            return handle_closure(node, n);

        case NodeType::MethodCall:
            error("E113", "Method calls not supported in MVP", n.location);
            return BufferAllocator::BUFFER_UNUSED;

        case NodeType::MiniLiteral:
            return handle_mini_literal(node, n);

        case NodeType::PostStmt:
            error("E115", "Post statements not supported in MVP", n.location);
            return BufferAllocator::BUFFER_UNUSED;

        case NodeType::FunctionDef:
            // Function definitions don't generate code directly
            // They're registered in the symbol table for inline expansion
            return BufferAllocator::BUFFER_UNUSED;

        case NodeType::MatchExpr:
            return handle_match_expr(node, n);

        case NodeType::MatchArm:
            // MatchArm nodes are handled by MatchExpr, not visited directly
            error("E122", "Match arm visited outside of match expression", n.location);
            return BufferAllocator::BUFFER_UNUSED;

        default:
            error("E199", "Unsupported node type", n.location);
            return BufferAllocator::BUFFER_UNUSED;
    }
}

// handle_user_function_call, handle_closure, handle_match_expr are in codegen_functions.cpp
// handle_pattern_reference is in codegen_patterns.cpp
// handle_len_call is in codegen_arrays.cpp
// handle_chord_call is in codegen_patterns.cpp

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

void CodeGenerator::warn(const std::string& code, const std::string& message,
                         SourceLocation loc) {
    Diagnostic diag;
    diag.severity = Severity::Warning;
    diag.code = code;
    diag.message = message;
    diag.filename = filename_;
    diag.location = loc;
    diagnostics_.push_back(std::move(diag));
}

// FM Detection: Check if buffer was produced by audio-rate source (recursively traces arithmetic)
// Note: is_audio_rate_producer, is_upgradeable_oscillator, upgrade_for_fm are inline helpers
// in akkado/codegen/fm_detection.hpp
bool CodeGenerator::is_fm_modulated(std::uint16_t freq_buffer) const {
    for (const auto& inst : instructions_) {
        if (inst.out_buffer == freq_buffer) {
            // Direct audio-rate producer
            if (is_audio_rate_producer(inst.opcode)) {
                return true;
            }
            // Arithmetic on FM source is still FM
            if (inst.opcode == cedar::Opcode::ADD ||
                inst.opcode == cedar::Opcode::SUB ||
                inst.opcode == cedar::Opcode::MUL ||
                inst.opcode == cedar::Opcode::DIV ||
                inst.opcode == cedar::Opcode::POW) {
                // Check if either input traces back to audio-rate source
                if (inst.inputs[0] != 0xFFFF && is_fm_modulated(inst.inputs[0])) {
                    return true;
                }
                if (inst.inputs[1] != 0xFFFF && is_fm_modulated(inst.inputs[1])) {
                    return true;
                }
            }
            // Found the producer but it's not audio-rate
            break;
        }
    }
    return false;
}

// Array HOF implementations (map, sum, fold, zipWith, zip, take, drop, reverse, range, repeat)
// and multi-buffer support functions are in codegen_arrays.cpp

} // namespace akkado
