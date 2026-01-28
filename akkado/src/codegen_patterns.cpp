// Pattern and chord codegen implementations
// Extracted from codegen.cpp for maintainability

#include "akkado/codegen.hpp"
#include "akkado/codegen/codegen.hpp"
#include "akkado/chord_parser.hpp"
#include "akkado/pattern_eval.hpp"
#include "akkado/mini_parser.hpp"
#include <cedar/opcodes/sequence.hpp>
#include <cmath>

namespace akkado {

using codegen::encode_const_value;
using codegen::unwrap_argument;
using codegen::emit_zero;

// ============================================================================
// SequenceCompiler - Converts mini-notation AST to Sequence/Event format
// ============================================================================
// This compiles the AST into sequences that can be evaluated at runtime
// using the new simplified query_sequence() function.
//
// Key mappings:
//   [a b c]    -> NORMAL sequence (events at subdivided times)
//   <a b c>    -> ALTERNATE sequence (one event per query, advances step)
//   a | b | c  -> RANDOM sequence (pick one randomly)
//   *N         -> Speed modifier (creates N SUB_SEQ events for alternates)
//   !N         -> Repeat modifier (duplicates events)
//   ?N         -> Chance modifier (sets event.chance)

class SequenceCompiler {
public:
    explicit SequenceCompiler(const AstArena& arena, SampleRegistry* sample_registry = nullptr)
        : arena_(arena), sample_registry_(sample_registry) {}

    // Set base offset for computing pattern-relative source offsets
    void set_pattern_base_offset(std::uint32_t offset) {
        pattern_base_offset_ = offset;
    }

    // Compile a pattern AST into Sequence format
    // Returns true on success, false if compilation fails
    bool compile(NodeIndex root) {
        sequences_.clear();
        sequence_events_.clear();
        sample_mappings_.clear();
        current_seq_idx_ = 0;
        total_events_ = 0;
        if (root == NULL_NODE) return false;

        // Create root sequence at index 0 (query_pattern always starts from sequence 0)
        cedar::Sequence root_seq;
        root_seq.mode = cedar::SequenceMode::NORMAL;
        root_seq.duration = 1.0f;  // Normalized to 1.0, scaled by cycle_length later

        // Reserve slot 0 for root - sub-sequences will be added at indices 1+
        sequences_.push_back(root_seq);
        sequence_events_.push_back({});  // Empty event vector for root

        compile_into_sequence(root, 0, 0.0f, 1.0f);

        if (sequence_events_[0].empty()) return false;

        // Update sequences with pointers to their event vectors and counts
        finalize_sequences();
        return true;
    }

    // Get the compiled sequences (with pointers set up)
    const std::vector<cedar::Sequence>& sequences() const { return sequences_; }

    // Get the event vectors (for storage in StateInitData)
    const std::vector<std::vector<cedar::Event>>& sequence_events() const { return sequence_events_; }

    // Get total event count
    std::uint32_t total_events() const { return total_events_; }

    // Check if pattern contains samples (vs pitch)
    bool is_sample_pattern() const { return is_sample_pattern_; }

    // Register required samples
    void collect_samples(std::set<std::string>& required) const {
        for (const auto& name : sample_names_) {
            required.insert(name);
        }
    }

    // Get sample mappings for deferred resolution
    const std::vector<SequenceSampleMapping>& sample_mappings() const {
        return sample_mappings_;
    }

    // Count top-level elements in a pattern (each element = 1 beat)
    // This determines cycle_length: pattern "a <b c> d" has 3 top-level elements
    std::uint32_t count_top_level_elements(NodeIndex node) {
        if (node == NULL_NODE) return 1;
        const Node& n = arena_[node];

        // For MiniPattern, count children (with repeat expansion)
        if (n.type == NodeType::MiniPattern) {
            std::uint32_t count = 0;
            NodeIndex child = n.first_child;
            while (child != NULL_NODE) {
                count += static_cast<std::uint32_t>(get_node_repeat(child));
                child = arena_[child].next_sibling;
            }
            return count > 0 ? count : 1;
        }

        // Single element
        return 1;
    }

private:
    // Finalize sequences after compilation
    // Sets up the Sequence structs to point to their event vectors
    void finalize_sequences() {
        for (std::size_t i = 0; i < sequences_.size(); ++i) {
            auto& seq = sequences_[i];
            auto& events = sequence_events_[i];
            if (!events.empty()) {
                seq.events = events.data();
                seq.num_events = static_cast<std::uint32_t>(events.size());
                seq.capacity = static_cast<std::uint32_t>(events.size());
                total_events_ += seq.num_events;
            } else {
                seq.events = nullptr;
                seq.num_events = 0;
                seq.capacity = 0;
            }
        }
    }

