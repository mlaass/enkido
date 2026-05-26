// Pattern and chord codegen implementations
// Extracted from codegen.cpp for maintainability

#include "akkado/codegen.hpp"
#include "akkado/codegen/codegen.hpp"
#include "akkado/codegen/options.hpp"
#include "akkado/chord_parser.hpp"
#include "akkado/pattern_eval.hpp"
#include "akkado/mini_parser.hpp"
#include "akkado/pattern_debug.hpp"
#include "akkado/tuning.hpp"
#include "akkado/voicing.hpp"
#include <cedar/opcodes/sequence.hpp>
#include <cedar/opcodes/event_transform_encoding.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace akkado {

using codegen::encode_const_value;
using codegen::unwrap_argument;
using codegen::SamplePatternEmitCtx;
using codegen::emit_sample_chain;

// Project per-event SequenceSampleMappings to a deduped Pattern-level
// sample_refs vector. One entry per (bank, name, variant) tuple — the global
// `required_samples_extended_` ledger does its own dedup across patterns, but
// per-Pattern dedup keeps the sample_refs surface small for any consumer
// that wants to iterate a single pattern's requirements.
static std::vector<RequiredSample> sample_refs_from_mappings(
    const std::vector<SequenceSampleMapping>& mappings) {
    std::vector<RequiredSample> refs;
    std::set<std::string> seen;
    refs.reserve(mappings.size());
    for (const auto& m : mappings) {
        RequiredSample r;
        r.bank = m.bank;
        r.name = m.sample_name;
        r.variant = static_cast<int>(m.variant);
        if (seen.insert(r.key()).second) {
            refs.push_back(std::move(r));
        }
    }
    return refs;
}

