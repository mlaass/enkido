// Pattern and chord codegen implementations
// Extracted from codegen.cpp for maintainability

#include "akkado/codegen.hpp"
#include "akkado/codegen/codegen.hpp"
#include "akkado/chord_parser.hpp"
#include "akkado/pattern_eval.hpp"
#include <cmath>

namespace akkado {

using codegen::encode_const_value;
using codegen::unwrap_argument;
using codegen::emit_zero;

// Handle MiniLiteral (pattern) nodes
std::uint16_t CodeGenerator::handle_mini_literal(NodeIndex node, const Node& n) {
    [[maybe_unused]] PatternType pat_type = n.as_pattern_type();

    NodeIndex pattern_node = n.first_child;
    NodeIndex closure_node = NULL_NODE;

    if (pattern_node != NULL_NODE) {
        closure_node = ast_->arena[pattern_node].next_sibling;
    }

    if (pattern_node == NULL_NODE) {
        error("E114", "Pattern has no parsed content", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint32_t pat_count = call_counters_["pat"]++;
    push_path("pat#" + std::to_string(pat_count));
    std::uint32_t state_id = compute_state_id();

    PatternEventStream events = evaluate_pattern(pattern_node, ast_->arena, 0);

    if (events.empty()) {
        std::uint16_t out = emit_zero(buffers_, instructions_);
        if (out == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
        }
        pop_path();
        node_buffers_[node] = out;
        return out;
    }

    bool is_sample_pattern = false;
    for (const auto& event : events.events) {
        if (event.type == PatternEventType::Sample) {
            is_sample_pattern = true;
            break;
        }
    }

    if (is_sample_pattern) {
        return handle_sample_pattern(node, n, events, state_id);
    }

    return handle_pitch_pattern(node, n, events, state_id, closure_node);
}

// Handle sample patterns (bd, sn, etc.)
std::uint16_t CodeGenerator::handle_sample_pattern(NodeIndex node, const Node& n,
                                                    const PatternEventStream& events,
                                                    std::uint32_t state_id) {
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

    cedar::Instruction seq_inst{};
    seq_inst.opcode = cedar::Opcode::SEQ_STEP;
    seq_inst.out_buffer = sample_id_buf;
    seq_inst.inputs[0] = velocity_buf;
    seq_inst.inputs[1] = trigger_buf;
    seq_inst.inputs[2] = 0xFFFF;
    seq_inst.inputs[3] = 0xFFFF;
    seq_inst.state_id = state_id;
    emit(seq_inst);

    StateInitData seq_init;
    seq_init.state_id = state_id;
    seq_init.type = StateInitData::Type::SeqStep;
    seq_init.cycle_length = 4.0f;
    seq_init.times.reserve(events.size());
    seq_init.values.reserve(events.size());
    seq_init.velocities.reserve(events.size());

    for (const auto& event : events.events) {
        seq_init.times.push_back(event.time * seq_init.cycle_length);

        if (event.type == PatternEventType::Sample) {
            if (!event.sample_name.empty()) {
                required_samples_.insert(event.sample_name);
            }
            seq_init.sample_names.push_back(event.sample_name);

            std::uint32_t sample_id = 0;
            if (sample_registry_) {
                sample_id = sample_registry_->get_id(event.sample_name);
            }
            seq_init.values.push_back(static_cast<float>(sample_id));
        } else {
            seq_init.sample_names.push_back("");
            seq_init.values.push_back(0.0f);
        }
        seq_init.velocities.push_back(event.velocity);
    }
    state_inits_.push_back(std::move(seq_init));

    cedar::Instruction pitch_inst{};
    pitch_inst.opcode = cedar::Opcode::PUSH_CONST;
    pitch_inst.out_buffer = pitch_buf;
    pitch_inst.inputs[0] = 0xFFFF;
    pitch_inst.inputs[1] = 0xFFFF;
    pitch_inst.inputs[2] = 0xFFFF;
    pitch_inst.inputs[3] = 0xFFFF;
    encode_const_value(pitch_inst, 1.0f);
    emit(pitch_inst);

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

// Handle pitch patterns (c4 e4 g4, etc.)
std::uint16_t CodeGenerator::handle_pitch_pattern(NodeIndex node, const Node& n,
                                                   const PatternEventStream& events,
                                                   std::uint32_t state_id,
                                                   NodeIndex closure_node) {
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

    cedar::Instruction seq_inst{};
    seq_inst.opcode = cedar::Opcode::SEQ_STEP;
    seq_inst.out_buffer = pitch_buf;
    seq_inst.inputs[0] = velocity_buf;
    seq_inst.inputs[1] = trigger_buf;
    seq_inst.inputs[2] = 0xFFFF;
    seq_inst.inputs[3] = 0xFFFF;
    seq_inst.state_id = state_id;
    emit(seq_inst);

    StateInitData pitch_init;
    pitch_init.state_id = state_id;
    pitch_init.type = StateInitData::Type::SeqStep;
    pitch_init.cycle_length = 4.0f;
    pitch_init.times.reserve(events.size());
    pitch_init.values.reserve(events.size());
    pitch_init.velocities.reserve(events.size());

    for (const auto& event : events.events) {
        pitch_init.times.push_back(event.time * pitch_init.cycle_length);

        if (event.type == PatternEventType::Pitch) {
            float freq = 440.0f * std::pow(2.0f, (static_cast<float>(event.midi_note) - 69.0f) / 12.0f);
            pitch_init.values.push_back(freq);
        } else {
            pitch_init.values.push_back(0.0f);
        }
        pitch_init.velocities.push_back(event.velocity);
    }
    state_inits_.push_back(std::move(pitch_init));

    std::uint16_t result_buf = pitch_buf;

    if (closure_node != NULL_NODE) {
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

        if (param_names.size() >= 1) symbols_->define_variable(param_names[0], trigger_buf);
        if (param_names.size() >= 2) symbols_->define_variable(param_names[1], velocity_buf);
        if (param_names.size() >= 3) symbols_->define_variable(param_names[2], pitch_buf);

        if (body != NULL_NODE) {
            result_buf = visit(body);
        }
    }

    pop_path();
    node_buffers_[node] = result_buf;
    return result_buf;
}

// Handle pattern variable reference
std::uint16_t CodeGenerator::handle_pattern_reference(const std::string& name,
                                                       NodeIndex pattern_node,
                                                       SourceLocation loc) {
    if (pattern_node == NULL_NODE) {
        error("E123", "Pattern variable '" + name + "' has invalid pattern node", loc);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& pattern_n = ast_->arena[pattern_node];
    if (pattern_n.type != NodeType::MiniLiteral) {
        error("E124", "Pattern variable '" + name + "' does not refer to a pattern", loc);
        return BufferAllocator::BUFFER_UNUSED;
    }

    push_path(name);
    std::uint32_t state_id = compute_state_id();

    NodeIndex mini_pattern = pattern_n.first_child;
    if (mini_pattern == NULL_NODE) {
        error("E114", "Pattern has no parsed content", loc);
        pop_path();
        return BufferAllocator::BUFFER_UNUSED;
    }

    PatternEventStream events = evaluate_pattern(mini_pattern, ast_->arena, 0);

    if (events.empty()) {
        std::uint16_t out = emit_zero(buffers_, instructions_);
        if (out == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", loc);
        }
        pop_path();
        return out;
    }

    bool is_sample_pattern = false;
    for (const auto& event : events.events) {
        if (event.type == PatternEventType::Sample) {
            is_sample_pattern = true;
            break;
        }
    }

    if (is_sample_pattern) {
        std::uint16_t sample_id_buf = buffers_.allocate();
        std::uint16_t velocity_buf = buffers_.allocate();
        std::uint16_t trigger_buf = buffers_.allocate();

        if (sample_id_buf == BufferAllocator::BUFFER_UNUSED ||
            velocity_buf == BufferAllocator::BUFFER_UNUSED ||
            trigger_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", loc);
            pop_path();
            return BufferAllocator::BUFFER_UNUSED;
        }

        cedar::Instruction seq_inst{};
        seq_inst.opcode = cedar::Opcode::SEQ_STEP;
        seq_inst.out_buffer = sample_id_buf;
        seq_inst.inputs[0] = velocity_buf;
        seq_inst.inputs[1] = trigger_buf;
        seq_inst.inputs[2] = 0xFFFF;
        seq_inst.inputs[3] = 0xFFFF;
        seq_inst.state_id = state_id;
        emit(seq_inst);

        StateInitData seq_init;
        seq_init.state_id = state_id;
        seq_init.type = StateInitData::Type::SeqStep;
        seq_init.cycle_length = 4.0f;
        seq_init.times.reserve(events.size());
        seq_init.values.reserve(events.size());
        seq_init.velocities.reserve(events.size());

        for (const auto& event : events.events) {
            seq_init.times.push_back(event.time * seq_init.cycle_length);

            if (event.type == PatternEventType::Sample) {
                if (!event.sample_name.empty()) {
                    required_samples_.insert(event.sample_name);
                }
                seq_init.sample_names.push_back(event.sample_name);

                std::uint32_t sample_id = 0;
                if (sample_registry_) {
                    sample_id = sample_registry_->get_id(event.sample_name);
                }
                seq_init.values.push_back(static_cast<float>(sample_id));
            } else {
                seq_init.sample_names.push_back("");
                seq_init.values.push_back(0.0f);
            }
            seq_init.velocities.push_back(event.velocity);
        }
        state_inits_.push_back(std::move(seq_init));

        pop_path();
        return sample_id_buf;
    }

    // Pitch pattern
    std::uint16_t pitch_buf = buffers_.allocate();
    std::uint16_t velocity_buf = buffers_.allocate();
    std::uint16_t trigger_buf = buffers_.allocate();

    if (pitch_buf == BufferAllocator::BUFFER_UNUSED ||
        velocity_buf == BufferAllocator::BUFFER_UNUSED ||
        trigger_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", loc);
        pop_path();
        return BufferAllocator::BUFFER_UNUSED;
    }

    cedar::Instruction seq_inst{};
    seq_inst.opcode = cedar::Opcode::SEQ_STEP;
    seq_inst.out_buffer = pitch_buf;
    seq_inst.inputs[0] = velocity_buf;
    seq_inst.inputs[1] = trigger_buf;
    seq_inst.inputs[2] = 0xFFFF;
    seq_inst.inputs[3] = 0xFFFF;
    seq_inst.state_id = state_id;
    emit(seq_inst);

    StateInitData pitch_init;
    pitch_init.state_id = state_id;
    pitch_init.type = StateInitData::Type::SeqStep;
    pitch_init.cycle_length = 4.0f;
    pitch_init.times.reserve(events.size());
    pitch_init.values.reserve(events.size());
    pitch_init.velocities.reserve(events.size());

    for (const auto& event : events.events) {
        pitch_init.times.push_back(event.time * pitch_init.cycle_length);

        if (event.type == PatternEventType::Pitch) {
            float freq = 440.0f * std::pow(2.0f, (static_cast<float>(event.midi_note) - 69.0f) / 12.0f);
            pitch_init.values.push_back(freq);
        } else {
            pitch_init.values.push_back(0.0f);
        }
        pitch_init.velocities.push_back(event.velocity);
    }
    state_inits_.push_back(std::move(pitch_init));

    pop_path();
    return pitch_buf;
}

// Handle chord() calls
std::uint16_t CodeGenerator::handle_chord_call(NodeIndex node, const Node& n) {
    NodeIndex arg = n.first_child;
    if (arg == NULL_NODE) {
        error("E125", "chord() requires exactly 1 argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& arg_node = ast_->arena[arg];
    NodeIndex str_node = (arg_node.type == NodeType::Argument) ? arg_node.first_child : arg;

    if (str_node == NULL_NODE) {
        error("E125", "chord() requires a string argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& str_n = ast_->arena[str_node];
    if (str_n.type != NodeType::StringLit) {
        error("E126", "chord() argument must be a string literal (e.g., \"Am\", \"C7 F G\")",
              str_n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::string chord_str = str_n.as_string();
    auto chords = parse_chord_pattern(chord_str);
    if (chords.empty()) {
        error("E127", "Invalid chord symbol: \"" + chord_str + "\"", str_n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    if (chords.size() == 1) {
        return handle_single_chord(node, n, chords[0], chord_str);
    }
    return handle_chord_progression(node, n, chords, chord_str);
}

// Handle single chord expansion
std::uint16_t CodeGenerator::handle_single_chord(NodeIndex node, const Node& n,
                                                  const ChordInfo& chord,
                                                  const std::string& chord_str) {
    auto notes = expand_chord(chord, 4);
    if (notes.empty()) {
        error("E128", "Chord expansion failed for: \"" + chord_str + "\"", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::vector<std::uint16_t> note_buffers;
    note_buffers.reserve(notes.size());

    for (int midi : notes) {
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
        encode_const_value(push_inst, static_cast<float>(midi));
        emit(push_inst);

        note_buffers.push_back(midi_buf);
    }

    std::uint16_t first_buf = register_multi_buffer(node, std::move(note_buffers));
    node_buffers_[node] = first_buf;
    return first_buf;
}

// Handle chord progression (multiple chords)
std::uint16_t CodeGenerator::handle_chord_progression(NodeIndex node, const Node& n,
                                                       const std::vector<ChordInfo>& chords,
                                                       const std::string& chord_str) {
    std::size_t max_voices = 0;
    for (const auto& chord : chords) {
        auto notes = expand_chord(chord, 4);
        max_voices = std::max(max_voices, notes.size());
    }

    if (max_voices == 0) {
        error("E128", "Chord expansion failed for: \"" + chord_str + "\"", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    push_path("chord#" + std::to_string(call_counters_["chord"]++));

    std::vector<std::uint16_t> voice_buffers;
    voice_buffers.reserve(max_voices);
    float step = 4.0f / static_cast<float>(chords.size());

    for (std::size_t voice = 0; voice < max_voices; ++voice) {
        push_path("voice" + std::to_string(voice));
        std::uint32_t state_id = compute_state_id();

        std::uint16_t pitch_buf = buffers_.allocate();
        std::uint16_t velocity_buf = buffers_.allocate();
        std::uint16_t trigger_buf = buffers_.allocate();

        if (pitch_buf == BufferAllocator::BUFFER_UNUSED ||
            velocity_buf == BufferAllocator::BUFFER_UNUSED ||
            trigger_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            pop_path();
            pop_path();
            return BufferAllocator::BUFFER_UNUSED;
        }

        cedar::Instruction seq_inst{};
        seq_inst.opcode = cedar::Opcode::SEQ_STEP;
        seq_inst.out_buffer = pitch_buf;
        seq_inst.inputs[0] = velocity_buf;
        seq_inst.inputs[1] = trigger_buf;
        seq_inst.inputs[2] = 0xFFFF;
        seq_inst.inputs[3] = 0xFFFF;
        seq_inst.state_id = state_id;
        emit(seq_inst);

        StateInitData voice_init;
        voice_init.state_id = state_id;
        voice_init.type = StateInitData::Type::SeqStep;
        voice_init.cycle_length = 4.0f;
        voice_init.times.reserve(chords.size());
        voice_init.values.reserve(chords.size());
        voice_init.velocities.reserve(chords.size());

        for (std::size_t i = 0; i < chords.size(); ++i) {
            voice_init.times.push_back(step * static_cast<float>(i));

            auto notes = expand_chord(chords[i], 4);
            int midi = 0;
            if (voice < notes.size()) {
                midi = notes[voice];
            } else if (!notes.empty()) {
                midi = notes[0];
            }
            voice_init.values.push_back(static_cast<float>(midi));
            voice_init.velocities.push_back(1.0f);
        }
        state_inits_.push_back(std::move(voice_init));

        voice_buffers.push_back(pitch_buf);
        pop_path();
    }

    pop_path();

    std::uint16_t first_buf = register_multi_buffer(node, std::move(voice_buffers));
    node_buffers_[node] = first_buf;
    return first_buf;
}

} // namespace akkado