    // Add event to a sequence by index
    void add_event_to_sequence(std::uint16_t seq_idx, const cedar::Event& e) {
        if (seq_idx < sequence_events_.size()) {
            sequence_events_[seq_idx].push_back(e);
        }
    }

    // Compile a node into events within an existing sequence
    // seq_idx: index of the target sequence
    // time_offset: where in the parent's time span this starts (0.0-1.0)
    // time_span: how much of the parent's time span this uses (0.0-1.0)
    void compile_into_sequence(NodeIndex ast_idx, std::uint16_t seq_idx,
                                float time_offset, float time_span) {
        if (ast_idx == NULL_NODE) return;

        const Node& n = arena_[ast_idx];

        switch (n.type) {
            case NodeType::MiniPattern:
                compile_pattern_node(ast_idx, n, seq_idx, time_offset, time_span);
                break;
            case NodeType::MiniAtom:
                compile_atom_event(n, seq_idx, time_offset, time_span);
                break;
            case NodeType::MiniGroup:
                compile_group_events(n, seq_idx, time_offset, time_span);
                break;
            case NodeType::MiniSequence:
                compile_alternate_sequence(n, seq_idx, time_offset, time_span);
                break;
            case NodeType::MiniPolyrhythm:
                compile_polyrhythm_events(n, seq_idx, time_offset, time_span);
                break;
            case NodeType::MiniPolymeter:
                // Treat polymeter as group for now
                compile_group_events(n, seq_idx, time_offset, time_span);
                break;
            case NodeType::MiniChoice:
                compile_choice_sequence(n, seq_idx, time_offset, time_span);
                break;
            case NodeType::MiniEuclidean:
                compile_euclidean_events(n, seq_idx, time_offset, time_span);
                break;
            case NodeType::MiniModified:
                compile_modified_node(n, seq_idx, time_offset, time_span);
                break;
            default:
                // Unknown node type - skip
                break;
        }
    }

    // MiniPattern: root containing children (sequential concatenation)
    void compile_pattern_node(NodeIndex /*ast_idx*/, const Node& n, std::uint16_t seq_idx,
                               float time_offset, float time_span) {
        // Count children and their weights
        std::vector<NodeIndex> children;
        std::vector<float> weights;
        float total_weight = 0.0f;

        NodeIndex child = n.first_child;
        while (child != NULL_NODE) {
            float weight = get_node_weight(child);
            int repeat = get_node_repeat(child);
            for (int i = 0; i < repeat; ++i) {
                children.push_back(child);
                weights.push_back(weight);
                total_weight += weight;
            }
            child = arena_[child].next_sibling;
        }

        if (children.empty()) return;
        if (total_weight <= 0.0f) total_weight = static_cast<float>(children.size());

        // Subdivide time among children
        float accumulated_time = 0.0f;
        for (std::size_t i = 0; i < children.size(); ++i) {
            float child_span = (weights[i] / total_weight) * time_span;
            float child_offset = time_offset + accumulated_time;
            compile_into_sequence(children[i], seq_idx, child_offset, child_span);
            accumulated_time += child_span;
        }
    }

    // MiniAtom: single note, sample, or rest -> DATA event
    void compile_atom_event(const Node& n, std::uint16_t seq_idx,
                            float time_offset, float time_span) {
        const auto& atom_data = n.as_mini_atom();

        if (atom_data.kind == Node::MiniAtomKind::Rest) {
            return;  // Rest = no event
        }

        cedar::Event e;
        e.type = cedar::EventType::DATA;
        e.time = time_offset;
        e.duration = time_span;
        e.chance = 1.0f;
        e.num_values = 1;
        // Use pattern-relative offset for UI highlighting
        e.source_offset = static_cast<std::uint16_t>(n.location.offset - pattern_base_offset_);
        e.source_length = static_cast<std::uint16_t>(n.location.length);

        if (atom_data.kind == Node::MiniAtomKind::Pitch) {
            // Convert MIDI note to frequency
            float freq = 440.0f * std::pow(2.0f,
                (static_cast<float>(atom_data.midi_note) - 69.0f) / 12.0f);
            e.values[0] = freq;
        } else {
            // Sample
            is_sample_pattern_ = true;
            std::uint32_t sample_id = 0;
            // Always collect sample name for runtime resolution
            if (!atom_data.sample_name.empty()) {
                sample_names_.insert(atom_data.sample_name);
                // Record mapping for deferred resolution in WASM
                // Use current event count as index (before adding)
                std::uint16_t event_idx = static_cast<std::uint16_t>(
                    seq_idx < sequence_events_.size() ? sequence_events_[seq_idx].size() : 0);
                sample_mappings_.push_back(SequenceSampleMapping{
                    seq_idx,
                    event_idx,
                    atom_data.sample_name
                });
            }
            if (sample_registry_ && !atom_data.sample_name.empty()) {
                sample_id = sample_registry_->get_id(atom_data.sample_name);
            }
            e.values[0] = static_cast<float>(sample_id);
        }

        add_event_to_sequence(seq_idx, e);
    }