// Adjust a string-literal token's SourceLocation so it points at the first
// character *inside* the quotes (and spans only the content). This mirrors the
// adjustment the main parser applies in parse_mini_literal(), and must be used
// by every parse_mini() caller so mini-notation node offsets — and therefore
// pattern step-highlighting in the IDE — are consistent across all forms.
static SourceLocation mini_content_location(const SourceLocation& string_tok) {
    SourceLocation loc = string_tok;
    loc.offset += 1;
    loc.column += 1;
    loc.length = (string_tok.length >= 2) ? string_tok.length - 2 : 0;
    return loc;
}

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
                // Top-level: each element occupies its own cycle (per-cycle alternation).
                // Use [...] for in-cycle subdivision. Deliberate divergence from Strudel.
                compile_alternate_sequence(n, seq_idx, time_offset, time_span);
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
                    int s = __builtin_ctz(new_bits);
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
                // /N: Slow down - just compile with same span (handled at cycle level)
                compile_into_sequence(child, seq_idx, time_offset, time_span);
                break;
            }

            case Node::MiniModifierType::Weight:
                // Weight is handled by parent (get_node_weight)
                compile_into_sequence(child, seq_idx, time_offset, time_span);
                break;
        }
    }

    // Get the weight (@N) of a node (default 1.0)
    float get_node_weight(NodeIndex node_idx) {
        const Node& n = (*arena_)[node_idx];
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

// All SAMPLE_PLAY emission goes through emit_sample_chain, defined in
// akkado/codegen/helpers.hpp. See docs/prd-sample-emission-unification.md.

// Handle MiniLiteral (pattern) nodes
TypedValue CodeGenerator::handle_mini_literal(NodeIndex node, const Node& n) {
    // PRD Phase 1b: MiniLiteralData replaces the legacy StringData(mode) +
    // first-child layout. The parsed mini-AST lives in a per-literal
    // sub-arena referenced via `mini_arena`.
    const auto& lit_data = n.as_mini_literal();
    if (lit_data.mode_marker == "timeline") {
        return handle_timeline_literal(node, n);
    }

    if (!lit_data.mini_arena || lit_data.mini_root == NULL_NODE) {
        error("E114", "Pattern has no parsed content", n.location);
        return TypedValue::void_val();
    }
    const AstArena& mini_arena = *lit_data.mini_arena;
    NodeIndex pattern_node = lit_data.mini_root;

    // PRD prd-remove-pat-builtin §6.3: keep "pat" path segments unchanged
    // to preserve hot-swap semantic-ID hashes across the migration.
    std::uint32_t pat_count = call_counters_["pat"]++;
    push_path("pat#" + std::to_string(pat_count));
    std::uint32_t state_id = compute_state_id();

    // Use the SequenceCompiler for lazy queryable patterns
    SequenceCompiler compiler(mini_arena, sample_registry_);
    // Set base offset so event source_offset values are pattern-relative
    const Node& pattern = mini_arena[pattern_node];
    compiler.set_pattern_base_offset(pattern.location.offset);
    if (!compiler.compile(pattern_node)) {
        // Empty pattern - emit zero
        std::uint16_t out = emit_zero();
        if (out == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
        }
        pop_path();
        return cache_and_return(node, TypedValue::signal(out));
    }

    // Collect required samples
    compiler.collect_samples(required_samples_);

    // cycle_length is per-sequence in beats. Default 1 (cycle = beat).
    float cycle_length = 1.0f;

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
        return TypedValue::void_val();
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

    // Check for polyphonic patterns (chords with multiple values per event)
    std::uint8_t max_voices = compiler.max_voices();

    // Track polyphonic patterns for error reporting (must be consumed by poly())
    if (max_voices > 1 && !is_sample_pattern) {
        polyphonic_pattern_nodes_[node] = {n.location, max_voices, state_id};
    }

    // Emit single-voice SEQPAT_STEP/GATE/TYPE (voice 0 only)
    auto pattern_payload = emit_per_voice_seqpat(node, state_id, max_voices, value_buf, velocity_buf,
                                                  trigger_buf, is_sample_pattern, n.location);
    if (!pattern_payload) {
        pop_path();
        return TypedValue::void_val();
    }

    // Phase 2.1 PRD §11: emit per-key SEQPAT_PROP buffers for custom properties
    // collected by the SequenceCompiler from `c4{cutoff:0.3}`-style suffixes.
    if (!emit_custom_property_buffers(compiler, *pattern_payload, state_id)) {
        pop_path();
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

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
    seq_init.ast_json = serialize_mini_ast_json(pattern_node, mini_arena);  // Serialize AST for debug UI
    state_inits_.push_back(std::move(seq_init));

    pattern_payload->sample_refs = sample_refs_from_mappings(compiler.sample_mappings());
    publish_sample_refs(pattern_payload->sample_refs);

    std::uint16_t result_buf = value_buf;

    // Handle sample patterns - need to wire to SAMPLE_PLAY
    if (is_sample_pattern) {
        SamplePatternEmitCtx ctx;
        ctx.kind = SamplePatternEmitCtx::Kind::Pattern;
        ctx.seq_state_id = state_id;
        ctx.value_buf = value_buf;
        ctx.trigger_buf = trigger_buf;
        ctx.velocity_buf = velocity_buf;
        ctx.loc = n.location;
        std::uint16_t output_buf = emit_sample_chain(
            buffers_, [this](const cedar::Instruction& i){ emit(i); }, ctx);
        if (output_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            pop_path();
            return TypedValue::signal(value_buf);  // Return value buffer as fallback
        }
        result_buf = output_buf;
    }

    pop_path();

    pattern_payload->state_id = state_id;
    pattern_payload->cycle_length = cycle_length;
    return cache_and_return(node, TypedValue::make_pattern(pattern_payload, result_buf));
}


// Emit single-voice SEQPAT_STEP for voice 0 (plus extra SEQPAT_STEPs for
// chord voices), then delegate the extended-field allocation/emission to
// emit_extended_field_buffers() so every pattern producer shares the same
// records-and-field-access PRD §3.1–§3.3 wiring.
std::shared_ptr<PatternPayload> CodeGenerator::emit_per_voice_seqpat(NodeIndex node, std::uint32_t state_id,
                                           std::uint8_t max_voices,
                                           std::uint16_t value_buf, std::uint16_t velocity_buf,
                                           std::uint16_t trigger_buf,
                                           bool is_sample_pattern, SourceLocation loc,
                                           std::uint16_t clock_override) {
    (void)node;
    cedar::Instruction step_inst{};
    step_inst.opcode = cedar::Opcode::SEQPAT_STEP;
    step_inst.out_buffer = value_buf;
    step_inst.inputs[0] = velocity_buf;
    step_inst.inputs[1] = trigger_buf;
    step_inst.inputs[2] = 0;  // voice 0
    step_inst.inputs[3] = clock_override;
    step_inst.inputs[4] = 0xFFFF;
    step_inst.state_id = state_id;
    emit(step_inst);

    // Per-voice freq buffers for chord polyphony. voice_freqs[0] = value_buf
    // (voice 0 already emitted above). Allocate and emit one extra SEQPAT_STEP
    // per chord voice so consumers like soundfont can read every voice's freq.
    std::vector<std::uint16_t> voice_freqs;
    if (max_voices > 1 && !is_sample_pattern) {
        voice_freqs.reserve(max_voices);
        voice_freqs.push_back(value_buf);
        for (std::uint8_t v = 1; v < max_voices; ++v) {
            std::uint16_t v_buf = buffers_.allocate();
            if (v_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", loc);
                return nullptr;
            }
            cedar::Instruction v_step{};
            v_step.opcode = cedar::Opcode::SEQPAT_STEP;
            v_step.out_buffer = v_buf;
            v_step.inputs[0] = 0xFFFF;          // velocity already on voice 0
            v_step.inputs[1] = 0xFFFF;          // trigger already on voice 0
            v_step.inputs[2] = v;               // voice index
            v_step.inputs[3] = clock_override;
            v_step.inputs[4] = 0xFFFF;
            v_step.state_id = state_id;
            emit(v_step);
            voice_freqs.push_back(v_buf);
        }
    }

    auto payload = std::make_shared<PatternPayload>();
    payload->fields[PatternPayload::FREQ] = value_buf;
    payload->fields[PatternPayload::VEL]  = velocity_buf;
    payload->fields[PatternPayload::TRIG] = trigger_buf;
    if (!emit_extended_field_buffers(*payload, state_id, loc, clock_override)) {
        error("E101", "Buffer pool exhausted", loc);
        return nullptr;
    }
    payload->is_sample_pattern = is_sample_pattern;
    payload->max_voices = max_voices;
    payload->voice_freqs = std::move(voice_freqs);
    return payload;
}

// Phase 2.1 PRD §11: emit one SEQPAT_PROP per registered custom-property slot,
// allocate a buffer per slot, and populate payload->custom_fields. The slot
// indices come from SequenceCompiler::custom_property_slots(), which is
// populated by record-suffix keys in MiniAtomData.properties (and by
// standalone bend()/aftertouch() transforms in compile_pattern_for_transform).
bool CodeGenerator::emit_custom_property_buffers(
    const SequenceCompiler& compiler,
    PatternPayload& payload,
    std::uint32_t state_id,
    std::uint16_t clock_override) {
    for (const auto& [key, slot] : compiler.custom_property_slots()) {
        std::uint16_t buf = buffers_.allocate();
        if (buf == BufferAllocator::BUFFER_UNUSED) return false;
        cedar::Instruction inst{};
        inst.opcode = cedar::Opcode::SEQPAT_PROP;
        inst.out_buffer = buf;
        inst.rate = slot;
        inst.inputs[0] = 0;            // voice 0
        inst.inputs[1] = clock_override;
        inst.inputs[2] = 0xFFFF;
        inst.inputs[3] = 0xFFFF;
        inst.inputs[4] = 0xFFFF;
        inst.state_id = state_id;
        emit(inst);
        payload.custom_fields[key] = buf;
        // Phase 3: also record the runtime prop slot so handle_poly_call can
        // plumb this custom field into the per-voice field bank.
        payload.custom_field_slots[key] = slot;
    }
    return true;
}

// Allocate the 8 extended pattern-field buffers and emit SEQPAT_GATE/TYPE/
// FIELD/PHASE for voice 0. SEQPAT_FIELD selectors must match op_seqpat_field
// in cedar/include/cedar/opcodes/sequencing.hpp (0=dur, 1=chance, 2=time,
// 3=note, 4=sample_id). Records-and-field-access PRD §3.1–§3.3.
bool CodeGenerator::emit_extended_field_buffers(
    PatternPayload& payload,
    std::uint32_t state_id,
    SourceLocation loc,
    std::uint16_t clock_override) {
    (void)loc;  // reserved for future per-instruction location plumbing
    std::uint16_t gate_buf      = buffers_.allocate();
    std::uint16_t type_buf      = buffers_.allocate();
    std::uint16_t note_buf      = buffers_.allocate();
    std::uint16_t dur_buf       = buffers_.allocate();
    std::uint16_t chance_buf    = buffers_.allocate();
    std::uint16_t time_buf      = buffers_.allocate();
    std::uint16_t phase_buf     = buffers_.allocate();
    std::uint16_t sample_id_buf = buffers_.allocate();

    if (gate_buf == BufferAllocator::BUFFER_UNUSED ||
        type_buf == BufferAllocator::BUFFER_UNUSED ||
        note_buf == BufferAllocator::BUFFER_UNUSED ||
        dur_buf == BufferAllocator::BUFFER_UNUSED ||
        chance_buf == BufferAllocator::BUFFER_UNUSED ||
        time_buf == BufferAllocator::BUFFER_UNUSED ||
        phase_buf == BufferAllocator::BUFFER_UNUSED ||
        sample_id_buf == BufferAllocator::BUFFER_UNUSED) {
        return false;
    }

    cedar::Instruction gate_inst{};
    gate_inst.opcode = cedar::Opcode::SEQPAT_GATE;
    gate_inst.out_buffer = gate_buf;
    gate_inst.inputs[0] = 0;  // voice 0
    gate_inst.inputs[1] = clock_override;
    gate_inst.inputs[2] = 0xFFFF;
    gate_inst.inputs[3] = 0xFFFF;
    gate_inst.inputs[4] = 0xFFFF;
    gate_inst.state_id = state_id;
    emit(gate_inst);

    cedar::Instruction type_inst{};
    type_inst.opcode = cedar::Opcode::SEQPAT_TYPE;
    type_inst.out_buffer = type_buf;
    type_inst.inputs[0] = 0;  // voice 0
    type_inst.inputs[1] = clock_override;
    type_inst.inputs[2] = 0xFFFF;
    type_inst.inputs[3] = 0xFFFF;
    type_inst.inputs[4] = 0xFFFF;
    type_inst.state_id = state_id;
    emit(type_inst);

    auto emit_field = [&](std::uint16_t out_buf, std::uint8_t selector) {
        cedar::Instruction inst{};
        inst.opcode = cedar::Opcode::SEQPAT_FIELD;
        inst.out_buffer = out_buf;
        inst.rate = selector;
        inst.inputs[0] = 0;  // voice 0
        inst.inputs[1] = clock_override;
        inst.inputs[2] = 0xFFFF;
        inst.inputs[3] = 0xFFFF;
        inst.inputs[4] = 0xFFFF;
        inst.state_id = state_id;
        emit(inst);
    };
    emit_field(dur_buf,       0);
    emit_field(chance_buf,    1);
    emit_field(time_buf,      2);
    emit_field(note_buf,      3);
    emit_field(sample_id_buf, 4);

    cedar::Instruction phase_inst{};
    phase_inst.opcode = cedar::Opcode::SEQPAT_PHASE;
    phase_inst.out_buffer = phase_buf;
    phase_inst.inputs[0] = 0;  // voice 0
    phase_inst.inputs[1] = clock_override;
    phase_inst.inputs[2] = 0xFFFF;
    phase_inst.inputs[3] = 0xFFFF;
    phase_inst.inputs[4] = 0xFFFF;
    phase_inst.state_id = state_id;
    emit(phase_inst);

    payload.fields[PatternPayload::GATE]      = gate_buf;
    payload.fields[PatternPayload::TYPE]      = type_buf;
    payload.fields[PatternPayload::NOTE]      = note_buf;
    payload.fields[PatternPayload::DUR]       = dur_buf;
    payload.fields[PatternPayload::CHANCE]    = chance_buf;
    payload.fields[PatternPayload::TIME]      = time_buf;
    payload.fields[PatternPayload::PHASE]     = phase_buf;
    payload.fields[PatternPayload::SAMPLE_ID] = sample_id_buf;
    return true;
}

// Handle pattern variable reference
TypedValue CodeGenerator::handle_pattern_reference(const std::string& name,
                                                    NodeIndex pattern_node,
                                                    SourceLocation loc) {
    if (pattern_node == NULL_NODE) {
        error("E123", "Pattern variable '" + name + "' has invalid pattern node", loc);
        return TypedValue::void_val();
    }

    const Node& pattern_n = ast_->arena[pattern_node];
    if (pattern_n.type != NodeType::MiniLiteral) {
        // Bindings whose RHS is a transform-on-pattern or a pattern-producer
        // call (e.g. `notes = n"…".slow(2)` or `notes = note("c d")`) arrive
        // here as Call nodes after the analyzer's method-call desugaring.
        // Delegate to the regular visit machinery — the transform / producer
        // handlers (handle_slow_call, handle_note_call, etc.) know how to
        // turn them into the correct sequenced TypedValue. Push the binding
        // name onto the path so internal state_ids stay tied to the name
        // for hot-swap state preservation.
        if (pattern_n.type == NodeType::Call ||
            pattern_n.type == NodeType::MethodCall) {
            push_path(name);
            TypedValue result = visit(pattern_node);
            pop_path();
            return result;
        }
        error("E124", "Pattern variable '" + name + "' does not refer to a pattern", loc);
        return TypedValue::void_val();
    }

    push_path(name);
    std::uint32_t state_id = compute_state_id();

    // PRD Phase 1b: the parsed mini-AST lives in MiniLiteralData's sub-arena.
    const auto& lit_data = pattern_n.as_mini_literal();
    if (!lit_data.mini_arena || lit_data.mini_root == NULL_NODE) {
        error("E114", "Pattern has no parsed content", loc);
        pop_path();
        return TypedValue::void_val();
    }
    const AstArena& mini_arena = *lit_data.mini_arena;
    NodeIndex mini_pattern = lit_data.mini_root;

    // Use the SequenceCompiler
    SequenceCompiler compiler(mini_arena, sample_registry_);
    if (!compiler.compile(mini_pattern)) {
        // Empty pattern - emit zero
        std::uint16_t out = emit_zero();
        if (out == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", loc);
        }
        pop_path();
        return TypedValue::signal(out);
    }

    // Collect required samples
    compiler.collect_samples(required_samples_);

    // cycle_length is per-sequence in beats. Default 1 (cycle = beat).
    float cycle_length = 1.0f;

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
        return TypedValue::void_val();
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

    // Emit single-voice SEQPAT_STEP/GATE/TYPE
    std::uint8_t max_voices = compiler.max_voices();

    // Track polyphonic patterns for error reporting (must be consumed by poly())
    if (max_voices > 1 && !is_sample_pattern) {
        polyphonic_pattern_nodes_[pattern_node] = {loc, max_voices, state_id};
    }

    auto pattern_payload = emit_per_voice_seqpat(pattern_node, state_id, max_voices, value_buf, velocity_buf,
                                                  trigger_buf, is_sample_pattern, loc);
    if (!pattern_payload) {
        pop_path();
        return TypedValue::void_val();
    }

    // Phase 2.1 PRD §11: emit per-key SEQPAT_PROP buffers for custom properties.
    if (!emit_custom_property_buffers(compiler, *pattern_payload, state_id)) {
        pop_path();
        error("E101", "Buffer pool exhausted", loc);
        return TypedValue::void_val();
    }

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

    pattern_payload->sample_refs = sample_refs_from_mappings(compiler.sample_mappings());
    publish_sample_refs(pattern_payload->sample_refs);

    // Wire up SAMPLE_PLAY for sample patterns. Without this the returned
    // buffer would be raw sample-IDs (DC), not audio.
    std::uint16_t result_buf = value_buf;
    if (is_sample_pattern) {
        SamplePatternEmitCtx ctx;
        ctx.kind = SamplePatternEmitCtx::Kind::Pattern;
        ctx.seq_state_id = state_id;
        ctx.value_buf = value_buf;
        ctx.trigger_buf = trigger_buf;
        ctx.velocity_buf = velocity_buf;
        ctx.loc = loc;
        std::uint16_t output_buf = emit_sample_chain(
            buffers_, [this](const cedar::Instruction& i){ emit(i); }, ctx);
        if (output_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", loc);
            pop_path();
            return TypedValue::void_val();
        }
        result_buf = output_buf;
    }

    pop_path();
    pattern_payload->state_id = state_id;
    pattern_payload->cycle_length = cycle_length;
    return cache_and_return(pattern_node, TypedValue::make_pattern(pattern_payload, result_buf));
}

// Handle chord() calls - uses SEQPAT system via SequenceCompiler
TypedValue CodeGenerator::handle_chord_call(NodeIndex node, const Node& n) {
    NodeIndex arg = n.first_child;
    if (arg == NULL_NODE) {
        error("E125", "chord() requires exactly 1 argument", n.location);
        return TypedValue::void_val();
    }

    const Node& arg_node = ast_->arena[arg];
    NodeIndex str_node = (arg_node.type == NodeType::Argument) ? arg_node.first_child : arg;

    if (str_node == NULL_NODE) {
        error("E125", "chord() requires a string argument", n.location);
        return TypedValue::void_val();
    }

    const Node& str_n = ast_->arena[str_node];
    if (str_n.type != NodeType::StringLit) {
        error("E126", "chord() argument must be a string literal (e.g., \"Am\", \"C7 F G\")",
              str_n.location);
        return TypedValue::void_val();
    }

    std::string chord_str = str_n.as_string();

    // PRD Phase 1b: parse into a codegen-owned scratch arena instead of
    // mutating ast_->arena via const_cast. The scratch arena lives until
    // CodeGenerator is destroyed, anchoring SequenceCompiler's traversal.
    auto scratch = std::make_shared<AstArena>();
    auto [pattern_root, diags] = parse_mini(chord_str, *scratch,
                                            mini_content_location(str_n.location),
                                            /*sample_only=*/false);
    codegen_mini_arenas_.push_back(scratch);

    // Report any parse errors
    for (const auto& diag : diags) {
        if (diag.severity == Severity::Error) {
            diagnostics_.push_back(diag);
        }
    }

    if (pattern_root == NULL_NODE) {
        error("E127", "Failed to parse chord pattern: \"" + chord_str + "\"", str_n.location);
        return TypedValue::void_val();
    }

    // Use SequenceCompiler to compile the chord pattern (same as pat())
    std::uint32_t chord_count = call_counters_["chord"]++;
    push_path("chord#" + std::to_string(chord_count));
    std::uint32_t state_id = compute_state_id();

    SequenceCompiler compiler(*scratch, sample_registry_);
    const Node& pattern = (*scratch)[pattern_root];
    compiler.set_pattern_base_offset(pattern.location.offset);

    if (!compiler.compile(pattern_root)) {
        error("E127", "Failed to compile chord pattern: \"" + chord_str + "\"", str_n.location);
        pop_path();
        return TypedValue::void_val();
    }

    // cycle_length is per-sequence in beats. Default 1 (cycle = beat).
    float cycle_length = 1.0f;

    // Allocate buffers for outputs
    std::uint16_t value_buf = buffers_.allocate();
    std::uint16_t velocity_buf = buffers_.allocate();
    std::uint16_t trigger_buf = buffers_.allocate();

    if (value_buf == BufferAllocator::BUFFER_UNUSED ||
        velocity_buf == BufferAllocator::BUFFER_UNUSED ||
        trigger_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return TypedValue::void_val();
    }

    // Emit SEQPAT_QUERY instruction
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

    // Check for polyphonic patterns (chords with multiple values per event)
    std::uint8_t max_voices = compiler.max_voices();

    // Track polyphonic patterns for error reporting. Consumers that handle
    // chord polyphony natively (e.g. soundfont) erase this entry; mono synth
    // chains still produce E410.
    if (max_voices > 1) {
        polyphonic_pattern_nodes_[node] = {n.location, max_voices, state_id};
    }

    // Emit per-voice SEQPAT_STEP plus the voice-0 GATE/TYPE/FIELD/PHASE block.
    // emit_per_voice_seqpat populates payload->voice_freqs when max_voices > 1
    // so consumers can iterate every chord voice.
    auto payload = emit_per_voice_seqpat(node, state_id, max_voices, value_buf,
                                          velocity_buf, trigger_buf,
                                          /*is_sample_pattern=*/false, n.location);
    if (!payload) {
        pop_path();
        return TypedValue::void_val();
    }

    // Store sequence program initialization data
    StateInitData seq_init;
    seq_init.state_id = state_id;
    seq_init.type = StateInitData::Type::SequenceProgram;
    seq_init.cycle_length = cycle_length;
    seq_init.sequences = compiler.sequences();
    seq_init.sequence_events = compiler.sequence_events();
    seq_init.total_events = compiler.total_events();
    seq_init.is_sample_pattern = false;
    seq_init.pattern_location = pattern.location;
    seq_init.sequence_sample_mappings = compiler.sample_mappings();
    state_inits_.push_back(std::move(seq_init));

    payload->state_id = state_id;
    payload->cycle_length = cycle_length;

    // Phase 2.1 PRD §11: chord patterns can carry record-suffix properties too
    // (e.g. `chord("Am{velmod:0.5}")`). Surface them via SEQPAT_PROP.
    if (!emit_custom_property_buffers(compiler, *payload, state_id)) {
        pop_path();
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    payload->sample_refs = sample_refs_from_mappings(compiler.sample_mappings());
    publish_sample_refs(payload->sample_refs);

    pop_path();
    return cache_and_return(node, TypedValue::make_pattern(payload, value_buf));
}

// PRD prd-patterns-as-scalar-values §5.6: explicit Pattern→Signal cast.
// scalar(p) returns p.freq for monophonic non-sample patterns; idempotent
// on a Signal arg. Sample / polyphonic patterns error E161.
TypedValue CodeGenerator::handle_scalar_call(NodeIndex node, const Node& n) {
    NodeIndex arg = n.first_child;
    if (arg == NULL_NODE) {
        error("E125", "scalar() takes exactly one argument", n.location);
        return TypedValue::error_val();
    }
    const Node& arg_n = ast_->arena[arg];
    NodeIndex inner = (arg_n.type == NodeType::Argument) ? arg_n.first_child : arg;
    if (inner == NULL_NODE) {
        error("E125", "scalar() takes exactly one argument", n.location);
        return TypedValue::error_val();
    }

    TypedValue tv = visit(inner);
    if (tv.error) return tv;

    // Idempotent on Signal/Number — return as-is so scalar(scalar(p)) is safe.
    if (tv.type == ValueType::Signal || tv.type == ValueType::Number) {
        return cache_and_return(node, tv);
    }

    if (tv.type != ValueType::Pattern || !tv.pattern) {
        error("E161", std::string("scalar() expects a Pattern or Signal, got ") +
                          value_type_name(tv.type),
              n.location);
        return TypedValue::error_val();
    }
    if (tv.pattern->is_sample_pattern) {
        error("E161",
              "scalar() cannot cast a sample pattern; pick a field explicitly (e.g. p.type)",
              n.location);
        return TypedValue::error_val();
    }
    if (tv.pattern->max_voices > 1) {
        error("E161",
              "scalar() cannot cast a polyphonic pattern; use poly() or pick a voice explicitly",
              n.location);
        return TypedValue::error_val();
    }
    std::uint16_t buf = tv.pattern->fields[PatternPayload::FREQ];
    if (buf == 0xFFFF) {
        error("E161", "scalar() pattern has no value buffer", n.location);
        return TypedValue::error_val();
    }
    return cache_and_return(node, TypedValue::signal(buf));
}

// ============================================================================
// Pattern transformation handlers
// ============================================================================

// Helper: Get pattern argument from a function call
// Returns the MiniLiteral node or NULL_NODE if not a valid pattern
static NodeIndex get_pattern_arg(const Ast& ast, const Node& n, std::size_t arg_index) {
    NodeIndex arg = n.first_child;
    std::size_t idx = 0;
    while (arg != NULL_NODE && idx < arg_index) {
        arg = ast.arena[arg].next_sibling;
        idx++;
    }
    if (arg == NULL_NODE) return NULL_NODE;

    // Unwrap Argument node if present
    const Node& arg_node = ast.arena[arg];
    if (arg_node.type == NodeType::Argument) {
        arg = arg_node.first_child;
    }

    return arg;
}

// Helper: Get numeric argument from a function call
// Returns the value or default if not a valid number
static std::optional<float> get_number_arg(const Ast& ast, const Node& n, std::size_t arg_index) {
    NodeIndex arg = get_pattern_arg(ast, n, arg_index);
    if (arg == NULL_NODE) return std::nullopt;

    const Node& arg_node = ast.arena[arg];
    if (arg_node.type == NodeType::NumberLit) {
        return static_cast<float>(arg_node.as_number());
    }
    return std::nullopt;
}

// Helper: Get string argument from a function call
// Returns the string value or nullopt if not a valid string
static std::optional<std::string> get_string_arg(const Ast& ast, const Node& n, std::size_t arg_index) {
    NodeIndex arg = get_pattern_arg(ast, n, arg_index);
    if (arg == NULL_NODE) return std::nullopt;

    const Node& arg_node = ast.arena[arg];
    if (arg_node.type == NodeType::StringLit) {
        return arg_node.as_string();
    }
    return std::nullopt;
}

// Helper: Check if a Call node calls a known pattern-producing function.
// Does not check MiniLiteral or StringLit (those should be handled separately).
static bool is_pattern_call(const Node& n) {
    if (n.type != NodeType::Call) return false;
    const std::string& func_name = n.as_identifier();
    return func_name == "timeline" ||
           func_name == "chord" ||
           func_name == "slow" || func_name == "fast" ||
           func_name == "rev" || func_name == "transpose" || func_name == "velocity" ||
           func_name == "bank" || func_name == "variant" || func_name == "transport" ||
           func_name == "tune" ||
           // Phase 2 PRD time/structure transforms
           func_name == "early" || func_name == "late" ||
           func_name == "palindrome" || func_name == "compress" ||
           func_name == "ply" || func_name == "linger" ||
           func_name == "zoom" || func_name == "segment" ||
           func_name == "swing" || func_name == "swingBy" ||
           func_name == "iter" || func_name == "iterBack" ||
           // Phase 2 PRD generators (constructors)
           func_name == "run" || func_name == "binary" || func_name == "binaryN" ||
           // Phase 2 PRD voicing transforms (anchor / mode / voicing only;
           // addVoicings is a registry call, not a pattern producer)
           func_name == "anchor" || func_name == "mode" || func_name == "voicing" ||
           // Phase 2.1 PRD §11.2: standalone note-property transforms
           func_name == "bend" || func_name == "aftertouch" || func_name == "dur";
}

// Helper: Check if a node is a pattern-producing expression.
// Uses symbol table for Identifier nodes (type-based), AST checks for literals/calls.
static bool is_pattern_node(const Ast& ast, const SymbolTable& symbols, NodeIndex node) {
    if (node == NULL_NODE) return false;

    const Node& n = ast.arena[node];

    if (n.type == NodeType::MiniLiteral) return true;
    if (n.type == NodeType::StringLit) return true;
    if (is_pattern_call(n)) return true;

    // Identifier: check symbol table for Pattern kind
    if (n.type == NodeType::Identifier) {
        auto sym = symbols.lookup(n.as_identifier());
        return sym && sym->kind == SymbolKind::Pattern;
    }

    return false;
}

// Helpers: synthesize events for Phase 2 PRD generators (run/binary/binaryN).
// Each returns the event count for cycle_length sizing; caller stores events
// either directly into the compiler or into out_events for transform recursion.
static std::vector<cedar::Event> synth_run_events(int n) {
    std::vector<cedar::Event> events;
    if (n <= 0) return events;
    events.reserve(static_cast<std::size_t>(n));
    float step = 1.0f / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
        cedar::Event e;
        e.type = cedar::EventType::DATA;
        e.time = static_cast<float>(i) * step;
        e.duration = step;
        e.num_values = 1;
        e.values[0] = static_cast<float>(i);
        e.velocity = 1.0f;
        e.chance = 1.0f;
        events.push_back(e);
    }
    return events;
}

static std::vector<cedar::Event> synth_binary_events(std::uint32_t n, int bits) {
    // Emit `bits` events MSB-first; set bits emit triggers (num_values=1,
    // value=1.0), unset bits emit rests (num_values=0). Per PRD §5.3.
    std::vector<cedar::Event> events;
    if (bits <= 0) return events;
    events.reserve(static_cast<std::size_t>(bits));
    float step = 1.0f / static_cast<float>(bits);
    for (int i = 0; i < bits; ++i) {
        int bit_pos = bits - 1 - i;  // MSB first
        bool is_set = (n >> bit_pos) & 1u;
        cedar::Event e;
        e.type = cedar::EventType::DATA;
        e.time = static_cast<float>(i) * step;
        e.duration = step;
        e.velocity = 1.0f;
        e.chance = 1.0f;
        if (is_set) {
            e.num_values = 1;
            e.values[0] = 1.0f;
        } else {
            e.num_values = 0;  // rest
        }
        events.push_back(e);
    }
    return events;
}

static int compute_binary_bits(std::uint32_t n) {
    if (n == 0) return 1;
    int bits = 0;
    std::uint32_t v = n;
    while (v > 0) { ++bits; v >>= 1; }
    return bits;
}

// Helper: Compile a pattern and return the compiled data
// Returns true on success. On success, populates out_* parameters.
// out_events and out_cycle_length carry already-transformed events from inner transforms.
// PRD Phase 1b: `out_arena` reports which AST arena `out_pattern_node` lives
// in — MiniLiteral leaves come from a sub-arena (MiniLiteralData::mini_arena),
// codegen-time re-parses from a codegen scratch arena, and transform/generator
// branches still point into the main analyzer AST arena.
static bool compile_pattern_for_transform(
    CodeGenerator& gen,
    const Ast& ast,
    NodeIndex pattern_arg,
    SampleRegistry* sample_registry,
    SequenceCompiler& compiler,
    NodeIndex& out_pattern_node,
    const AstArena*& out_arena,
    std::uint32_t& out_num_elements,
    std::vector<std::vector<cedar::Event>>& out_events,
    float& out_cycle_length) {

    const Node& pat_node = ast.arena[pattern_arg];

    // Case 1: MiniLiteral (base case)
    if (pat_node.type == NodeType::MiniLiteral) {
        // PRD Phase 1b: the parsed mini-AST lives in MiniLiteralData's
        // sub-arena, not as a first-child of the MiniLiteral node.
        const auto& lit_data = pat_node.as_mini_literal();
        if (!lit_data.mini_arena || lit_data.mini_root == NULL_NODE) {
            return false;
        }
        const AstArena& mini_arena = *lit_data.mini_arena;
        out_pattern_node = lit_data.mini_root;
        out_arena = &mini_arena;

        const Node& pattern = mini_arena[out_pattern_node];
        compiler.set_arena(mini_arena);
        compiler.set_pattern_base_offset(pattern.location.offset);

        if (!compiler.compile(out_pattern_node)) {
            return false;
        }

        out_num_elements = compiler.count_top_level_elements(out_pattern_node);
        out_events = compiler.sequence_events();
        out_cycle_length = 1.0f;  // cycle_length in beats; default 1 (cycle = beat)
        return true;
    }

    // Case 1b: StringLit — parse as mini-notation into a codegen scratch arena.
    // PRD Phase 1b: replaces the legacy const_cast<AstArena&>(ast.arena).
    if (pat_node.type == NodeType::StringLit) {
        std::string pattern_str = pat_node.as_string();
        AstArena& scratch = gen.acquire_mini_scratch_arena();
        auto [pattern_root, diags] = parse_mini(pattern_str, scratch,
            mini_content_location(pat_node.location), false);
        if (pattern_root == NULL_NODE) return false;

        out_pattern_node = pattern_root;
        out_arena = &scratch;
        const Node& pattern = scratch[out_pattern_node];
        compiler.set_arena(scratch);
        compiler.set_pattern_base_offset(pattern.location.offset);

        if (!compiler.compile(out_pattern_node)) return false;

        out_num_elements = compiler.count_top_level_elements(out_pattern_node);
        out_events = compiler.sequence_events();
        out_cycle_length = 1.0f;  // cycle_length in beats; default 1 (cycle = beat)
        return true;
    }

    // Case 1c: Identifier bound to a Pattern — recurse on the bound AST node.
    // The analyzer records the originating MiniLiteral / pat-call node in
    // PatternInfo.pattern_node when it sees `name = n"…"` or `name = pat(…)`,
    // so we follow the binding back to its source. Without this case,
    // `melody = n"…"; transpose(melody, …)` (or its pipe form `melody |>
    // transpose(@, …)`) fails E130 even though `is_pattern_node()` accepts
    // the identifier as a valid pattern argument.
    if (pat_node.type == NodeType::Identifier) {
        auto sym = gen.symbols().lookup(pat_node.as_identifier());
        if (!sym || sym->kind != SymbolKind::Pattern) return false;
        NodeIndex bound = sym->pattern.pattern_node;
        if (bound == NULL_NODE || bound == pattern_arg) return false;
        return compile_pattern_for_transform(gen, ast, bound, sample_registry,
                                             compiler, out_pattern_node, out_arena,
                                             out_num_elements,
                                             out_events, out_cycle_length);
    }

    // Handle Call nodes
    if (pat_node.type == NodeType::Call) {
        const std::string& func_name = pat_node.as_identifier();

        // Case 2.4: chord(...) base case — parse chord string and compile
        // through the same mini-notation pipeline used by handle_chord_call.
        // PRD Phase 1b: parse into a codegen scratch arena.
        if (func_name == "chord") {
            NodeIndex first_arg = pat_node.first_child;
            if (first_arg == NULL_NODE) return false;
            const Node& arg_node = ast.arena[first_arg];
            NodeIndex str_node = (arg_node.type == NodeType::Argument)
                ? arg_node.first_child : first_arg;
            if (str_node == NULL_NODE) return false;
            const Node& str_n = ast.arena[str_node];
            if (str_n.type != NodeType::StringLit) return false;
            std::string chord_str = str_n.as_string();
            AstArena& scratch = gen.acquire_mini_scratch_arena();
            auto [pattern_root, diags] = parse_mini(chord_str, scratch,
                mini_content_location(str_n.location), /*sample_only=*/false);
            if (pattern_root == NULL_NODE) return false;
            out_pattern_node = pattern_root;
            out_arena = &scratch;
            const Node& pattern = scratch[out_pattern_node];
            compiler.set_arena(scratch);
            compiler.set_pattern_base_offset(pattern.location.offset);
            if (!compiler.compile(out_pattern_node)) return false;
            out_num_elements = compiler.count_top_level_elements(out_pattern_node);
            out_events = compiler.sequence_events();
            out_cycle_length = 1.0f;  // cycle_length in beats; default 1 (cycle = beat)
            return true;
        }

        // Case 2.5: Generator constructors (run/binary/binaryN) — synthesize
        // events directly into the compiler and seed out_* like a base case.
        if (func_name == "run") {
            auto n_arg = get_number_arg(ast, pat_node, 0);
            if (!n_arg.has_value() || *n_arg < 0) return false;
            int n_int = static_cast<int>(*n_arg);
            auto events = synth_run_events(n_int);
            compiler.populate_synthetic(std::move(events));
            out_pattern_node = pat_node.first_child;
            out_arena = &ast.arena;
            out_num_elements = static_cast<std::uint32_t>(std::max(1, n_int));
            out_events = compiler.sequence_events();
            out_cycle_length = 1.0f;  // cycle_length in beats; default 1 (cycle = beat)
            return true;
        }
        if (func_name == "binary") {
            auto n_arg = get_number_arg(ast, pat_node, 0);
            if (!n_arg.has_value() || *n_arg < 0) return false;
            std::uint32_t n_val = static_cast<std::uint32_t>(*n_arg);
            int bits = compute_binary_bits(n_val);
            auto events = synth_binary_events(n_val, bits);
            compiler.populate_synthetic(std::move(events));
            out_pattern_node = pat_node.first_child;
            out_arena = &ast.arena;
            out_num_elements = static_cast<std::uint32_t>(std::max(1, bits));
            out_events = compiler.sequence_events();
            out_cycle_length = 1.0f;  // cycle_length in beats; default 1 (cycle = beat)
            return true;
        }
        if (func_name == "binaryN") {
            auto n_arg = get_number_arg(ast, pat_node, 0);
            auto bits_arg = get_number_arg(ast, pat_node, 1);
            if (!n_arg.has_value() || !bits_arg.has_value()) return false;
            if (*n_arg < 0 || *bits_arg < 0) return false;
            int bits = static_cast<int>(*bits_arg);
            if (bits < 0) return false;
            std::uint32_t n_val = static_cast<std::uint32_t>(*n_arg);
            // Truncate to lower `bits` bits per PRD §9.2.
            if (bits < 32) {
                n_val &= (1u << bits) - 1u;
            }
            auto events = synth_binary_events(n_val, bits);
            compiler.populate_synthetic(std::move(events));
            out_pattern_node = pat_node.first_child;
            out_arena = &ast.arena;
            out_num_elements = static_cast<std::uint32_t>(std::max(1, bits));
            out_events = compiler.sequence_events();
            out_cycle_length = 1.0f;  // cycle_length in beats; default 1 (cycle = beat)
            return true;
        }

        // Case 3: Transform calls (recursive case)
        bool is_transform = (func_name == "slow" || func_name == "fast" ||
                             func_name == "rev" || func_name == "transpose" ||
                             func_name == "velocity" || func_name == "bank" ||
                             func_name == "variant" || func_name == "tune" ||
                             // Phase 2 PRD time/structure transforms
                             func_name == "early" || func_name == "late" ||
                             func_name == "palindrome" || func_name == "compress" ||
                             func_name == "ply" || func_name == "linger" ||
                             func_name == "zoom" || func_name == "segment" ||
                             func_name == "swing" || func_name == "swingBy" ||
                             func_name == "iter" || func_name == "iterBack" ||
                             // Phase 2 PRD voicing transforms (state-only;
                             // top-level handler applies voice_chords)
                             func_name == "anchor" || func_name == "mode" ||
                             func_name == "voicing" ||
                             // Phase 2.1 PRD §11.2: note-property transforms
                             func_name == "bend" || func_name == "aftertouch" ||
                             func_name == "dur");
        if (is_transform) {
            // tune() sets context BEFORE compilation (not post-processing)
            if (func_name == "tune") {
                auto tuning_name = get_string_arg(ast, pat_node, 0);
                NodeIndex inner_arg = get_pattern_arg(ast, pat_node, 1);
                if (!tuning_name.has_value() || inner_arg == NULL_NODE) return false;

                auto tuning = parse_tuning(*tuning_name);
                if (!tuning.has_value()) return false;

                compiler.set_tuning(*tuning);
                return compile_pattern_for_transform(gen, ast, inner_arg, sample_registry,
                                                      compiler, out_pattern_node, out_arena,
                                                      out_num_elements,
                                                      out_events, out_cycle_length);
            }

            // Get the inner pattern argument (first arg of this transform call)
            NodeIndex inner_arg = get_pattern_arg(ast, pat_node, 0);
            if (inner_arg == NULL_NODE) return false;

            // Recursively compile the inner pattern
            if (!compile_pattern_for_transform(gen, ast, inner_arg, sample_registry,
                                                compiler, out_pattern_node, out_arena,
                                                out_num_elements,
                                                out_events, out_cycle_length)) {
                return false;
            }

            // Apply this transform to the already-compiled events
            if (func_name == "slow") {
                auto factor = get_number_arg(ast, pat_node, 1);
                if (!factor.has_value() || *factor <= 0) return false;
                // Only scale cycle_length — event times are normalized [0,1)
                out_cycle_length *= *factor;
            } else if (func_name == "fast") {
                auto factor = get_number_arg(ast, pat_node, 1);
                if (!factor.has_value() || *factor <= 0) return false;
                // Only scale cycle_length — event times are normalized [0,1)
                out_cycle_length /= *factor;
            } else if (func_name == "rev") {
                for (auto& seq_events : out_events) {
                    for (auto& event : seq_events) {
                        float new_time = 1.0f - event.time - event.duration;
                        if (new_time < 0.0f) new_time = 0.0f;
                        event.time = new_time;
                    }
                }
            } else if (func_name == "transpose") {
                auto semitones = get_number_arg(ast, pat_node, 1);
                if (!semitones.has_value()) return false;
                float ratio = std::pow(2.0f, *semitones / 12.0f);
                const float st = static_cast<float>(*semitones);
                if (!compiler.is_sample_pattern()) {
                    for (auto& seq_events : out_events) {
                        for (auto& event : seq_events) {
                            // Shift frequencies *and* the MIDI note fields so
                            // %.note / per-voice .note track the transposition.
                            if (event.type == cedar::EventType::DATA &&
                                event.num_values > 0) {
                                for (std::uint8_t i = 0; i < event.num_values; ++i) {
                                    event.values[i] *= ratio;
                                    event.notes[i] += st;
                                }
                                event.midi_note += st;
                            }
                        }
                    }
                }
            } else if (func_name == "velocity") {
                auto vel = get_number_arg(ast, pat_node, 1);
                if (!vel.has_value() || *vel < 0 || *vel > 1) return false;
                for (auto& seq_events : out_events) {
                    for (auto& event : seq_events) {
                        event.velocity *= *vel;
                    }
                }
            } else if (func_name == "bend" || func_name == "aftertouch") {
                // Phase 2.1 PRD §11.2: standalone bend/aftertouch transforms.
                // Reserve a slot keyed by the transform name (so users can
                // access via e.bend / e.aftertouch after pipe-binding) and
                // write the value to event.prop_vals[slot] on every event.
                auto value = get_number_arg(ast, pat_node, 1);
                if (!value.has_value()) return false;
                int slot = compiler.allocate_property_slot(func_name);
                if (slot < 0) return false;  // overflow — drop the transform
                for (auto& seq_events : out_events) {
                    for (auto& event : seq_events) {
                        event.prop_vals[slot] = *value;
                        event.prop_set_mask |=
                            static_cast<std::uint8_t>(1u << slot);
                    }
                }
            } else if (func_name == "dur") {
                // Phase 2.1 PRD §11.2: pure compile-time duration multiplier.
                // event.duration already exists on cedar::Event, so no slot
                // plumbing needed.
                auto factor = get_number_arg(ast, pat_node, 1);
                if (!factor.has_value() || *factor <= 0) return false;
                for (auto& seq_events : out_events) {
                    for (auto& event : seq_events) {
                        event.duration *= *factor;
                    }
                }
            } else if (func_name == "bank") {
                auto bank_name = get_string_arg(ast, pat_node, 1);
                if (!bank_name.has_value()) return false;
                for (auto& mapping : compiler.mutable_sample_mappings()) {
                    mapping.bank = *bank_name;
                }
            } else if (func_name == "variant") {
                auto variant = get_number_arg(ast, pat_node, 1);
                if (!variant.has_value() || *variant < 0) return false;
                for (auto& mapping : compiler.mutable_sample_mappings()) {
                    mapping.variant = static_cast<std::uint8_t>(*variant);
                }
            } else if (func_name == "early") {
                // early(pat, n): t' = (t - n + 1) mod 1; wraps within [0,1).
                // Only shift root sequence events. Sub-seq events have local
                // time relative to their parent slot; shifting them double-applies
                // when the parent NORMAL evaluator re-adds e.time.
                auto amount = get_number_arg(ast, pat_node, 1);
                if (!amount.has_value()) return false;
                float a = std::fmod(*amount, 1.0f);
                if (a < 0.0f) a += 1.0f;
                if (!out_events.empty()) {
                    auto& seq_events = out_events[0];
                    for (auto& event : seq_events) {
                        float t = event.time - a;
                        t = std::fmod(t, 1.0f);
                        if (t < 0.0f) t += 1.0f;
                        event.time = t;
                    }
                    std::sort(seq_events.begin(), seq_events.end(),
                        [](const cedar::Event& x, const cedar::Event& y) {
                            return x.time < y.time;
                        });
                }
            } else if (func_name == "late") {
                // late(pat, n): t' = (t + n) mod 1; wraps within [0,1).
                // Only shift root sequence events (see early() above for why).
                auto amount = get_number_arg(ast, pat_node, 1);
                if (!amount.has_value()) return false;
                float a = std::fmod(*amount, 1.0f);
                if (a < 0.0f) a += 1.0f;
                if (!out_events.empty()) {
                    auto& seq_events = out_events[0];
                    for (auto& event : seq_events) {
                        float t = event.time + a;
                        t = std::fmod(t, 1.0f);
                        if (t < 0.0f) t += 1.0f;
                        event.time = t;
                    }
                    std::sort(seq_events.begin(), seq_events.end(),
                        [](const cedar::Event& x, const cedar::Event& y) {
                            return x.time < y.time;
                        });
                }
            } else if (func_name == "palindrome") {
                // palindrome(pat): forward in [0, 0.5), reversed in [0.5, 1);
                // doubles cycle_length so the full forward+reverse takes 2x.
                for (auto& seq_events : out_events) {
                    std::vector<cedar::Event> doubled;
                    doubled.reserve(seq_events.size() * 2);
                    for (const auto& event : seq_events) {
                        cedar::Event e = event;
                        e.time = event.time * 0.5f;
                        e.duration = event.duration * 0.5f;
                        doubled.push_back(e);
                    }
                    for (const auto& event : seq_events) {
                        cedar::Event e = event;
                        e.time = 1.0f - 0.5f * event.time - 0.5f * event.duration;
                        e.duration = event.duration * 0.5f;
                        if (e.time < 0.0f) e.time = 0.0f;
                        doubled.push_back(e);
                    }
                    std::sort(doubled.begin(), doubled.end(),
                        [](const cedar::Event& x, const cedar::Event& y) {
                            return x.time < y.time;
                        });
                    seq_events = std::move(doubled);
                }
                out_cycle_length *= 2.0f;
            } else if (func_name == "compress") {
                // compress(pat, s, e): squash all events into [s, e) of cycle.
                auto start_arg = get_number_arg(ast, pat_node, 1);
                auto end_arg = get_number_arg(ast, pat_node, 2);
                if (!start_arg.has_value() || !end_arg.has_value()) return false;
                float s = *start_arg;
                float e = *end_arg;
                if (e <= s) return false;
                float width = e - s;
                for (auto& seq_events : out_events) {
                    for (auto& event : seq_events) {
                        event.time = s + event.time * width;
                        event.duration *= width;
                    }
                }
            } else if (func_name == "ply") {
                // ply(pat, n): replace each event of duration d at time t with n
                // events of duration d/n at times t, t+d/n, ..., t+(n-1)*d/n.
                auto n_arg = get_number_arg(ast, pat_node, 1);
                if (!n_arg.has_value() || *n_arg < 1) return false;
                int n = static_cast<int>(*n_arg);
                if (n < 1) return false;
                for (auto& seq_events : out_events) {
                    std::vector<cedar::Event> plied;
                    plied.reserve(seq_events.size() * static_cast<std::size_t>(n));
                    for (const auto& ev : seq_events) {
                        float sub_d = ev.duration / static_cast<float>(n);
                        for (int i = 0; i < n; ++i) {
                            cedar::Event pe = ev;
                            pe.duration = sub_d;
                            pe.time = ev.time + static_cast<float>(i) * sub_d;
                            plied.push_back(pe);
                        }
                    }
                    seq_events = std::move(plied);
                }
            } else if (func_name == "linger") {
                // linger(pat, frac): equivalent to zoom(0, frac).fast(1/frac).
                // Drops events with time >= frac, scales remaining by 1/frac so
                // they fill [0, 1), then divides cycle_length by 1/frac (= *frac)
                // so the truncated portion repeats 1/frac times per logical cycle.
                auto frac_arg = get_number_arg(ast, pat_node, 1);
                if (!frac_arg.has_value() || *frac_arg <= 0) return false;
                float frac = *frac_arg;
                if (frac < 1.0f) {
                    for (auto& seq_events : out_events) {
                        std::vector<cedar::Event> kept;
                        for (const auto& ev : seq_events) {
                            if (ev.time < frac) {
                                cedar::Event ke = ev;
                                ke.time = ev.time / frac;
                                ke.duration = ev.duration / frac;
                                if (ke.time >= 1.0f) continue;
                                if (ke.time + ke.duration > 1.0f) {
                                    ke.duration = 1.0f - ke.time;
                                }
                                kept.push_back(ke);
                            }
                        }
                        seq_events = std::move(kept);
                    }
                    out_cycle_length *= frac;
                }
                // frac >= 1: no-op (keeps full pattern)
            } else if (func_name == "zoom") {
                // zoom(pat, s, e): keep events overlapping [s, e), remap to [0, 1).
                auto start_arg = get_number_arg(ast, pat_node, 1);
                auto end_arg = get_number_arg(ast, pat_node, 2);
                if (!start_arg.has_value() || !end_arg.has_value()) return false;
                float s = *start_arg;
                float e_val = *end_arg;
                if (e_val <= s) return false;
                float width = e_val - s;
                for (auto& seq_events : out_events) {
                    std::vector<cedar::Event> kept;
                    for (const auto& ev : seq_events) {
                        float event_end = ev.time + ev.duration;
                        if (event_end <= s || ev.time >= e_val) continue;
                        cedar::Event ke = ev;
                        float clipped_start = std::max(ev.time, s);
                        float clipped_end = std::min(event_end, e_val);
                        ke.time = (clipped_start - s) / width;
                        ke.duration = (clipped_end - clipped_start) / width;
                        kept.push_back(ke);
                    }
                    seq_events = std::move(kept);
                }
            } else if (func_name == "iter" || func_name == "iterBack") {
                // iter/iterBack configure runtime rotation state, not events.
                // In nested context (recursive case), inner pattern is already
                // compiled. The outer transform's handler will emit state
                // without iter set, so iter/iterBack are only effective when
                // applied at the outermost transform level. Documented in §5.2.
            } else if (func_name == "anchor") {
                // Voicing transform — accumulate state only. The outermost
                // voicing transform's handler calls apply_voicing.
                auto anchor_str = get_string_arg(ast, pat_node, 1);
                if (anchor_str.has_value()) {
                    auto midi = voicing::parse_anchor(*anchor_str);
                    if (midi.has_value()) compiler.set_voicing_anchor(*midi);
                }
            } else if (func_name == "mode") {
                auto mode_str = get_string_arg(ast, pat_node, 1);
                if (mode_str.has_value()) {
                    auto m = voicing::parse_mode(*mode_str);
                    if (m.has_value()) compiler.set_voicing_mode(*m);
                }
            } else if (func_name == "voicing") {
                auto name_str = get_string_arg(ast, pat_node, 1);
                if (name_str.has_value()) compiler.set_voicing_dict(*name_str);
            } else if (func_name == "swing" || func_name == "swingBy") {
                // swing(pat, n=4) ≡ swingBy(pat, 1/3, n=4).
                // swingBy(pat, amount, n=4): divide cycle into n slices; events
                // whose offset within their slice is >= 0.5 get time +=
                // amount * (1/(2n)).
                float amount;
                int n_slices;
                if (func_name == "swing") {
                    amount = 1.0f / 3.0f;
                    auto n_arg = get_number_arg(ast, pat_node, 1);
                    n_slices = n_arg.has_value() ? static_cast<int>(*n_arg) : 4;
                } else {
                    auto amt_arg = get_number_arg(ast, pat_node, 1);
                    if (!amt_arg.has_value()) return false;
                    amount = *amt_arg;
                    auto n_arg = get_number_arg(ast, pat_node, 2);
                    n_slices = n_arg.has_value() ? static_cast<int>(*n_arg) : 4;
                }
                if (n_slices < 1) return false;
                float slice_w = 1.0f / static_cast<float>(n_slices);
                float shift = amount * (slice_w * 0.5f);
                for (auto& seq_events : out_events) {
                    for (auto& event : seq_events) {
                        float slice_pos = event.time / slice_w;
                        float frac = slice_pos - std::floor(slice_pos);
                        if (frac >= 0.5f) {
                            event.time += shift;
                            if (event.time >= 1.0f) event.time -= 1.0f;
                        }
                    }
                    std::sort(seq_events.begin(), seq_events.end(),
                        [](const cedar::Event& x, const cedar::Event& y) {
                            return x.time < y.time;
                        });
                }
            } else if (func_name == "segment") {
                // segment(pat, n): sample at n evenly-spaced points; emit one
                // event per sample carrying the active event's value.
                auto n_arg = get_number_arg(ast, pat_node, 1);
                if (!n_arg.has_value() || *n_arg < 1) return false;
                int n = static_cast<int>(*n_arg);
                if (n < 1) return false;
                float step = 1.0f / static_cast<float>(n);
                for (auto& seq_events : out_events) {
                    if (seq_events.empty()) continue;
                    std::sort(seq_events.begin(), seq_events.end(),
                        [](const cedar::Event& x, const cedar::Event& y) {
                            return x.time < y.time;
                        });
                    std::vector<cedar::Event> sampled;
                    sampled.reserve(static_cast<std::size_t>(n));
                    for (int i = 0; i < n; ++i) {
                        float st = static_cast<float>(i) * step;
                        const cedar::Event* active = nullptr;
                        for (const auto& ev : seq_events) {
                            if (ev.time <= st && (ev.time + ev.duration) > st) {
                                active = &ev;
                            }
                        }
                        if (active == nullptr) {
                            // Fall back to last event whose start is <= st;
                            // else first event in the list.
                            for (const auto& ev : seq_events) {
                                if (ev.time <= st) active = &ev;
                            }
                            if (active == nullptr) active = &seq_events.front();
                        }
                        cedar::Event se = *active;
                        se.time = st;
                        se.duration = step;
                        sampled.push_back(se);
                    }
                    seq_events = std::move(sampled);
                }
            }
            return true;
        }

        // Case 2: pat/seq/timeline calls (base case — unwrap to MiniLiteral)
        NodeIndex first_arg = pat_node.first_child;
        if (first_arg != NULL_NODE) {
            const Node& arg_node = ast.arena[first_arg];
            NodeIndex actual_arg = first_arg;

            // Unwrap Argument node
            if (arg_node.type == NodeType::Argument) {
                actual_arg = arg_node.first_child;
            }

            if (actual_arg != NULL_NODE) {
                const Node& actual_node = ast.arena[actual_arg];
                if (actual_node.type == NodeType::MiniLiteral) {
                    // PRD Phase 1b: mini-AST lives in MiniLiteralData's sub-arena.
                    const auto& lit_data = actual_node.as_mini_literal();
                    if (lit_data.mini_arena && lit_data.mini_root != NULL_NODE) {
                        const AstArena& mini_arena = *lit_data.mini_arena;
                        out_pattern_node = lit_data.mini_root;
                        out_arena = &mini_arena;
                        const Node& pattern = mini_arena[out_pattern_node];
                        compiler.set_arena(mini_arena);
                        compiler.set_pattern_base_offset(pattern.location.offset);

                        if (compiler.compile(out_pattern_node)) {
                            out_num_elements = compiler.count_top_level_elements(out_pattern_node);
                            out_events = compiler.sequence_events();
                            out_cycle_length = 1.0f;  // cycle_length in beats; default 1 (cycle = beat)
                            return true;
                        }
                    }
                }
            }
        }
    }

    return false;
}

// Helper: Emit a compiled pattern with transformations applied
// This is the common code for emitting SEQPAT_QUERY/SEQPAT_STEP
static TypedValue emit_pattern_with_state(
    CodeGenerator& gen,
    BufferAllocator& buffers,
    std::vector<StateInitData>& state_inits,
    std::set<std::string>& required_samples,
    std::unordered_map<NodeIndex, TypedValue>& node_types,
    NodeIndex node,
    std::uint32_t state_id,
    float cycle_length,
    const SequenceCompiler& compiler,
    std::vector<std::vector<cedar::Event>>& sequence_events,
    const SourceLocation& pattern_loc,
    const SourceLocation& call_loc) {

    // Collect required samples
    compiler.collect_samples(required_samples);

    bool is_sample_pattern = compiler.is_sample_pattern();

    // Allocate buffers for outputs
    std::uint16_t value_buf = buffers.allocate();
    std::uint16_t velocity_buf = buffers.allocate();
    std::uint16_t trigger_buf = buffers.allocate();

    if (value_buf == BufferAllocator::BUFFER_UNUSED ||
        velocity_buf == BufferAllocator::BUFFER_UNUSED ||
        trigger_buf == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::void_val();
    }

    // Emit SEQPAT_QUERY instruction
    cedar::Instruction query_inst{};
    query_inst.opcode = cedar::Opcode::SEQPAT_QUERY;
    query_inst.out_buffer = 0xFFFF;
    query_inst.inputs[0] = 0xFFFF;
    query_inst.inputs[1] = 0xFFFF;
    query_inst.inputs[2] = 0xFFFF;
    query_inst.inputs[3] = 0xFFFF;
    query_inst.inputs[4] = 0xFFFF;
    query_inst.state_id = state_id;
    gen.emit(query_inst);

    // Check for polyphonic patterns. Compute from the local (possibly
    // post-voicing) sequence_events rather than compiler.max_voices(), since
    // apply_voicing rewrites events.num_values on the local copy without
    // touching the compiler's internal state. Reading from compiler would
    // give stale parser-time voice counts (e.g. 3 for a CM triad) even when
    // voicing has expanded the chord to 5 voices.
    std::uint8_t max_voices = 1;
    for (const auto& seq : sequence_events) {
        for (const auto& e : seq) {
            if (e.num_values > max_voices) max_voices = e.num_values;
        }
    }
    std::vector<std::uint16_t> voice_buffers;

    // Emit SEQPAT_STEP for each voice
    for (std::uint8_t voice = 0; voice < max_voices; ++voice) {
        std::uint16_t voice_value_buf = (voice == 0) ? value_buf : buffers.allocate();
        if (voice_value_buf == BufferAllocator::BUFFER_UNUSED) {
            return TypedValue::void_val();
        }

        cedar::Instruction step_inst{};
        step_inst.opcode = cedar::Opcode::SEQPAT_STEP;
        step_inst.out_buffer = voice_value_buf;
        step_inst.inputs[0] = (voice == 0) ? velocity_buf : 0xFFFF;
        step_inst.inputs[1] = (voice == 0) ? trigger_buf : 0xFFFF;
        step_inst.inputs[2] = voice;
        step_inst.inputs[3] = 0xFFFF;
        step_inst.inputs[4] = 0xFFFF;
        step_inst.state_id = state_id;
        gen.emit(step_inst);

        voice_buffers.push_back(voice_value_buf);
    }

    // Store sequence program initialization data
    StateInitData seq_init;
    seq_init.state_id = state_id;
    seq_init.type = StateInitData::Type::SequenceProgram;
    seq_init.cycle_length = cycle_length;
    seq_init.sequences = compiler.sequences();
    seq_init.sequence_events = std::move(sequence_events);
    seq_init.total_events = compiler.total_events();
    seq_init.is_sample_pattern = is_sample_pattern;
    seq_init.pattern_location = pattern_loc;
    seq_init.sequence_sample_mappings = compiler.sample_mappings();
    state_inits.push_back(std::move(seq_init));

    std::uint16_t result_buf = value_buf;

    // All SAMPLE_PLAY emission goes through emit_sample_chain. See
    // docs/prd-sample-emission-unification.md.
    if (is_sample_pattern) {
        SamplePatternEmitCtx ctx;
        ctx.kind = SamplePatternEmitCtx::Kind::Pattern;
        ctx.seq_state_id = state_id;
        ctx.value_buf = value_buf;
        ctx.trigger_buf = trigger_buf;
        ctx.velocity_buf = velocity_buf;
        ctx.loc = call_loc;
        std::uint16_t output_buf = emit_sample_chain(
            buffers,
            [&gen](const cedar::Instruction& i) { gen.emit(i); },
            ctx);
        if (output_buf == BufferAllocator::BUFFER_UNUSED) {
            return TypedValue::void_val();
        }
        result_buf = output_buf;
    }

    // Build PatternPayload
    auto payload = std::make_shared<PatternPayload>();
    payload->fields[PatternPayload::FREQ] = value_buf;
    payload->fields[PatternPayload::VEL] = velocity_buf;
    payload->fields[PatternPayload::TRIG] = trigger_buf;
    payload->state_id = state_id;
    payload->cycle_length = cycle_length;
    payload->is_sample_pattern = is_sample_pattern;
    payload->max_voices = max_voices;
    if (max_voices > 1 && !is_sample_pattern) {
        payload->voice_freqs = std::move(voice_buffers);
    }

    // Records-and-field-access PRD §3: allocate the 8 extended field buffers
    // (gate, type, note, dur, chance, time, phase, sample_id) and emit the
    // corresponding SEQPAT_GATE/TYPE/FIELD/PHASE opcodes. Same wiring as
    // emit_per_voice_seqpat so %.note, %.dur, etc. work on transformed patterns.
    if (!gen.emit_extended_field_buffers(*payload, state_id, call_loc)) {
        return TypedValue::void_val();
    }

    // Phase 2.1 PRD §11: emit per-key SEQPAT_PROP buffers for custom properties
    // collected by SequenceCompiler. Covers all transforms going through this
    // helper (slow, fast, rev, transpose, early, late, palindrome, ...).
    if (!gen.emit_custom_property_buffers(compiler, *payload, state_id)) {
        return TypedValue::void_val();
    }

    // Pattern-as-value: the local SequenceCompiler's mappings already reflect
    // any nested .bank()/.variant() applied through compile_pattern_for_transform,
    // so projecting them here gives the correct bank-qualified sample refs
    // regardless of how this transform is wrapped.
    payload->sample_refs = sample_refs_from_mappings(compiler.sample_mappings());
    gen.publish_sample_refs(payload->sample_refs);

    auto tv = TypedValue::make_pattern(payload, result_buf);
    node_types[node] = tv;
    return tv;
}

CodeGenerator::PatternQuerySource CodeGenerator::emit_pattern_query_only(
    std::uint32_t state_id,
    float cycle_length,
    const SequenceCompiler& compiler,
    std::vector<std::vector<cedar::Event>>& sequence_events,
    const SourceLocation& pattern_loc) {

    PatternQuerySource src;
    compiler.collect_samples(required_samples_);

    // Compute max_voices from the local (possibly post-voicing) events.
    std::uint8_t max_voices = 1;
    for (const auto& seq : sequence_events) {
        for (const auto& e : seq) {
            if (e.num_values > max_voices) max_voices = e.num_values;
        }
    }

    // Emit SEQPAT_QUERY. SEQPAT_STEP / extended fields are emitted by the
    // downstream readout against the transform state_id.
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

    // Store sequence program initialization data for the inner SequenceState.
    StateInitData seq_init;
    seq_init.state_id = state_id;
    seq_init.type = StateInitData::Type::SequenceProgram;
    seq_init.cycle_length = cycle_length;
    seq_init.sequences = compiler.sequences();
    seq_init.sequence_events = std::move(sequence_events);
    seq_init.total_events = compiler.total_events();
    seq_init.is_sample_pattern = compiler.is_sample_pattern();
    seq_init.pattern_location = pattern_loc;
    seq_init.sequence_sample_mappings = compiler.sample_mappings();
    state_inits_.push_back(std::move(seq_init));

    src.ok = true;
    src.state_id = state_id;
    src.cycle_length = cycle_length;
    src.is_sample_pattern = compiler.is_sample_pattern();
    src.max_voices = max_voices;
    src.total_events = compiler.total_events();
    src.sample_refs = sample_refs_from_mappings(compiler.sample_mappings());
    publish_sample_refs(src.sample_refs);
    for (const auto& [key, slot] : compiler.custom_property_slots()) {
        src.custom_props.emplace_back(key, slot);
    }
    return src;
}

// ============================================================================
// Timeline curve literal codegen
// ============================================================================

TypedValue CodeGenerator::handle_timeline_literal(NodeIndex node, const Node& n) {
    // PRD Phase 1b: curve pattern lives in MiniLiteralData's sub-arena.
    const auto& lit_data = n.as_mini_literal();
    if (!lit_data.mini_arena || lit_data.mini_root == NULL_NODE) {
        error("E114", "Timeline curve has no parsed content", n.location);
        return TypedValue::void_val();
    }
    const AstArena& mini_arena = *lit_data.mini_arena;
    NodeIndex pattern_node = lit_data.mini_root;

    // Evaluate the curve pattern to events
    PatternEvaluator evaluator(mini_arena);
    PatternEventStream stream = evaluator.evaluate(pattern_node, 0);

    // Convert events to breakpoints
    auto breakpoints = events_to_breakpoints(stream.events);
    if (breakpoints.empty()) {
        // Empty curve - emit zero
        std::uint16_t out = emit_zero();
        if (out == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
        }
        return cache_and_return(node, TypedValue::signal(out));
    }

    if (breakpoints.size() > cedar::TimelineState::MAX_BREAKPOINTS) {
        warn("W200", "Timeline curve exceeds 64 breakpoints, truncating", n.location);
        breakpoints.resize(cedar::TimelineState::MAX_BREAKPOINTS);
    }

    // Allocate state and output buffer
    std::uint32_t tl_count = call_counters_["timeline"]++;
    push_path("timeline#" + std::to_string(tl_count));
    std::uint32_t state_id = compute_state_id();
    std::uint16_t out_buf = buffers_.allocate();

    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return TypedValue::void_val();
    }

    // Emit TIMELINE instruction
    cedar::Instruction inst{};
    inst.opcode = cedar::Opcode::TIMELINE;
    inst.out_buffer = out_buf;
    inst.inputs[0] = 0xFFFF;
    inst.inputs[1] = 0xFFFF;
    inst.inputs[2] = 0xFFFF;
    inst.inputs[3] = 0xFFFF;
    inst.inputs[4] = 0xFFFF;
    inst.state_id = state_id;
    emit(inst);

    // Create StateInitData for timeline breakpoints
    StateInitData timeline_init;
    timeline_init.state_id = state_id;
    timeline_init.type = StateInitData::Type::Timeline;
    timeline_init.timeline_breakpoints = std::move(breakpoints);
    timeline_init.timeline_loop = true;
    timeline_init.timeline_loop_length = stream.cycle_span;  // cycle = beat
    state_inits_.push_back(std::move(timeline_init));

    pop_path();
    return cache_and_return(node, TypedValue::signal(out_buf));
}

// ============================================================================
// Timeline function call form: timeline("__/''")
// ============================================================================

TypedValue CodeGenerator::handle_timeline_call(NodeIndex node, const Node& n) {
    // Extract the first string argument
    NodeIndex arg = n.first_child;
    if (arg == NULL_NODE) {
        error("E114", "timeline() requires a string argument", n.location);
        return TypedValue::void_val();
    }

    const Node& arg_node = ast_->arena[arg];
    NodeIndex arg_value = arg;
    if (arg_node.type == NodeType::Argument) {
        arg_value = arg_node.first_child;
    }
    if (arg_value == NULL_NODE) {
        error("E114", "timeline() requires a string argument", n.location);
        return TypedValue::void_val();
    }

    const Node& value_node = ast_->arena[arg_value];
    if (value_node.type != NodeType::StringLit) {
        error("E114", "timeline() argument must be a string literal", n.location);
        return TypedValue::void_val();
    }

    std::string curve_str = value_node.as_string();

    // PRD Phase 1b: parse into a codegen scratch arena instead of mutating
    // ast_->arena via const_cast.
    AstArena& scratch = acquire_mini_scratch_arena();
    auto [pattern_root, diags] = parse_mini(curve_str, scratch,
        mini_content_location(value_node.location), false, true);

    for (const auto& d : diags) {
        if (d.severity == Severity::Error) {
            error("E114", d.message, n.location);
        } else {
            warn("W200", d.message, n.location);
        }
    }

    if (pattern_root == NULL_NODE) {
        error("E114", "timeline() failed to parse curve notation", n.location);
        return TypedValue::void_val();
    }

    // Evaluate the curve pattern to events
    PatternEvaluator evaluator(scratch);
    PatternEventStream stream = evaluator.evaluate(pattern_root, 0);

    // Convert events to breakpoints
    auto breakpoints = events_to_breakpoints(stream.events);
    if (breakpoints.empty()) {
        std::uint16_t out = emit_zero();
        if (out == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
        }
        return cache_and_return(node, TypedValue::signal(out));
    }

    if (breakpoints.size() > cedar::TimelineState::MAX_BREAKPOINTS) {
        warn("W200", "Timeline curve exceeds 64 breakpoints, truncating", n.location);
        breakpoints.resize(cedar::TimelineState::MAX_BREAKPOINTS);
    }

    // Allocate state and output buffer
    std::uint32_t tl_count = call_counters_["timeline"]++;
    push_path("timeline#" + std::to_string(tl_count));
    std::uint32_t state_id = compute_state_id();
    std::uint16_t out_buf = buffers_.allocate();

    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return TypedValue::void_val();
    }

    // Emit TIMELINE instruction
    cedar::Instruction inst{};
    inst.opcode = cedar::Opcode::TIMELINE;
    inst.out_buffer = out_buf;
    inst.inputs[0] = 0xFFFF;
    inst.inputs[1] = 0xFFFF;
    inst.inputs[2] = 0xFFFF;
    inst.inputs[3] = 0xFFFF;
    inst.inputs[4] = 0xFFFF;
    inst.state_id = state_id;
    emit(inst);

    // Create StateInitData for timeline breakpoints
    StateInitData timeline_init;
    timeline_init.state_id = state_id;
    timeline_init.type = StateInitData::Type::Timeline;
    timeline_init.timeline_breakpoints = std::move(breakpoints);
    timeline_init.timeline_loop = true;
    timeline_init.timeline_loop_length = stream.cycle_span;  // cycle = beat
    state_inits_.push_back(std::move(timeline_init));

    pop_path();
    return cache_and_return(node, TypedValue::signal(out_buf));
}

