// Pattern and chord codegen implementations
// Extracted from codegen.cpp for maintainability

#include "akkado/codegen.hpp"
#include "akkado/codegen/codegen.hpp"
#include "akkado/chord_parser.hpp"
#include "akkado/pattern_eval.hpp"
#include "akkado/mini_parser.hpp"
#include <cmath>

namespace akkado {

using codegen::encode_const_value;
using codegen::unwrap_argument;
using codegen::emit_zero;

// ============================================================================
// PatternCompiler - Converts mini-notation AST to PatternNode array
// ============================================================================
// This compiles the AST into a flat array of PatternNodes that can be
// evaluated at runtime by the PAT_QUERY opcode.

class PatternCompiler {
public:
    explicit PatternCompiler(const AstArena& arena, SampleRegistry* sample_registry = nullptr)
        : arena_(arena), sample_registry_(sample_registry) {}

    // Compile a pattern AST into PatternNode array
    // Returns true on success, false if compilation fails
    bool compile(NodeIndex root) {
        nodes_.clear();
        if (root == NULL_NODE) return false;
        compile_node(root);
        return !nodes_.empty();
    }

    // Get the compiled nodes
    const std::vector<cedar::PatternNode>& nodes() const { return nodes_; }

    // Check if pattern contains samples (vs pitch)
    bool is_sample_pattern() const { return is_sample_pattern_; }

    // Register required samples
    void collect_samples(std::set<std::string>& required) const {
        for (const auto& name : sample_names_) {
            required.insert(name);
        }
    }

private:
    // Allocate a new node and return its index
    std::uint16_t alloc_node() {
        if (nodes_.size() >= cedar::PatternQueryState::MAX_NODES) {
            return 0xFFFF;  // Overflow
        }
        nodes_.emplace_back();
        return static_cast<std::uint16_t>(nodes_.size() - 1);
    }

    // Compile a node recursively
    std::uint16_t compile_node(NodeIndex ast_idx) {
        if (ast_idx == NULL_NODE) return 0xFFFF;

        const Node& n = arena_[ast_idx];

        switch (n.type) {
            case NodeType::MiniPattern:
                return compile_pattern(ast_idx, n);
            case NodeType::MiniAtom:
                return compile_atom(ast_idx, n);
            case NodeType::MiniGroup:
                return compile_group(ast_idx, n);
            case NodeType::MiniSequence:
                return compile_sequence(ast_idx, n);
            case NodeType::MiniPolyrhythm:
                return compile_polyrhythm(ast_idx, n);
            case NodeType::MiniPolymeter:
                return compile_polymeter(ast_idx, n);
            case NodeType::MiniChoice:
                return compile_choice(ast_idx, n);
            case NodeType::MiniEuclidean:
                return compile_euclidean(ast_idx, n);
            case NodeType::MiniModified:
                return compile_modified(ast_idx, n);
            default:
                // Unknown node type - emit silence
                return compile_silence();
        }
    }

    // Compile a MiniPattern (root) node - contains children
    std::uint16_t compile_pattern(NodeIndex ast_idx, const Node& n) {
        // MiniPattern with single child - just compile child
        // MiniPattern with multiple children - wrap in CAT
        std::vector<std::uint16_t> child_indices;
        NodeIndex child = n.first_child;
        while (child != NULL_NODE) {
            std::uint16_t idx = compile_node(child);
            if (idx != 0xFFFF) {
                child_indices.push_back(idx);
            }
            child = arena_[child].next_sibling;
        }

        if (child_indices.empty()) {
            return compile_silence();
        }
        if (child_indices.size() == 1) {
            return child_indices[0];
        }

        // Multiple children - wrap in CAT
        std::uint16_t cat_idx = alloc_node();
        if (cat_idx == 0xFFFF) return 0xFFFF;

        auto& cat_node = nodes_[cat_idx];
        cat_node.op = cedar::PatternOp::CAT;
        cat_node.num_children = static_cast<std::uint8_t>(
            std::min(child_indices.size(), std::size_t(255)));
        cat_node.first_child_idx = child_indices[0];

        return cat_idx;
    }