    // MiniGroup [a b c]: sequential concatenation, subdivide time
    void compile_group_events(const Node& n, std::uint16_t seq_idx,
                               float time_offset, float time_span) {
        // Same logic as compile_pattern_node
        std::vector<NodeIndex> children;
        std::vector<float> weights;
        float total_weight = 0.0f;

        NodeIndex child = n.first_child;
        while (child != NULL_NODE) {
            float weight = get_node_weight(child);
            int repeat = get_node_repeat(child);
            for (int i = 0; i < repeat; ++i) {
                children.push_back(child);
                weights.push_back(weight);
                total_weight += weight;
            }
            child = arena_[child].next_sibling;
        }

        if (children.empty()) return;
        if (total_weight <= 0.0f) total_weight = static_cast<float>(children.size());

        float accumulated_time = 0.0f;
        for (std::size_t i = 0; i < children.size(); ++i) {
            float child_span = (weights[i] / total_weight) * time_span;
            float child_offset = time_offset + accumulated_time;
            compile_into_sequence(children[i], seq_idx, child_offset, child_span);
            accumulated_time += child_span;
        }
    }

    // Create a new sub-sequence and return its index
    std::uint16_t create_sub_sequence(cedar::SequenceMode mode) {
        cedar::Sequence new_seq;
        new_seq.mode = mode;
        new_seq.duration = 1.0f;
        new_seq.events = nullptr;  // Will be set in finalize_sequences
        new_seq.num_events = 0;
        new_seq.capacity = 0;

        std::uint16_t new_idx = static_cast<std::uint16_t>(sequences_.size());
        sequences_.push_back(new_seq);
        sequence_events_.push_back({});  // Add empty event vector
        return new_idx;
    }

    // MiniSequence <a b c>: ALTERNATE mode (one per call, cycles through)
    void compile_alternate_sequence(const Node& n, std::uint16_t parent_seq_idx,
                                     float time_offset, float time_span) {
        // Create a sub-sequence with ALTERNATE mode
        std::uint16_t new_seq_idx = create_sub_sequence(cedar::SequenceMode::ALTERNATE);

        // Track sequence index for sample mappings
        std::uint16_t saved_seq_idx = current_seq_idx_;
        current_seq_idx_ = new_seq_idx;

        // Add each child as a separate event in the alternate sequence
        // Support !N repeat modifier: <a!3 b> becomes 4 choices (a, a, a, b)
        NodeIndex child = n.first_child;
        while (child != NULL_NODE) {
            int repeat = get_node_repeat(child);
            for (int i = 0; i < repeat; ++i) {
                compile_into_sequence(child, new_seq_idx, 0.0f, 1.0f);
            }
            child = arena_[child].next_sibling;
        }

        current_seq_idx_ = saved_seq_idx;

        if (sequence_events_[new_seq_idx].empty()) return;

        // Add a SUB_SEQ event pointing to it
        cedar::Event e;
        e.type = cedar::EventType::SUB_SEQ;
        e.time = time_offset;
        e.duration = time_span;
        e.chance = 1.0f;
        e.seq_id = new_seq_idx;
        add_event_to_sequence(parent_seq_idx, e);
    }

