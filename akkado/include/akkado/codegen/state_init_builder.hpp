#pragma once

// StateInitBuilder — PRD prd-codegen-sprawl-cleanup.md Phase 2.
//
// Fluent builder for StateInitData. One factory per StateInitData::Type;
// publish() is the single state_inits_ push site, so adding a field to a
// Type means one setter here + the sites that use it — not N copies of a
// field-by-field construction block.
//
// StateInitData is a flat struct (not a variant), so one builder class with
// typed factories covers every arm; per-Type sub-builder classes would only
// duplicate the setters.

#include "akkado/codegen.hpp"
#include <utility>
#include <vector>

namespace akkado {
namespace codegen {

class StateInitBuilder {
public:
    // ---- one factory per StateInitData::Type -----------------------------
    static StateInitBuilder timeline(std::uint32_t state_id) {
        return StateInitBuilder(StateInitData::Type::Timeline, state_id);
    }
    static StateInitBuilder sequence_program(std::uint32_t state_id) {
        return StateInitBuilder(StateInitData::Type::SequenceProgram, state_id);
    }
    static StateInitBuilder poly_alloc(std::uint32_t state_id) {
        return StateInitBuilder(StateInitData::Type::PolyAlloc, state_id);
    }
    /// Note: pre-fills every ext_buffer_indices slot with 0xFFFF (constant
    /// mode) — slots the caller doesn't set read their 0.0f constant, never
    /// a bogus buffer 0.
    static StateInitBuilder extended_params(std::uint32_t state_id) {
        StateInitBuilder b(StateInitData::Type::ExtendedParams, state_id);
        b.data_.ext_buffer_indices.fill(0xFFFFu);
        return b;
    }
    static StateInitBuilder soundfont_events(std::uint32_t state_id) {
        return StateInitBuilder(StateInitData::Type::SoundfontEvents, state_id);
    }
    static StateInitBuilder foreach_alloc(std::uint32_t state_id) {
        return StateInitBuilder(StateInitData::Type::ForeachAlloc, state_id);
    }
    static StateInitBuilder event_transform(std::uint32_t state_id) {
        return StateInitBuilder(StateInitData::Type::EventTransform, state_id);
    }
    static StateInitBuilder rate_scale(std::uint32_t state_id) {
        return StateInitBuilder(StateInitData::Type::RateScale, state_id);
    }
    static StateInitBuilder reorder(std::uint32_t state_id) {
        return StateInitBuilder(StateInitData::Type::Reorder, state_id);
    }
    static StateInitBuilder fanout(std::uint32_t state_id) {
        return StateInitBuilder(StateInitData::Type::Fanout, state_id);
    }

    // ---- shared / SequenceProgram fields ---------------------------------
    StateInitBuilder& cycle_length(float c) {
        data_.cycle_length = c;
        return *this;
    }
    StateInitBuilder& sequences(std::vector<cedar::Sequence> s) {
        data_.sequences = std::move(s);
        return *this;
    }
    StateInitBuilder& sequence_events(std::vector<std::vector<cedar::Event>> e) {
        data_.sequence_events = std::move(e);
        return *this;
    }
    StateInitBuilder& is_sample_pattern(bool v) {
        data_.is_sample_pattern = v;
        return *this;
    }
    StateInitBuilder& total_events(std::uint32_t n) {
        data_.total_events = n;
        return *this;
    }
    StateInitBuilder& sequence_sample_mappings(std::vector<SequenceSampleMapping> m) {
        data_.sequence_sample_mappings = std::move(m);
        return *this;
    }
    StateInitBuilder& pattern_location(SourceLocation loc) {
        data_.pattern_location = loc;
        return *this;
    }
    StateInitBuilder& ast_json(std::string json) {
        data_.ast_json = std::move(json);
        return *this;
    }

