// SequenceCompiler — mini-notation AST -> cedar::Sequence/Event compilation.
// Internal header for akkado/src/codegen: the class is header-inline (all
// methods defined in-class) because it is instantiated by patterns.cpp and
// pattern_transforms.cpp in addition to pattern_compiler.cpp.
// Extracted verbatim from patterns.cpp (prd-codegen-sprawl-cleanup Phase 3).
#pragma once

#include "akkado/codegen.hpp"
#include "akkado/chord_parser.hpp"
#include "akkado/pattern_eval.hpp"
#include "akkado/mini_parser.hpp"
#include "akkado/tuning.hpp"
#include "akkado/voicing.hpp"
#include <cedar/opcodes/sequence.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace akkado {

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
        : arena_(&arena), sample_registry_(sample_registry) {}

    // PRD Phase 1b: rebind the arena before compiling a mini-AST that lives in
    // a sub-arena (MiniLiteralData::mini_arena or a codegen scratch arena).
    // The sub-AST is self-contained so the traversal stays inside it.
    void set_arena(const AstArena& a) { arena_ = &a; }

    /// Set the tuning context for microtonal Hz resolution
    void set_tuning(const TuningContext& tuning) { tuning_ = tuning; }

    // Phase 2 PRD voicing accumulators. Set by recursive case as it descends
    // through nested anchor/mode/voicing transforms. The top-level voicing
    // handler reads these to apply voice_chords once at the end.
    void set_voicing_anchor(int midi) { voicing_anchor_ = midi; }
    void set_voicing_mode(voicing::Mode m) { voicing_mode_ = m; voicing_mode_explicit_ = true; }
    void set_voicing_dict(std::string name) { voicing_dict_name_ = std::move(name); }

    [[nodiscard]] int voicing_anchor() const { return voicing_anchor_; }
    [[nodiscard]] voicing::Mode voicing_mode() const { return voicing_mode_; }
    [[nodiscard]] bool voicing_mode_explicit() const { return voicing_mode_explicit_; }
    [[nodiscard]] const std::string& voicing_dict_name() const { return voicing_dict_name_; }

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
        chord_contexts_root_.clear();
        current_seq_idx_ = 0;
        total_events_ = 0;
        cycle_length_ = 1.0f;
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

    // Pattern's cycle length in beats. Defaults to 1.0 (one beat per cycle).
    // For top-level MiniPattern, equals the sum of element weights (each
    // top-level element contributes weight beats — Slow modifiers stretch,
    // single-child patterns inherit the child's weight).
    float cycle_length() const { return cycle_length_; }

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

    // Mutable access for transforms that modify sample mappings (bank, n)
    std::vector<SequenceSampleMapping>& mutable_sample_mappings() {
        return sample_mappings_;
    }

    // Phase 2.1 PRD §11: custom property slot map. Populated as
    // unrecognized record-suffix keys are encountered in MiniAtomData.properties
    // and by standalone bend()/aftertouch() transforms. Slot indices are
    // assigned in insertion order, capped at cedar::MAX_PROPS_PER_EVENT.
    const std::unordered_map<std::string, std::uint8_t>& custom_property_slots() const {
        return custom_slots_;
    }
    std::unordered_map<std::string, std::uint8_t>& mutable_custom_slots() {
        return custom_slots_;
    }
    bool custom_slots_overflowed() const { return custom_slots_overflowed_; }
    const std::string& overflow_first_extra_key() const {
        return overflow_first_extra_key_;
    }

    // Allocate or look up a slot for a custom property key. Returns the slot
    // index (0..MAX_PROPS_PER_EVENT-1) on success. Returns -1 if the slot
    // table is full and the key was not previously registered. Also records
    // overflow state so handlers can emit a diagnostic.
    int allocate_property_slot(const std::string& key) {
        auto it = custom_slots_.find(key);
        if (it != custom_slots_.end()) return static_cast<int>(it->second);
        if (custom_slots_.size() >= cedar::MAX_PROPS_PER_EVENT) {
            if (!custom_slots_overflowed_) {
                custom_slots_overflowed_ = true;
                overflow_first_extra_key_ = key;
            }
            return -1;
        }
        std::uint8_t slot = static_cast<std::uint8_t>(custom_slots_.size());
        custom_slots_[key] = slot;
        return static_cast<int>(slot);
    }

    // Get maximum number of values per event (for polyphonic chord support)
    // Returns 1 for monophonic patterns, >1 for patterns with chords
    std::uint8_t max_voices() const {
        std::uint8_t max = 1;
        for (const auto& seq : sequence_events_) {
            for (const auto& e : seq) {
                if (e.num_values > max) max = e.num_values;
            }
        }
        return max;
    }

    // Side-channel: original chord context per event in sequence_events_[0].
    // Populated when a chord MiniAtom is compiled. nullopt for non-chord
    // events. Phase 2 PRD voicing transforms use this to re-voice without
    // losing the original interval data after frequencies are derived.
    const std::vector<std::optional<voicing::ChordSpec>>& chord_contexts_root() const {
        return chord_contexts_root_;
    }
    std::vector<std::optional<voicing::ChordSpec>>& mutable_chord_contexts_root() {
        return chord_contexts_root_;
    }

    // Populate the compiler directly from synthesized events (Phase 2 PRD
    // generators: run/binary/binaryN). Bypasses AST traversal — used when
    // there is no inner pattern node to compile.
    void populate_synthetic(std::vector<cedar::Event> events,
                            cedar::SequenceMode mode = cedar::SequenceMode::NORMAL) {
        sequences_.clear();
        sequence_events_.clear();
        sample_mappings_.clear();
        chord_contexts_root_.clear();
        chord_contexts_root_.resize(events.size());  // all nullopt for synth events
        current_seq_idx_ = 0;
        total_events_ = 0;
        sample_names_.clear();
        is_sample_pattern_ = false;

        cedar::Sequence root_seq;
        root_seq.mode = mode;
        root_seq.duration = 1.0f;
        sequences_.push_back(root_seq);
        sequence_events_.push_back(std::move(events));

        finalize_sequences();
    }

    // Count top-level elements in a pattern (each element = 1 beat)
    // This determines cycle_length: pattern "a <b c> d" has 3 top-level elements
    std::uint32_t count_top_level_elements(NodeIndex node) {
        if (node == NULL_NODE) return 1;
        const Node& n = (*arena_)[node];

        // For MiniPattern, count children (with repeat expansion)
        if (n.type == NodeType::MiniPattern) {
            std::uint32_t count = 0;
            NodeIndex child = n.first_child;
            while (child != NULL_NODE) {
                count += static_cast<std::uint32_t>(get_node_repeat(child));
                child = (*arena_)[child].next_sibling;
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

        // Clock-derived alternation (cedar::query_pattern): compute each
        // sequence's expected queries per pattern-cycle. The root is queried
        // once per cycle; a NORMAL parent forwards its full rate per SUB_SEQ
        // reference (<a b>*4 -> 4); ALTERNATE/RANDOM parents query one child
        // per visit, so each reference carries rate/num_events — the
        // fractional rate gives nested <a <b c>> the Tidal a-b-a-c order.
        // Sub-sequences are created during descent, so SUB_SEQ refs always
        // point to higher indices and one ascending pass is exact.
        std::vector<float> rate(sequences_.size(), 0.0f);
        if (!rate.empty()) rate[0] = 1.0f;
        for (std::size_t i = 0; i < sequences_.size(); ++i) {
            const auto& events = sequence_events_[i];
            if (events.empty()) continue;
            float share = rate[i];
            if (sequences_[i].mode != cedar::SequenceMode::NORMAL) {
                share /= static_cast<float>(events.size());
            }
            for (const auto& e : events) {
                if (e.type == cedar::EventType::SUB_SEQ && e.seq_id < rate.size()) {
                    rate[e.seq_id] += share;
                }
            }
        }
        for (std::size_t i = 0; i < sequences_.size(); ++i) {
            sequences_[i].steps_per_cycle = rate[i];
        }
    }

    // Add event to a sequence by index
    void add_event_to_sequence(std::uint16_t seq_idx, const cedar::Event& e) {
        if (seq_idx < sequence_events_.size()) {
            sequence_events_[seq_idx].push_back(e);
        }
    }

    // Check if a node is "compound" (produces multiple events when compiled)
    // Such nodes need to be wrapped in a sub-sequence when added to ALTERNATE/RANDOM
    bool is_compound_node(NodeIndex idx) const {
        if (idx == NULL_NODE) return false;
        const Node& n = (*arena_)[idx];
        // Unwrap modifiers to check the underlying node
        if (n.type == NodeType::MiniModified) {
            return is_compound_node(n.first_child);
        }
        switch (n.type) {
            case NodeType::MiniGroup:
            case NodeType::MiniPattern:
            case NodeType::MiniPolyrhythm:
            case NodeType::MiniPolymeter:
            case NodeType::MiniEuclidean:
                return true;
            default:
                return false;
        }
    }

    // Compile a child into an ALTERNATE or RANDOM sequence
    // If the child is compound, wrap it in a NORMAL sub-sequence first
    void compile_alternate_child(NodeIndex child, std::uint16_t parent_seq_idx) {
        if (is_compound_node(child)) {
            // Create a NORMAL sub-sequence to hold the compound child
            std::uint16_t sub_seq_idx = create_sub_sequence(cedar::SequenceMode::NORMAL);

            std::uint16_t saved_seq_idx = current_seq_idx_;
            current_seq_idx_ = sub_seq_idx;
            compile_into_sequence(child, sub_seq_idx, 0.0f, 1.0f);
            current_seq_idx_ = saved_seq_idx;

            if (sequence_events_[sub_seq_idx].empty()) return;

            // Add SUB_SEQ event pointing to the wrapped sequence
            cedar::Event e;
            e.type = cedar::EventType::SUB_SEQ;
            e.time = 0.0f;
            e.duration = 1.0f;
            e.chance = 1.0f;
            e.seq_id = sub_seq_idx;
            add_event_to_sequence(parent_seq_idx, e);
        } else {
            // Simple atom - compile directly
            compile_into_sequence(child, parent_seq_idx, 0.0f, 1.0f);
        }
    }

    // Compile a node into events within an existing sequence
    // seq_idx: index of the target sequence
    // time_offset: where in the parent's time span this starts (0.0-1.0)
    // time_span: how much of the parent's time span this uses (0.0-1.0)
    void compile_into_sequence(NodeIndex ast_idx, std::uint16_t seq_idx,
                                float time_offset, float time_span) {
        if (ast_idx == NULL_NODE) return;

        const Node& n = (*arena_)[ast_idx];

        switch (n.type) {
            case NodeType::MiniPattern:
                // Top-level: subdivide [0,1) time among children by per-element
                // weight; cycle_length grows to sum-of-weights. Default unit
                // weight gives one-element-per-beat (the per-cycle-alternation
                // feel from CLAUDE.md). Modifiers like `/N` stretch elements
                // per-element, uniformly with nested groups.
                compile_top_level_pattern(n, seq_idx, time_offset, time_span);
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

    // MiniAtom: single note, sample, chord, or rest -> DATA event
    void compile_atom_event(const Node& n, std::uint16_t seq_idx,
                            float time_offset, float time_span) {
        const auto& atom_data = n.as_mini_atom();

        cedar::Event e;
        e.type = cedar::EventType::DATA;
        e.time = time_offset;
        e.duration = time_span;
        e.chance = 1.0f;
        e.num_values = 1;
        // Use pattern-relative offset for UI highlighting
        e.source_offset = static_cast<std::uint16_t>(n.location.offset - pattern_base_offset_);
        e.source_length = static_cast<std::uint16_t>(n.location.length);

        // Phase 2 PRD §5.5 / Phase 2.1 §11.1: apply record-suffix keys.
        // apply_atom_properties is the single extraction path used by both
        // this scalar/pitched event builder and flatten_to_timelines (the
        // polyrhythm flatten path). Recognized short-form keys (vel/dur)
        // map to fixed cedar::Event fields; unrecognized keys (cutoff,
        // bend, aftertouch, …) map to prop_vals slots.
        AtomPropertiesOut props = apply_atom_properties(atom_data);
        e.velocity = props.velocity;
        e.velocities[0] = props.velocity;
        if (props.duration_mul != 1.0f) e.duration = props.duration_mul * time_span;
        for (std::size_t s = 0; s < cedar::MAX_PROPS_PER_EVENT; ++s) {
            if (props.prop_vals_used & (1u << s)) {
                e.prop_vals[s] = props.prop_vals[s];
                e.prop_set_mask |= static_cast<std::uint8_t>(1u << s);
            }
        }

        if (atom_data.kind == Node::MiniAtomKind::Rest) {
            // Rest: emit event with num_values=0 for UI highlighting (no trigger/sound)
            e.num_values = 0;
            add_event_to_sequence(seq_idx, e);
            if (seq_idx == 0) chord_contexts_root_.emplace_back();
            return;
        }

        std::optional<voicing::ChordSpec> chord_ctx;
        if (atom_data.kind == Node::MiniAtomKind::Value) {
            // PRD prd-patterns-as-scalar-values: v"…" raw numeric atoms.
            // Bypass mtof — the user wrote the literal number they want.
            e.values[0] = atom_data.scalar_value;
        } else if (atom_data.kind == Node::MiniAtomKind::Pitch) {
            // Convert MIDI note + micro_offset to frequency using tuning context
            float freq = tuning_.resolve_hz(atom_data.midi_note, atom_data.micro_offset);
            e.values[0] = freq;
            // Records-and-fields PRD §3.1: %.note returns the MIDI note the
            // user wrote (with microtonal offset). 0 for non-pitch events.
            e.midi_note = static_cast<float>(atom_data.midi_note) +
                          atom_data.micro_offset;
            // Set the per-voice notes[0] so an event_map closure that reads
            // `e.notes[0]` (DynArray view) sees the same MIDI the user wrote.
            // process_event copies this through verbatim; without it the
            // chord-array view would see the default-initialised `0.0f`.
            e.notes[0] = e.midi_note;
        } else if (atom_data.kind == Node::MiniAtomKind::Chord) {
            // Chord symbol: expand intervals to frequencies
            // Root MIDI is at octave 4 by default
            int root_midi = static_cast<int>(atom_data.chord_root_midi);
            // %.note on a chord returns the root MIDI (primary voice).
            e.midi_note = static_cast<float>(root_midi);
            std::size_t num_notes = std::min(atom_data.chord_intervals.size(),
                                              static_cast<std::size_t>(8));  // Max 8 values
            e.num_values = static_cast<std::uint8_t>(num_notes);

            // Capture original chord context for Phase 2 PRD voicing post-pass.
            voicing::ChordSpec spec;
            spec.root_midi = root_midi;
            spec.intervals.reserve(num_notes);
            // Pass the chord quality string ("M", "m7", ...) through so
            // voice_chords() can look up dict.qualities overrides registered
            // via addVoicings().
            spec.quality = atom_data.chord_quality;

            for (std::size_t i = 0; i < num_notes; ++i) {
                int midi = root_midi + static_cast<int>(atom_data.chord_intervals[i]);
                float freq = 440.0f * std::pow(2.0f,
                    (static_cast<float>(midi) - 69.0f) / 12.0f);
                e.values[i] = freq;
                // Per-voice MIDI note — each chord voice has its own pitch.
                e.notes[i] = static_cast<float>(midi);
                spec.intervals.push_back(static_cast<int>(atom_data.chord_intervals[i]));
            }
            chord_ctx = std::move(spec);
        } else {
            // Sample
            is_sample_pattern_ = true;
            // Sample patterns route per-atom velocity through evt.velocities[0]
            // (consumed by op_sample_play per voice). Pin event.velocity to 1.0
            // so the codegen post-MUL on the sampler output is a no-op for
            // bare sample patterns; `velocity(pat, V)` still scales velocity_buf
            // at runtime via the per-builtin MUL.
            e.velocity = 1.0f;
            std::uint32_t sample_id = 0;
            // Always collect sample name for runtime resolution
            if (!atom_data.sample_name.empty()) {
                sample_names_.insert(atom_data.sample_name);
                // Assign type_id for per-type routing (e.g., match e.type { 1: kick, 2: snare })
                e.type_id = get_or_assign_type_id(atom_data.sample_name);
                // Record mapping for deferred resolution in WASM
                // Use current event count as index (before adding)
                std::uint16_t event_idx = static_cast<std::uint16_t>(
                    seq_idx < sequence_events_.size() ? sequence_events_[seq_idx].size() : 0);
                sample_mappings_.push_back(SequenceSampleMapping{
                    seq_idx,
                    event_idx,
                    /*value_slot=*/0,
                    atom_data.sample_name,
                    atom_data.sample_bank,
                    atom_data.sample_variant
                });
            }
            if (sample_registry_ && !atom_data.sample_name.empty()) {
                sample_id = sample_registry_->get_id(atom_data.sample_name);
            }
            e.values[0] = static_cast<float>(sample_id);
        }

        add_event_to_sequence(seq_idx, e);
        if (seq_idx == 0) chord_contexts_root_.push_back(std::move(chord_ctx));
    }

    // MiniGroup [a b c]: sequential concatenation, subdivide time
    void compile_group_events(const Node& n, std::uint16_t seq_idx,
                               float time_offset, float time_span) {
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
            child = (*arena_)[child].next_sibling;
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

    // Top-level MiniPattern: hybrid dispatch.
    //   - All weights == 1 (no Slow / Weight modifiers anywhere at the top
    //     level): use the per-cycle-alternation path (compile_alternate_sequence).
    //     This preserves the runtime ERS contract (`.slow(N)`, `.fast(N)`)
    //     which assumes cycle_length == 1 statically.
    //   - Any weight != 1 (Slow/Weight on a top-level element): subdivide
    //     [0,1) time among children proportionally and set
    //     cycle_length_ = sum_of_weights. Per-element `/N` then stretches
    //     uniformly with nested groups — `<a b c d>/4` → cycle_length 4,
    //     `a [b c]/2 d` → cycle_length 4.
    // Per-element rate semantics are identical in both paths; only the
    // emitted bytecode shape differs.
    void compile_top_level_pattern(const Node& n, std::uint16_t seq_idx,
                                    float time_offset, float time_span) {
        std::vector<NodeIndex> children;
        std::vector<float> weights;
        float total_weight = 0.0f;
        bool any_non_unit = false;

        NodeIndex child = n.first_child;
        while (child != NULL_NODE) {
            float weight = get_node_weight(child);
            int repeat = get_node_repeat(child);
            for (int i = 0; i < repeat; ++i) {
                children.push_back(child);
                weights.push_back(weight);
                total_weight += weight;
                if (weight != 1.0f) any_non_unit = true;
            }
            child = (*arena_)[child].next_sibling;
        }

        if (children.empty()) return;

        if (!any_non_unit) {
            // Uniform weights: legacy per-cycle-alternation path.
            // cycle_length_ stays at the default 1.0 so `.slow(N)` /
            // `.fast(N)` runtime ERS continues to scale from a 1.0 base.
            compile_alternate_sequence(n, seq_idx, time_offset, time_span);
            return;
        }

        if (total_weight <= 0.0f) total_weight = static_cast<float>(children.size());

        // Record cycle_length in beats so handle_mini_literal / handle_chord_call
        // and the pattern-for-transform base cases can pull it out.
        cycle_length_ = total_weight;

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
        // Single-child alternation is degenerate (always picks the same option).
        // Inline directly to match `[X]` semantics so the compiled events are
        // byte-identical and pattern transforms like late()/early() don't
        // double-shift through a needless sub-sequence wrapper.
        if (n.first_child != NULL_NODE &&
            (*arena_)[n.first_child].next_sibling == NULL_NODE &&
            get_node_repeat(n.first_child) == 1) {
            compile_into_sequence(n.first_child, parent_seq_idx, time_offset, time_span);
            return;
        }

        // Create a sub-sequence with ALTERNATE mode
        std::uint16_t new_seq_idx = create_sub_sequence(cedar::SequenceMode::ALTERNATE);

        // Add each child as an event in the alternate sequence
        // Compound children (groups, patterns, etc.) are wrapped in NORMAL sub-sequences
        // so <[a b] [c d]> alternates between groups, not individual elements
        // Support !N repeat modifier: <a!3 b> becomes 4 choices (a, a, a, b)
        NodeIndex child = n.first_child;
        while (child != NULL_NODE) {
            int repeat = get_node_repeat(child);
            for (int i = 0; i < repeat; ++i) {
                compile_alternate_child(child, new_seq_idx);
            }
            child = (*arena_)[child].next_sibling;
        }

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
        // Single-option choice is degenerate (always picks the same option).
        // Inline directly so transforms don't double-shift through the wrapper.
        if (n.first_child != NULL_NODE &&
            (*arena_)[n.first_child].next_sibling == NULL_NODE &&
            get_node_repeat(n.first_child) == 1) {
            compile_into_sequence(n.first_child, parent_seq_idx, time_offset, time_span);
            return;
        }

        // Create a sub-sequence with RANDOM mode
        std::uint16_t new_seq_idx = create_sub_sequence(cedar::SequenceMode::RANDOM);

        // Add each child as an event in the random sequence
        // Compound children (groups, patterns, etc.) are wrapped in NORMAL sub-sequences
        // so [a b] | [c d] picks between groups, not individual elements
        // Support !N repeat modifier: a!3 | b becomes 4 choices (a, a, a, b)
        NodeIndex child = n.first_child;
        while (child != NULL_NODE) {
            int repeat = get_node_repeat(child);
            for (int i = 0; i < repeat; ++i) {
                compile_alternate_child(child, new_seq_idx);
            }
            child = (*arena_)[child].next_sibling;
        }

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

    // ========================================================================
    // MiniPolyrhythm [a, b, c, …]: parallel branches via recursive flatten+merge
    // ========================================================================
    // Each polyrhythm branch is recursively expanded into one or more parallel
    // BranchTimelines. Group / Pattern / Polymeter / Euclidean nodes also widen
    // to N timelines when they enclose a polyrhythm — sequential children get
    // padded so [bd [hh, sn] cp] yields two parallel timelines, sn populated
    // only in the middle third.
    //
    // All timelines are then merged into one sorted DATA-event stream by
    // walking the union of unique trigger times. At each time we emit one
    // multi-voice event whose values[i] = branch i's sample_id at that time
    // (0 means "no trigger on this slot now"). SAMPLE_PLAY already treats
    // values[i] == 0 as "skip this voice slot", so silence-on-empty falls out
    // for free.
    //
    // Cap: branches > MAX_VALUES_PER_EVENT silently truncate — same
    // convention as the previous merge path. Pitched / chord / alternation /
    // random subtrees aren't statically flattenable, so we fall back to the
    // older per-child compile in those cases.

    struct BranchEvent {
        float time;
        float duration;
        float velocity;
        std::uint16_t type_id;
        std::uint16_t source_offset;
        std::uint16_t source_length;
        bool is_rest;
        std::uint32_t sample_id;     // resolved (or 0 if unknown / rest)
        std::string sample_name;     // for sample_mappings_ deferred resolution
        std::string sample_bank;
        std::uint8_t sample_variant;
        // Custom property slots carried through the polyrhythm flatten/merge
        // path (see apply_atom_properties / compile_polyrhythm_events). Width
        // matches cedar::Event::prop_vals so per-voice {cutoff:V, bend:V, ...}
        // suffixes propagate to the merged event.
        std::array<float, cedar::MAX_PROPS_PER_EVENT> prop_vals{};
        // Bitmap of populated slots — bit S set means prop_vals[S] was
        // explicitly assigned by an atom in this branch (bitmap-aware merge
        // preserves explicit `{cutoff:0}` against unset zeros, see PRD §10 E3).
        std::uint32_t prop_vals_used = 0;
    };
    static_assert(cedar::MAX_PROPS_PER_EVENT <= 32,
                  "BranchEvent::prop_vals_used bitmap is uint32_t; widen if "
                  "MAX_PROPS_PER_EVENT exceeds 32.");
    using BranchTimeline = std::vector<BranchEvent>;

    // Output of apply_atom_properties() — single source of truth for
    // {vel, dur, cutoff, bend, ...} extraction from a MiniAtomData.
    struct AtomPropertiesOut {
        float velocity     = 1.0f;
        float duration_mul = 1.0f;  // multiplied by t_span by caller
        std::array<float, cedar::MAX_PROPS_PER_EVENT> prop_vals{};
        std::uint32_t prop_vals_used = 0;
    };

    // Single source of truth for `{vel, dur, cutoff, bend, ...}` on atoms.
    // `compile_atom_event` writes the result to a cedar::Event directly;
    // `flatten_to_timelines` writes it to a BranchEvent and the polyrhythm
    // merge propagates prop_vals onto the merged Event. Slots that overflow
    // cedar::MAX_PROPS_PER_EVENT silently drop (matches today's behavior).
    AtomPropertiesOut apply_atom_properties(const Node::MiniAtomData& ad) {
        AtomPropertiesOut out;
        out.velocity = ad.velocity;
        for (const auto& [key, value] : ad.properties) {
            if (key == "vel") {
                out.velocity = std::clamp(value, 0.0f, 1.0f);
            } else if (key == "dur") {
                if (value > 0.0f) out.duration_mul = value;
            } else {
                int slot = allocate_property_slot(key);
                if (slot >= 0 && slot < static_cast<int>(cedar::MAX_PROPS_PER_EVENT)) {
                    out.prop_vals[static_cast<std::size_t>(slot)] = value;
                    out.prop_vals_used |= (1u << slot);
                }
            }
        }
        return out;
    }

    // True if the subtree contains only sample/rest atoms inside group /
    // polyrhythm / polymeter / euclidean structures (with optional Repeat /
    // Weight modifiers). Pitched atoms, chords, alternation, random, and
    // dynamic modifiers all break flattening.
    bool is_flattenable_sample_subtree(NodeIndex idx) const {
        if (idx == NULL_NODE) return true;
        const Node& n = (*arena_)[idx];
        switch (n.type) {
            case NodeType::MiniAtom: {
                const auto& ad = n.as_mini_atom();
                return ad.kind == Node::MiniAtomKind::Sample ||
                       ad.kind == Node::MiniAtomKind::Rest;
            }
            case NodeType::MiniGroup:
            case NodeType::MiniPattern:
            case NodeType::MiniPolyrhythm:
            case NodeType::MiniPolymeter:
            case NodeType::MiniEuclidean: {
                for (NodeIndex c = n.first_child; c != NULL_NODE;
                     c = (*arena_)[c].next_sibling) {
                    if (!is_flattenable_sample_subtree(c)) return false;
                }
                return true;
            }
            case NodeType::MiniModified: {
                const auto& mod = n.as_mini_modifier();
                if (mod.modifier_type == Node::MiniModifierType::Repeat ||
                    mod.modifier_type == Node::MiniModifierType::Weight) {
                    return is_flattenable_sample_subtree(n.first_child);
                }
                return false;
            }
            default:
                return false;
        }
    }

    // Recursively flatten a subtree into N≥1 parallel BranchTimelines covering
    // [t_offset, t_offset + t_span].
    std::vector<BranchTimeline> flatten_to_timelines(
            NodeIndex idx, float t_offset, float t_span) {
        if (idx == NULL_NODE) return { BranchTimeline{} };
        const Node& n = (*arena_)[idx];

        switch (n.type) {
            case NodeType::MiniPolyrhythm: {
                std::vector<BranchTimeline> all;
                for (NodeIndex c = n.first_child; c != NULL_NODE;
                     c = (*arena_)[c].next_sibling) {
                    auto child_tls = flatten_to_timelines(c, t_offset, t_span);
                    for (auto& tl : child_tls) all.push_back(std::move(tl));
                }
                if (all.empty()) all.push_back({});
                return all;
            }

            case NodeType::MiniGroup:
            case NodeType::MiniPattern:
            case NodeType::MiniPolymeter: {
                std::vector<NodeIndex> children;
                std::vector<float> weights;
                float total_weight = 0.0f;
                for (NodeIndex c = n.first_child; c != NULL_NODE;
                     c = (*arena_)[c].next_sibling) {
                    float w = get_node_weight(c);
                    int repeat = get_node_repeat(c);
                    for (int i = 0; i < repeat; ++i) {
                        children.push_back(c);
                        weights.push_back(w);
                        total_weight += w;
                    }
                }
                if (children.empty()) return { BranchTimeline{} };
                if (total_weight <= 0.0f)
                    total_weight = static_cast<float>(children.size());

                std::vector<std::vector<BranchTimeline>> per_child;
                std::size_t max_width = 1;
                float accumulated = 0.0f;
                for (std::size_t i = 0; i < children.size(); ++i) {
                    float child_span = (weights[i] / total_weight) * t_span;
                    float child_offset = t_offset + accumulated;
                    accumulated += child_span;
                    per_child.push_back(
                        flatten_to_timelines(children[i], child_offset, child_span));
                    max_width = std::max(max_width, per_child.back().size());
                }

                std::vector<BranchTimeline> result(max_width);
                for (auto& child_timelines : per_child) {
                    for (std::size_t ti = 0;
                         ti < child_timelines.size() && ti < max_width; ++ti) {
                        for (auto& ev : child_timelines[ti]) {
                            result[ti].push_back(std::move(ev));
                        }
                    }
                }
                return result;
            }

            case NodeType::MiniEuclidean: {
                const auto& euclid = n.as_mini_euclidean();
                std::uint32_t hits = euclid.hits;
                std::uint32_t steps = euclid.steps;
                std::uint32_t rotation = euclid.rotation;
                if (steps == 0 || hits == 0) return { BranchTimeline{} };
                NodeIndex child = n.first_child;
                if (child == NULL_NODE) return { BranchTimeline{} };

                std::uint32_t pattern = compute_euclidean_pattern(hits, steps, rotation);
                float step_span = t_span / static_cast<float>(steps);

                std::size_t max_width = 1;
                std::vector<std::vector<BranchTimeline>> per_hit;
                std::vector<bool> hit_active(steps, false);
                for (std::uint32_t i = 0; i < steps; ++i) {
                    if (!((pattern >> i) & 1)) continue;
                    hit_active[i] = true;
                    float step_offset = t_offset + static_cast<float>(i) * step_span;
                    per_hit.push_back(
                        flatten_to_timelines(child, step_offset, step_span));
                    max_width = std::max(max_width, per_hit.back().size());
                }

                std::vector<BranchTimeline> result(max_width);
                for (auto& hit_tls : per_hit) {
                    for (std::size_t ti = 0;
                         ti < hit_tls.size() && ti < max_width; ++ti) {
                        for (auto& ev : hit_tls[ti]) {
                            result[ti].push_back(std::move(ev));
                        }
                    }
                }
                return result;
            }

            case NodeType::MiniAtom: {
                const auto& ad = n.as_mini_atom();
                BranchEvent be{};
                be.time = t_offset;
                be.duration = t_span;
                be.source_offset = static_cast<std::uint16_t>(
                    n.location.offset - pattern_base_offset_);
                be.source_length = static_cast<std::uint16_t>(n.location.length);
                // Single property-extraction path — see apply_atom_properties.
                // Custom slots (cutoff, bend, ...) ride on be.prop_vals so the
                // polyrhythm merge can propagate them onto the merged event.
                AtomPropertiesOut props = apply_atom_properties(ad);
                be.velocity = props.velocity;
                if (props.duration_mul != 1.0f) be.duration = props.duration_mul * t_span;
                be.prop_vals = props.prop_vals;
                be.prop_vals_used = props.prop_vals_used;
                if (ad.kind == Node::MiniAtomKind::Rest) {
                    be.is_rest = true;
                } else if (ad.kind == Node::MiniAtomKind::Sample) {
                    be.is_rest = false;
                    be.sample_name = ad.sample_name;
                    be.sample_bank = ad.sample_bank;
                    be.sample_variant = ad.sample_variant;
                    if (!ad.sample_name.empty()) {
                        be.type_id = get_or_assign_type_id(ad.sample_name);
                        if (sample_registry_) {
                            be.sample_id = sample_registry_->get_id(ad.sample_name);
                        }
                    }
                }
                return { BranchTimeline{ be } };
            }

            case NodeType::MiniModified:
                // Repeat / Weight pass through; parents already consume those
                // via get_node_repeat / get_node_weight at this level.
                return flatten_to_timelines(n.first_child, t_offset, t_span);

            default:
                // Caller should have gated us via is_flattenable_sample_subtree.
                return { BranchTimeline{} };
        }
    }

    void compile_polyrhythm_events(const Node& n, std::uint16_t seq_idx,
                                    float time_offset, float time_span) {
        bool flattenable = true;
        for (NodeIndex c = n.first_child; c != NULL_NODE;
             c = (*arena_)[c].next_sibling) {
            if (!is_flattenable_sample_subtree(c)) { flattenable = false; break; }
        }
        if (!flattenable) {
            // Non-sample / dynamic subtree — fall back to per-child compile.
            // The single-stream runtime collapses overlaps to last-event-wins
            // here, but that matches prior behavior for pitched polyrhythms.
            for (NodeIndex c = n.first_child; c != NULL_NODE;
                 c = (*arena_)[c].next_sibling) {
                compile_into_sequence(c, seq_idx, time_offset, time_span);
            }
            return;
        }

        std::vector<BranchTimeline> branches;
        for (NodeIndex c = n.first_child; c != NULL_NODE;
             c = (*arena_)[c].next_sibling) {
            auto child_tls = flatten_to_timelines(c, time_offset, time_span);
            for (auto& tl : child_tls) branches.push_back(std::move(tl));
        }
        if (branches.empty()) return;

        // Cap at the per-event voice limit. Extras truncate silently
        // (matches the previous merge-path behavior).
        std::size_t branch_count = std::min(branches.size(),
            static_cast<std::size_t>(cedar::MAX_VALUES_PER_EVENT));

        // Collect unique trigger times across all retained branches.
        constexpr float TIME_EPS = 1e-6f;
        std::vector<float> times;
        for (std::size_t i = 0; i < branch_count; ++i) {
            for (const auto& ev : branches[i]) {
                if (ev.is_rest) continue;
                bool found = false;
                for (float t : times) {
                    if (std::fabs(t - ev.time) < TIME_EPS) { found = true; break; }
                }
                if (!found) times.push_back(ev.time);
            }
        }
        if (times.empty()) return;
        std::sort(times.begin(), times.end());

        is_sample_pattern_ = true;

        for (float t : times) {
            cedar::Event e;
            e.type = cedar::EventType::DATA;
            e.time = t;
            e.duration = time_span;
            e.chance = 1.0f;
            // Sample-merge polyrhythm: event-wide velocity is unused (op_sample_play
            // applies per-voice velocities[v]). Pinning to 1.0 keeps the codegen
            // post-MUL on the sampler output a no-op unless `velocity(pat, V)`
            // scales velocity_buf at runtime.
            e.velocity = 1.0f;
            e.num_values = static_cast<std::uint8_t>(branch_count);
            for (std::size_t s = 0; s < cedar::MAX_VALUES_PER_EVENT; ++s) {
                e.values[s] = 0.0f;
                e.velocities[s] = 1.0f;
            }

            std::uint16_t event_idx = static_cast<std::uint16_t>(
                seq_idx < sequence_events_.size()
                    ? sequence_events_[seq_idx].size() : 0);

            bool primary_set = false;
            float min_dur = std::numeric_limits<float>::max();
            // Track which prop slots have been populated on the merged event
            // so the bitmap-aware merge takes the *first* branch with each
            // slot set rather than the last (preserves explicit `{cutoff:0}`
            // — see PRD §10 E3).
            std::uint32_t merged_props_used = 0;
            for (std::size_t i = 0; i < branch_count; ++i) {
                const BranchEvent* hit = nullptr;
                for (const auto& ev : branches[i]) {
                    if (!ev.is_rest && std::fabs(ev.time - t) < TIME_EPS) {
                        hit = &ev;
                        break;
                    }
                }
                if (!hit) continue;

                e.values[i] = static_cast<float>(hit->sample_id);
                // Per-voice velocity: each branch's atom velocity (including
                // {vel:V} overrides) lands on its own slot, so
                // [cp, bd{vel:0.05}] plays cp at 1.0 and bd at 0.05.
                e.velocities[i] = hit->velocity;
                min_dur = std::min(min_dur, hit->duration);
                if (!primary_set) {
                    e.type_id = hit->type_id;
                    e.source_offset = hit->source_offset;
                    e.source_length = hit->source_length;
                    primary_set = true;
                }
                // Bitmap-aware prop_vals merge: for each slot set in this
                // branch but not yet on the merged event, copy the value.
                std::uint32_t new_bits = hit->prop_vals_used & ~merged_props_used;
                while (new_bits) {
                    int s = std::countr_zero(new_bits);
                    e.prop_vals[static_cast<std::size_t>(s)] =
                        hit->prop_vals[static_cast<std::size_t>(s)];
                    e.prop_set_mask |= static_cast<std::uint8_t>(1u << s);
                    new_bits &= new_bits - 1;
                }
                merged_props_used |= hit->prop_vals_used;
                if (!hit->sample_name.empty()) {
                    sample_names_.insert(hit->sample_name);
                    sample_mappings_.push_back(SequenceSampleMapping{
                        seq_idx,
                        event_idx,
                        static_cast<std::uint8_t>(i),
                        hit->sample_name,
                        hit->sample_bank,
                        hit->sample_variant
                    });
                }
            }
            if (min_dur < std::numeric_limits<float>::max()) {
                e.duration = min_dur;
            }

            add_event_to_sequence(seq_idx, e);
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
                const Node& child_node = (*arena_)[child];
                if (child_node.type == NodeType::MiniSequence) {
                    // <a b c>*8 -> 8 SUB_SEQ events pointing to ALTERNATE sequence
                    std::uint16_t new_seq_idx = create_sub_sequence(cedar::SequenceMode::ALTERNATE);

                    // Track sequence index for sample mappings
                    std::uint16_t saved_seq_idx = current_seq_idx_;
                    current_seq_idx_ = new_seq_idx;

                    NodeIndex alt_child = child_node.first_child;
                    while (alt_child != NULL_NODE) {
                        int repeat = get_node_repeat(alt_child);
                        for (int i = 0; i < repeat; ++i) {
                            compile_alternate_child(alt_child, new_seq_idx);
                        }
                        alt_child = (*arena_)[alt_child].next_sibling;
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
                // /N: per-element duration. The parent's get_node_weight()
                // already included this Slow factor when allocating
                // time_span, so we just compile the child into the
                // (already-stretched) slot we were handed.
                compile_into_sequence(child, seq_idx, time_offset, time_span);
                break;
            }

            case Node::MiniModifierType::Weight:
                // Weight is handled by parent (get_node_weight)
                compile_into_sequence(child, seq_idx, time_offset, time_span);
                break;
        }
    }

    // Get the weight of a node (default 1.0). Combines:
    //   @N  (Weight)  — explicit weight multiplier
    //   /N  (Slow)    — per-element duration: element occupies N× its slot
    // Speed (*N) does NOT change weight; `*N` plays N copies in the same
    // slot, handled by compile_modified_node. Walks the full Modified
    // chain so `a@2/3` returns weight = 6.
    float get_node_weight(NodeIndex node_idx) {
        float weight = 1.0f;
        NodeIndex cur = node_idx;
        while (cur != NULL_NODE && (*arena_)[cur].type == NodeType::MiniModified) {
            const auto& mod = (*arena_)[cur].as_mini_modifier();
            if (mod.modifier_type == Node::MiniModifierType::Weight) {
                weight *= mod.value;
            } else if (mod.modifier_type == Node::MiniModifierType::Slow) {
                weight *= mod.value;
            }
            cur = (*arena_)[cur].first_child;
        }
        return weight;
    }

    // Get the repeat count (!N) of a node (default 1)
    int get_node_repeat(NodeIndex node_idx) {
        const Node& n = (*arena_)[node_idx];
        if (n.type == NodeType::MiniModified) {
            const auto& mod = n.as_mini_modifier();
            if (mod.modifier_type == Node::MiniModifierType::Repeat) {
                return static_cast<int>(mod.value);
            }
        }
        return 1;
    }

    // Get or assign a type_id for a sample name (for per-type routing)
    std::uint16_t get_or_assign_type_id(const std::string& sample_name) {
        auto it = sample_type_ids_.find(sample_name);
        if (it != sample_type_ids_.end()) {
            return it->second;
        }
        // Assign new type_id (starting from 1, 0 = no type)
        std::uint16_t type_id = next_type_id_++;
        sample_type_ids_[sample_name] = type_id;
        return type_id;
    }

    const AstArena* arena_ = nullptr;
    SampleRegistry* sample_registry_ = nullptr;
    TuningContext tuning_;  // Microtonal tuning context (default: 12-EDO)
    std::vector<cedar::Sequence> sequences_;
    std::vector<std::vector<cedar::Event>> sequence_events_;  // Event storage for each sequence
    std::set<std::string> sample_names_;
    std::vector<SequenceSampleMapping> sample_mappings_;
    std::unordered_map<std::string, std::uint16_t> sample_type_ids_;  // Sample name → type_id
    std::uint16_t next_type_id_ = 1;  // Start at 1, 0 = no type (pitch patterns)
    bool is_sample_pattern_ = false;
    std::uint32_t pattern_base_offset_ = 0;
    std::uint16_t current_seq_idx_ = 0;  // Track current sequence index for sample mappings
    std::uint32_t total_events_ = 0;     // Total event count across all sequences
    float cycle_length_ = 1.0f;          // Beats per pattern cycle (computed at top-level)

    // Phase 2 PRD voicing side-channel: original chord (root_midi, intervals)
    // for each event in sequence_events_[0]. Empty/nullopt for non-chord
    // events. Populated when MiniAtomKind::Chord is compiled; consumed by
    // voicing transforms in apply_voicing_to_compiler().
    std::vector<std::optional<voicing::ChordSpec>> chord_contexts_root_;

    // Phase 2 PRD voicing accumulators. -1 for anchor = "not set" (default
    // c4 / MIDI 60). voicing_mode_explicit_ tracks whether the user set a
    // mode (drives PRD §9.3 default behavior: anchor without mode → below).
    int voicing_anchor_ = -1;
    voicing::Mode voicing_mode_ = voicing::Mode::Below;
    bool voicing_mode_explicit_ = false;
    std::string voicing_dict_name_;

    // Phase 2.1 PRD §11: custom property slot tracking. Populated by
    // record-suffix keys and standalone bend()/aftertouch() transforms.
    std::unordered_map<std::string, std::uint8_t> custom_slots_;
    bool custom_slots_overflowed_ = false;
    std::string overflow_first_extra_key_;
};

// ============================================================================
// End Compilers
// ============================================================================

} // namespace akkado