    // MiniChoice a | b | c: RANDOM mode (pick one randomly)
    void compile_choice_sequence(const Node& n, std::uint16_t parent_seq_idx,
                                  float time_offset, float time_span) {
        // Create a sub-sequence with RANDOM mode
        std::uint16_t new_seq_idx = create_sub_sequence(cedar::SequenceMode::RANDOM);

        // Track sequence index for sample mappings
        std::uint16_t saved_seq_idx = current_seq_idx_;
        current_seq_idx_ = new_seq_idx;

        // Add each child as a separate event
        // Support !N repeat modifier: a!3 | b becomes 4 choices (a, a, a, b)
        NodeIndex child = n.first_child;
        while (child != NULL_NODE) {
            int repeat = get_node_repeat(child);
            for (int i = 0; i < repeat; ++i) {
                compile_into_sequence(child, new_seq_idx, 0.0f, 1.0f);
            }
            child = arena_[child].next_sibling;
        }

        current_seq_idx_ = saved_seq_idx;

        if (sequence_events_[new_seq_idx].empty()) return;

        // Add a SUB_SEQ event pointing to it
        cedar::Event e;
        e.type = cedar::EventType::SUB_SEQ;
        e.time = time_offset;
        e.duration = time_span;
        e.chance = 1.0f;
        e.seq_id = new_seq_idx;
        add_event_to_sequence(parent_seq_idx, e);
    }

    // MiniPolyrhythm [a, b, c]: all elements simultaneously
    void compile_polyrhythm_events(const Node& n, std::uint16_t seq_idx,
                                    float time_offset, float time_span) {
        // Each child occupies the same time span
        NodeIndex child = n.first_child;
        while (child != NULL_NODE) {
            compile_into_sequence(child, seq_idx, time_offset, time_span);
            child = arena_[child].next_sibling;
        }
    }

    // MiniEuclidean: Euclidean rhythm pattern
    void compile_euclidean_events(const Node& n, std::uint16_t seq_idx,
                                   float time_offset, float time_span) {
        const auto& euclid_data = n.as_mini_euclidean();
        std::uint32_t hits = euclid_data.hits;
        std::uint32_t steps = euclid_data.steps;
        std::uint32_t rotation = euclid_data.rotation;

        if (steps == 0 || hits == 0) return;

        // Generate Euclidean pattern
        std::uint32_t pattern = compute_euclidean_pattern(hits, steps, rotation);

        // Child element to place on hits
        NodeIndex child = n.first_child;

        float step_span = time_span / static_cast<float>(steps);
        for (std::uint32_t i = 0; i < steps; ++i) {
            if ((pattern >> i) & 1) {
                float step_offset = time_offset + static_cast<float>(i) * step_span;
                if (child != NULL_NODE) {
                    compile_into_sequence(child, seq_idx, step_offset, step_span);
                }
            }
        }
    }

    // Compute Euclidean pattern as bitmask
    std::uint32_t compute_euclidean_pattern(std::uint32_t hits, std::uint32_t steps,
                                             std::uint32_t rotation) {
        if (steps == 0 || hits == 0) return 0;
        if (hits >= steps) return (1u << steps) - 1;

        std::uint32_t pattern = 0;
        float bucket = 0.0f;
        float increment = static_cast<float>(hits) / static_cast<float>(steps);

        for (std::uint32_t i = 0; i < steps; ++i) {
            bucket += increment;
            if (bucket >= 1.0f) {
                pattern |= (1u << i);
                bucket -= 1.0f;
            }
        }

        // Apply rotation
        if (rotation > 0 && steps > 0) {
            rotation = rotation % steps;
            std::uint32_t mask = (1u << steps) - 1;
            pattern = ((pattern >> rotation) | (pattern << (steps - rotation))) & mask;
        }

        return pattern;
    }