// Note: the previous `emit_instruction_helper(instructions, inst)` thunk was
// removed in PRD prd-parser-codegen-correctness.md Phase 3 (F2) — it pushed
// to `instructions` directly without updating `source_locations_`. Pattern
// emission now routes through `CodeGenerator::emit()` (passed into
// `emit_pattern_with_state` via its `gen` parameter), which keeps both
// parallel vectors in sync.

// ============================================================================
// fast / slow — runtime rate scaling via EVENT_RATE_SCALE
// ============================================================================
//
// PRD prd-runtime-event-transforms Phase 3. fast/slow no longer mutate
// cycle_length at compile time; they emit an EVENT_RATE_SCALE opcode whose
// output (a modulated beat-position signal) feeds the new SEQPAT_QUERY's
// external-clock input (sequencing.hpp op_seqpat_query, inputs[0]).
//
// Implementation strategy: each fast/slow call recompiles its inner pattern
// into a NEW SequenceState (`compile_pattern_for_transform` +
// `emit_pattern_with_state`) so that two transforms applied to the same
// pattern stay independent. The OLD compile-time `cycle_length` mutation is
// removed — the runtime ERS opcode is the sole authority on playback rate.
// Composition `fast(slow(p, 2), 3)` works because each call adds its own
// ERS upstream of its own SEQPAT_QUERY; the inner slow's ERS runs first,
// the outer fast's ERS reads from the inner via the upstream-phase input.
// For mixed chains with event_map transforms (e.g.
// `fast(transpose(p, 7), 2)`), `compile_pattern_for_transform`'s recursive
// path applies the inner transform at compile time (legacy fallback), so
// the EVENT_MAP doesn't appear in fast's stream — the events are already
// transposed in the SequenceProgram init.