    // ---- Timeline fields --------------------------------------------------
    StateInitBuilder& timeline_breakpoints(
            std::vector<cedar::TimelineState::Breakpoint> bp) {
        data_.timeline_breakpoints = std::move(bp);
        return *this;
    }
    StateInitBuilder& timeline_loop(bool loop, float loop_length) {
        data_.timeline_loop = loop;
        data_.timeline_loop_length = loop_length;
        return *this;
    }

    // ---- PolyAlloc / ForeachAlloc(VOICE_POOL) fields ---------------------
    StateInitBuilder& poly_seq_state_id(std::uint32_t id) {
        data_.poly_seq_state_id = id;
        return *this;
    }
    StateInitBuilder& poly_max_voices(std::uint8_t n) {
        data_.poly_max_voices = n;
        return *this;
    }
    StateInitBuilder& poly_mode(std::uint8_t m) {
        data_.poly_mode = m;
        return *this;
    }
    StateInitBuilder& poly_steal_strategy(std::uint8_t s) {
        data_.poly_steal_strategy = s;
        return *this;
    }
    StateInitBuilder& poly_release_seconds(float s) {
        data_.poly_release_seconds = s;
        return *this;
    }
    StateInitBuilder& poly_props(
            std::uint8_t count,
            const std::array<float, cedar::MAX_PROPS_PER_EVENT>& defaults) {
        data_.poly_prop_count = count;
        for (std::size_t i = 0; i < cedar::MAX_PROPS_PER_EVENT; ++i) {
            data_.poly_prop_defaults[i] = defaults[i];
        }
        return *this;
    }

    // ---- SoundfontEvents fields ------------------------------------------
    StateInitBuilder& sf_seq_state_id(std::uint32_t id) {
        data_.sf_seq_state_id = id;
        return *this;
    }
    StateInitBuilder& sf_preset_idx(int idx) {
        data_.sf_preset_idx = idx;
        return *this;
    }

    // ---- ForeachAlloc fields ---------------------------------------------
    StateInitBuilder& foreach_block_id(std::uint32_t id) {
        data_.foreach_block_id = id;
        return *this;
    }
    StateInitBuilder& foreach_event_src_state_id(std::uint32_t id) {
        data_.foreach_event_src_state_id = id;
        return *this;
    }
    StateInitBuilder& foreach_max_iterations(std::uint16_t n) {
        data_.foreach_max_iterations = n;
        return *this;
    }
    StateInitBuilder& foreach_allocator_kind(std::uint8_t k) {
        data_.foreach_allocator_kind = k;
        return *this;
    }
    StateInitBuilder& foreach_field_slot_count(std::uint8_t n) {
        data_.foreach_field_slot_count = n;
        return *this;
    }
    StateInitBuilder& foreach_output_count(std::uint8_t n) {
        data_.foreach_output_count = n;
        return *this;
    }

    // ---- ExtendedParams fields -------------------------------------------
    /// Set slot i: buffer_idx == 0xFFFF reads `constant`, else the buffer.
    StateInitBuilder& ext_slot(std::size_t i, float constant,
                               std::uint16_t buffer_idx) {
        data_.ext_constants[i] = constant;
        data_.ext_buffer_indices[i] = buffer_idx;
        return *this;
    }
    StateInitBuilder& ext_count(std::uint8_t n) {
        data_.ext_count = n;
        return *this;
    }

    // ---- terminal ---------------------------------------------------------
    /// Push onto cg's state-init list. The single push site
    /// (PRD Phase 2 exit criterion).
    void publish(CodeGenerator& cg) { cg.push_state_init(std::move(data_)); }

    /// Escape hatch for call sites that need the raw struct (e.g. to stash
    /// into a container other than cg's list).
    [[nodiscard]] StateInitData take() { return std::move(data_); }

private:
    StateInitBuilder(StateInitData::Type type, std::uint32_t state_id) {
        data_.type = type;
        data_.state_id = state_id;
    }

    StateInitData data_{};
};

} // namespace codegen
} // namespace akkado