    // MiniModified: handle modifiers (*n, !n, ?n, @n)
    void compile_modified_node(const Node& n, std::uint16_t seq_idx,
                                float time_offset, float time_span) {
        const auto& mod_data = n.as_mini_modifier();
        NodeIndex child = n.first_child;

        if (child == NULL_NODE) return;

        switch (mod_data.modifier_type) {
            case Node::MiniModifierType::Speed: {
                // *N: Speed up - creates N events (for alternates) or compresses time
                int count = static_cast<int>(mod_data.value);
                if (count <= 0) count = 1;

                // Check if child is MiniSequence (alternate)
                const Node& child_node = arena_[child];
                if (child_node.type == NodeType::MiniSequence) {
                    // <a b c>*8 -> 8 SUB_SEQ events pointing to ALTERNATE sequence
                    std::uint16_t new_seq_idx = create_sub_sequence(cedar::SequenceMode::ALTERNATE);

                    // Track sequence index for sample mappings
                    std::uint16_t saved_seq_idx = current_seq_idx_;
                    current_seq_idx_ = new_seq_idx;

                    NodeIndex alt_child = child_node.first_child;
                    while (alt_child != NULL_NODE) {
                        compile_into_sequence(alt_child, new_seq_idx, 0.0f, 1.0f);
                        alt_child = arena_[alt_child].next_sibling;
                    }

                    current_seq_idx_ = saved_seq_idx;

                    if (!sequence_events_[new_seq_idx].empty()) {
                        // Create N SUB_SEQ events
                        float event_span = time_span / static_cast<float>(count);
                        for (int i = 0; i < count; ++i) {
                            cedar::Event e;
                            e.type = cedar::EventType::SUB_SEQ;
                            e.time = time_offset + static_cast<float>(i) * event_span;
                            e.duration = event_span;
                            e.chance = 1.0f;
                            e.seq_id = new_seq_idx;
                            add_event_to_sequence(seq_idx, e);
                        }
                    }
                } else {
                    // Regular speed modifier - wrap N fast events in a sub-sequence
                    // so they form ONE element (not N separate elements)
                    std::uint16_t new_seq_idx = create_sub_sequence(cedar::SequenceMode::NORMAL);

                    // Track sequence index for sample mappings
                    std::uint16_t saved_seq_idx = current_seq_idx_;
                    current_seq_idx_ = new_seq_idx;

                    float event_span = 1.0f / static_cast<float>(count);
                    for (int i = 0; i < count; ++i) {
                        float event_offset = static_cast<float>(i) * event_span;
                        compile_into_sequence(child, new_seq_idx, event_offset, event_span);
                    }

                    current_seq_idx_ = saved_seq_idx;

                    if (!sequence_events_[new_seq_idx].empty()) {
                        cedar::Event e;
                        e.type = cedar::EventType::SUB_SEQ;
                        e.time = time_offset;
                        e.duration = time_span;
                        e.chance = 1.0f;
                        e.seq_id = new_seq_idx;
                        add_event_to_sequence(seq_idx, e);
                    }
                }
                break;
            }

            case Node::MiniModifierType::Repeat: {
                // !N: Handled by parent enumeration via get_node_repeat()
                // Just compile the child once with full time span
                compile_into_sequence(child, seq_idx, time_offset, time_span);
                break;
            }

            case Node::MiniModifierType::Chance: {
                // ?N: Chance modifier - wrap in a sequence that applies chance
                // For simplicity, we compile the child and then modify the last event's chance
                std::size_t events_before = sequence_events_[seq_idx].size();
                compile_into_sequence(child, seq_idx, time_offset, time_span);

                // Apply chance to all new events
                float chance = mod_data.value;
                auto& events = sequence_events_[seq_idx];
                for (std::size_t i = events_before; i < events.size(); ++i) {
                    events[i].chance = chance;
                }
                break;
            }

            case Node::MiniModifierType::Slow: {
                // /N: Slow down - just compile with same span (handled at cycle level)
                compile_into_sequence(child, seq_idx, time_offset, time_span);
                break;
            }

            case Node::MiniModifierType::Weight:
            case Node::MiniModifierType::Duration:
                // Weight and Duration are handled by parent (get_node_weight)
                compile_into_sequence(child, seq_idx, time_offset, time_span);
                break;
        }
    }

    // Get the weight (@N) of a node (default 1.0)
    float get_node_weight(NodeIndex node_idx) {
        const Node& n = arena_[node_idx];
        if (n.type == NodeType::MiniModified) {
            const auto& mod = n.as_mini_modifier();
            if (mod.modifier_type == Node::MiniModifierType::Weight) {
                return mod.value;
            }
        }
        return 1.0f;
    }

    // Get the repeat count (!N) of a node (default 1)
    int get_node_repeat(NodeIndex node_idx) {
        const Node& n = arena_[node_idx];
        if (n.type == NodeType::MiniModified) {
            const auto& mod = n.as_mini_modifier();
            if (mod.modifier_type == Node::MiniModifierType::Repeat) {
                return static_cast<int>(mod.value);
            }
        }
        return 1;
    }

    const AstArena& arena_;
    SampleRegistry* sample_registry_ = nullptr;
    std::vector<cedar::Sequence> sequences_;
    std::vector<std::vector<cedar::Event>> sequence_events_;  // Event storage for each sequence
    std::set<std::string> sample_names_;
    std::vector<SequenceSampleMapping> sample_mappings_;
    bool is_sample_pattern_ = false;
    std::uint32_t pattern_base_offset_ = 0;
    std::uint16_t current_seq_idx_ = 0;  // Track current sequence index for sample mappings
    std::uint32_t total_events_ = 0;     // Total event count across all sequences
};

// ============================================================================
// End Compilers
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

