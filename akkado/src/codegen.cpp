#include "akkado/codegen.hpp"
#include "akkado/builtins.hpp"
#include "akkado/pattern_eval.hpp"
#include <cedar/vm/state_pool.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace akkado {

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
    filename_ = std::string(filename);
    path_stack_.clear();
    anonymous_counter_ = 0;
    node_buffers_.clear();
    call_counters_.clear();

    // Start with "main" path
    push_path("main");

    if (!ast.valid()) {
        error("E100", "Invalid AST", {});
        return {{}, std::move(diagnostics_), {}, false};
    }

    // Visit root (Program node)
    visit(ast.root);

    pop_path();

    bool success = !has_errors(diagnostics_);

    return {std::move(instructions_), std::move(diagnostics_), std::move(state_inits_), success};
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
            inst.inputs[3] = 0xFFFF;

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
            inst.inputs[3] = 0xFFFF;

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
            push_inst.inputs[3] = 0xFFFF;

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
            mtof_inst.inputs[3] = 0xFFFF;
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
                    push_inst.inputs[3] = 0xFFFF;

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
            inst.inputs[3] = arg_buffers.size() > 3 ? arg_buffers[3] : 0xFFFF;
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
                float zero = 0.0f;
                std::memcpy(&inst.state_id, &zero, sizeof(float));
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
                        std::uint32_t sample_id = 0;
                        if (sample_registry_) {
                            sample_id = sample_registry_->get_id(event.sample_name);
                            if (sample_id == 0) {
                                error("W001", "Sample '" + event.sample_name + "' not registered in sample bank", n.location);
                            }
                        }
                        seq_init.values.push_back(static_cast<float>(sample_id));
                    } else {
                        // Rest or other event type - use sample ID 0 (no sample)
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
                float one = 1.0f;
                std::memcpy(&pitch_inst.state_id, &one, sizeof(float));
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
