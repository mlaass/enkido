// Test helper that mirrors `cedar_apply_state_inits` (web/wasm/nkido_wasm.cpp)
// exactly — every StateInitData::Type, not just SequenceProgram. CLI fuzz
// tests that re-init only SequenceProgram miss bugs in the other re-init
// paths; this header exists so a regression test can hit the same surface
// the browser does.
//
// Keep in lockstep with `cedar_apply_state_inits` in web/wasm/nkido_wasm.cpp.
#pragma once

#include "akkado/akkado.hpp"
#include <cedar/opcodes/dsp_state.hpp>
#include <cedar/opcodes/sequence.hpp>
#include <cedar/vm/vm.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace nkido_test {

// Apply EVERY state-init type from the compile result to the VM. The
// `seq_storage` parameter keeps SequenceProgram event vectors alive for the
// lifetime of the run (same shape as the WASM g_compile_result pin).
inline void live_apply_state_inits(
    cedar::VM& vm,
    const akkado::CompileResult& cr,
    std::vector<std::vector<cedar::Sequence>>& seq_storage) {

    for (const auto& init : cr.program.state_inits) {
        using Type = akkado::StateInitData::Type;
        switch (init.type) {
            case Type::SequenceProgram: {
                std::vector<cedar::Sequence> seq_copy = init.sequences;
                for (std::size_t i = 0;
                     i < seq_copy.size() && i < init.sequence_events.size();
                     ++i) {
                    if (!init.sequence_events[i].empty()) {
                        seq_copy[i].events =
                            const_cast<cedar::Event*>(init.sequence_events[i].data());
                        seq_copy[i].num_events =
                            static_cast<std::uint32_t>(init.sequence_events[i].size());
                        seq_copy[i].capacity =
                            static_cast<std::uint32_t>(init.sequence_events[i].size());
                    }
                }
                seq_storage.push_back(std::move(seq_copy));
                auto& stored = seq_storage.back();
                vm.init_sequence_program_state(
                    init.state_id, stored.data(), stored.size(),
                    init.cycle_length, init.is_sample_pattern,
                    init.total_events);
                break;
            }
            case Type::PolyAlloc:
                vm.init_poly_state(
                    init.state_id, init.poly_seq_state_id,
                    init.poly_max_voices, init.poly_mode,
                    init.poly_steal_strategy, init.poly_release_seconds,
                    init.poly_prop_count, init.poly_prop_defaults);
                break;
            case Type::ForeachAlloc:
                vm.init_foreach_state(
                    init.state_id, init.foreach_allocator_kind,
                    init.foreach_block_id,
                    init.foreach_event_src_state_id,
                    init.foreach_max_iterations,
                    init.poly_max_voices, init.poly_mode,
                    init.poly_steal_strategy, init.poly_release_seconds,
                    init.poly_prop_count, init.poly_prop_defaults);
                break;
            case Type::ExtendedParams:
                vm.init_extended_params(
                    init.state_id,
                    init.ext_constants.data(),
                    init.ext_buffer_indices.data(),
                    init.ext_count);
                break;
            case Type::Timeline: {
                auto& state =
                    vm.states().get_or_create<cedar::TimelineState>(init.state_id);
                state.num_points = std::min(
                    static_cast<std::uint32_t>(init.timeline_breakpoints.size()),
                    static_cast<std::uint32_t>(cedar::TimelineState::MAX_BREAKPOINTS));
                for (std::uint32_t i = 0; i < state.num_points; ++i) {
                    state.points[i] = init.timeline_breakpoints[i];
                }
                state.loop = init.timeline_loop;
                state.loop_length = init.timeline_loop_length;
                break;
            }
            case Type::EventTransform:
            case Type::Reorder:
            case Type::Fanout:
                vm.init_event_transform_state(
                    init.state_id, init.cycle_length,
                    init.is_sample_pattern, init.total_events);
                break;
            case Type::RateScale:
                vm.init_event_rate_scale_state(init.state_id);
                break;
            case Type::SoundfontEvents:
                // CEDAR_NO_SOUNDFONT-guarded in WASM; tests don't exercise.
                break;
        }
    }
}

}  // namespace nkido_test
