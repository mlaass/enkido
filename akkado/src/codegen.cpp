#include "akkado/codegen.hpp"
#include "akkado/builtins.hpp"
#include "akkado/pattern_eval.hpp"
#include <cedar/vm/state_pool.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

namespace akkado {

// Helper to properly encode a float constant in a PUSH_CONST instruction.
// The float is stored directly in state_id (32 bits).
static void encode_const_value(cedar::Instruction& inst, float value) {
    std::memcpy(&inst.state_id, &value, sizeof(float));
    inst.inputs[4] = cedar::BUFFER_UNUSED;
}

// ============================================================================
// Oscillator Type Resolution for osc() Function
// ============================================================================
// Maps string type names to Cedar oscillator opcodes.
// Supports both short and long names (e.g., "sin" and "sine").

struct OscTypeMapping {
    cedar::Opcode opcode;
    bool requires_pwm;  // true if this oscillator needs a pwm parameter
};

static std::optional<OscTypeMapping> resolve_osc_type(std::string_view type_name) {
    // Basic oscillators (1 arg: freq)
    if (type_name == "sin" || type_name == "sine") {
        return OscTypeMapping{cedar::Opcode::OSC_SIN, false};
    }
    if (type_name == "tri" || type_name == "triangle") {
        return OscTypeMapping{cedar::Opcode::OSC_TRI, false};
    }
    if (type_name == "saw" || type_name == "sawtooth") {
        return OscTypeMapping{cedar::Opcode::OSC_SAW, false};
    }
    if (type_name == "sqr" || type_name == "square") {
        return OscTypeMapping{cedar::Opcode::OSC_SQR, false};
    }
    if (type_name == "ramp") {
        return OscTypeMapping{cedar::Opcode::OSC_RAMP, false};
    }
    if (type_name == "phasor") {
        return OscTypeMapping{cedar::Opcode::OSC_PHASOR, false};
    }
    if (type_name == "noise" || type_name == "white") {
        return OscTypeMapping{cedar::Opcode::NOISE, false};
    }

    // PWM oscillators (2 args: freq, pwm) - use osc("sqr_pwm", freq, pwm)
    if (type_name == "sqr_pwm" || type_name == "pulse") {
        return OscTypeMapping{cedar::Opcode::OSC_SQR_PWM, true};
    }
    if (type_name == "saw_pwm" || type_name == "var_saw") {
        return OscTypeMapping{cedar::Opcode::OSC_SAW_PWM, true};
    }

    // MinBLEP variants
    if (type_name == "sqr_minblep") {
        return OscTypeMapping{cedar::Opcode::OSC_SQR_MINBLEP, false};
    }
    if (type_name == "sqr_pwm_minblep") {
        return OscTypeMapping{cedar::Opcode::OSC_SQR_PWM_MINBLEP, true};
    }

    return std::nullopt;
}

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
    filename_ = std::string(filename);
    path_stack_.clear();
    anonymous_counter_ = 0;
    node_buffers_.clear();
    call_counters_.clear();

    // Start with "main" path
    push_path("main");

    if (!ast.valid()) {
        error("E100", "Invalid AST", {});
        return {{}, std::move(diagnostics_), {}, {}, false};
    }

    // Visit root (Program node)
    visit(ast.root);

    pop_path();

    bool success = !has_errors(diagnostics_);

    // Convert required_samples set to vector
    std::vector<std::string> required_samples_vec(required_samples_.begin(), required_samples_.end());

    return {std::move(instructions_), std::move(diagnostics_), std::move(state_inits_),
            std::move(required_samples_vec), success};
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
            push_inst.inputs[3] = 0xFFFF;

            float midi_value = static_cast<float>(n.as_pitch());
            encode_const_value(push_inst, midi_value);
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
            mtof_inst.inputs[3] = 0xFFFF;
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
            push_inst.inputs[3] = 0xFFFF;