    // Compile an atom (pitch, sample, or rest)
    std::uint16_t compile_atom(NodeIndex /*ast_idx*/, const Node& n) {
        const auto& atom_data = n.as_mini_atom();

        std::uint16_t idx = alloc_node();
        if (idx == 0xFFFF) return 0xFFFF;

        auto& node = nodes_[idx];

        switch (atom_data.kind) {
            case Node::MiniAtomKind::Pitch: {
                node.op = cedar::PatternOp::ATOM;
                // Convert MIDI note to frequency
                float freq = 440.0f * std::pow(2.0f,
                    (static_cast<float>(atom_data.midi_note) - 69.0f) / 12.0f);
                node.data.float_val = freq;
                break;
            }
            case Node::MiniAtomKind::Sample: {
                is_sample_pattern_ = true;
                node.op = cedar::PatternOp::ATOM;
                // Store sample ID
                std::uint32_t sample_id = 0;
                if (sample_registry_ && !atom_data.sample_name.empty()) {
                    sample_id = sample_registry_->get_id(atom_data.sample_name);
                    sample_names_.insert(atom_data.sample_name);
                }
                node.data.sample_id = sample_id;
                break;
            }
            case Node::MiniAtomKind::Rest:
                node.op = cedar::PatternOp::SILENCE;
                break;
        }

        return idx;
    }

    // Compile silence
    std::uint16_t compile_silence() {
        std::uint16_t idx = alloc_node();
        if (idx == 0xFFFF) return 0xFFFF;
        nodes_[idx].op = cedar::PatternOp::SILENCE;
        return idx;
    }

    // Compile a group [a b c] - sequential concatenation
    std::uint16_t compile_group(NodeIndex ast_idx, const Node& n) {
        std::vector<std::uint16_t> child_indices;
        NodeIndex child = n.first_child;
        while (child != NULL_NODE) {
            std::uint16_t idx = compile_node(child);
            if (idx != 0xFFFF) {
                child_indices.push_back(idx);
            }
            child = arena_[child].next_sibling;
        }

        if (child_indices.empty()) {
            return compile_silence();
        }
        if (child_indices.size() == 1) {
            return child_indices[0];
        }

        std::uint16_t cat_idx = alloc_node();
        if (cat_idx == 0xFFFF) return 0xFFFF;

        auto& cat_node = nodes_[cat_idx];
        cat_node.op = cedar::PatternOp::CAT;
        cat_node.num_children = static_cast<std::uint8_t>(
            std::min(child_indices.size(), std::size_t(255)));
        cat_node.first_child_idx = child_indices[0];

        return cat_idx;
    }

    // Compile a sequence <a b c> - alternating per cycle
    std::uint16_t compile_sequence(NodeIndex ast_idx, const Node& n) {
        std::vector<std::uint16_t> child_indices;
        NodeIndex child = n.first_child;
        while (child != NULL_NODE) {
            std::uint16_t idx = compile_node(child);
            if (idx != 0xFFFF) {
                child_indices.push_back(idx);
            }
            child = arena_[child].next_sibling;
        }

        if (child_indices.empty()) {
            return compile_silence();
        }
        if (child_indices.size() == 1) {
            return child_indices[0];
        }

        std::uint16_t seq_idx = alloc_node();
        if (seq_idx == 0xFFFF) return 0xFFFF;

        auto& seq_node = nodes_[seq_idx];
        seq_node.op = cedar::PatternOp::SLOWCAT;
        seq_node.num_children = static_cast<std::uint8_t>(
            std::min(child_indices.size(), std::size_t(255)));
        seq_node.first_child_idx = child_indices[0];

        return seq_idx;
    }