    // Use the SequenceCompiler for lazy queryable patterns
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    // Set base offset so event source_offset values are pattern-relative
    const Node& pattern = ast_->arena[pattern_node];
    compiler.set_pattern_base_offset(pattern.location.offset);
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

    // Determine cycle length from top-level element count (each element = 1 beat)
    std::uint32_t num_elements = compiler.count_top_level_elements(pattern_node);
    float cycle_length = static_cast<float>(std::max(1u, num_elements));

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

    // Emit SEQPAT_QUERY instruction (queries pattern at block boundaries)
    cedar::Instruction query_inst{};
    query_inst.opcode = cedar::Opcode::SEQPAT_QUERY;
    query_inst.out_buffer = 0xFFFF;  // No direct output
    query_inst.inputs[0] = 0xFFFF;
    query_inst.inputs[1] = 0xFFFF;
    query_inst.inputs[2] = 0xFFFF;
    query_inst.inputs[3] = 0xFFFF;
    query_inst.inputs[4] = 0xFFFF;
    query_inst.state_id = state_id;
    emit(query_inst);

    // Emit SEQPAT_STEP instruction (steps through query results)
    cedar::Instruction step_inst{};
    step_inst.opcode = cedar::Opcode::SEQPAT_STEP;
    step_inst.out_buffer = value_buf;  // value output (freq or sample_id)
    step_inst.inputs[0] = velocity_buf;  // velocity output
    step_inst.inputs[1] = trigger_buf;   // trigger output
    step_inst.inputs[2] = 0xFFFF;
    step_inst.inputs[3] = 0xFFFF;
    step_inst.inputs[4] = 0xFFFF;
    step_inst.state_id = state_id;
    emit(step_inst);

    // Store sequence program initialization data
    StateInitData seq_init;
    seq_init.state_id = state_id;
    seq_init.type = StateInitData::Type::SequenceProgram;
    seq_init.cycle_length = cycle_length;
    seq_init.sequences = compiler.sequences();
    seq_init.sequence_events = compiler.sequence_events();  // Store event vectors
    seq_init.total_events = compiler.total_events();        // Size hint for arena allocation
    seq_init.is_sample_pattern = is_sample_pattern;
    seq_init.pattern_location = pattern.location;  // Store pattern content location for UI
    seq_init.sequence_sample_mappings = compiler.sample_mappings();  // For deferred sample ID resolution
    state_inits_.push_back(std::move(seq_init));

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

    // Use the SequenceCompiler
    SequenceCompiler compiler(ast_->arena, sample_registry_);
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

    // Determine cycle length from top-level element count (each element = 1 beat)
    std::uint32_t num_elements = compiler.count_top_level_elements(mini_pattern);
    float cycle_length = static_cast<float>(std::max(1u, num_elements));

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

    // Emit SEQPAT_QUERY
    cedar::Instruction query_inst{};
    query_inst.opcode = cedar::Opcode::SEQPAT_QUERY;
    query_inst.out_buffer = 0xFFFF;
    query_inst.inputs[0] = 0xFFFF;
    query_inst.inputs[1] = 0xFFFF;
    query_inst.inputs[2] = 0xFFFF;
    query_inst.inputs[3] = 0xFFFF;
    query_inst.inputs[4] = 0xFFFF;
    query_inst.state_id = state_id;
    emit(query_inst);

    // Emit SEQPAT_STEP
    cedar::Instruction step_inst{};
    step_inst.opcode = cedar::Opcode::SEQPAT_STEP;
    step_inst.out_buffer = value_buf;
    step_inst.inputs[0] = velocity_buf;
    step_inst.inputs[1] = trigger_buf;
    step_inst.inputs[2] = 0xFFFF;
    step_inst.inputs[3] = 0xFFFF;
    step_inst.inputs[4] = 0xFFFF;
    step_inst.state_id = state_id;
    emit(step_inst);

    // Store sequence program
    StateInitData seq_init;
    seq_init.state_id = state_id;
    seq_init.type = StateInitData::Type::SequenceProgram;
    seq_init.cycle_length = cycle_length;
    seq_init.sequences = compiler.sequences();
    seq_init.sequence_events = compiler.sequence_events();  // Store event vectors
    seq_init.total_events = compiler.total_events();        // Size hint for arena allocation
    seq_init.is_sample_pattern = is_sample_pattern;
    seq_init.sequence_sample_mappings = compiler.sample_mappings();  // For deferred sample ID resolution
    state_inits_.push_back(std::move(seq_init));

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