            float midi_value = static_cast<float>(root_midi);
            encode_const_value(push_inst, midi_value);
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
            mtof_inst.inputs[3] = 0xFFFF;
            mtof_inst.state_id = 0;
            emit(mtof_inst);

            node_buffers_[node] = freq_buf;
            return freq_buf;
        }

        case NodeType::ArrayLit: {
            // Arrays: for now, emit first element as the value
            // Full array expansion will be implemented in a future phase
            // This allows basic code to run while we build out array semantics
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

            // Visit first element
            std::uint16_t first_buf = visit(first_elem);
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

            // Check user-defined functions FIRST (allows stdlib osc to work)
            // This enables the stdlib osc() to be defined in user-space and
            // also allows users to shadow stdlib definitions.
            auto sym = symbols_->lookup(func_name);
            if (sym && sym->kind == SymbolKind::UserFunction) {
                return handle_user_function_call(node, n, sym->user_function);
            }

            // ================================================================
            // Fallback: Special handling for osc() - Strudel-style oscillator selection
            // ================================================================
            // This is kept as fallback in case stdlib isn't loaded or for direct calls.
            // osc(type, freq) or osc(type, freq, pwm) where type is a string literal
            // Resolves the type string at compile-time to the appropriate opcode.
            if (func_name == "osc") {
                return handle_osc_call(node, n);
            }

            // ================================================================
            // Special handling for len() - compile-time array length
            // ================================================================
            // len(arr) returns the number of elements in an array literal
            if (func_name == "len") {
                return handle_len_call(node, n);
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

        case NodeType::MiniLiteral: {
            // Get pattern type (used for future pat/seq/timeline differentiation)
            [[maybe_unused]] PatternType pat_type = n.as_pattern_type();

            // Get children: first is MiniPattern, second (optional) is closure
            NodeIndex pattern_node = n.first_child;
            NodeIndex closure_node = NULL_NODE;

            if (pattern_node != NULL_NODE) {
                closure_node = ast_->arena[pattern_node].next_sibling;
            }

            if (pattern_node == NULL_NODE) {
                error("E114", "Pattern has no parsed content", n.location);
                return BufferAllocator::BUFFER_UNUSED;
            }

            // Push path for semantic ID
            std::uint32_t pat_count = call_counters_["pat"]++;
            push_path("pat#" + std::to_string(pat_count));
            std::uint32_t state_id = compute_state_id();

            // Evaluate pattern to get events
            PatternEventStream events = evaluate_pattern(pattern_node, ast_->arena, 0);

            if (events.empty()) {
                // Empty pattern - just return a constant 0
                std::uint16_t out = buffers_.allocate();
                if (out == BufferAllocator::BUFFER_UNUSED) {
                    error("E101", "Buffer pool exhausted", n.location);
                    pop_path();
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
                pop_path();
                node_buffers_[node] = out;
                return out;
            }

            // Detect if this is a sample pattern or pitch pattern
            bool is_sample_pattern = false;
            for (const auto& event : events.events) {
                if (event.type == PatternEventType::Sample) {
                    is_sample_pattern = true;
                    break;
                }
            }

            // Handle sample patterns differently
            if (is_sample_pattern) {
                // For sample patterns, we need to:
                // 1. Generate SEQ_STEP which outputs sample_id, velocity, and trigger
                // 2. Generate pitch/speed (constant 1.0 for now)
                // 3. Call SAMPLE_PLAY opcode

                std::uint16_t sample_id_buf = buffers_.allocate();
                std::uint16_t velocity_buf = buffers_.allocate();
                std::uint16_t trigger_buf = buffers_.allocate();
                std::uint16_t pitch_buf = buffers_.allocate();
                std::uint16_t output_buf = buffers_.allocate();

                if (sample_id_buf == BufferAllocator::BUFFER_UNUSED ||
                    velocity_buf == BufferAllocator::BUFFER_UNUSED ||
                    trigger_buf == BufferAllocator::BUFFER_UNUSED ||
                    pitch_buf == BufferAllocator::BUFFER_UNUSED ||
                    output_buf == BufferAllocator::BUFFER_UNUSED) {
                    error("E101", "Buffer pool exhausted", n.location);
                    pop_path();
                    return BufferAllocator::BUFFER_UNUSED;
                }

                // Emit SEQ_STEP - outputs sample_id, velocity, and trigger
                // out_buffer = sample_id, inputs[0] = velocity, inputs[1] = trigger
                std::uint32_t seq_state_id = state_id;
                cedar::Instruction seq_inst{};
                seq_inst.opcode = cedar::Opcode::SEQ_STEP;
                seq_inst.out_buffer = sample_id_buf;
                seq_inst.inputs[0] = velocity_buf;
                seq_inst.inputs[1] = trigger_buf;
                seq_inst.inputs[2] = 0xFFFF;
                seq_inst.inputs[3] = 0xFFFF;
                seq_inst.state_id = seq_state_id;
                emit(seq_inst);

                // Build state initialization with times, values, velocities
                StateInitData seq_init;
                seq_init.state_id = seq_state_id;
                seq_init.type = StateInitData::Type::SeqStep;
                seq_init.cycle_length = 4.0f;  // 4 beats per cycle (1 bar in 4/4)
                seq_init.times.reserve(events.size());
                seq_init.values.reserve(events.size());
                seq_init.velocities.reserve(events.size());

                for (const auto& event : events.events) {
                    // Convert event.time from 0-1 cycle fraction to beats
                    seq_init.times.push_back(event.time * seq_init.cycle_length);

                    if (event.type == PatternEventType::Sample) {
                        // Collect sample name for runtime loading
                        if (!event.sample_name.empty()) {
                            required_samples_.insert(event.sample_name);
                        }
                        // Store sample name for deferred resolution
                        seq_init.sample_names.push_back(event.sample_name);

                        // Try to resolve ID if registry available, otherwise use 0 (resolved at runtime)
                        std::uint32_t sample_id = 0;
                        if (sample_registry_) {
                            sample_id = sample_registry_->get_id(event.sample_name);
                        }
                        seq_init.values.push_back(static_cast<float>(sample_id));
                    } else {
                        // Rest or other event type - use sample ID 0 (no sample)
                        seq_init.sample_names.push_back("");  // Empty for non-sample events
                        seq_init.values.push_back(0.0f);
                    }

                    seq_init.velocities.push_back(event.velocity);
                }
                state_inits_.push_back(std::move(seq_init));

                // Emit constant 1.0 for pitch (original speed)
                cedar::Instruction pitch_inst{};
                pitch_inst.opcode = cedar::Opcode::PUSH_CONST;
                pitch_inst.out_buffer = pitch_buf;
                pitch_inst.inputs[0] = 0xFFFF;
                pitch_inst.inputs[1] = 0xFFFF;
                pitch_inst.inputs[2] = 0xFFFF;
                pitch_inst.inputs[3] = 0xFFFF;
                encode_const_value(pitch_inst, 1.0f);
                emit(pitch_inst);

                // Emit SAMPLE_PLAY opcode
                cedar::Instruction sample_inst{};
                sample_inst.opcode = cedar::Opcode::SAMPLE_PLAY;
                sample_inst.out_buffer = output_buf;
                sample_inst.inputs[0] = trigger_buf;
                sample_inst.inputs[1] = pitch_buf;
                sample_inst.inputs[2] = sample_id_buf;
                sample_inst.inputs[3] = 0xFFFF;
                sample_inst.state_id = state_id + 1;
                emit(sample_inst);

                pop_path();
                node_buffers_[node] = output_buf;
                return output_buf;
            }

            // Allocate buffers for pattern outputs
            std::uint16_t pitch_buf = buffers_.allocate();
            std::uint16_t velocity_buf = buffers_.allocate();
            std::uint16_t trigger_buf = buffers_.allocate();

            if (pitch_buf == BufferAllocator::BUFFER_UNUSED ||
                velocity_buf == BufferAllocator::BUFFER_UNUSED ||
                trigger_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                pop_path();
                return BufferAllocator::BUFFER_UNUSED;
            }

            // Emit SEQ_STEP - outputs pitch, velocity, and trigger
            // out_buffer = pitch, inputs[0] = velocity, inputs[1] = trigger
            std::uint32_t pitch_seq_state_id = state_id;
            cedar::Instruction seq_inst{};
            seq_inst.opcode = cedar::Opcode::SEQ_STEP;
            seq_inst.out_buffer = pitch_buf;
            seq_inst.inputs[0] = velocity_buf;
            seq_inst.inputs[1] = trigger_buf;
            seq_inst.inputs[2] = 0xFFFF;
            seq_inst.inputs[3] = 0xFFFF;
            seq_inst.state_id = pitch_seq_state_id;
            emit(seq_inst);

            // Build state initialization with times, values (pitches), velocities
            StateInitData pitch_init;
            pitch_init.state_id = pitch_seq_state_id;
            pitch_init.type = StateInitData::Type::SeqStep;
            pitch_init.cycle_length = 4.0f;  // 4 beats per cycle (1 bar in 4/4)
            pitch_init.times.reserve(events.size());
            pitch_init.values.reserve(events.size());
            pitch_init.velocities.reserve(events.size());

            for (const auto& event : events.events) {
                // Convert event.time from 0-1 cycle fraction to beats
                pitch_init.times.push_back(event.time * pitch_init.cycle_length);

                if (event.type == PatternEventType::Pitch) {
                    // Convert MIDI note to frequency
                    float freq = 440.0f * std::pow(2.0f, (static_cast<float>(event.midi_note) - 69.0f) / 12.0f);
                    pitch_init.values.push_back(freq);
                } else if (event.type == PatternEventType::Rest) {
                    // For rests, use 0 frequency (will produce silence)
                    pitch_init.values.push_back(0.0f);
                } else {
                    // Other event types (shouldn't happen in note pattern path)
                    pitch_init.values.push_back(0.0f);
                }

                pitch_init.velocities.push_back(event.velocity);
            }
            state_inits_.push_back(std::move(pitch_init));

            std::uint16_t result_buf = pitch_buf;

            // If there's a closure, bind parameters and generate it
            if (closure_node != NULL_NODE) {
                // Closure params are typically (t, v, p) for trigger, velocity, pitch
                // Find parameter names from closure
                const Node& closure = ast_->arena[closure_node];
                std::vector<std::string> param_names;
                NodeIndex child = closure.first_child;
                NodeIndex body = NULL_NODE;

                while (child != NULL_NODE) {
                    const Node& child_node = ast_->arena[child];
                    if (child_node.type == NodeType::Identifier) {
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

                // Bind parameters: t=trigger, v=velocity, p=pitch
                if (param_names.size() >= 1) {
                    symbols_->define_variable(param_names[0], trigger_buf);
                }
                if (param_names.size() >= 2) {
                    symbols_->define_variable(param_names[1], velocity_buf);
                }
                if (param_names.size() >= 3) {
                    symbols_->define_variable(param_names[2], pitch_buf);
                }

                // Generate closure body
                if (body != NULL_NODE) {
                    result_buf = visit(body);
                }
            }

            pop_path();
            node_buffers_[node] = result_buf;
            return result_buf;
        }

        case NodeType::PostStmt:
            error("E115", "Post statements not supported in MVP", n.location);
            return BufferAllocator::BUFFER_UNUSED;

        case NodeType::FunctionDef:
            // Function definitions don't generate code directly
            // They're registered in the symbol table for inline expansion
            return BufferAllocator::BUFFER_UNUSED;

        case NodeType::MatchExpr: {
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

        case NodeType::MatchArm:
            // MatchArm nodes are handled by MatchExpr, not visited directly
            error("E122", "Match arm visited outside of match expression", n.location);
            return BufferAllocator::BUFFER_UNUSED;

        default:
            error("E199", "Unsupported node type", n.location);
            return BufferAllocator::BUFFER_UNUSED;
    }
}

// ============================================================================
// User Function Call Handler (Inline Expansion)
// ============================================================================
// Inlines user-defined function bodies at call sites.
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

// ============================================================================
// osc() Function Handler
// ============================================================================
// Handles osc(type, freq) and osc(type, freq, pwm) calls.
// The type must be a string literal that is resolved at compile time.
std::uint16_t CodeGenerator::handle_osc_call(NodeIndex node, const Node& n) {
    // Get arguments: first should be string literal (type), rest are signal args
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

    if (args.empty()) {
        error("E116", "osc() requires at least 2 arguments: osc(type, freq)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // First argument must be a string literal
    const Node& type_node = ast_->arena[args[0]];
    if (type_node.type != NodeType::StringLit) {
        error("E117", "osc() first argument must be a string literal (e.g., \"sin\", \"saw\")",
              type_node.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::string type_name = type_node.as_string();
    auto osc_type = resolve_osc_type(type_name);

    if (!osc_type) {
        error("E118", "Unknown oscillator type: \"" + type_name +
              "\". Valid types: sin/sine, tri/triangle, saw/sawtooth, sqr/square, "
              "ramp, phasor, noise, sqr_pwm/pulse, saw_pwm, sqr_minblep",
              type_node.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Check argument count based on oscillator type
    std::size_t required_args = osc_type->requires_pwm ? 3 : 2;  // type + freq [+ pwm]
    if (args.size() < required_args) {
        std::string expected = osc_type->requires_pwm
            ? "osc(\"" + type_name + "\", freq, pwm)"
            : "osc(\"" + type_name + "\", freq)";
        error("E119", "osc(\"" + type_name + "\") requires " + std::to_string(required_args) +
              " arguments: " + expected, n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Push path for state ID
    std::uint32_t count = call_counters_["osc"]++;
    std::string unique_name = "osc#" + std::to_string(count);
    push_path(unique_name);

    // Visit signal arguments (skip the type string)
    std::vector<std::uint16_t> arg_buffers;
    for (std::size_t i = 1; i < args.size() && i < required_args; ++i) {
        std::uint16_t buf = visit(args[i]);
        arg_buffers.push_back(buf);
    }

    // Allocate output buffer
    std::uint16_t out = buffers_.allocate();
    if (out == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Build instruction
    cedar::Instruction inst{};
    inst.opcode = osc_type->opcode;
    inst.out_buffer = out;
    inst.inputs[0] = arg_buffers.size() > 0 ? arg_buffers[0] : 0xFFFF;
    inst.inputs[1] = arg_buffers.size() > 1 ? arg_buffers[1] : 0xFFFF;
    inst.inputs[2] = 0xFFFF;
    inst.inputs[3] = 0xFFFF;
    inst.rate = 0;
    inst.state_id = compute_state_id();

    // FM Detection: Automatically upgrade oscillators to 4x when frequency
    // input comes from an audio-rate source
    if (is_upgradeable_oscillator(inst.opcode) && !arg_buffers.empty()) {
        std::uint16_t freq_buffer = arg_buffers[0];
        if (is_fm_modulated(freq_buffer)) {
            inst.opcode = upgrade_for_fm(inst.opcode);
        }
    }

    pop_path();

    emit(inst);
    node_buffers_[node] = out;
    return out;
}

// Handles len(arr) calls - returns compile-time array length
std::uint16_t CodeGenerator::handle_len_call(NodeIndex node, const Node& n) {
    // Get the argument
    NodeIndex arg = n.first_child;
    if (arg == NULL_NODE) {
        error("E120", "len() requires exactly 1 argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Unwrap Argument node if present
    const Node& arg_node = ast_->arena[arg];
    NodeIndex arr_node = arg;
    if (arg_node.type == NodeType::Argument) {
        arr_node = arg_node.first_child;
    }

    if (arr_node == NULL_NODE) {
        error("E120", "len() requires an array argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& arr = ast_->arena[arr_node];

    // Count elements based on node type
    std::size_t length = 0;

    if (arr.type == NodeType::ArrayLit) {
        // Count children of array literal
        NodeIndex elem = arr.first_child;
        while (elem != NULL_NODE) {
            length++;
            elem = ast_->arena[elem].next_sibling;
        }
    } else if (arr.type == NodeType::Identifier) {
        // Look up the symbol to see if it's a known array
        // For now, error - we'd need more sophisticated tracking
        error("E121", "len() currently only supports array literals, not variables",
              arr.location);
        return BufferAllocator::BUFFER_UNUSED;
    } else {
        error("E122", "len() argument must be an array", arr.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Emit the length as a constant
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

    float len_value = static_cast<float>(length);
    encode_const_value(inst, len_value);
    emit(inst);

    node_buffers_[node] = out;
    return out;
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

// FM Detection: Check if opcode produces audio-rate signal
bool CodeGenerator::is_audio_rate_producer(cedar::Opcode op) {
    switch (op) {
        // All oscillators produce audio-rate signals
        case cedar::Opcode::OSC_SIN:
        case cedar::Opcode::OSC_SIN_2X:
        case cedar::Opcode::OSC_SIN_4X:
        case cedar::Opcode::OSC_TRI:
        case cedar::Opcode::OSC_TRI_2X:
        case cedar::Opcode::OSC_TRI_4X:
        case cedar::Opcode::OSC_SAW:
        case cedar::Opcode::OSC_SAW_2X:
        case cedar::Opcode::OSC_SAW_4X:
        case cedar::Opcode::OSC_SQR:
        case cedar::Opcode::OSC_SQR_2X:
        case cedar::Opcode::OSC_SQR_4X:
        case cedar::Opcode::OSC_RAMP:
        case cedar::Opcode::OSC_PHASOR:
        case cedar::Opcode::OSC_SQR_MINBLEP:
        case cedar::Opcode::OSC_SQR_PWM:
        case cedar::Opcode::OSC_SAW_PWM:
        case cedar::Opcode::OSC_SQR_PWM_MINBLEP:
        case cedar::Opcode::OSC_SQR_PWM_4X:
        case cedar::Opcode::OSC_SAW_PWM_4X:
        case cedar::Opcode::NOISE:
            return true;
        default:
            return false;
    }
}

// FM Detection: Check if opcode is a basic oscillator that can be upgraded
bool CodeGenerator::is_upgradeable_oscillator(cedar::Opcode op) {
    switch (op) {
        case cedar::Opcode::OSC_SIN:
        case cedar::Opcode::OSC_TRI:
        case cedar::Opcode::OSC_SAW:
        case cedar::Opcode::OSC_SQR:
        case cedar::Opcode::OSC_SQR_PWM:
        case cedar::Opcode::OSC_SAW_PWM:
            return true;
        default:
            return false;
    }
}

// FM Detection: Upgrade basic oscillator to 4x oversampled variant
cedar::Opcode CodeGenerator::upgrade_for_fm(cedar::Opcode op) {
    switch (op) {
        case cedar::Opcode::OSC_SIN: return cedar::Opcode::OSC_SIN_4X;
        case cedar::Opcode::OSC_TRI: return cedar::Opcode::OSC_TRI_4X;
        case cedar::Opcode::OSC_SAW: return cedar::Opcode::OSC_SAW_4X;
        case cedar::Opcode::OSC_SQR: return cedar::Opcode::OSC_SQR_4X;
        case cedar::Opcode::OSC_SQR_PWM: return cedar::Opcode::OSC_SQR_PWM_4X;
        case cedar::Opcode::OSC_SAW_PWM: return cedar::Opcode::OSC_SAW_PWM_4X;
        default: return op;  // No upgrade available
    }
}

// FM Detection: Check if buffer was produced by audio-rate source (recursively traces arithmetic)
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

} // namespace akkado