    // Compile a polyrhythm [a, b, c] - parallel stacking
    std::uint16_t compile_polyrhythm(NodeIndex ast_idx, const Node& n) {
        std::vector<std::uint16_t> child_indices;
        NodeIndex child = n.first_child;
        while (child != NULL_NODE) {
            std::uint16_t idx = compile_node(child);
            if (idx != 0xFFFF) {
                child_indices.push_back(idx);
            }
            child = arena_[child].next_sibling;
        }

        if (child_indices.empty()) {
            return compile_silence();
        }
        if (child_indices.size() == 1) {
            return child_indices[0];
        }

        std::uint16_t stack_idx = alloc_node();
        if (stack_idx == 0xFFFF) return 0xFFFF;

        auto& stack_node = nodes_[stack_idx];
        stack_node.op = cedar::PatternOp::STACK;
        stack_node.num_children = static_cast<std::uint8_t>(
            std::min(child_indices.size(), std::size_t(255)));
        stack_node.first_child_idx = child_indices[0];

        return stack_idx;
    }

    // Compile a polymeter {a b}%n
    std::uint16_t compile_polymeter(NodeIndex ast_idx, const Node& n) {
        // For now, treat polymeter similar to group
        return compile_group(ast_idx, n);
    }

    // Compile a choice a | b | c - random selection
    std::uint16_t compile_choice(NodeIndex ast_idx, const Node& n) {
        std::vector<std::uint16_t> child_indices;
        NodeIndex child = n.first_child;
        while (child != NULL_NODE) {
            std::uint16_t idx = compile_node(child);
            if (idx != 0xFFFF) {
                child_indices.push_back(idx);
            }
            child = arena_[child].next_sibling;
        }

        if (child_indices.empty()) {
            return compile_silence();
        }
        if (child_indices.size() == 1) {
            return child_indices[0];
        }

        std::uint16_t choice_idx = alloc_node();
        if (choice_idx == 0xFFFF) return 0xFFFF;

        auto& choice_node = nodes_[choice_idx];
        choice_node.op = cedar::PatternOp::CHOOSE;
        choice_node.num_children = static_cast<std::uint8_t>(
            std::min(child_indices.size(), std::size_t(255)));
        choice_node.first_child_idx = child_indices[0];

        return choice_idx;
    }

    // Compile euclidean rhythm
    std::uint16_t compile_euclidean(NodeIndex ast_idx, const Node& n) {
        const auto& euclid_data = n.as_mini_euclidean();

        // First compile the child (atom to place on hits)
        std::uint16_t child_idx = 0xFFFF;
        if (n.first_child != NULL_NODE) {
            child_idx = compile_node(n.first_child);
        }
        if (child_idx == 0xFFFF) {
            child_idx = compile_silence();
        }

        std::uint16_t idx = alloc_node();
        if (idx == 0xFFFF) return 0xFFFF;

        auto& node = nodes_[idx];
        node.op = cedar::PatternOp::EUCLID;
        node.data.euclid.hits = euclid_data.hits;
        node.data.euclid.steps = euclid_data.steps;
        node.data.euclid.rotation = euclid_data.rotation;
        node.num_children = 1;
        node.first_child_idx = child_idx;

        return idx;
    }

    // Compile modified node (with modifiers like *n, /n, !n, ?n, @n)
    std::uint16_t compile_modified(NodeIndex ast_idx, const Node& n) {
        const auto& mod_data = n.as_mini_modifier();

        // First compile the child
        std::uint16_t child_idx = 0xFFFF;
        if (n.first_child != NULL_NODE) {
            child_idx = compile_node(n.first_child);
        }
        if (child_idx == 0xFFFF) {
            return compile_silence();
        }

        std::uint16_t idx = alloc_node();
        if (idx == 0xFFFF) return child_idx;  // Fall back to child if we can't allocate

        auto& node = nodes_[idx];
        node.num_children = 1;
        node.first_child_idx = child_idx;
        node.data.float_val = mod_data.value;

        switch (mod_data.modifier_type) {
            case Node::MiniModifierType::Speed:
                node.op = cedar::PatternOp::FAST;
                break;
            case Node::MiniModifierType::Slow:
                node.op = cedar::PatternOp::SLOW;
                break;
            case Node::MiniModifierType::Repeat:
                node.op = cedar::PatternOp::REPLICATE;
                break;
            case Node::MiniModifierType::Chance:
                node.op = cedar::PatternOp::DEGRADE;
                break;
            case Node::MiniModifierType::Weight:
                node.op = cedar::PatternOp::WEIGHT;
                break;
            case Node::MiniModifierType::Duration:
                // Duration is handled differently - just pass through
                return child_idx;
        }

        return idx;
    }