// Shared implementation of fast/slow. `is_fast=true` → rate = factor;
// `is_fast=false` → rate = 1/factor (constant-folded for numeric factors,
// emitted as a DIV for signal-rate factors).
TypedValue CodeGenerator::emit_rate_scale_call(NodeIndex node, const Node& n,
                                               bool is_fast) {
    const char* fn_name = is_fast ? "fast" : "slow";

    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    if (pattern_arg == NULL_NODE) {
        error("E130", std::string(fn_name) +
                  "() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }

    // Locate the (raw) factor AST node — needed for the signal-rate path.
    NodeIndex factor_arg_raw = NULL_NODE;
    {
        NodeIndex arg = n.first_child;
        std::size_t idx = 0;
        while (arg != NULL_NODE && idx < 1) {
            arg = ast_->arena[arg].next_sibling;
            idx++;
        }
        if (arg == NULL_NODE) {
            error("E130", std::string(fn_name) +
                      "() requires a factor as second argument", n.location);
            return TypedValue::void_val();
        }
        const Node& arg_node = ast_->arena[arg];
        factor_arg_raw = (arg_node.type == NodeType::Argument)
                            ? arg_node.first_child
                            : arg;
    }

    // Compile-time constant factor → static validation + 1/x folding for slow.
    auto factor_const = get_number_arg(*ast_, n, 1);
    if (factor_const.has_value() && *factor_const <= 0.0f) {
        error("E185", std::string(fn_name) +
                  "() requires a positive factor", n.location);
        return TypedValue::void_val();
    }

    if (!is_pattern_node(*ast_, *symbols_, pattern_arg)) {
        error("E133", std::string(fn_name) +
                  "() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }

    // Recompile the inner pattern into a fresh sequence so multi-use
    // (`slow(f, 2)` and `fast(f, 2)` on the same `f`) stays independent.
    // compile_pattern_for_transform's recursive path applies inner compile-
    // time transforms (transpose/velocity legacy fallback / inner fast/slow
    // cycle_length); the cycle_length we get back already reflects any
    // inner fast/slow nesting.
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    NodeIndex pattern_node = NULL_NODE;
    const AstArena* pattern_arena = nullptr;
    std::uint32_t num_elements = 1;
    std::vector<std::vector<cedar::Event>> sequence_events;
    float cycle_length = 1.0f;

    if (!compile_pattern_for_transform(*this, *ast_, pattern_arg,
                                       sample_registry_, compiler,
                                       pattern_node, pattern_arena, num_elements,
                                       sequence_events, cycle_length)) {
        error("E130", std::string(fn_name) +
                  "() failed to compile pattern argument", n.location);
        return TypedValue::void_val();
    }

    // Resolve the factor → a buffer index. Constants are folded (and slow's
    // 1/x done at compile time); signals get a runtime DIV for slow.
    std::uint16_t factor_buf;
    if (factor_const.has_value()) {
        float rate = is_fast ? *factor_const : (1.0f / *factor_const);
        factor_buf = emit_push_const(rate);
        if (factor_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return TypedValue::void_val();
        }
    } else {
        TypedValue factor_tv = visit(factor_arg_raw);
        if (factor_tv.error) return factor_tv;
        if (factor_tv.type != ValueType::Signal &&
            factor_tv.type != ValueType::Number) {
            error("E186", std::string(fn_name) +
                      "() factor must be a number or signal", n.location);
            return TypedValue::void_val();
        }
        if (factor_tv.buffer == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return TypedValue::void_val();
        }
        if (is_fast) {
            factor_buf = factor_tv.buffer;
        } else {
            // slow(p, sig) → rate = 1.0 / sig at runtime.
            std::uint16_t one_buf =
                emit_push_const(1.0f);
            if (one_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return TypedValue::void_val();
            }
            factor_buf = buffers_.allocate();
            if (factor_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return TypedValue::void_val();
            }
            cedar::Instruction div_inst{};
            div_inst.opcode = cedar::Opcode::DIV;
            div_inst.out_buffer = factor_buf;
            div_inst.inputs[0] = one_buf;
            div_inst.inputs[1] = factor_tv.buffer;
            div_inst.inputs[2] = 0xFFFF;
            div_inst.inputs[3] = 0xFFFF;
            div_inst.inputs[4] = 0xFFFF;
            emit(div_inst);
        }
    }

    // Allocate the pattern's own state_id and emit its full SEQPAT pipeline.
    std::uint32_t call_count = call_counters_[fn_name]++;
    push_path(std::string(fn_name) + "#" + std::to_string(call_count));
    std::uint32_t state_id = compute_state_id();

    const Node& pattern = (*pattern_arena)[pattern_node];
    auto result_tv = emit_pattern_with_state(
        *this, buffers_, state_inits_, required_samples_,
        node_types_, node, state_id, cycle_length,
        compiler, sequence_events, pattern.location, n.location);
    pop_path();

    if (result_tv.buffer == BufferAllocator::BUFFER_UNUSED && !result_tv.pattern) {
        error("E101", "Buffer pool exhausted", n.location);
        return result_tv;
    }

    // Emit EVENT_RATE_SCALE that targets the just-emitted SequenceState
    // (state_id). ERS mutates upstream->cycle_length each block; SEQPAT_*
    // opcodes pick it up automatically. Emission order doesn't matter for
    // correctness — ERS runs each block before the SEQPAT_QUERY because
    // codegen emits it after the pattern but the program loader sequences
    // by instruction order, and ERS reads/writes the same SequenceState as
    // the SEQPAT_QUERY. For the first block, ERS captures the SequenceState's
    // initial cycle_length BEFORE SEQPAT_QUERY consults it; subsequent
    // blocks see the modulated value.
    push_path("ers");
    std::uint32_t ers_state_id = compute_state_id();
    pop_path();
    pop_path();  // back out of fast#N / slow#N path

    auto& stream = emit_stream();
    auto& locs = loc_stream();
    cedar::Instruction ers{};
    ers.opcode = cedar::Opcode::EVENT_RATE_SCALE;
    ers.out_buffer = 0xFFFF;
    ers.inputs[0] = 0xFFFF;
    ers.inputs[1] = factor_buf;
    // Pack upstream SequenceState state_id across inputs[2..3] (same encoding
    // as EVENT_MAP / EVENT_FILTER via event_transform_upstream_id).
    ers.inputs[2] = static_cast<std::uint16_t>(state_id & 0xFFFF);
    ers.inputs[3] = static_cast<std::uint16_t>((state_id >> 16) & 0xFFFF);
    ers.inputs[4] = 0xFFFF;
    ers.state_id = ers_state_id;

    // Insert ERS BEFORE the first SEQPAT_* instruction reading this state_id
    // so the cycle_length mutation lands before any opcode consults it on
    // this block. emit_pattern_with_state appends in topological order, so
    // SEQPAT_QUERY is the earliest consumer. PRD prd-parser-codegen-
    // correctness.md Phase 3 (F2): the non-tail insert must touch both
    // `stream` AND `locs` in lock-step or the parallel invariant breaks.
    std::optional<std::size_t> insert_idx;
    for (std::size_t i = 0; i < stream.size(); ++i) {
        if (stream[i].state_id == state_id &&
            stream[i].opcode == cedar::Opcode::SEQPAT_QUERY) {
            insert_idx = i;
            break;
        }
    }
    if (insert_idx) {
        const auto pos = static_cast<std::ptrdiff_t>(*insert_idx);
        stream.insert(stream.begin() + pos, ers);
        locs.insert(locs.begin() + pos, n.location);
    } else {
        // Defensive: emit_pattern_with_state should always emit SEQPAT_QUERY.
        emit(ers);
    }

    StateInitData rs_init{};
    rs_init.state_id = ers_state_id;
    rs_init.type = StateInitData::Type::RateScale;
    rs_init.pattern_location = n.location;
    state_inits_.push_back(std::move(rs_init));

    return result_tv;
}

TypedValue CodeGenerator::handle_slow_call(NodeIndex node, const Node& n) {
    return emit_rate_scale_call(node, n, /*is_fast=*/false);
}

TypedValue CodeGenerator::handle_fast_call(NodeIndex node, const Node& n) {
    return emit_rate_scale_call(node, n, /*is_fast=*/true);
}

// ============================================================================
// EVENT_REORDER — rev / palindrome / zoom / compress (PRD Phase 4)
// ============================================================================
//
// Shared shape for the 4 structural transforms shipped in Commit A:
//   1. compile_pattern_for_transform → fresh sequence_events + cycle_length
//      (recursive compile-time fold remains the legacy fallback for nested
//      const-only chains like fast(rev(p), 2)).
//   2. Allocate inner state_id under "<fn>#N" path; emit SEQPAT_QUERY +
//      StateInitData::SequenceProgram via emit_pattern_query_only.
//   3. Allocate transform state_id under "<fn>#N/reorder" path.
//   4. Emit EVENT_REORDER opcode with kind/flags packed in rate byte and
//      params in inputs[0..1]; inputs[2..3] carry the upstream (inner) state.
//   5. Push StateInitData::Reorder with downstream cycle_length + capacity.
//   6. Re-target the PatternQuerySource to the transform state and call
//      emit_pattern_readout to emit SEQPAT_STEP / extended fields / SAMPLE_PLAY
//      / SEQPAT_PROP against the transform state.
//
// Composes naturally with upstream runtime EVENT_MAP / EVENT_FILTER /
// EVENT_RATE_SCALE — the inner SequenceState's OutputEvents are read each
// block by EVENT_REORDER and the rewritten events feed downstream readout.

TypedValue CodeGenerator::emit_reorder_call(
    NodeIndex node, const Node& n, const char* fn_name,
    std::uint8_t kind, std::uint8_t flags,
    std::uint16_t param0_buf, std::uint16_t param1_buf,
    float cycle_length_factor, std::uint32_t capacity_factor) {

    SequenceCompiler compiler(ast_->arena, sample_registry_);
    NodeIndex pattern_node = NULL_NODE;
    const AstArena* pattern_arena = nullptr;
    std::uint32_t num_elements = 1;
    std::vector<std::vector<cedar::Event>> sequence_events;
    float cycle_length = 1.0f;

    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    if (pattern_arg == NULL_NODE ||
        !compile_pattern_for_transform(*this, *ast_, pattern_arg, sample_registry_,
                                       compiler, pattern_node, pattern_arena, num_elements,
                                       sequence_events, cycle_length)) {
        error("E130", std::string(fn_name) +
                  "() failed to compile pattern argument", n.location);
        return TypedValue::void_val();
    }

    std::uint32_t call_count = call_counters_[fn_name]++;
    push_path(std::string(fn_name) + "#" + std::to_string(call_count));
    std::uint32_t inner_state_id = compute_state_id();

    const Node& pattern = (*pattern_arena)[pattern_node];
    PatternQuerySource src = emit_pattern_query_only(
        inner_state_id, cycle_length, compiler, sequence_events, pattern.location);
    if (!src.ok) {
        pop_path();
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    push_path("reorder");
    std::uint32_t transform_state_id = compute_state_id();
    pop_path();

    cedar::Instruction op{};
    op.opcode = cedar::Opcode::EVENT_REORDER;
    op.rate = cedar::event_reorder_rate(kind, flags);
    op.out_buffer = 0xFFFF;
    op.inputs[0] = param0_buf;
    op.inputs[1] = param1_buf;
    op.inputs[2] = static_cast<std::uint16_t>(inner_state_id & 0xFFFF);
    op.inputs[3] = static_cast<std::uint16_t>((inner_state_id >> 16) & 0xFFFF);
    op.inputs[4] = 0xFFFF;
    op.state_id = transform_state_id;
    emit(op);

    // Downstream transform state: holds the rewritten OutputEvents that the
    // readout reads via SEQPAT_STEP / SEQPAT_FIELD etc.
    StateInitData rd_init{};
    rd_init.state_id = transform_state_id;
    rd_init.type = StateInitData::Type::Reorder;
    rd_init.cycle_length = cycle_length * cycle_length_factor;
    rd_init.is_sample_pattern = compiler.is_sample_pattern();
    // OutputEvents capacity: max(upstream * fanout_factor, 32). The state
    // pool clamps to a 32-floor and rounds up internally.
    rd_init.total_events = compiler.total_events() * capacity_factor;
    rd_init.pattern_location = pattern.location;
    state_inits_.push_back(std::move(rd_init));

    pop_path();  // out of "<fn>#N"

    // Re-target the readout to the transform state, with the kind-scaled
    // downstream cycle_length stamped on the PatternPayload.
    src.state_id = transform_state_id;
    src.cycle_length = cycle_length * cycle_length_factor;
    return emit_pattern_readout(node, src, n.location);
}

TypedValue CodeGenerator::handle_rev_call(NodeIndex node, const Node& n) {
    // rev(pattern) — reverse event order. PRD Phase 4 runtime form: each
    // block, EVENT_REORDER(REV) reads upstream OutputEvents and re-times each
    // event with `new_time = cycle_length - t - dur` (clamp >= 0). Composes
    // with upstream EVENT_MAP (e.g. transpose); composition was broken under
    // the pre-Phase-4 compile-time path because compile_pattern_for_transform
    // didn't see runtime EVENT_MAP transforms.
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    if (pattern_arg == NULL_NODE) {
        error("E130", "rev() requires a pattern as argument", n.location);
        return TypedValue::void_val();
    }
    if (!is_pattern_node(*ast_, *symbols_, pattern_arg)) {
        error("E133", "rev() argument must be a pattern", n.location);
        return TypedValue::void_val();
    }
    return emit_reorder_call(node, n, "rev",
                             cedar::EVENT_REORDER_REV, /*flags=*/0,
                             /*param0=*/0xFFFF, /*param1=*/0xFFFF,
                             /*cycle_length_factor=*/1.0f,
                             /*capacity_factor=*/1u);
}

// ============================================================================
// EVENT_FANOUT — ply / linger / segment (PRD Phase 4 Commit B)
// ============================================================================
//
// Same structure as emit_reorder_call: compile inner pattern, emit inner
// SEQPAT_QUERY + StateInitData::SequenceProgram via emit_pattern_query_only,
// allocate transform state_id, emit EVENT_FANOUT instruction + Fanout init,
// re-target the readout. Differs from emit_reorder_call only in the opcode
// emitted and the StateInitData type tag.

TypedValue CodeGenerator::emit_fanout_call(
    NodeIndex node, const Node& n, const char* fn_name,
    std::uint8_t kind, std::uint16_t param0_buf,
    float cycle_length_factor, std::uint32_t capacity_factor) {

    SequenceCompiler compiler(ast_->arena, sample_registry_);
    NodeIndex pattern_node = NULL_NODE;
    const AstArena* pattern_arena = nullptr;
    std::uint32_t num_elements = 1;
    std::vector<std::vector<cedar::Event>> sequence_events;
    float cycle_length = 1.0f;

    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    if (pattern_arg == NULL_NODE ||
        !compile_pattern_for_transform(*this, *ast_, pattern_arg, sample_registry_,
                                       compiler, pattern_node, pattern_arena, num_elements,
                                       sequence_events, cycle_length)) {
        error("E130", std::string(fn_name) +
                  "() failed to compile pattern argument", n.location);
        return TypedValue::void_val();
    }

    std::uint32_t call_count = call_counters_[fn_name]++;
    push_path(std::string(fn_name) + "#" + std::to_string(call_count));
    std::uint32_t inner_state_id = compute_state_id();

    const Node& pattern = (*pattern_arena)[pattern_node];
    PatternQuerySource src = emit_pattern_query_only(
        inner_state_id, cycle_length, compiler, sequence_events, pattern.location);
    if (!src.ok) {
        pop_path();
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    push_path("fanout");
    std::uint32_t transform_state_id = compute_state_id();
    pop_path();

    cedar::Instruction op{};
    op.opcode = cedar::Opcode::EVENT_FANOUT;
    op.rate = cedar::event_fanout_rate(kind);
    op.out_buffer = 0xFFFF;
    op.inputs[0] = param0_buf;
    op.inputs[1] = 0xFFFF;
    op.inputs[2] = static_cast<std::uint16_t>(inner_state_id & 0xFFFF);
    op.inputs[3] = static_cast<std::uint16_t>((inner_state_id >> 16) & 0xFFFF);
    op.inputs[4] = 0xFFFF;
    op.state_id = transform_state_id;
    emit(op);

    StateInitData fn_init{};
    fn_init.state_id = transform_state_id;
    fn_init.type = StateInitData::Type::Fanout;
    fn_init.cycle_length = cycle_length * cycle_length_factor;
    fn_init.is_sample_pattern = compiler.is_sample_pattern();
    fn_init.total_events = compiler.total_events() * capacity_factor;
    fn_init.pattern_location = pattern.location;
    state_inits_.push_back(std::move(fn_init));

    pop_path();  // out of "<fn>#N"

    src.state_id = transform_state_id;
    src.cycle_length = cycle_length * cycle_length_factor;
    return emit_pattern_readout(node, src, n.location);
}


TypedValue CodeGenerator::emit_pattern_readout(NodeIndex node,
                                               const PatternQuerySource& src,
                                               SourceLocation loc) {
    const std::uint8_t max_voices = (src.max_voices < 1) ? 1 : src.max_voices;

    std::uint16_t value_buf = buffers_.allocate();
    std::uint16_t velocity_buf = buffers_.allocate();
    std::uint16_t trigger_buf = buffers_.allocate();
    if (value_buf == BufferAllocator::BUFFER_UNUSED ||
        velocity_buf == BufferAllocator::BUFFER_UNUSED ||
        trigger_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", loc);
        return TypedValue::void_val();
    }

    // Per-voice SEQPAT_STEP reading the final transform's SequenceState.
    std::vector<std::uint16_t> voice_buffers;
    for (std::uint8_t voice = 0; voice < max_voices; ++voice) {
        std::uint16_t voice_value_buf =
            (voice == 0) ? value_buf : buffers_.allocate();
        if (voice_value_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", loc);
            return TypedValue::void_val();
        }
        cedar::Instruction step_inst{};
        step_inst.opcode = cedar::Opcode::SEQPAT_STEP;
        step_inst.out_buffer = voice_value_buf;
        step_inst.inputs[0] = (voice == 0) ? velocity_buf : 0xFFFF;
        step_inst.inputs[1] = (voice == 0) ? trigger_buf : 0xFFFF;
        step_inst.inputs[2] = voice;
        step_inst.inputs[3] = 0xFFFF;
        step_inst.inputs[4] = 0xFFFF;
        step_inst.state_id = src.state_id;
        emit(step_inst);
        voice_buffers.push_back(voice_value_buf);
    }

    auto payload = std::make_shared<PatternPayload>();
    payload->fields[PatternPayload::FREQ] = value_buf;
    payload->fields[PatternPayload::VEL] = velocity_buf;
    payload->fields[PatternPayload::TRIG] = trigger_buf;
    payload->state_id = src.state_id;
    payload->cycle_length = src.cycle_length;
    payload->is_sample_pattern = src.is_sample_pattern;
    payload->max_voices = max_voices;
    if (max_voices > 1 && !src.is_sample_pattern) {
        payload->voice_freqs = std::move(voice_buffers);
    }

    std::uint16_t result_buf = value_buf;
    if (src.is_sample_pattern) {
        SamplePatternEmitCtx ctx;
        ctx.kind = SamplePatternEmitCtx::Kind::Pattern;
        ctx.seq_state_id = src.state_id;
        ctx.value_buf = value_buf;
        ctx.trigger_buf = trigger_buf;
        ctx.velocity_buf = velocity_buf;  // EVENT_MAP already scaled velocity
        ctx.loc = loc;
        std::uint16_t output_buf = emit_sample_chain(
            buffers_, [this](const cedar::Instruction& i){ emit(i); }, ctx);
        if (output_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", loc);
            return TypedValue::void_val();
        }
        result_buf = output_buf;
    }

    // Extended field buffers (gate/type/note/dur/chance/time/phase/sample_id).
    if (!emit_extended_field_buffers(*payload, src.state_id, loc)) {
        error("E101", "Buffer pool exhausted", loc);
        return TypedValue::void_val();
    }

    // SEQPAT_PROP buffers for custom properties carried across the chain.
    // EVENT_MAP copies prop_vals/prop_set_mask through unchanged, so the slot
    // indices match the upstream pattern.
    for (const auto& [key, slot] : src.custom_props) {
        std::uint16_t buf = buffers_.allocate();
        if (buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", loc);
            return TypedValue::void_val();
        }
        cedar::Instruction prop_inst{};
        prop_inst.opcode = cedar::Opcode::SEQPAT_PROP;
        prop_inst.out_buffer = buf;
        prop_inst.rate = slot;
        prop_inst.inputs[0] = 0;       // voice 0
        prop_inst.inputs[1] = 0xFFFF;  // internal clock
        prop_inst.inputs[2] = 0xFFFF;
        prop_inst.inputs[3] = 0xFFFF;
        prop_inst.inputs[4] = 0xFFFF;
        prop_inst.state_id = src.state_id;
        emit(prop_inst);
        payload->custom_fields[key] = buf;
        payload->custom_field_slots[key] = slot;
    }

    payload->sample_refs = src.sample_refs;
    publish_sample_refs(payload->sample_refs);

    return cache_and_return(node, TypedValue::make_pattern(payload, result_buf));
}

TypedValue CodeGenerator::handle_bank_call(NodeIndex node, const Node& n) {
    // bank(pattern, bank_name) - set sample bank for all events
    // Sets the bank field on all sample mappings for deferred resolution

    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    auto bank_name = get_string_arg(*ast_, n, 1);

    if (pattern_arg == NULL_NODE) {
        error("E130", "bank() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }

    if (!bank_name.has_value()) {
        error("E131", "bank() requires a string as second argument (e.g., \"TR808\")", n.location);
        return TypedValue::void_val();
    }

    if (!is_pattern_node(*ast_, *symbols_, pattern_arg)) {
        error("E133", "bank() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }

    // Compile the pattern (may include inner transforms applied recursively)
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    NodeIndex pattern_node = NULL_NODE;
    const AstArena* pattern_arena = nullptr;
    std::uint32_t num_elements = 1;
    std::vector<std::vector<cedar::Event>> sequence_events;
    float cycle_length = 1.0f;

    if (!compile_pattern_for_transform(*this, *ast_, pattern_arg, sample_registry_,
                                        compiler, pattern_node, pattern_arena, num_elements,
                                        sequence_events, cycle_length)) {
        error("E130", "bank() failed to compile pattern argument", n.location);
        return TypedValue::void_val();
    }

    // Set up state ID
    std::uint32_t bank_count = call_counters_["bank"]++;
    push_path("bank#" + std::to_string(bank_count));
    std::uint32_t state_id = compute_state_id();

    auto sample_mappings = compiler.sample_mappings();

    // Update bank field on all sample mappings
    for (auto& mapping : sample_mappings) {
        mapping.bank = *bank_name;
    }

    // Collect required samples (with updated bank info)
    compiler.collect_samples(required_samples_);

    bool is_sample_pattern = compiler.is_sample_pattern();

    // Allocate buffers for outputs
    std::uint16_t value_buf = buffers_.allocate();
    std::uint16_t velocity_buf = buffers_.allocate();
    std::uint16_t trigger_buf = buffers_.allocate();

    if (value_buf == BufferAllocator::BUFFER_UNUSED ||
        velocity_buf == BufferAllocator::BUFFER_UNUSED ||
        trigger_buf == BufferAllocator::BUFFER_UNUSED) {
        pop_path();
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    // Emit SEQPAT_QUERY instruction
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
    step_inst.inputs[2] = 0;
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
    seq_init.sequence_events = std::move(sequence_events);
    seq_init.total_events = compiler.total_events();
    seq_init.is_sample_pattern = is_sample_pattern;
    const Node& pattern = (*pattern_arena)[pattern_node];
    seq_init.pattern_location = pattern.location;
    seq_init.sequence_sample_mappings = std::move(sample_mappings);  // Use updated mappings
    state_inits_.push_back(std::move(seq_init));

    // Wire up SAMPLE_PLAY for sample patterns. Without this the returned
    // buffer would be raw sample-IDs (DC), not audio.
    std::uint16_t result_buf = value_buf;
    if (is_sample_pattern) {
        SamplePatternEmitCtx ctx;
        ctx.kind = SamplePatternEmitCtx::Kind::Pattern;
        ctx.seq_state_id = state_id;
        ctx.value_buf = value_buf;
        ctx.trigger_buf = trigger_buf;
        ctx.velocity_buf = velocity_buf;
        ctx.loc = n.location;
        std::uint16_t output_buf = emit_sample_chain(
            buffers_, [this](const cedar::Instruction& i){ emit(i); }, ctx);
        if (output_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            pop_path();
            return TypedValue::void_val();
        }
        result_buf = output_buf;
    }

    // Build PatternPayload
    auto payload = std::make_shared<PatternPayload>();
    payload->fields[PatternPayload::FREQ] = value_buf;
    payload->fields[PatternPayload::VEL] = velocity_buf;
    payload->fields[PatternPayload::TRIG] = trigger_buf;
    payload->state_id = state_id;
    payload->cycle_length = cycle_length;
    payload->is_sample_pattern = is_sample_pattern;
    payload->max_voices = compiler.max_voices();

    // Records-and-field-access PRD §3: populate extended fields so %.field
    // works on bank-mutated patterns.
    if (!emit_extended_field_buffers(*payload, state_id, n.location)) {
        pop_path();
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    // Phase 2.1 PRD §11: emit per-key SEQPAT_PROP buffers for custom properties.
    if (!emit_custom_property_buffers(compiler, *payload, state_id)) {
        pop_path();
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    // Bank-mutated mappings live in state_inits_.back() (we moved the local
    // sample_mappings into it above). Project them into the Pattern's
    // sample_refs so the bank info travels with the value.
    payload->sample_refs = sample_refs_from_mappings(
        state_inits_.back().sequence_sample_mappings);
    publish_sample_refs(payload->sample_refs);

    pop_path();
    return cache_and_return(node, TypedValue::make_pattern(payload, result_buf));
}

TypedValue CodeGenerator::handle_variant_call(NodeIndex node, const Node& n) {
    // variant(pattern, index) - set sample variant for all events
    // Two cases:
    //   1. variant(pattern, number) - fixed variant for all events
    //   2. variant(pattern, pattern) - variant per-event from another pattern

    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    NodeIndex variant_arg = get_pattern_arg(*ast_, n, 1);

    if (pattern_arg == NULL_NODE) {
        error("E130", "variant() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }

    if (variant_arg == NULL_NODE) {
        error("E131", "variant() requires an index number or pattern as second argument", n.location);
        return TypedValue::void_val();
    }

    if (!is_pattern_node(*ast_, *symbols_, pattern_arg)) {
        error("E133", "variant() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }

    // Check variant argument type
    const Node& variant_node = ast_->arena[variant_arg];
    bool is_fixed_variant = (variant_node.type == NodeType::NumberLit);
    int fixed_variant = 0;

    if (is_fixed_variant) {
        fixed_variant = static_cast<int>(variant_node.as_number());
        if (fixed_variant < 0) {
            error("E131", "variant() index must be non-negative", n.location);
            return TypedValue::void_val();
        }
    } else if (!is_pattern_node(*ast_, *symbols_, variant_arg)) {
        error("E131", "variant() second argument must be a number or pattern", n.location);
        return TypedValue::void_val();
    }

    // Compile the main pattern (may include inner transforms applied recursively)
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    NodeIndex pattern_node = NULL_NODE;
    const AstArena* pattern_arena = nullptr;
    std::uint32_t num_elements = 1;
    std::vector<std::vector<cedar::Event>> sequence_events;
    float cycle_length = 1.0f;

    if (!compile_pattern_for_transform(*this, *ast_, pattern_arg, sample_registry_,
                                        compiler, pattern_node, pattern_arena, num_elements,
                                        sequence_events, cycle_length)) {
        error("E130", "variant() failed to compile pattern argument", n.location);
        return TypedValue::void_val();
    }

    // Set up state ID
    std::uint32_t variant_count = call_counters_["variant"]++;
    push_path("variant#" + std::to_string(variant_count));
    std::uint32_t state_id = compute_state_id();

    auto sample_mappings = compiler.sample_mappings();

    if (is_fixed_variant) {
        // Case 1: Fixed variant - set on all sample mappings
        for (auto& mapping : sample_mappings) {
            mapping.variant = static_cast<std::uint8_t>(fixed_variant);
        }
    } else {
        // Case 2: Per-event variant from pattern
        // Compile the variant pattern to get its events
        SequenceCompiler variant_compiler(ast_->arena, sample_registry_);
        NodeIndex variant_pattern_node = NULL_NODE;
        const AstArena* variant_pattern_arena = nullptr;
        std::uint32_t variant_num_elements = 1;
        std::vector<std::vector<cedar::Event>> variant_events;
        float variant_cycle_length = 1.0f;

        if (compile_pattern_for_transform(*this, *ast_, variant_arg, sample_registry_,
                                          variant_compiler, variant_pattern_node, variant_pattern_arena, variant_num_elements,
                                          variant_events, variant_cycle_length)) {

            // Match events: for each sample event in the main pattern,
            // look up the corresponding variant value from the variant pattern.
            // If the variant pattern has fewer events, cycle through them.
            if (!variant_events.empty() && !variant_events[0].empty()) {
                std::size_t variant_idx = 0;
                std::size_t variant_count = variant_events[0].size();

                for (auto& mapping : sample_mappings) {
                    // Get the variant value from the variant pattern
                    const auto& variant_evt = variant_events[0][variant_idx % variant_count];
                    if (variant_evt.type == cedar::EventType::DATA && variant_evt.num_values > 0) {
                        // Use the first value as the variant index
                        int var_val = static_cast<int>(variant_evt.values[0]);
                        if (var_val >= 0) {
                            mapping.variant = static_cast<std::uint8_t>(var_val);
                        }
                    }
                    variant_idx++;
                }
            }
        }
    }

    // Collect required samples
    compiler.collect_samples(required_samples_);

    bool is_sample_pattern = compiler.is_sample_pattern();

    // Allocate buffers for outputs
    std::uint16_t value_buf = buffers_.allocate();
    std::uint16_t velocity_buf = buffers_.allocate();
    std::uint16_t trigger_buf = buffers_.allocate();

    if (value_buf == BufferAllocator::BUFFER_UNUSED ||
        velocity_buf == BufferAllocator::BUFFER_UNUSED ||
        trigger_buf == BufferAllocator::BUFFER_UNUSED) {
        pop_path();
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    // Emit SEQPAT_QUERY instruction
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
    step_inst.inputs[2] = 0;
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
    seq_init.sequence_events = std::move(sequence_events);
    seq_init.total_events = compiler.total_events();
    seq_init.is_sample_pattern = is_sample_pattern;
    const Node& pattern = (*pattern_arena)[pattern_node];
    seq_init.pattern_location = pattern.location;
    seq_init.sequence_sample_mappings = std::move(sample_mappings);  // Use updated mappings
    state_inits_.push_back(std::move(seq_init));

    // Wire up SAMPLE_PLAY for sample patterns. Without this the returned
    // buffer would be raw sample-IDs (DC), not audio.
    std::uint16_t result_buf = value_buf;
    if (is_sample_pattern) {
        SamplePatternEmitCtx ctx;
        ctx.kind = SamplePatternEmitCtx::Kind::Pattern;
        ctx.seq_state_id = state_id;
        ctx.value_buf = value_buf;
        ctx.trigger_buf = trigger_buf;
        ctx.velocity_buf = velocity_buf;
        ctx.loc = n.location;
        std::uint16_t output_buf = emit_sample_chain(
            buffers_, [this](const cedar::Instruction& i){ emit(i); }, ctx);
        if (output_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            pop_path();
            return TypedValue::void_val();
        }
        result_buf = output_buf;
    }

    // Build PatternPayload
    auto payload = std::make_shared<PatternPayload>();
    payload->fields[PatternPayload::FREQ] = value_buf;
    payload->fields[PatternPayload::VEL] = velocity_buf;
    payload->fields[PatternPayload::TRIG] = trigger_buf;
    payload->state_id = state_id;
    payload->cycle_length = cycle_length;
    payload->is_sample_pattern = is_sample_pattern;
    payload->max_voices = compiler.max_voices();

    // Records-and-field-access PRD §3: populate extended fields so %.field
    // works on variant-mutated patterns.
    if (!emit_extended_field_buffers(*payload, state_id, n.location)) {
        pop_path();
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    // Phase 2.1 PRD §11: emit per-key SEQPAT_PROP buffers for custom properties.
    if (!emit_custom_property_buffers(compiler, *payload, state_id)) {
        pop_path();
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    // Variant-mutated mappings live in state_inits_.back() (moved above).
    payload->sample_refs = sample_refs_from_mappings(
        state_inits_.back().sequence_sample_mappings);
    publish_sample_refs(payload->sample_refs);

    pop_path();
    return cache_and_return(node, TypedValue::make_pattern(payload, result_buf));
}

TypedValue CodeGenerator::handle_transport_call(NodeIndex node, const Node& n) {
    // transport(pattern, trig, step?, reset?) - trigger-driven pattern clock
    // Decouples pattern from global BPM by using trigger edges to advance

    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    if (pattern_arg == NULL_NODE) {
        error("E130", "transport() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }

    if (!is_pattern_node(*ast_, *symbols_, pattern_arg)) {
        error("E133", "transport() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }

    // Second arg: trigger signal (required)
    NodeIndex trig_arg = get_pattern_arg(*ast_, n, 1);
    if (trig_arg == NULL_NODE) {
        error("E131", "transport() requires a trigger signal as second argument", n.location);
        return TypedValue::void_val();
    }

    // Third arg: step size (optional, default 1.0)
    NodeIndex step_arg = get_pattern_arg(*ast_, n, 2);

    // Fourth arg: reset trigger (optional)
    NodeIndex reset_arg = get_pattern_arg(*ast_, n, 3);

    // Compile the pattern (may include inner transforms applied recursively)
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    NodeIndex pattern_node = NULL_NODE;
    const AstArena* pattern_arena = nullptr;
    std::uint32_t num_elements = 1;
    std::vector<std::vector<cedar::Event>> sequence_events;
    float cycle_length = 1.0f;

    if (!compile_pattern_for_transform(*this, *ast_, pattern_arg, sample_registry_,
                                        compiler, pattern_node, pattern_arena, num_elements,
                                        sequence_events, cycle_length)) {
        error("E130", "transport() failed to compile pattern argument", n.location);
        return TypedValue::void_val();
    }

    // Set up state IDs
    std::uint32_t transport_count = call_counters_["transport"]++;
    push_path("transport#" + std::to_string(transport_count));
    std::uint32_t transport_state_id = compute_state_id();

    // PRD prd-remove-pat-builtin §6.3: keep "pat" path segment unchanged
    // to preserve hot-swap semantic-ID hashes after the pat builtin removal.
    push_path("pat");
    std::uint32_t seq_state_id = compute_state_id();
    pop_path();
    bool is_sample_pattern = compiler.is_sample_pattern();

    // Visit trigger argument
    std::uint16_t trig_buf = visit(trig_arg).buffer;
    if (trig_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Failed to compile trigger argument", n.location);
        pop_path();
        return TypedValue::void_val();
    }

    // Visit optional step argument
    std::uint16_t step_buf = BufferAllocator::BUFFER_UNUSED;
    if (step_arg != NULL_NODE) {
        step_buf = visit(step_arg).buffer;
    }

    // Visit optional reset argument
    std::uint16_t reset_buf = BufferAllocator::BUFFER_UNUSED;
    if (reset_arg != NULL_NODE) {
        reset_buf = visit(reset_arg).buffer;
    }

    // Allocate beat_pos output buffer
    std::uint16_t beat_pos_buf = buffers_.allocate();
    if (beat_pos_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return TypedValue::void_val();
    }

    // Emit SEQPAT_TRANSPORT. cycle_length used to be bit-cast into the
    // inputs[3]/[4] slot pair; prd-extended-params-migration §4.9 moves it
    // to an ExtendedParams<1> companion state, freeing both signal slots.
    cedar::Instruction transport_inst{};
    transport_inst.opcode = cedar::Opcode::SEQPAT_TRANSPORT;
    transport_inst.out_buffer = beat_pos_buf;
    transport_inst.inputs[0] = trig_buf;
    transport_inst.inputs[1] = step_buf;
    transport_inst.inputs[2] = reset_buf;
    transport_inst.inputs[3] = BufferAllocator::BUFFER_UNUSED;
    transport_inst.inputs[4] = BufferAllocator::BUFFER_UNUSED;
    transport_inst.state_id = transport_state_id;
    emit(transport_inst);

    // cycle_length → ExtendedParams<1> slot 0 (constant).
    StateInitData transport_ext{};
    transport_ext.state_id = cedar::ext_params_state_id(transport_state_id);
    transport_ext.type = StateInitData::Type::ExtendedParams;
    transport_ext.ext_count = 1;
    transport_ext.ext_buffer_indices.fill(0xFFFFu);
    transport_ext.ext_constants[0] = cycle_length;
    state_inits_.push_back(transport_ext);

    // Emit SEQPAT_QUERY with clock override
    cedar::Instruction query_inst{};
    query_inst.opcode = cedar::Opcode::SEQPAT_QUERY;
    query_inst.out_buffer = 0xFFFF;
    query_inst.inputs[0] = beat_pos_buf;  // Clock override
    query_inst.inputs[1] = 0xFFFF;
    query_inst.inputs[2] = 0xFFFF;
    query_inst.inputs[3] = 0xFFFF;
    query_inst.inputs[4] = 0xFFFF;
    query_inst.state_id = seq_state_id;
    emit(query_inst);

    // Collect required samples
    compiler.collect_samples(required_samples_);

    // Allocate buffers for pattern outputs
    std::uint16_t value_buf = buffers_.allocate();
    std::uint16_t velocity_buf = buffers_.allocate();
    std::uint16_t trigger_buf = buffers_.allocate();

    if (value_buf == BufferAllocator::BUFFER_UNUSED ||
        velocity_buf == BufferAllocator::BUFFER_UNUSED ||
        trigger_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return TypedValue::void_val();
    }

    // Emit per-voice SEQPAT_STEP/GATE/TYPE with clock override
    std::uint8_t max_voices = compiler.max_voices();
    auto pattern_payload = emit_per_voice_seqpat(node, seq_state_id, max_voices, value_buf, velocity_buf,
                                                  trigger_buf, is_sample_pattern, n.location, beat_pos_buf);
    if (!pattern_payload) {
        pop_path();
        return TypedValue::void_val();
    }

    // Phase 2.1 PRD §11: emit per-key SEQPAT_PROP buffers (with same clock override).
    if (!emit_custom_property_buffers(compiler, *pattern_payload, seq_state_id, beat_pos_buf)) {
        pop_path();
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    // Store sequence program initialization data
    StateInitData seq_init;
    seq_init.state_id = seq_state_id;
    seq_init.type = StateInitData::Type::SequenceProgram;
    seq_init.cycle_length = cycle_length;
    seq_init.sequences = compiler.sequences();
    seq_init.sequence_events = std::move(sequence_events);
    seq_init.total_events = compiler.total_events();
    seq_init.is_sample_pattern = is_sample_pattern;
    const Node& pattern = (*pattern_arena)[pattern_node];
    seq_init.pattern_location = pattern.location;
    seq_init.sequence_sample_mappings = compiler.sample_mappings();
    state_inits_.push_back(std::move(seq_init));

    pattern_payload->sample_refs = sample_refs_from_mappings(compiler.sample_mappings());
    publish_sample_refs(pattern_payload->sample_refs);

    // Wire up SAMPLE_PLAY for sample patterns. Without this the returned
    // buffer would be raw sample-IDs (DC), not audio.
    std::uint16_t result_buf = value_buf;
    if (is_sample_pattern) {
        SamplePatternEmitCtx ctx;
        ctx.kind = SamplePatternEmitCtx::Kind::Pattern;
        ctx.seq_state_id = seq_state_id;
        ctx.value_buf = value_buf;
        ctx.trigger_buf = trigger_buf;
        ctx.velocity_buf = velocity_buf;
        ctx.loc = n.location;
        std::uint16_t output_buf = emit_sample_chain(
            buffers_, [this](const cedar::Instruction& i){ emit(i); }, ctx);
        if (output_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            pop_path();
            return TypedValue::void_val();
        }
        result_buf = output_buf;
    }

    pop_path();
    pattern_payload->state_id = seq_state_id;
    pattern_payload->cycle_length = cycle_length;
    return cache_and_return(node, TypedValue::make_pattern(pattern_payload, result_buf));
}

TypedValue CodeGenerator::handle_tune_call(NodeIndex node, const Node& n) {
    // tune(tuning_name, pattern) - apply microtonal tuning context to a pattern
    // tuning_name is a string like "31edo", "24edo"
    // The tuning context affects how micro_offset is resolved to Hz

    auto tuning_name = get_string_arg(*ast_, n, 0);
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 1);

    if (!tuning_name.has_value()) {
        error("E130", "tune() requires a tuning name string as first argument (e.g., \"31edo\")", n.location);
        return TypedValue::void_val();
    }

    if (pattern_arg == NULL_NODE) {
        error("E131", "tune() requires a pattern as second argument", n.location);
        return TypedValue::void_val();
    }

    auto tuning = parse_tuning(*tuning_name);
    if (!tuning.has_value()) {
        error("E132", "Unknown tuning: '" + *tuning_name + "'. Use format like '31edo', '24edo'", n.location);
        return TypedValue::void_val();
    }

    if (!is_pattern_node(*ast_, *symbols_, pattern_arg)) {
        error("E133", "tune() second argument must be a pattern", n.location);
        return TypedValue::void_val();
    }

    // Compile the pattern with the tuning context set
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    compiler.set_tuning(*tuning);

    NodeIndex pattern_node = NULL_NODE;
    const AstArena* pattern_arena = nullptr;
    std::uint32_t num_elements = 1;
    std::vector<std::vector<cedar::Event>> sequence_events;
    float cycle_length = 1.0f;

    if (!compile_pattern_for_transform(*this, *ast_, pattern_arg, sample_registry_,
                                        compiler, pattern_node, pattern_arena, num_elements,
                                        sequence_events, cycle_length)) {
        error("E130", "tune() failed to compile pattern argument", n.location);
        return TypedValue::void_val();
    }

    // Set up state ID
    std::uint32_t tune_count = call_counters_["tune"]++;
    push_path("tune#" + std::to_string(tune_count));
    std::uint32_t state_id = compute_state_id();

    const Node& pattern = (*pattern_arena)[pattern_node];
    auto result_tv = emit_pattern_with_state(
        *this, buffers_, state_inits_, required_samples_,
        node_types_, node, state_id, cycle_length,
        compiler, sequence_events, pattern.location, n.location);

    pop_path();

    if (result_tv.buffer == BufferAllocator::BUFFER_UNUSED && !result_tv.pattern) {
        error("E101", "Buffer pool exhausted", n.location);
    }

    return result_tv;
}

// ============================================================================
// Phase 2 PRD time/structure transform handlers
// ============================================================================


TypedValue CodeGenerator::handle_palindrome_call(NodeIndex node, const Node& n) {
    // palindrome(pattern) — PRD Phase 4. Each block, EVENT_REORDER(PALINDROME)
    // emits each upstream event twice: forward at (0.5*t, 0.5*dur), reverse at
    // (1 - 0.5*t - 0.5*dur, 0.5*dur). Downstream cycle_length is 2x upstream.
    // OutputEvents capacity is 2x upstream to fit the fanout.
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    if (pattern_arg == NULL_NODE) {
        error("E130", "palindrome() requires a pattern as argument", n.location);
        return TypedValue::void_val();
    }
    if (!is_pattern_node(*ast_, *symbols_, pattern_arg)) {
        error("E133", "palindrome() argument must be a pattern", n.location);
        return TypedValue::void_val();
    }
    return emit_reorder_call(node, n, "palindrome",
                             cedar::EVENT_REORDER_PALINDROME, /*flags=*/0,
                             /*param0=*/0xFFFF, /*param1=*/0xFFFF,
                             /*cycle_length_factor=*/2.0f,
                             /*capacity_factor=*/2u);
}

TypedValue CodeGenerator::handle_ply_call(NodeIndex node, const Node& n) {
    // ply(pattern, n) — PRD Phase 4 runtime form: EVENT_FANOUT(PLY) emits N
    // sub-events per upstream event, each with duration d/N. `n` is a
    // compile-time constant (it directly determines OutputEvents capacity).
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    auto n_arg = get_number_arg(*ast_, n, 1);
    if (pattern_arg == NULL_NODE) {
        error("E130", "ply() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }
    if (!n_arg.has_value() || *n_arg < 1) {
        error("E131", "ply() requires a positive integer (>= 1) as second argument",
              n.location);
        return TypedValue::void_val();
    }
    if (!is_pattern_node(*ast_, *symbols_, pattern_arg)) {
        error("E133", "ply() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }
    std::uint32_t n_int = static_cast<std::uint32_t>(*n_arg);
    std::uint16_t n_buf = emit_push_const(static_cast<float>(n_int));
    if (n_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }
    return emit_fanout_call(node, n, "ply",
                            cedar::EVENT_FANOUT_PLY, n_buf,
                            /*cycle_length_factor=*/1.0f,
                            /*capacity_factor=*/n_int);
}

TypedValue CodeGenerator::handle_linger_call(NodeIndex node, const Node& n) {
    // linger(pattern, frac) — PRD Phase 4 runtime form: EVENT_FANOUT(LINGER)
    // drops events with t >= frac and rescales survivors to [0, 1). The
    // downstream cycle_length is upstream * frac, so the truncated segment
    // loops 1/frac times per upstream cycle (preserves legacy compile-time
    // semantics). frac is signal-rate ok; clamped to (0, 1] at runtime.
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    if (pattern_arg == NULL_NODE) {
        error("E130", "linger() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }
    if (!is_pattern_node(*ast_, *symbols_, pattern_arg)) {
        error("E133", "linger() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }

    auto frac_const = get_number_arg(*ast_, n, 1);
    if (frac_const.has_value() && *frac_const <= 0.0f) {
        error("E131", "linger() requires a positive frac (signal or constant)",
              n.location);
        return TypedValue::void_val();
    }

    std::uint16_t frac_buf = resolve_scalar_or_signal_arg(
        n, 1, "E131",
        "linger() requires a number or signal as second argument (frac)");
    if (frac_buf == BufferAllocator::BUFFER_UNUSED) return TypedValue::void_val();

    // For constants, fold the cycle_length factor at compile time so the
    // PatternPayload reports the correct cycle. For signals, leave the
    // compile-time factor at 1.0 (runtime mutates the SequenceState's
    // cycle_length each block per the LINGER body). This matches the
    // Phase 3 fast/slow precedent.
    const float clf = frac_const.has_value()
                          ? std::min(1.0f, static_cast<float>(*frac_const))
                          : 1.0f;
    return emit_fanout_call(node, n, "linger",
                            cedar::EVENT_FANOUT_LINGER, frac_buf,
                            clf,
                            /*capacity_factor=*/1u);
}

std::uint16_t CodeGenerator::resolve_scalar_or_signal_arg(
    const Node& call, std::size_t idx,
    const char* err_code, const char* err_msg) {

    // Locate the raw AST node at position `idx`.
    NodeIndex arg = call.first_child;
    std::size_t i = 0;
    while (arg != NULL_NODE && i < idx) {
        arg = ast_->arena[arg].next_sibling;
        i++;
    }
    if (arg == NULL_NODE) {
        error(err_code, err_msg, call.location);
        return BufferAllocator::BUFFER_UNUSED;
    }
    const Node& arg_node = ast_->arena[arg];
    NodeIndex value_node = (arg_node.type == NodeType::Argument)
                              ? arg_node.first_child : arg;

    // Constant: fold via PUSH_CONST.
    auto const_val = get_number_arg(*ast_, call, idx);
    if (const_val.has_value()) {
        std::uint16_t buf = emit_push_const(static_cast<float>(*const_val));
        if (buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", call.location);
        }
        return buf;
    }

    // Signal: visit and require a Number/Signal-typed buffer.
    TypedValue tv = visit(value_node);
    if (tv.error) {
        return BufferAllocator::BUFFER_UNUSED;
    }
    if (tv.type != ValueType::Signal && tv.type != ValueType::Number) {
        error(err_code, err_msg, call.location);
        return BufferAllocator::BUFFER_UNUSED;
    }
    if (tv.buffer == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", call.location);
    }
    return tv.buffer;
}

TypedValue CodeGenerator::handle_zoom_call(NodeIndex node, const Node& n) {
    // zoom(pattern, start, end) — PRD Phase 4 runtime form: EVENT_REORDER(ZOOM)
    // reads upstream OutputEvents, drops events outside [s, e) and rescales
    // survivors to [0, 1). Both start and end may be signal-rate (sampled at
    // sample 0 each block); degenerate end <= start at runtime → empty output.
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    if (pattern_arg == NULL_NODE) {
        error("E130", "zoom() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }
    if (!is_pattern_node(*ast_, *symbols_, pattern_arg)) {
        error("E133", "zoom() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }

    // Compile-time validation when both are constants — surfaces E132 early.
    auto start_const = get_number_arg(*ast_, n, 1);
    auto end_const = get_number_arg(*ast_, n, 2);
    if (start_const.has_value() && end_const.has_value() &&
        *end_const <= *start_const) {
        error("E132", "zoom() end must be greater than start", n.location);
        return TypedValue::void_val();
    }

    std::uint16_t start_buf = resolve_scalar_or_signal_arg(
        n, 1, "E131",
        "zoom() requires a number or signal as second argument (start)");
    if (start_buf == BufferAllocator::BUFFER_UNUSED) return TypedValue::void_val();
    std::uint16_t end_buf = resolve_scalar_or_signal_arg(
        n, 2, "E131",
        "zoom() requires a number or signal as third argument (end)");
    if (end_buf == BufferAllocator::BUFFER_UNUSED) return TypedValue::void_val();

    return emit_reorder_call(node, n, "zoom",
                             cedar::EVENT_REORDER_ZOOM, /*flags=*/0,
                             start_buf, end_buf,
                             /*cycle_length_factor=*/1.0f,
                             /*capacity_factor=*/1u);
}

TypedValue CodeGenerator::handle_segment_call(NodeIndex node, const Node& n) {
    // segment(pattern, n) — PRD Phase 4 runtime form: EVENT_FANOUT(SEGMENT)
    // samples N evenly-spaced grid points across the cycle and emits one
    // event per grid point, holding the active upstream event at that time
    // (or the latest preceding event). `n` is a compile-time constant —
    // it determines the OutputEvents capacity.
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    auto n_arg = get_number_arg(*ast_, n, 1);
    if (pattern_arg == NULL_NODE) {
        error("E130", "segment() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }
    if (!n_arg.has_value() || *n_arg < 1) {
        error("E131",
              "segment() requires a positive integer (>= 1) as second argument",
              n.location);
        return TypedValue::void_val();
    }
    if (!is_pattern_node(*ast_, *symbols_, pattern_arg)) {
        error("E133", "segment() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }
    std::uint32_t n_int = static_cast<std::uint32_t>(*n_arg);
    std::uint16_t n_buf = emit_push_const(static_cast<float>(n_int));
    if (n_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }
    return emit_fanout_call(node, n, "segment",
                            cedar::EVENT_FANOUT_SEGMENT, n_buf,
                            /*cycle_length_factor=*/1.0f,
                            /*capacity_factor=*/n_int);
}

// iter / iterBack — PRD Phase 4 Commit C runtime form. Both lower to
// EVENT_REORDER with kind=ITER and the direction flag packed into the rate
// byte's high nibble. The runtime opcode reads ctx.global_sample_counter to
// derive cycle_index and rotates upstream events by -dir * (cycle%n)/n.
// `n` is a compile-time integer constant in [1, 255] (caps at a const
// buffer; n is the rotation period, not a continuous parameter).

TypedValue CodeGenerator::handle_iter_call(NodeIndex node, const Node& n) {
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    auto n_arg = get_number_arg(*ast_, n, 1);
    if (pattern_arg == NULL_NODE) {
        error("E130", "iter() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }
    if (!n_arg.has_value() || *n_arg < 1) {
        error("E131", "iter() requires a positive integer (>= 1) as second argument",
              n.location);
        return TypedValue::void_val();
    }
    int n_int = static_cast<int>(*n_arg);
    if (n_int < 1 || n_int > 255) {
        error("E131", "iter() n must be in [1, 255]", n.location);
        return TypedValue::void_val();
    }
    if (!is_pattern_node(*ast_, *symbols_, pattern_arg)) {
        error("E133", "iter() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }
    std::uint16_t n_buf = emit_push_const(static_cast<float>(n_int));
    if (n_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }
    return emit_reorder_call(node, n, "iter",
                             cedar::EVENT_REORDER_ITER, /*flags=*/0,
                             n_buf, /*param1=*/0xFFFF,
                             /*cycle_length_factor=*/1.0f,
                             /*capacity_factor=*/1u);
}

TypedValue CodeGenerator::handle_iter_back_call(NodeIndex node, const Node& n) {
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    auto n_arg = get_number_arg(*ast_, n, 1);
    if (pattern_arg == NULL_NODE) {
        error("E130", "iterBack() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }
    if (!n_arg.has_value() || *n_arg < 1) {
        error("E131",
              "iterBack() requires a positive integer (>= 1) as second argument",
              n.location);
        return TypedValue::void_val();
    }
    int n_int = static_cast<int>(*n_arg);
    if (n_int < 1 || n_int > 255) {
        error("E131", "iterBack() n must be in [1, 255]", n.location);
        return TypedValue::void_val();
    }
    if (!is_pattern_node(*ast_, *symbols_, pattern_arg)) {
        error("E133", "iterBack() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }
    std::uint16_t n_buf = emit_push_const(static_cast<float>(n_int));
    if (n_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }
    return emit_reorder_call(node, n, "iterBack",
                             cedar::EVENT_REORDER_ITER_BACK,
                             /*flags=*/cedar::EVENT_REORDER_ITER_DIR_BACK,
                             n_buf, /*param1=*/0xFFFF,
                             /*cycle_length_factor=*/1.0f,
                             /*capacity_factor=*/1u);
}

// ============================================================================
// Phase 2 PRD voicing helpers
// ============================================================================

// Apply accumulated voicing state on the compiler to the chord events in
// sequence_events. Walks chord_contexts in lockstep with events[0], voices
// the chord progression, and rewrites cedar::Event values[] with the
// resolved frequencies. Phase 2 PRD §5.4.
static void apply_voicing(SequenceCompiler& compiler,
                          std::vector<std::vector<cedar::Event>>& sequence_events) {
    if (sequence_events.empty()) return;
    const auto& contexts = compiler.chord_contexts_root();
    auto& events = sequence_events[0];
    if (contexts.size() != events.size()) return;  // structure mismatch — bail safely

    int anchor = compiler.voicing_anchor();
    if (anchor < 0) anchor = 60;  // PRD §9.3: default c4

    voicing::Mode mode = compiler.voicing_mode();
    // PRD §9.3: anchor without mode → default below (mode_explicit_ false
    // means user set anchor without setting mode — use the default Below).
    // The set_voicing_mode default (Below) already handles this.

    const voicing::VoicingDict* dict = nullptr;
    if (!compiler.voicing_dict_name().empty()) {
        dict = voicing::lookup_voicing(compiler.voicing_dict_name());
    }
    if (dict == nullptr) dict = voicing::lookup_voicing("close");

    // Collect chord specs in sequence order.
    std::vector<voicing::ChordSpec> chords;
    std::vector<std::size_t> chord_indices;
    for (std::size_t i = 0; i < contexts.size(); ++i) {
        if (contexts[i].has_value()) {
            chords.push_back(*contexts[i]);
            chord_indices.push_back(i);
        }
    }
    if (chords.empty()) return;

    auto voiced = voicing::voice_chords(chords, anchor, mode, dict);
    for (std::size_t k = 0; k < chord_indices.size() && k < voiced.size(); ++k) {
        std::size_t ev_idx = chord_indices[k];
        auto& ev = events[ev_idx];
        const auto& notes = voiced[k];
        std::size_t num = std::min(notes.size(), static_cast<std::size_t>(cedar::MAX_VALUES_PER_EVENT));
        ev.num_values = static_cast<std::uint8_t>(num);
        for (std::size_t i = 0; i < num; ++i) {
            float midi_f = static_cast<float>(notes[i]);
            float freq = 440.0f * std::pow(2.0f, (midi_f - 69.0f) / 12.0f);
            ev.values[i] = freq;
            // Per-voice MIDI note for the revoiced chord.
            ev.notes[i] = midi_f;
        }
    }
}

// ============================================================================
// Phase 2 PRD generator handlers (run, binary, binaryN)
// ============================================================================

TypedValue CodeGenerator::handle_run_call(NodeIndex node, const Node& n) {
    auto n_arg = get_number_arg(*ast_, n, 0);
    if (!n_arg.has_value() || *n_arg < 0) {
        error("E131", "run() requires a non-negative integer", n.location);
        return TypedValue::void_val();
    }
    int n_int = static_cast<int>(*n_arg);

    auto events = synth_run_events(n_int);
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    compiler.populate_synthetic(std::move(events));

    std::uint32_t cnt = call_counters_["run"]++;
    push_path("run#" + std::to_string(cnt));
    std::uint32_t state_id = compute_state_id();

    auto sequence_events = compiler.sequence_events();
    float cycle_length = 1.0f;  // cycle_length in beats; default 1 (cycle = beat)

    auto result_tv = emit_pattern_with_state(
        *this, buffers_, state_inits_, required_samples_,
        node_types_, node, state_id, cycle_length,
        compiler, sequence_events, n.location, n.location);

    pop_path();
    if (result_tv.buffer == BufferAllocator::BUFFER_UNUSED && !result_tv.pattern) {
        error("E101", "Buffer pool exhausted", n.location);
    }
    return result_tv;
}

TypedValue CodeGenerator::handle_binary_call(NodeIndex node, const Node& n) {
    auto n_arg = get_number_arg(*ast_, n, 0);
    if (!n_arg.has_value() || *n_arg < 0) {
        error("E131", "binary() requires a non-negative integer", n.location);
        return TypedValue::void_val();
    }
    std::uint32_t n_val = static_cast<std::uint32_t>(*n_arg);
    int bits = compute_binary_bits(n_val);

    auto events = synth_binary_events(n_val, bits);
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    compiler.populate_synthetic(std::move(events));

    std::uint32_t cnt = call_counters_["binary"]++;
    push_path("binary#" + std::to_string(cnt));
    std::uint32_t state_id = compute_state_id();

    auto sequence_events = compiler.sequence_events();
    float cycle_length = 1.0f;  // cycle_length in beats; default 1 (cycle = beat)

    auto result_tv = emit_pattern_with_state(
        *this, buffers_, state_inits_, required_samples_,
        node_types_, node, state_id, cycle_length,
        compiler, sequence_events, n.location, n.location);

    pop_path();
    if (result_tv.buffer == BufferAllocator::BUFFER_UNUSED && !result_tv.pattern) {
        error("E101", "Buffer pool exhausted", n.location);
    }
    return result_tv;
}

TypedValue CodeGenerator::handle_binary_n_call(NodeIndex node, const Node& n) {
    auto n_arg = get_number_arg(*ast_, n, 0);
    auto bits_arg = get_number_arg(*ast_, n, 1);
    if (!n_arg.has_value() || !bits_arg.has_value()) {
        error("E131", "binaryN() requires two non-negative integers (n, bits)", n.location);
        return TypedValue::void_val();
    }
    if (*n_arg < 0 || *bits_arg < 0) {
        error("E131", "binaryN() requires non-negative integers", n.location);
        return TypedValue::void_val();
    }
    int bits = static_cast<int>(*bits_arg);
    std::uint32_t n_val = static_cast<std::uint32_t>(*n_arg);
    if (bits < 32) {
        n_val &= (1u << bits) - 1u;  // PRD §9.2: truncate
    }

    auto events = synth_binary_events(n_val, bits);
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    compiler.populate_synthetic(std::move(events));

    std::uint32_t cnt = call_counters_["binaryN"]++;
    push_path("binaryN#" + std::to_string(cnt));
    std::uint32_t state_id = compute_state_id();

    auto sequence_events = compiler.sequence_events();
    float cycle_length = 1.0f;  // cycle_length in beats; default 1 (cycle = beat)

    auto result_tv = emit_pattern_with_state(
        *this, buffers_, state_inits_, required_samples_,
        node_types_, node, state_id, cycle_length,
        compiler, sequence_events, n.location, n.location);

    pop_path();
    if (result_tv.buffer == BufferAllocator::BUFFER_UNUSED && !result_tv.pattern) {
        error("E101", "Buffer pool exhausted", n.location);
    }
    return result_tv;
}

// ============================================================================
// Phase 2 PRD voicing handlers (anchor, mode, voicing, addVoicings)
// ============================================================================

TypedValue CodeGenerator::handle_anchor_call(NodeIndex node, const Node& n) {
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    auto anchor_str = get_string_arg(*ast_, n, 1);
    if (pattern_arg == NULL_NODE) {
        error("E130", "anchor() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }
    if (!anchor_str.has_value()) {
        error("E131", "anchor() requires a string note name (e.g., \"c4\") as second argument", n.location);
        return TypedValue::void_val();
    }
    auto midi = voicing::parse_anchor(*anchor_str);
    if (!midi.has_value()) {
        error("E140", "anchor() could not parse note name \"" + *anchor_str + "\"", n.location);
        return TypedValue::void_val();
    }
    if (!is_pattern_node(*ast_, *symbols_, pattern_arg)) {
        error("E133", "anchor() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }

    SequenceCompiler compiler(ast_->arena, sample_registry_);
    NodeIndex pattern_node = NULL_NODE;
    const AstArena* pattern_arena = nullptr;
    std::uint32_t num_elements = 1;
    std::vector<std::vector<cedar::Event>> sequence_events;
    float cycle_length = 1.0f;
    if (!compile_pattern_for_transform(*this, *ast_, pattern_arg, sample_registry_,
                                        compiler, pattern_node, pattern_arena, num_elements,
                                        sequence_events, cycle_length)) {
        error("E130", "anchor() failed to compile pattern argument", n.location);
        return TypedValue::void_val();
    }

    compiler.set_voicing_anchor(*midi);
    apply_voicing(compiler, sequence_events);

    std::uint32_t cnt = call_counters_["anchor"]++;
    push_path("anchor#" + std::to_string(cnt));
    std::uint32_t state_id = compute_state_id();

    const Node& pattern = (*pattern_arena)[pattern_node];
    auto result_tv = emit_pattern_with_state(
        *this, buffers_, state_inits_, required_samples_,
        node_types_, node, state_id, cycle_length,
        compiler, sequence_events, pattern.location, n.location);

    pop_path();
    if (result_tv.buffer == BufferAllocator::BUFFER_UNUSED && !result_tv.pattern) {
        error("E101", "Buffer pool exhausted", n.location);
    }
    return result_tv;
}

TypedValue CodeGenerator::handle_mode_call(NodeIndex node, const Node& n) {
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    auto mode_str = get_string_arg(*ast_, n, 1);
    if (pattern_arg == NULL_NODE) {
        error("E130", "mode() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }
    if (!mode_str.has_value()) {
        error("E131", "mode() requires a string mode name (\"below\"/\"above\"/\"duck\"/\"root\") as second argument", n.location);
        return TypedValue::void_val();
    }
    auto m = voicing::parse_mode(*mode_str);
    if (!m.has_value()) {
        error("E140", "mode() unknown mode \"" + *mode_str + "\"; expected below/above/duck/root", n.location);
        return TypedValue::void_val();
    }
    if (!is_pattern_node(*ast_, *symbols_, pattern_arg)) {
        error("E133", "mode() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }

    SequenceCompiler compiler(ast_->arena, sample_registry_);
    NodeIndex pattern_node = NULL_NODE;
    const AstArena* pattern_arena = nullptr;
    std::uint32_t num_elements = 1;
    std::vector<std::vector<cedar::Event>> sequence_events;
    float cycle_length = 1.0f;
    if (!compile_pattern_for_transform(*this, *ast_, pattern_arg, sample_registry_,
                                        compiler, pattern_node, pattern_arena, num_elements,
                                        sequence_events, cycle_length)) {
        error("E130", "mode() failed to compile pattern argument", n.location);
        return TypedValue::void_val();
    }

    compiler.set_voicing_mode(*m);
    apply_voicing(compiler, sequence_events);

    std::uint32_t cnt = call_counters_["mode"]++;
    push_path("mode#" + std::to_string(cnt));
    std::uint32_t state_id = compute_state_id();

    const Node& pattern = (*pattern_arena)[pattern_node];
    auto result_tv = emit_pattern_with_state(
        *this, buffers_, state_inits_, required_samples_,
        node_types_, node, state_id, cycle_length,
        compiler, sequence_events, pattern.location, n.location);

    pop_path();
    if (result_tv.buffer == BufferAllocator::BUFFER_UNUSED && !result_tv.pattern) {
        error("E101", "Buffer pool exhausted", n.location);
    }
    return result_tv;
}

TypedValue CodeGenerator::handle_voicing_call(NodeIndex node, const Node& n) {
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    auto name_str = get_string_arg(*ast_, n, 1);
    if (pattern_arg == NULL_NODE) {
        error("E130", "voicing() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }
    if (!name_str.has_value()) {
        error("E131", "voicing() requires a string dictionary name as second argument", n.location);
        return TypedValue::void_val();
    }
    if (voicing::lookup_voicing(*name_str) == nullptr) {
        error("E141", "voicing() dictionary \"" + *name_str + "\" not registered (use addVoicings to register)", n.location);
        return TypedValue::void_val();
    }
    if (!is_pattern_node(*ast_, *symbols_, pattern_arg)) {
        error("E133", "voicing() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }

    SequenceCompiler compiler(ast_->arena, sample_registry_);
    NodeIndex pattern_node = NULL_NODE;
    const AstArena* pattern_arena = nullptr;
    std::uint32_t num_elements = 1;
    std::vector<std::vector<cedar::Event>> sequence_events;
    float cycle_length = 1.0f;
    if (!compile_pattern_for_transform(*this, *ast_, pattern_arg, sample_registry_,
                                        compiler, pattern_node, pattern_arena, num_elements,
                                        sequence_events, cycle_length)) {
        error("E130", "voicing() failed to compile pattern argument", n.location);
        return TypedValue::void_val();
    }

    compiler.set_voicing_dict(*name_str);
    apply_voicing(compiler, sequence_events);

    std::uint32_t cnt = call_counters_["voicing"]++;
    push_path("voicing#" + std::to_string(cnt));
    std::uint32_t state_id = compute_state_id();

    const Node& pattern = (*pattern_arena)[pattern_node];
    auto result_tv = emit_pattern_with_state(
        *this, buffers_, state_inits_, required_samples_,
        node_types_, node, state_id, cycle_length,
        compiler, sequence_events, pattern.location, n.location);

    pop_path();
    if (result_tv.buffer == BufferAllocator::BUFFER_UNUSED && !result_tv.pattern) {
        error("E101", "Buffer pool exhausted", n.location);
    }
    return result_tv;
}

TypedValue CodeGenerator::handle_add_voicings_call(NodeIndex node, const Node& n) {
    (void)node;
    auto name_str = get_string_arg(*ast_, n, 0);
    if (!name_str.has_value()) {
        error("E131", "addVoicings() requires a name string as first argument", n.location);
        return TypedValue::void_val();
    }
    // Second argument should be a record literal {quality: [intervals], ...}.
    NodeIndex second = NULL_NODE;
    NodeIndex arg = n.first_child;
    int idx = 0;
    while (arg != NULL_NODE) {
        const Node& a = ast_->arena[arg];
        NodeIndex actual = arg;
        if (a.type == NodeType::Argument && a.first_child != NULL_NODE) actual = a.first_child;
        if (idx == 1) { second = actual; break; }
        ++idx;
        arg = a.next_sibling;
    }
    if (second == NULL_NODE) {
        error("E131", "addVoicings() requires a record literal {quality: [intervals], ...} as second argument", n.location);
        return TypedValue::void_val();
    }

    voicing::VoicingDict dict;
    dict.builtin_kind = -1;  // user dict — quality-table only
    const Node& rec = ast_->arena[second];
    if (rec.type != NodeType::RecordLit) {
        error("E131", "addVoicings() second argument must be a record literal", n.location);
        return TypedValue::void_val();
    }
    // Record fields are NodeType::Argument with RecordFieldData attached.
    NodeIndex field = rec.first_child;
    while (field != NULL_NODE) {
        const Node& f = ast_->arena[field];
        if (f.type == NodeType::Argument &&
            std::holds_alternative<Node::RecordFieldData>(f.data)) {
            const auto& fd = std::get<Node::RecordFieldData>(f.data);
            const std::string& key = fd.name;
            NodeIndex val_node = f.first_child;
            if (val_node != NULL_NODE) {
                const Node& vn = ast_->arena[val_node];
                if (vn.type == NodeType::ArrayLit) {
                    std::vector<int> intervals;
                    NodeIndex ele = vn.first_child;
                    while (ele != NULL_NODE) {
                        const Node& en = ast_->arena[ele];
                        if (std::holds_alternative<Node::NumberData>(en.data)) {
                            intervals.push_back(static_cast<int>(en.as_number()));
                        }
                        ele = en.next_sibling;
                    }
                    if (!key.empty()) dict.qualities[key] = std::move(intervals);
                }
            }
        }
        field = f.next_sibling;
    }

    voicing::register_voicing(*name_str, std::move(dict));
    return TypedValue::void_val();
}


TypedValue CodeGenerator::handle_compress_call(NodeIndex node, const Node& n) {
    // compress(pattern, start, end) — PRD Phase 4 runtime form: EVENT_REORDER
    // (COMPRESS) reads upstream OutputEvents and rescales each event's time
    // (and duration) from [0, 1) into [s, e). Both endpoints may be
    // signal-rate; degenerate end <= start → empty output.
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    if (pattern_arg == NULL_NODE) {
        error("E130", "compress() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }
    if (!is_pattern_node(*ast_, *symbols_, pattern_arg)) {
        error("E133", "compress() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }

    auto start_const = get_number_arg(*ast_, n, 1);
    auto end_const = get_number_arg(*ast_, n, 2);
    if (start_const.has_value() && end_const.has_value() &&
        *end_const <= *start_const) {
        error("E132", "compress() end must be greater than start", n.location);
        return TypedValue::void_val();
    }

    std::uint16_t start_buf = resolve_scalar_or_signal_arg(
        n, 1, "E131",
        "compress() requires a number or signal as second argument (start)");
    if (start_buf == BufferAllocator::BUFFER_UNUSED) return TypedValue::void_val();
    std::uint16_t end_buf = resolve_scalar_or_signal_arg(
        n, 2, "E131",
        "compress() requires a number or signal as third argument (end)");
    if (end_buf == BufferAllocator::BUFFER_UNUSED) return TypedValue::void_val();

    return emit_reorder_call(node, n, "compress",
                             cedar::EVENT_REORDER_COMPRESS, /*flags=*/0,
                             start_buf, end_buf,
                             /*cycle_length_factor=*/1.0f,
                             /*capacity_factor=*/1u);
}

TypedValue CodeGenerator::handle_soundfont_call(NodeIndex node, const Node& n) {
    // soundfont(pattern, "file.sf2", preset) - SoundFont playback
    // The pattern provides gate, freq, velocity signals via polyphonic fields.
    // The file is a string literal (SF2 filename) resolved at compile time.
    // The preset is a number literal (index into the SF2's preset list).
    //
    // Usage: pat("c4 e4 g4") |> soundfont(%, "piano.sf2", 0) |> out(%, %)

    // Arg 0: pattern input (from pipe via %)
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    if (pattern_arg == NULL_NODE) {
        error("E130", "soundfont() requires a pattern as first argument "
              "(e.g., pat(\"c4 e4 g4\") |> soundfont(%, \"piano.sf2\", 0))", n.location);
        return TypedValue::void_val();
    }

    // Arg 1: filename (string literal)
    auto filename = get_string_arg(*ast_, n, 1);
    if (!filename.has_value()) {
        error("E131", "soundfont() requires a filename string as second argument "
              "(e.g., \"piano.sf2\")", n.location);
        return TypedValue::void_val();
    }

    // Arg 2: preset index (number literal)
    auto preset_opt = get_number_arg(*ast_, n, 2);
    if (!preset_opt.has_value()) {
        error("E132", "soundfont() requires a preset number as third argument", n.location);
        return TypedValue::void_val();
    }
    int preset_index = static_cast<int>(*preset_opt);

    // Visit pattern argument — this compiles the pattern and creates typed value
    auto pattern_tv = visit(pattern_arg);
    if (pattern_tv.buffer == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Failed to compile pattern argument", n.location);
        return TypedValue::void_val();
    }

    // PRD prd-midi-input §7.1: detect runtime event source upstream (today:
    // midi(); future: anything else that sets is_runtime_event_source on
    // PatternPayload). Phase 5 Commit I removed the legacy EventSourcePayload
    // path — every runtime event stream now rides on PatternPayload with the
    // flag set.
    const bool upstream_is_event_source =
        pattern_tv.pattern && pattern_tv.pattern->is_runtime_event_source;
    const std::uint32_t upstream_seq_state_id = upstream_is_event_source
        ? pattern_tv.pattern->state_id : 0u;

    // Get pattern's fields (gate, freq, vel) from PatternPayload — only the
    // legacy buffer-driven path needs them.
    std::uint16_t gate_buf = 0xFFFF;
    std::uint16_t freq_buf = 0xFFFF;
    std::uint16_t vel_buf  = 0xFFFF;
    std::uint16_t trig_buf = 0xFFFF;
    std::vector<std::uint16_t> freq_per_voice;

    if (!upstream_is_event_source) {
        if (!pattern_tv.pattern) {
            error("E133", "soundfont() first argument must be a pattern that produces "
                  "gate/freq/vel fields", n.location);
            return TypedValue::void_val();
        }

        const auto& pat_fields = pattern_tv.pattern->fields;
        gate_buf = pat_fields[PatternPayload::GATE];
        freq_buf = pat_fields[PatternPayload::FREQ];
        vel_buf  = pat_fields[PatternPayload::VEL];
        trig_buf = pat_fields[PatternPayload::TRIG];

        if (gate_buf == 0xFFFF || freq_buf == 0xFFFF || vel_buf == 0xFFFF) {
            error("E133", "soundfont() pattern is missing required fields (gate, freq, vel)",
                  n.location);
            return TypedValue::void_val();
        }

        const auto& voice_freqs = pattern_tv.pattern->voice_freqs;
        if (voice_freqs.empty()) {
            freq_per_voice.push_back(freq_buf);
        } else {
            freq_per_voice = voice_freqs;
        }
    }

    // soundfont natively dispatches chord polyphony: each chord voice goes to
    // its own SoundFontVoiceState (with its own internal 32-voice allocator),
    // and the per-voice outputs are summed. Clear the E410 tracking entry —
    // poly() is not required for soundfont.
    polyphonic_pattern_nodes_.erase(pattern_arg);

    // Find or assign SF2 slot index (deduplicate by filename)
    std::uint8_t sf_slot = 0;
    bool found_slot = false;
    for (std::size_t i = 0; i < required_soundfonts_.size(); i++) {
        if (required_soundfonts_[i].filename == *filename) {
            sf_slot = static_cast<std::uint8_t>(i);
            found_slot = true;
            break;
        }
    }
    if (!found_slot) {
        sf_slot = static_cast<std::uint8_t>(required_soundfonts_.size());
        required_soundfonts_.push_back(RequiredSoundFont{*filename, preset_index});
    }

    std::uint32_t sf_count = call_counters_["soundfont"]++;
    push_path("soundfont#" + std::to_string(sf_count));

    // PRD prd-midi-input §7.1: MIDI-upstream (event-driven) path. One
    // SOUNDFONT_VOICE with unwired inputs; the opcode resolves events from
    // seq_state_id and uses its internal 32-voice allocator for chord
    // polyphony driven by live note-ons.
    std::vector<std::uint16_t> per_voice_outs;
    if (upstream_is_event_source) {
        std::uint32_t state_id = compute_state_id();

        std::uint16_t out_buf = buffers_.allocate();
        if (out_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            pop_path();
            return TypedValue::void_val();
        }

        cedar::Instruction sf_inst{};
        sf_inst.opcode = cedar::Opcode::SOUNDFONT_VOICE;
        sf_inst.out_buffer = out_buf;
        sf_inst.inputs[0] = 0xFFFF;  // signals event-driven mode to op_soundfont_voice
        sf_inst.inputs[1] = 0xFFFF;
        sf_inst.inputs[2] = 0xFFFF;
        sf_inst.inputs[3] = 0xFFFF;
        sf_inst.inputs[4] = 0xFFFF;
        sf_inst.state_id = state_id;
        sf_inst.rate = sf_slot;
        emit(sf_inst);

        // Tell the host to seed the SoundFontVoiceState with the upstream
        // state_id and the preset index before audio starts.
        StateInitData sf_init;
        sf_init.state_id          = state_id;
        sf_init.type              = StateInitData::Type::SoundfontEvents;
        sf_init.sf_seq_state_id   = upstream_seq_state_id;
        sf_init.sf_preset_idx     = preset_index;
        state_inits_.push_back(std::move(sf_init));

        per_voice_outs.push_back(out_buf);
    } else {
        // Legacy buffer-driven path: emit a constant preset buffer and one
        // SOUNDFONT_VOICE per chord voice, each with its own state_id.
        std::uint16_t preset_buf = emit_push_const(static_cast<float>(preset_index));
        if (preset_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            pop_path();
            return TypedValue::void_val();
        }

        per_voice_outs.reserve(freq_per_voice.size());
        for (std::size_t v = 0; v < freq_per_voice.size(); ++v) {
            push_path("voice#" + std::to_string(v));
            std::uint32_t state_id = compute_state_id();

            std::uint16_t out_buf = buffers_.allocate();
            if (out_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                pop_path();
                pop_path();
                return TypedValue::void_val();
            }

            cedar::Instruction sf_inst{};
            sf_inst.opcode = cedar::Opcode::SOUNDFONT_VOICE;
            sf_inst.out_buffer = out_buf;
            sf_inst.inputs[0] = gate_buf;
            sf_inst.inputs[1] = freq_per_voice[v];
            sf_inst.inputs[2] = vel_buf;
            sf_inst.inputs[3] = preset_buf;
            sf_inst.inputs[4] = trig_buf;
            sf_inst.state_id = state_id;
            sf_inst.rate = sf_slot;
            emit(sf_inst);

            per_voice_outs.push_back(out_buf);
            pop_path();
        }
    }

    // Sum per-voice outputs into one buffer. Single voice = no sum needed.
    std::uint16_t mixed = per_voice_outs[0];
    for (std::size_t v = 1; v < per_voice_outs.size(); ++v) {
        std::uint16_t sum_buf = buffers_.allocate();
        if (sum_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            pop_path();
            return TypedValue::void_val();
        }
        emit(cedar::Instruction::make_binary(cedar::Opcode::ADD, sum_buf,
                                              mixed, per_voice_outs[v]));
        mixed = sum_buf;
    }

    // Chord-polyphony RMS normalization: scale the summed N voices by
    // 1/sqrt(N) so an N-note chord has the same perceived loudness as a
    // single note. Without this, e.g. `c"Am" |> soundfont(@, "gm", 0)` would
    // sum 3 full-amplitude voices and clip in the WAV output stage.
    const std::size_t n_voices = per_voice_outs.size();
    if (n_voices > 1) {
        const float scale = 1.0f / std::sqrt(static_cast<float>(n_voices));
        std::uint16_t scale_buf = emit_push_const(scale);
        if (scale_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            pop_path();
            return TypedValue::void_val();
        }
        std::uint16_t scaled_buf = buffers_.allocate();
        if (scaled_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            pop_path();
            return TypedValue::void_val();
        }
        emit(cedar::Instruction::make_binary(cedar::Opcode::MUL, scaled_buf,
                                              mixed, scale_buf));
        mixed = scaled_buf;
    }

    pop_path();
    return cache_and_return(node, TypedValue::signal(mixed));
}

// sf_voice(file, preset, freq, gate, vel) — single-voice SoundFont player.
//
// Unlike soundfont() (which owns its own 32-voice pool and consumes a
// pattern), sf_voice is an ordinary instrument expression: it takes three
// signal arguments and produces a stereo signal. Used as the instrument body
// of poly(), it gives soundfont playback the same voice-management path as
// every other instrument.
//
//   n"c4 e4 g4" |> poly(@, (f,g,v) -> sf_voice("piano.sf2", 0, f, g, v)) |> out(@, @)
//
// `file` is a string literal — either a path or an alias registered via the
// $soundfont_alias directive. `preset` is a number literal (SF2 preset
// index). freq/gate/vel are arbitrary signal expressions.
TypedValue CodeGenerator::handle_sf_voice_call(NodeIndex node, const Node& n) {
    // Arg 0: file (string literal — path or $soundfont_alias name)
    auto filename = get_string_arg(*ast_, n, 0);
    if (!filename.has_value()) {
        error("E520", "sf_voice() requires a filename string as first argument "
              "(a path or a $soundfont_alias name)", n.location);
        return TypedValue::error_val();
    }

    // Arg 1: preset index (number literal)
    auto preset_opt = get_number_arg(*ast_, n, 1);
    if (!preset_opt.has_value()) {
        error("E521", "sf_voice() requires a preset number as second argument",
              n.location);
        return TypedValue::error_val();
    }
    int preset_index = static_cast<int>(*preset_opt);

    // Args 2/3/4: freq, gate, vel signal expressions.
    NodeIndex freq_arg = get_pattern_arg(*ast_, n, 2);
    NodeIndex gate_arg = get_pattern_arg(*ast_, n, 3);
    NodeIndex vel_arg  = get_pattern_arg(*ast_, n, 4);
    if (freq_arg == NULL_NODE || gate_arg == NULL_NODE || vel_arg == NULL_NODE) {
        error("E522", "sf_voice() requires freq, gate and vel signal arguments "
              "(sf_voice(file, preset, freq, gate, vel))", n.location);
        return TypedValue::error_val();
    }

    TypedValue freq_tv = visit(freq_arg);
    TypedValue gate_tv = visit(gate_arg);
    TypedValue vel_tv  = visit(vel_arg);
    if (freq_tv.buffer == BufferAllocator::BUFFER_UNUSED ||
        gate_tv.buffer == BufferAllocator::BUFFER_UNUSED ||
        vel_tv.buffer  == BufferAllocator::BUFFER_UNUSED) {
        error("E522", "sf_voice() freq/gate/vel arguments must be signals",
              n.location);
        return TypedValue::error_val();
    }

    // Resolve $soundfont_alias entries at compile time (directive precedence,
    // PRD §3.5). Depth-limited so a cyclic alias chain can't hang codegen;
    // unresolved names pass through to the host, which applies runtime
    // --soundfont-alias config and the built-in alias table.
    std::string resolved_file = *filename;
    for (int depth = 0; depth < 8; ++depth) {
        auto it = soundfont_aliases_.find(resolved_file);
        if (it == soundfont_aliases_.end()) break;
        resolved_file = it->second;
    }

    // Find or assign SF2 slot index (deduplicate by resolved filename). The
    // runtime registry loads required_soundfonts_ in source order, so slot N
    // here matches registry slot N (see program_loader.cpp).
    std::uint8_t sf_slot = 0;
    bool found_slot = false;
    for (std::size_t i = 0; i < required_soundfonts_.size(); i++) {
        if (required_soundfonts_[i].filename == resolved_file) {
            sf_slot = static_cast<std::uint8_t>(i);
            found_slot = true;
            break;
        }
    }
    if (!found_slot) {
        sf_slot = static_cast<std::uint8_t>(required_soundfonts_.size());
        required_soundfonts_.push_back(RequiredSoundFont{resolved_file, preset_index});
    }

    std::uint32_t sf_count = call_counters_["sf_voice"]++;
    push_path("sf_voice#" + std::to_string(sf_count));

    // Preset index as a constant input buffer (read at inputs[3]).
    std::uint16_t preset_buf = emit_push_const(static_cast<float>(preset_index));
    if (preset_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return TypedValue::error_val();
    }

    // Allocate the adjacent stereo output pair (L = out_left, R = out_left+1).
    std::uint16_t out_left = buffers_.allocate();
    if (out_left == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return TypedValue::error_val();
    }
    std::uint16_t out_right = buffers_.allocate();
    if (out_right == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return TypedValue::error_val();
    }
    if (out_right != out_left + 1) {
        error("E166", "Internal error: stereo buffer allocation not adjacent",
              n.location);
        pop_path();
        return TypedValue::error_val();
    }

    cedar::Instruction sf_inst{};
    sf_inst.opcode = cedar::Opcode::SF_VOICE;
    sf_inst.out_buffer = out_left;
    sf_inst.inputs[0] = gate_tv.buffer;
    sf_inst.inputs[1] = freq_tv.buffer;
    sf_inst.inputs[2] = vel_tv.buffer;
    sf_inst.inputs[3] = preset_buf;
    sf_inst.inputs[4] = 0xFFFF;
    sf_inst.rate = sf_slot;
    sf_inst.state_id = compute_state_id();
    sf_inst.flags = static_cast<std::uint16_t>(
        cedar::InstructionFlag::STEREO_OUTPUT);
    emit(sf_inst);

    pop_path();

    register_stereo(node, out_left, out_right);
    return cache_and_return(node, TypedValue::stereo_signal(out_left, out_right));
}

// PRD prd-midi-input §4.7 + §7.5: midi() builtin. Emits one MIDI_QUERY with
// four output buffers (gate/freq/vel/trig) baked monophonically from the
// runtime event stream, and records a RequiredMidiSource that the host
// iterates to call vm.init_midi_queue_state after load.
//
// Returns a TypedValue::Pattern with `is_runtime_event_source = true`. Both
// shapes work downstream:
//   * `midi() as e |> osc("sin", e.freq) |> @ * adsr(e.gate)` reads the
//     mono-baked buffers via pattern field access.
//   * `midi() |> poly(@, synth, 8)` and (Phase 7.1) `midi() |> soundfont(...)`
//     read the polyphonic OutputEvents via state_pool_.resolve_output_events.
// handle_poly_call still picks up state_id from pattern->state_id.
TypedValue CodeGenerator::handle_midi_call(NodeIndex node, const Node& n) {
    // §10 Q4 / Non-Goal: midi() top-level only in v1.
    if (user_function_depth_ > 0) {
        error("E412",
              "midi() may only be called at the top level — not inside fn bodies "
              "(per prd-midi-input §10).",
              n.location);
        return TypedValue::void_val();
    }

    // Walk children. Accept 0 or 1 record argument.
    NodeIndex arg = n.first_child;
    NodeIndex options_arg = NULL_NODE;
    std::size_t arg_count = 0;
    while (arg != NULL_NODE) {
        ++arg_count;
        if (arg_count == 1) options_arg = arg;
        arg = ast_->arena[arg].next_sibling;
    }
    if (arg_count > 1) {
        error("E400",
              "midi() takes at most one argument: an options record "
              "(e.g. midi({device: \"name\"}) or midi({file: \"song.mid\"})).",
              n.location);
        return TypedValue::void_val();
    }

    // Defaults
    cedar::MidiSourceKind kind = cedar::MidiSourceKind::DefaultDevice;
    std::string           name_or_path;
    std::uint8_t          channel_filter = 0;
    bool                  loop = false;
    cedar::MidiQueueState::TempoMode tempo_mode =
        cedar::MidiQueueState::TempoMode::Follow;

    if (options_arg != NULL_NODE) {
        // Look up the options schema declared on the builtin.
        const OptionSchema* schema_ptr = nullptr;
        if (const BuiltinInfo* info = lookup_builtin("midi")) {
            schema_ptr = info->find_option_schema(/*param_index=*/0);
        }
        static const OptionSchema empty_schema{};
        const OptionSchema& schema = schema_ptr ? *schema_ptr : empty_schema;

        codegen::OptionsPayload payload =
            codegen::extract_options(ast_->arena, options_arg, schema);

        auto device_opt = payload.get_string("device");
        auto file_opt   = payload.get_string("file");
        auto channel_opt = payload.get_number("channel");
        auto loop_opt   = payload.get_bool("loop");
        auto tempo_opt  = payload.get_string("tempo");

        const bool has_device = device_opt.has_value() && !device_opt->empty();
        const bool has_file   = file_opt.has_value()   && !file_opt->empty();

        if (has_device && has_file) {
            error("E411",
                  "midi(): 'file' and 'device' are mutually exclusive — "
                  "pick one source.",
                  n.location);
            return TypedValue::void_val();
        }

        if (has_file) {
            kind = cedar::MidiSourceKind::File;
            name_or_path.assign(file_opt->data(), file_opt->size());
        } else if (has_device) {
            kind = cedar::MidiSourceKind::NamedDevice;
            name_or_path.assign(device_opt->data(), device_opt->size());
        }

        if (channel_opt.has_value()) {
            double ch = *channel_opt;
            int chi = static_cast<int>(ch);
            if (chi < 0 || chi > 16 ||
                static_cast<double>(chi) != ch) {
                error("E414",
                      "midi(): 'channel' must be an integer in 0..16 "
                      "(0 = any channel, 1..16 = specific MIDI channel).",
                      n.location);
                return TypedValue::void_val();
            }
            channel_filter = static_cast<std::uint8_t>(chi);
        }

        if (loop_opt.has_value()) {
            loop = *loop_opt;
        }

        if (tempo_opt.has_value()) {
            if (*tempo_opt == "follow") {
                tempo_mode = cedar::MidiQueueState::TempoMode::Follow;
            } else if (*tempo_opt == "file") {
                tempo_mode = cedar::MidiQueueState::TempoMode::File;
            } else {
                error("E413",
                      "midi(): 'tempo' must be \"follow\" or \"file\" "
                      "(got \"" + std::string(*tempo_opt) + "\").",
                      n.location);
                return TypedValue::void_val();
            }
        }
    }

    // Unique semantic-id path per call site so hot-swap state preservation
    // matches the right ring across recompiles.
    std::uint32_t midi_count = call_counters_["midi"]++;
    push_path("midi#" + std::to_string(midi_count));
    std::uint32_t state_id = compute_state_id();

    // PRD §7.5: allocate per-block mono buffers. op_midi_query reads these
    // indices off the instruction (inst.inputs[0..3]) and fills them via
    // fill_mono_buffers. All four allocate together — if any fails, signal
    // E101 and skip emission. (Buffer pool exhaustion is extremely rare
    // for a top-level midi() call but we report it cleanly to match the
    // pattern in handle_poly_call.)
    std::uint16_t gate_buf = buffers_.allocate();
    std::uint16_t freq_buf = buffers_.allocate();
    std::uint16_t vel_buf  = buffers_.allocate();
    std::uint16_t trig_buf = buffers_.allocate();
    if (gate_buf == BufferAllocator::BUFFER_UNUSED ||
        freq_buf == BufferAllocator::BUFFER_UNUSED ||
        vel_buf  == BufferAllocator::BUFFER_UNUSED ||
        trig_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted in midi()", n.location);
        pop_path();
        return TypedValue::void_val();
    }

    // Emit one MIDI_QUERY. The four mono buffers ride in inputs[0..3]
    // (treated as output destinations by op_midi_query). Slot 4 is unused.
    // POLY / SOUNDFONT continue to read MidiQueueState.output via
    // state_pool_.resolve_output_events for full polyphonic event access.
    cedar::Instruction midi_inst{};
    midi_inst.opcode = cedar::Opcode::MIDI_QUERY;
    midi_inst.out_buffer = 0xFFFF;
    midi_inst.inputs[0] = gate_buf;
    midi_inst.inputs[1] = freq_buf;
    midi_inst.inputs[2] = vel_buf;
    midi_inst.inputs[3] = trig_buf;
    midi_inst.inputs[4] = 0xFFFF;
    midi_inst.rate = 0;
    midi_inst.state_id = state_id;
    emit(midi_inst);

    // Publish host-facing config. Duplicates by name are NOT collapsed —
    // each call gets its own state_id and its own ring (PRD §3.4).
    required_midi_sources_.push_back(RequiredMidiSource{
        /*state_id=*/state_id,
        /*kind=*/kind,
        /*name_or_path=*/std::move(name_or_path),
        /*channel_filter=*/channel_filter,
        /*loop=*/loop,
        /*tempo_mode=*/tempo_mode,
    });

    pop_path();

    // PRD §7.5: return a Pattern-shaped TypedValue so `as e |> ... e.freq`
    // field access goes through the existing pattern_field() dispatch.
    // is_runtime_event_source signals to consumers (Phase 7.1 soundfont,
    // future migrations) that OutputEvents is the source of truth.
    auto payload = std::make_shared<PatternPayload>();
    payload->state_id                 = state_id;
    payload->cycle_length             = 1.0f;
    payload->max_voices               = 1;
    payload->is_runtime_event_source  = true;
    payload->fields[PatternPayload::FREQ] = freq_buf;
    payload->fields[PatternPayload::VEL]  = vel_buf;
    payload->fields[PatternPayload::TRIG] = trig_buf;
    payload->fields[PatternPayload::GATE] = gate_buf;
    return cache_and_return(node, TypedValue::make_pattern(payload, freq_buf));
}

// PRD prd-midi-input §4.8: midi_cc("name", {...}) — compile-time only.
// Records a RequiredMidiCcRoute entry in `required_midi_cc_routes_`. The
// host MIDI callback walks this list and calls vm.set_param() for matching
// events. No instruction is emitted.
TypedValue CodeGenerator::handle_midi_cc_call(NodeIndex node, const Node& n) {
    // §10 Q4 / Non-Goal: midi_cc() top-level only in v1 (same as midi()).
    if (user_function_depth_ > 0) {
        error("E412",
              "midi_cc() may only be called at the top level — not inside fn bodies "
              "(per prd-midi-input §10).",
              n.location);
        return TypedValue::void_val();
    }

    // Walk children. Require name (string literal) + options record.
    NodeIndex name_arg = NULL_NODE;
    NodeIndex options_arg = NULL_NODE;
    std::size_t arg_count = 0;
    NodeIndex arg = n.first_child;
    while (arg != NULL_NODE) {
        ++arg_count;
        if (arg_count == 1) name_arg = arg;
        else if (arg_count == 2) options_arg = arg;
        arg = ast_->arena[arg].next_sibling;
    }
    if (arg_count != 2) {
        error("E400",
              "midi_cc() takes two arguments: a param name string and an options "
              "record (e.g. midi_cc(\"cutoff\", {cc: 74})).",
              n.location);
        return TypedValue::void_val();
    }

    auto name_opt = get_string_arg(*ast_, n, 0);
    if (!name_opt.has_value() || name_opt->empty()) {
        error("E400",
              "midi_cc(): first argument must be a non-empty string literal "
              "naming the target param() slot.",
              n.location);
        return TypedValue::void_val();
    }
    std::string param_name = *name_opt;
    (void)name_arg;  // suppress unused-warning; the walk above is only for arity

    // Look up the options schema declared on the builtin.
    const OptionSchema* schema_ptr = nullptr;
    if (const BuiltinInfo* info = lookup_builtin("midi_cc")) {
        schema_ptr = info->find_option_schema(/*param_index=*/1);
    }
    static const OptionSchema empty_schema{};
    const OptionSchema& schema = schema_ptr ? *schema_ptr : empty_schema;

    codegen::OptionsPayload payload =
        codegen::extract_options(ast_->arena, options_arg, schema);

    auto cc_opt      = payload.get_number("cc");
    auto channel_opt = payload.get_number("channel");
    auto pb_opt      = payload.get_bool("pb");
    auto at_opt      = payload.get_bool("at");
    auto min_opt     = payload.get_number("min");
    auto max_opt     = payload.get_number("max");
    auto slew_opt    = payload.get_number("slew");

    // Default-cc sentinel is -128 in the schema; treat any negative value as "unset".
    const bool cc_present = cc_opt.has_value() && *cc_opt >= 0.0;
    const bool pb_set     = pb_opt.value_or(false);
    const bool at_set     = at_opt.value_or(false);

    const int set_count = (cc_present ? 1 : 0) + (pb_set ? 1 : 0) + (at_set ? 1 : 0);
    if (set_count == 0) {
        error("E421",
              "midi_cc(): one of cc:, pb:, or at: must be set "
              "(e.g. midi_cc(\"name\", {cc: 74}), {pb: true}, or {at: true}).",
              n.location);
        return TypedValue::void_val();
    }
    if (set_count > 1) {
        error("E420",
              "midi_cc(): exactly one of cc:, pb:, or at: may be set — "
              "they are mutually exclusive.",
              n.location);
        return TypedValue::void_val();
    }

    std::int16_t cc_num = 0;
    if (cc_present) {
        double cv = *cc_opt;
        int cci = static_cast<int>(cv);
        if (cci < 0 || cci > 127 || static_cast<double>(cci) != cv) {
            error("E422",
                  "midi_cc(): 'cc' must be an integer in 0..127.",
                  n.location);
            return TypedValue::void_val();
        }
        cc_num = static_cast<std::int16_t>(cci);
    } else if (pb_set) {
        cc_num = -1;
    } else {
        cc_num = -2;
    }

    std::uint8_t channel_filter = 0;
    if (channel_opt.has_value()) {
        double ch = *channel_opt;
        int chi = static_cast<int>(ch);
        if (chi < 0 || chi > 16 || static_cast<double>(chi) != ch) {
            error("E414",
                  "midi_cc(): 'channel' must be an integer in 0..16 "
                  "(0 = any channel, 1..16 = specific MIDI channel).",
                  n.location);
            return TypedValue::void_val();
        }
        channel_filter = static_cast<std::uint8_t>(chi);
    }

    // PRD §4.8: default range is 0..1 for cc/at and -1..+1 for pb.
    double range_min = pb_set ? -1.0 : 0.0;
    double range_max = 1.0;
    if (min_opt.has_value()) range_min = *min_opt;
    if (max_opt.has_value()) range_max = *max_opt;
    float scale = static_cast<float>(range_max - range_min);
    float bias  = static_cast<float>(range_min);

    float slew_ms = 5.0f;
    if (slew_opt.has_value()) {
        double s = *slew_opt;
        if (s < 0.0) s = 0.0;
        slew_ms = static_cast<float>(s);
    }

    required_midi_cc_routes_.push_back(RequiredMidiCcRoute{
        /*param_name=*/std::move(param_name),
        /*cc_num=*/cc_num,
        /*channel_filter=*/channel_filter,
        /*scale=*/scale,
        /*bias=*/bias,
        /*slew_ms=*/slew_ms,
    });

    // No instruction emitted; the directive is compile-time metadata only.
    return cache_and_return(node, TypedValue::void_val());
}

TypedValue CodeGenerator::handle_input_call(NodeIndex node, const Node& n) {
    // in() — defaults to host UI source.
    // in("mic" | "tab" | "file:NAME") — overrides the source for this compile.
    //
    // Emits a single INPUT instruction that copies ctx.input_left/right into
    // an adjacent buffer pair. Result is a Stereo signal so downstream mono
    // DSP auto-lifts via the universal stereo signal semantics.

    // Validate argument count: 0 or 1.
    std::size_t arg_count = 0;
    NodeIndex arg = n.first_child;
    while (arg != NULL_NODE) {
        ++arg_count;
        arg = ast_->arena[arg].next_sibling;
    }
    if (arg_count > 1) {
        error("E190", "in() takes 0 or 1 arguments (got " +
              std::to_string(arg_count) + ")", n.location);
        return TypedValue::error_val();
    }

    // Optional source string literal (e.g. "mic", "tab", "file:voice.wav").
    std::string source_str;
    if (arg_count == 1) {
        auto src = get_string_arg(*ast_, n, 0);
        if (!src.has_value()) {
            error("E191", "in() argument must be a string literal "
                  "(\"mic\", \"tab\", or \"file:NAME\")", n.location);
            return TypedValue::error_val();
        }
        const std::string& s = *src;
        // Validate the source grammar — fail loud on typos so the user gets a
        // clear diagnostic rather than silent fallback to default.
        bool ok = (s == "mic" || s == "tab")
               || (s.rfind("file:", 0) == 0 && s.size() > 5);
        if (!ok) {
            error("E192", "in() unknown source \"" + s +
                  "\". Expected \"mic\", \"tab\", or \"file:NAME\".",
                  n.location);
            return TypedValue::error_val();
        }
        source_str = s;
    }
    required_input_sources_.push_back(source_str);

    // Allocate adjacent L/R buffer pair (linear allocator guarantees right=left+1).
    std::uint16_t left_buf  = buffers_.allocate();
    std::uint16_t right_buf = buffers_.allocate();
    if (left_buf == BufferAllocator::BUFFER_UNUSED ||
        right_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::error_val();
    }

    // Sanity-check the adjacent-buffer invariant — every other stereo opcode
    // depends on it (PAN, WIDTH, INPUT all read out_buffer+1 for the right slot).
    if (right_buf != left_buf + 1) {
        error("E101", "Internal: input() failed to allocate adjacent stereo "
              "buffer pair", n.location);
        return TypedValue::error_val();
    }

    cedar::Instruction inst{};
    inst.opcode = cedar::Opcode::INPUT;
    inst.out_buffer = left_buf;
    inst.inputs[0] = BufferAllocator::BUFFER_UNUSED;
    inst.inputs[1] = BufferAllocator::BUFFER_UNUSED;
    inst.inputs[2] = BufferAllocator::BUFFER_UNUSED;
    inst.inputs[3] = BufferAllocator::BUFFER_UNUSED;
    inst.inputs[4] = BufferAllocator::BUFFER_UNUSED;
    inst.state_id = 0;  // Stateless
    emit(inst);

    register_stereo(node, left_buf, right_buf);
    return cache_and_return(node, TypedValue::stereo_signal(left_buf, right_buf));
}

TypedValue CodeGenerator::handle_wt_load_call(NodeIndex node, const Node& n) {
    // wt_load("name", "path") — register a wavetable bank for the host to
    // load. Compile-time directive only (emits no audio-time instruction).
    // Bank IDs are assigned in source order: first wt_load = 0, second = 1,
    // etc. The runtime registry MUST be cleared before loading these in
    // the same order so the IDs match.
    (void)node;

    std::size_t arg_count = 0;
    NodeIndex arg = n.first_child;
    while (arg != NULL_NODE) {
        ++arg_count;
        arg = ast_->arena[arg].next_sibling;
    }
    if (arg_count != 2) {
        error("E193",
              "wt_load() takes 2 arguments (name, path) — got "
              + std::to_string(arg_count),
              n.location);
        return TypedValue::error_val();
    }

    auto name_opt = get_string_arg(*ast_, n, 0);
    if (!name_opt.has_value()) {
        error("E194",
              "wt_load() first argument must be a string literal "
              "(bank name, e.g. \"Basic Shapes\")",
              n.location);
        return TypedValue::error_val();
    }
    auto path_opt = get_string_arg(*ast_, n, 1);
    if (!path_opt.has_value()) {
        error("E195",
              "wt_load() second argument must be a string literal "
              "(file path, e.g. \"wavetables/basic.wav\")",
              n.location);
        return TypedValue::error_val();
    }

    // Dedup on name. If the user repeats wt_load("foo", ...) we keep the
    // first declaration (and reuse its ID for subsequent smooch references).
    // A path mismatch is a warning-worthy event but for now we accept the
    // first entry silently; multi-source-file imports are the typical
    // reason for the duplicate.
    for (const auto& w : required_wavetables_) {
        if (w.name == *name_opt) {
            return TypedValue::void_val();
        }
    }

    if (required_wavetables_.size() >= 64) {
        error("E196",
              "wt_load: too many wavetable banks (max 64 per program)",
              n.location);
        return TypedValue::error_val();
    }

    RequiredWavetable rw;
    rw.name = *name_opt;
    rw.path = *path_opt;
    rw.id   = static_cast<int>(required_wavetables_.size());
    required_wavetables_.push_back(std::move(rw));
    return TypedValue::void_val();
}

TypedValue CodeGenerator::handle_samples_call(NodeIndex node, const Node& n) {
    // samples("uri") — declare a sample-bank URI for the host to load.
    // Compile-time directive only (emits no audio-time instruction). The
    // argument must be a string literal; any URI scheme is accepted (the
    // host's URI resolver dispatches based on the scheme prefix).
    (void)node;

    std::size_t arg_count = 0;
    NodeIndex arg = n.first_child;
    while (arg != NULL_NODE) {
        ++arg_count;
        arg = ast_->arena[arg].next_sibling;
    }
    if (arg_count != 1) {
        error("E230",
              "samples() takes 1 argument (uri) — got "
              + std::to_string(arg_count),
              n.location);
        return TypedValue::error_val();
    }

    auto uri_opt = get_string_arg(*ast_, n, 0);
    if (!uri_opt.has_value()) {
        error("E231",
              "samples() argument must be a string literal "
              "(URI, e.g. \"github:tidalcycles/Dirt-Samples\")",
              n.location);
        return TypedValue::error_val();
    }

    if (uri_opt->empty()) {
        error("E232",
              "samples() URI cannot be empty",
              n.location);
        return TypedValue::error_val();
    }

    // Dedup: same URI declared twice in source is a no-op.
    for (const auto& u : required_uris_) {
        if (u.kind == UriKind::SampleBank && u.uri == *uri_opt) {
            return TypedValue::void_val();
        }
    }

    UriRequest req;
    req.uri  = *uri_opt;
    req.kind = UriKind::SampleBank;
    required_uris_.push_back(std::move(req));
    return TypedValue::void_val();
}

TypedValue CodeGenerator::handle_smooch_call(NodeIndex node, const Node& n) {
    // smooch("bank_name", freq, phase?, tablePos?) — wavetable oscillator.
    // First arg is a string literal naming a bank previously declared via
    // wt_load(). The bank ID is resolved at compile time and packed into
    // inst.rate. Remaining args are audio-rate signals (phase and tablePos
    // default to BUFFER_UNUSED → 0 via get_input_or_zero in the opcode).

    // Count args
    std::size_t arg_count = 0;
    NodeIndex arg = n.first_child;
    while (arg != NULL_NODE) {
        ++arg_count;
        arg = ast_->arena[arg].next_sibling;
    }
    if (arg_count < 2 || arg_count > 4) {
        error("E197",
              "smooch() takes 2-4 arguments (\"bank\", freq, phase?, tablePos?) — got "
              + std::to_string(arg_count),
              n.location);
        return TypedValue::error_val();
    }

    auto bank_name = get_string_arg(*ast_, n, 0);
    if (!bank_name.has_value()) {
        error("E198",
              "smooch() first argument must be a string literal (bank name "
              "from a wt_load() call, e.g. smooch(\"morph\", freq))",
              n.location);
        return TypedValue::error_val();
    }

    // Resolve bank name → ID via the required_wavetables_ list. The user
    // must call wt_load("name", "path") earlier in source.
    int bank_id = -1;
    for (const auto& w : required_wavetables_) {
        if (w.name == *bank_name) {
            bank_id = w.id;
            break;
        }
    }
    if (bank_id < 0) {
        error("E199",
              "smooch(\"" + *bank_name + "\", ...) references an unknown wavetable. "
              "Add wt_load(\"" + *bank_name + "\", \"path/to/wavetable.wav\") "
              "before this call.",
              n.location);
        return TypedValue::error_val();
    }

    // Compile each signal arg via the generic visit() pipeline. Position 0
    // was the bank-name string and is skipped.
    auto compile_signal = [&](std::size_t arg_index, std::uint16_t& out_buf,
                                bool required) -> bool {
        NodeIndex sig_arg = arg_index < arg_count
                          ? get_pattern_arg(*ast_, n, arg_index)
                          : NULL_NODE;
        if (sig_arg == NULL_NODE) {
            if (required) {
                error("E197",
                      "smooch() argument " + std::to_string(arg_index)
                      + " is required",
                      n.location);
                return false;
            }
            out_buf = BufferAllocator::BUFFER_UNUSED;
            return true;
        }
        TypedValue tv = visit(sig_arg);
        if (tv.error) return false;
        if (tv.type != ValueType::Signal && tv.type != ValueType::Number) {
            error("E199",
                  "smooch() argument " + std::to_string(arg_index)
                  + " must be a signal or number, got "
                  + value_type_name(tv.type),
                  n.location);
            return false;
        }
        out_buf = tv.buffer;
        return true;
    };

    std::uint16_t freq_buf  = BufferAllocator::BUFFER_UNUSED;
    std::uint16_t phase_buf = BufferAllocator::BUFFER_UNUSED;
    std::uint16_t pos_buf   = BufferAllocator::BUFFER_UNUSED;
    if (!compile_signal(1, freq_buf,  /*required=*/true))  return TypedValue::error_val();
    if (!compile_signal(2, phase_buf, /*required=*/false)) return TypedValue::error_val();
    if (!compile_signal(3, pos_buf,   /*required=*/false)) return TypedValue::error_val();

    // Allocate the per-voice state ID using the standard semantic-path
    // hashing (matches the convention used by every other stateful opcode
    // — preserves phase across hot-swap when the same smooch call survives
    // a recompile).
    std::uint32_t smooch_count = call_counters_["smooch"]++;
    push_path("smooch#" + std::to_string(smooch_count));
    std::uint32_t state_id = compute_state_id();
    pop_path();

    std::uint16_t out_buf = buffers_.allocate();
    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::error_val();
    }

    cedar::Instruction inst{};
    inst.opcode    = cedar::Opcode::OSC_WAVETABLE;
    inst.out_buffer = out_buf;
    inst.inputs[0] = freq_buf;
    inst.inputs[1] = phase_buf;
    inst.inputs[2] = pos_buf;
    inst.inputs[3] = BufferAllocator::BUFFER_UNUSED;
    inst.inputs[4] = BufferAllocator::BUFFER_UNUSED;
    inst.state_id  = state_id;
    inst.rate      = static_cast<std::uint8_t>(bank_id);
    emit(inst);

    return cache_and_return(node, TypedValue::signal(out_buf));
}

} // namespace akkado