    const AstArena& arena_;
    SampleRegistry* sample_registry_ = nullptr;
    std::vector<cedar::PatternNode> nodes_;
    std::set<std::string> sample_names_;
    bool is_sample_pattern_ = false;
};

// ============================================================================
// End PatternCompiler
// ============================================================================

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

    // Use the new PatternCompiler for lazy queryable patterns
    PatternCompiler compiler(ast_->arena, sample_registry_);
    if (!compiler.compile(pattern_node)) {
        // Fallback: try the old evaluation method for empty patterns
        PatternEventStream events = evaluate_pattern_multi_cycle(pattern_node, ast_->arena);
        if (events.empty()) {
            std::uint16_t out = emit_zero(buffers_, instructions_);
            if (out == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
            }
            pop_path();
            node_buffers_[node] = out;
            return out;
        }
        // Old evaluation found events but compiler failed - fall back to SEQ_STEP
        bool is_sample_pattern_old = false;
        for (const auto& event : events.events) {
            if (event.type == PatternEventType::Sample) {
                is_sample_pattern_old = true;
                break;
            }
        }
        if (is_sample_pattern_old) {
            return handle_sample_pattern(node, n, events, state_id);
        }
        return handle_pitch_pattern(node, n, events, state_id, closure_node);
    }

    // Collect required samples
    compiler.collect_samples(required_samples_);

    // Determine cycle count from pattern evaluation
    PatternEvaluator evaluator(ast_->arena);
    std::uint32_t num_cycles = evaluator.count_cycles(pattern_node);
    float cycle_length = 4.0f * static_cast<float>(std::max(1u, num_cycles));

    bool is_sample_pattern = compiler.is_sample_pattern();

    // Allocate buffers for outputs
    std::uint16_t value_buf = buffers_.allocate();
    std::uint16_t velocity_buf = buffers_.allocate();
    std::uint16_t trigger_buf = buffers_.allocate();

    if (value_buf == BufferAllocator::BUFFER_UNUSED ||
        velocity_buf == BufferAllocator::BUFFER_UNUSED ||
        trigger_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Emit PAT_QUERY instruction (queries pattern at block boundaries)
    cedar::Instruction query_inst{};
    query_inst.opcode = cedar::Opcode::PAT_QUERY;
    query_inst.out_buffer = 0xFFFF;  // No direct output
    query_inst.inputs[0] = 0xFFFF;
    query_inst.inputs[1] = 0xFFFF;
    query_inst.inputs[2] = 0xFFFF;
    query_inst.inputs[3] = 0xFFFF;
    query_inst.inputs[4] = 0xFFFF;
    query_inst.state_id = state_id;
    emit(query_inst);

    // Emit PAT_STEP instruction (steps through query results)
    cedar::Instruction step_inst{};
    step_inst.opcode = cedar::Opcode::PAT_STEP;
    step_inst.out_buffer = value_buf;  // value output (freq or sample_id)
    step_inst.inputs[0] = velocity_buf;  // velocity output
    step_inst.inputs[1] = trigger_buf;   // trigger output
    step_inst.inputs[2] = 0xFFFF;
    step_inst.inputs[3] = 0xFFFF;
    step_inst.inputs[4] = 0xFFFF;
    step_inst.state_id = state_id;
    emit(step_inst);

    // Store pattern program initialization data
    StateInitData pat_init;
    pat_init.state_id = state_id;
    pat_init.type = StateInitData::Type::PatternProgram;
    pat_init.cycle_length = cycle_length;
    pat_init.pattern_nodes = compiler.nodes();
    pat_init.is_sample_pattern = is_sample_pattern;
    state_inits_.push_back(std::move(pat_init));

    std::uint16_t result_buf = value_buf;

    // Handle sample patterns - need to wire to SAMPLE_PLAY
    if (is_sample_pattern) {
        std::uint16_t pitch_buf = buffers_.allocate();
        std::uint16_t output_buf = buffers_.allocate();

        if (pitch_buf == BufferAllocator::BUFFER_UNUSED ||
            output_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            pop_path();
            return value_buf;  // Return value buffer as fallback
        }

        // Set pitch to 1.0 for sample playback
        cedar::Instruction pitch_inst{};
        pitch_inst.opcode = cedar::Opcode::PUSH_CONST;
        pitch_inst.out_buffer = pitch_buf;
        pitch_inst.inputs[0] = 0xFFFF;
        pitch_inst.inputs[1] = 0xFFFF;
        pitch_inst.inputs[2] = 0xFFFF;
        pitch_inst.inputs[3] = 0xFFFF;
        encode_const_value(pitch_inst, 1.0f);
        emit(pitch_inst);

        // Wire up sample player
        cedar::Instruction sample_inst{};
        sample_inst.opcode = cedar::Opcode::SAMPLE_PLAY;
        sample_inst.out_buffer = output_buf;
        sample_inst.inputs[0] = trigger_buf;   // trigger
        sample_inst.inputs[1] = pitch_buf;     // pitch
        sample_inst.inputs[2] = value_buf;     // sample_id
        sample_inst.inputs[3] = 0xFFFF;
        sample_inst.state_id = state_id + 1;
        emit(sample_inst);

        result_buf = output_buf;
    } else if (closure_node != NULL_NODE) {
        // Handle closure for pitch patterns
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
        if (param_names.size() >= 3) symbols_->define_variable(param_names[2], value_buf);

        if (body != NULL_NODE) {
            result_buf = visit(body);
        }
    }

    pop_path();
    node_buffers_[node] = result_buf;
    return result_buf;
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
    seq_init.cycle_length = 4.0f * events.cycle_span;  // Scale by pattern's cycle span
    seq_init.times.reserve(events.size());
    seq_init.values.reserve(events.size());
    seq_init.velocities.reserve(events.size());

    for (const auto& event : events.events) {
        seq_init.times.push_back(event.time * 4.0f);  // Convert normalized time to beats

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
    pitch_init.cycle_length = 4.0f * events.cycle_span;  // Scale by pattern's cycle span
    pitch_init.times.reserve(events.size());
    pitch_init.values.reserve(events.size());
    pitch_init.velocities.reserve(events.size());

    for (const auto& event : events.events) {
        pitch_init.times.push_back(event.time * 4.0f);  // Convert normalized time to beats

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

    // Use the new PatternCompiler
    PatternCompiler compiler(ast_->arena, sample_registry_);
    if (!compiler.compile(mini_pattern)) {
        // Fallback to old evaluation
        PatternEventStream events = evaluate_pattern_multi_cycle(mini_pattern, ast_->arena);
        if (events.empty()) {
            std::uint16_t out = emit_zero(buffers_, instructions_);
            if (out == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", loc);
            }
            pop_path();
            return out;
        }

        // Use old SEQ_STEP path for fallback
        bool is_sample = false;
        for (const auto& event : events.events) {
            if (event.type == PatternEventType::Sample) {
                is_sample = true;
                break;
            }
        }

        // Allocate buffers
        std::uint16_t value_buf = buffers_.allocate();
        std::uint16_t velocity_buf = buffers_.allocate();
        std::uint16_t trigger_buf = buffers_.allocate();

        if (value_buf == BufferAllocator::BUFFER_UNUSED ||
            velocity_buf == BufferAllocator::BUFFER_UNUSED ||
            trigger_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", loc);
            pop_path();
            return BufferAllocator::BUFFER_UNUSED;
        }

        cedar::Instruction seq_inst{};
        seq_inst.opcode = cedar::Opcode::SEQ_STEP;
        seq_inst.out_buffer = value_buf;
        seq_inst.inputs[0] = velocity_buf;
        seq_inst.inputs[1] = trigger_buf;
        seq_inst.inputs[2] = 0xFFFF;
        seq_inst.inputs[3] = 0xFFFF;
        seq_inst.state_id = state_id;
        emit(seq_inst);

        StateInitData seq_init;
        seq_init.state_id = state_id;
        seq_init.type = StateInitData::Type::SeqStep;
        seq_init.cycle_length = 4.0f * events.cycle_span;

        for (const auto& event : events.events) {
            seq_init.times.push_back(event.time * 4.0f);
            if (event.type == PatternEventType::Sample) {
                if (!event.sample_name.empty()) {
                    required_samples_.insert(event.sample_name);
                }
                seq_init.sample_names.push_back(event.sample_name);
                std::uint32_t sample_id = sample_registry_ ?
                    sample_registry_->get_id(event.sample_name) : 0;
                seq_init.values.push_back(static_cast<float>(sample_id));
            } else if (event.type == PatternEventType::Pitch) {
                float freq = 440.0f * std::pow(2.0f,
                    (static_cast<float>(event.midi_note) - 69.0f) / 12.0f);
                seq_init.values.push_back(freq);
            } else {
                seq_init.values.push_back(0.0f);
            }
            seq_init.velocities.push_back(event.velocity);
        }
        state_inits_.push_back(std::move(seq_init));

        pop_path();
        return value_buf;
    }

    // Collect required samples
    compiler.collect_samples(required_samples_);

    // Determine cycle count
    PatternEvaluator evaluator(ast_->arena);
    std::uint32_t num_cycles = evaluator.count_cycles(mini_pattern);
    float cycle_length = 4.0f * static_cast<float>(std::max(1u, num_cycles));

    bool is_sample_pattern = compiler.is_sample_pattern();

    // Allocate buffers
    std::uint16_t value_buf = buffers_.allocate();
    std::uint16_t velocity_buf = buffers_.allocate();
    std::uint16_t trigger_buf = buffers_.allocate();

    if (value_buf == BufferAllocator::BUFFER_UNUSED ||
        velocity_buf == BufferAllocator::BUFFER_UNUSED ||
        trigger_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", loc);
        pop_path();
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Emit PAT_QUERY
    cedar::Instruction query_inst{};
    query_inst.opcode = cedar::Opcode::PAT_QUERY;
    query_inst.out_buffer = 0xFFFF;
    query_inst.inputs[0] = 0xFFFF;
    query_inst.inputs[1] = 0xFFFF;
    query_inst.inputs[2] = 0xFFFF;
    query_inst.inputs[3] = 0xFFFF;
    query_inst.inputs[4] = 0xFFFF;
    query_inst.state_id = state_id;
    emit(query_inst);

    // Emit PAT_STEP
    cedar::Instruction step_inst{};
    step_inst.opcode = cedar::Opcode::PAT_STEP;
    step_inst.out_buffer = value_buf;
    step_inst.inputs[0] = velocity_buf;
    step_inst.inputs[1] = trigger_buf;
    step_inst.inputs[2] = 0xFFFF;
    step_inst.inputs[3] = 0xFFFF;
    step_inst.inputs[4] = 0xFFFF;
    step_inst.state_id = state_id;
    emit(step_inst);

    // Store pattern program
    StateInitData pat_init;
    pat_init.state_id = state_id;
    pat_init.type = StateInitData::Type::PatternProgram;
    pat_init.cycle_length = cycle_length;
    pat_init.pattern_nodes = compiler.nodes();
    pat_init.is_sample_pattern = is_sample_pattern;
    state_inits_.push_back(std::move(pat_init));

    pop_path();
    return value_buf;
}

// Handle chord() calls - now uses mini-notation parsing
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

    // Parse using mini-notation parser with sample_only=true
    // This ensures "C7" is treated as chord symbol, not pitch C at octave 7
    auto [pattern_root, diags] = parse_mini(chord_str, const_cast<AstArena&>(ast_->arena),
                                            str_n.location, /*sample_only=*/true);

    // Report any parse errors
    for (const auto& diag : diags) {
        if (diag.severity == Severity::Error) {
            diagnostics_.push_back(diag);
        }
    }

    if (pattern_root == NULL_NODE) {
        error("E127", "Failed to parse chord pattern: \"" + chord_str + "\"", str_n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Evaluate pattern with chord mode enabled, supporting multi-cycle
    PatternEvaluator evaluator(ast_->arena);
    evaluator.set_chord_mode(true);

    // Determine how many cycles this pattern spans
    std::uint32_t num_cycles = evaluator.count_cycles(pattern_root);
    PatternEventStream events;

    if (num_cycles <= 1) {
        events = evaluator.evaluate(pattern_root, 0);
    } else {
        // Multi-cycle evaluation for chord progressions
        for (std::uint32_t cycle = 0; cycle < num_cycles; cycle++) {
            PatternEventStream cycle_events = evaluator.evaluate(pattern_root, cycle);
            for (auto& event : cycle_events.events) {
                event.time += static_cast<float>(cycle);
            }
            events.events.insert(events.events.end(),
                                 cycle_events.events.begin(),
                                 cycle_events.events.end());
        }
        events.cycle_span = static_cast<float>(num_cycles);
        events.sort_by_time();
    }

    if (events.empty()) {
        error("E127", "No valid chords in pattern: \"" + chord_str + "\"", str_n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Count chord events and collect them
    std::vector<const PatternEvent*> chord_events;
    for (const auto& event : events.events) {
        if (event.type == PatternEventType::Chord) {
            chord_events.push_back(&event);
        }
    }

    if (chord_events.empty()) {
        error("E127", "No valid chord symbols found in: \"" + chord_str + "\"", str_n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Single chord - emit as multi-buffer constant
    if (chord_events.size() == 1 && chord_events[0]->chord_data.has_value()) {
        return handle_single_chord(node, n, ChordInfo{
            .root = chord_events[0]->chord_data->root,
            .quality = chord_events[0]->chord_data->quality,
            .root_midi = chord_events[0]->chord_data->root_midi,
            .intervals = chord_events[0]->chord_data->intervals
        }, chord_str);
    }

    // Multiple chords - use mini-notation timing
    return handle_chord_progression_events(node, n, events, chord_str);
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

// Handle chord progression using mini-notation event timing
std::uint16_t CodeGenerator::handle_chord_progression_events(
    NodeIndex node, const Node& n,
    const PatternEventStream& events,
    const std::string& chord_str) {

    // Collect chord events with their timing
    std::vector<std::pair<float, const ChordEventData*>> chord_events;
    for (const auto& event : events.events) {
        if (event.type == PatternEventType::Chord && event.chord_data.has_value()) {
            chord_events.emplace_back(event.time, &(*event.chord_data));
        }
    }

    if (chord_events.empty()) {
        error("E127", "No valid chords in pattern: \"" + chord_str + "\"", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Find max voices across all chords
    std::size_t max_voices = 0;
    for (const auto& [time, chord] : chord_events) {
        max_voices = std::max(max_voices, chord->intervals.size());
    }

    if (max_voices == 0) {
        error("E128", "Chord expansion failed for: \"" + chord_str + "\"", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    push_path("chord#" + std::to_string(call_counters_["chord"]++));

    std::vector<std::uint16_t> voice_buffers;
    voice_buffers.reserve(max_voices);
    float cycle_length = 4.0f * events.cycle_span;  // Scale by pattern's cycle span

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
        voice_init.cycle_length = cycle_length;
        voice_init.times.reserve(chord_events.size());
        voice_init.values.reserve(chord_events.size());
        voice_init.velocities.reserve(chord_events.size());

        for (const auto& [time, chord] : chord_events) {
            // Use timing from mini-notation evaluation
            voice_init.times.push_back(time * 4.0f);  // Convert normalized time to beats

            // Expand chord to MIDI notes
            auto notes = expand_chord(ChordInfo{
                .root = chord->root,
                .quality = chord->quality,
                .root_midi = chord->root_midi,
                .intervals = chord->intervals
            }, 4);

            int midi = 0;
            if (voice < notes.size()) {
                midi = notes[voice];
            } else if (!notes.empty()) {
                // Wrap around for chords with fewer notes
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
