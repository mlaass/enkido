// Round-trip test for the state-init buffer codec. Pins the wire format
// declared in akkado/include/akkado/state_init_buffer.hpp (PRD
// prd-compile-off-audio-thread §5).
//
// Strategy: compile a small program with akkado::compile() to get a real
// CompileResult, pack it via the C++ packer, then apply the buffer to a
// fresh VM. Compare to a "control" VM that runs the legacy in-WASM apply
// path (which mirrors what cedar_apply_state_inits did pre-PRD).

#include <catch2/catch_test_macros.hpp>

#include <akkado/akkado.hpp>
#include <akkado/state_init_buffer.hpp>

#include <cedar/vm/vm.hpp>
#include <cedar/opcodes/sequence.hpp>
#include <cedar/opcodes/midi.hpp>

#include <cstring>
#include <span>
#include <vector>

namespace {

// Re-implements the legacy pre-PRD apply path so the test can compare it
// against the new buffer-driven path. Mirrors the body of
// `cedar_apply_state_inits` in web/wasm/nkido_wasm.cpp pre-rewire.
std::uint32_t apply_state_inits_legacy(cedar::VM& vm,
                                       const akkado::CompileResult& cr) {
    std::uint32_t count = 0;
    for (const auto& init : cr.state_inits) {
        using T = akkado::StateInitData::Type;
        switch (init.type) {
        case T::SequenceProgram: {
            std::vector<cedar::Sequence> seq_copy = init.sequences;
            for (std::size_t i = 0;
                 i < seq_copy.size() && i < init.sequence_events.size(); ++i) {
                if (!init.sequence_events[i].empty()) {
                    seq_copy[i].events = const_cast<cedar::Event*>(
                        init.sequence_events[i].data());
                    seq_copy[i].num_events =
                        static_cast<std::uint32_t>(init.sequence_events[i].size());
                    seq_copy[i].capacity = seq_copy[i].num_events;
                }
            }
            vm.init_sequence_program_state(init.state_id, seq_copy.data(),
                                           seq_copy.size(), init.cycle_length,
                                           init.is_sample_pattern,
                                           init.total_events);
            ++count;
            break;
        }
        case T::PolyAlloc:
            vm.init_poly_state(init.state_id, init.poly_seq_state_id,
                               init.poly_max_voices, init.poly_mode,
                               init.poly_steal_strategy,
                               init.poly_release_seconds,
                               init.poly_prop_count, init.poly_prop_defaults);
            ++count;
            break;
        case T::ForeachAlloc:
            vm.init_foreach_state(init.state_id, init.foreach_allocator_kind,
                                  init.foreach_block_id,
                                  init.foreach_event_src_state_id,
                                  init.foreach_max_iterations,
                                  init.poly_max_voices, init.poly_mode,
                                  init.poly_steal_strategy,
                                  init.poly_release_seconds,
                                  init.poly_prop_count,
                                  init.poly_prop_defaults);
            ++count;
            break;
        case T::ExtendedParams:
            vm.init_extended_params(init.state_id,
                                    init.ext_constants.data(),
                                    init.ext_buffer_indices.data(),
                                    init.ext_count);
            ++count;
            break;
        case T::Timeline: {
            auto& state = vm.states().get_or_create<cedar::TimelineState>(init.state_id);
            state.num_points = std::min(
                static_cast<std::uint32_t>(init.timeline_breakpoints.size()),
                static_cast<std::uint32_t>(cedar::TimelineState::MAX_BREAKPOINTS));
            for (std::uint32_t i = 0; i < state.num_points; ++i) {
                state.points[i] = init.timeline_breakpoints[i];
            }
            state.loop = init.timeline_loop;
            state.loop_length = init.timeline_loop_length;
            ++count;
            break;
        }
        case T::EventTransform:
        case T::Reorder:
        case T::Fanout:
            vm.init_event_transform_state(init.state_id, init.cycle_length,
                                          init.is_sample_pattern,
                                          init.total_events);
            ++count;
            break;
        case T::RateScale:
            vm.init_event_rate_scale_state(init.state_id);
            ++count;
            break;
#ifndef CEDAR_NO_SOUNDFONT
        case T::SoundfontEvents:
            vm.init_soundfont_voice_event_state(init.state_id,
                                                init.sf_seq_state_id,
                                                init.sf_preset_idx);
            ++count;
            break;
#else
        case T::SoundfontEvents: break;
#endif
        }
    }
    return count;
}

bool load_program_into_vm(cedar::VM& vm, const akkado::CompileResult& cr) {
    const std::size_t inst_size = sizeof(cedar::Instruction);
    if (cr.bytecode.size() % inst_size != 0) return false;
    const auto* instructions =
        reinterpret_cast<const cedar::Instruction*>(cr.bytecode.data());
    std::size_t inst_count = cr.bytecode.size() / inst_size;
    if (!cr.block_table.empty()) {
        vm.set_block_table(cr.block_table, cr.main_instruction_count);
    }
    return vm.load_program_immediate({instructions, inst_count});
}

bool sequence_states_match(cedar::VM& a, cedar::VM& b,
                           std::uint32_t state_id) {
    auto* sa = a.states().get_if<cedar::SequenceState>(state_id);
    auto* sb = b.states().get_if<cedar::SequenceState>(state_id);
    if (!sa || !sb) return false;
    if (sa->num_sequences != sb->num_sequences) return false;
    if (sa->cycle_length != sb->cycle_length) return false;
    if (sa->is_sample_pattern != sb->is_sample_pattern) return false;
    for (std::uint32_t i = 0; i < sa->num_sequences; ++i) {
        const auto& seqa = sa->sequences[i];
        const auto& seqb = sb->sequences[i];
        if (seqa.num_events != seqb.num_events) return false;
        if (seqa.duration != seqb.duration) return false;
        if (seqa.mode != seqb.mode) return false;
        for (std::uint32_t e = 0; e < seqa.num_events; ++e) {
            if (std::memcmp(&seqa.events[e], &seqb.events[e],
                            sizeof(cedar::Event)) != 0) {
                return false;
            }
        }
    }
    return true;
}

bool poly_states_match(cedar::VM& a, cedar::VM& b, std::uint32_t state_id) {
    auto* pa = a.states().get_if<cedar::PolyAllocState>(state_id);
    auto* pb = b.states().get_if<cedar::PolyAllocState>(state_id);
    if (!pa || !pb) return false;
    if (pa->seq_state_id != pb->seq_state_id) return false;
    if (pa->max_voices  != pb->max_voices)  return false;
    if (pa->mode        != pb->mode)        return false;
    if (pa->steal_strategy != pb->steal_strategy) return false;
    if (pa->release_window_samples != pb->release_window_samples) return false;
    if (pa->prop_count  != pb->prop_count)  return false;
    for (std::size_t i = 0; i < cedar::MAX_PROPS_PER_EVENT; ++i) {
        if (pa->prop_defaults[i] != pb->prop_defaults[i]) return false;
    }
    return true;
}

} // namespace

TEST_CASE("state-init buffer round-trip — simple pattern", "[state_init_codec]") {
    auto cr = akkado::compile(R"(out(osc("sin", "c4 e4 g4"))
)", "<test>");
    REQUIRE(cr.success);

    cedar::VM legacy_vm;
    cedar::VM buffer_vm;
    REQUIRE(load_program_into_vm(legacy_vm, cr));
    REQUIRE(load_program_into_vm(buffer_vm, cr));

    auto legacy_count = apply_state_inits_legacy(legacy_vm, cr);

    auto buf = akkado::state_init_buffer::pack_state_inits(
        cr.state_inits, cr.scalar_sample_mappings);
    auto buffer_count = akkado::state_init_buffer::apply_state_inits(
        buffer_vm, buf.data(), static_cast<std::uint32_t>(buf.size()));

    REQUIRE(buffer_count >= 0);
    REQUIRE(static_cast<std::uint32_t>(buffer_count) == legacy_count);

    // Spot-check every SequenceProgram state matches between the two VMs.
    for (const auto& init : cr.state_inits) {
        if (init.type == akkado::StateInitData::Type::SequenceProgram) {
            REQUIRE(sequence_states_match(legacy_vm, buffer_vm, init.state_id));
        }
    }
}

TEST_CASE("state-init buffer round-trip — poly pattern", "[state_init_codec]") {
    auto cr = akkado::compile(R"(
fn pad({freq, gate, vel}) ->
    saw(freq) * adsr(gate, 0.01, 0.1, 0.7, 0.3) * vel
chord("Cmaj7 Am7") |> poly(@, pad) |> out(@)
)", "<test>");
    REQUIRE(cr.success);

    cedar::VM legacy_vm;
    cedar::VM buffer_vm;
    REQUIRE(load_program_into_vm(legacy_vm, cr));
    REQUIRE(load_program_into_vm(buffer_vm, cr));

    apply_state_inits_legacy(legacy_vm, cr);
    auto buf = akkado::state_init_buffer::pack_state_inits(
        cr.state_inits, cr.scalar_sample_mappings);
    auto applied = akkado::state_init_buffer::apply_state_inits(
        buffer_vm, buf.data(), static_cast<std::uint32_t>(buf.size()));
    REQUIRE(applied >= 0);

    for (const auto& init : cr.state_inits) {
        if (init.type == akkado::StateInitData::Type::SequenceProgram) {
            REQUIRE(sequence_states_match(legacy_vm, buffer_vm, init.state_id));
        } else if (init.type == akkado::StateInitData::Type::PolyAlloc) {
            REQUIRE(poly_states_match(legacy_vm, buffer_vm, init.state_id));
        }
    }
}

TEST_CASE("state-init buffer rejects bad magic", "[state_init_codec]") {
    cedar::VM vm;
    std::vector<std::uint8_t> buf(16, 0xAB);  // garbage
    REQUIRE(akkado::state_init_buffer::apply_state_inits(
                vm, buf.data(),
                static_cast<std::uint32_t>(buf.size())) == -1);
    REQUIRE(akkado::state_init_buffer::resolve_sample_ids(
                vm, buf.data(),
                static_cast<std::uint32_t>(buf.size())) == -1);
}

TEST_CASE("midi sources buffer rejects bad magic", "[state_init_codec]") {
    cedar::VM vm;
    std::vector<std::uint8_t> buf(16, 0xAB);
    REQUIRE(akkado::state_init_buffer::apply_midi_sources(
                vm, buf.data(),
                static_cast<std::uint32_t>(buf.size())) == -1);
}

TEST_CASE("midi sources buffer round-trip", "[state_init_codec]") {
    // Synthesize two RequiredMidiSource entries by hand; no MIDI builtin
    // in akkado compiles down to two yet on every host configuration, so
    // skip the akkado::compile dance here.
    std::vector<akkado::RequiredMidiSource> sources;
    {
        akkado::RequiredMidiSource s{};
        s.state_id       = 0x12345678;
        s.kind           = cedar::MidiSourceKind::DefaultDevice;
        s.name_or_path   = "";
        s.channel_filter = 0;
        s.loop           = false;
        s.tempo_mode     = cedar::MidiQueueState::TempoMode::Follow;
        sources.push_back(s);
    }
    {
        akkado::RequiredMidiSource s{};
        s.state_id       = 0xABCDEF01;
        s.kind           = cedar::MidiSourceKind::NamedDevice;
        s.name_or_path   = "USB MIDI Device 0";
        s.channel_filter = 3;
        s.loop           = true;
        s.tempo_mode     = cedar::MidiQueueState::TempoMode::File;
        sources.push_back(s);
    }

    auto buf = akkado::state_init_buffer::pack_midi_sources(sources);

    cedar::VM vm;
    auto applied = akkado::state_init_buffer::apply_midi_sources(
        vm, buf.data(), static_cast<std::uint32_t>(buf.size()));
    REQUIRE(applied == 2);

    auto* s0 = vm.states().get_if<cedar::MidiQueueState>(0x12345678);
    REQUIRE(s0 != nullptr);
    REQUIRE(s0->kind           == cedar::MidiSourceKind::DefaultDevice);
    REQUIRE(s0->channel_filter == 0);
    REQUIRE(s0->loop           == false);

    auto* s1 = vm.states().get_if<cedar::MidiQueueState>(0xABCDEF01);
    REQUIRE(s1 != nullptr);
    REQUIRE(s1->kind           == cedar::MidiSourceKind::NamedDevice);
    REQUIRE(s1->channel_filter == 3);
    REQUIRE(s1->loop           == true);
    REQUIRE(s1->tempo_mode     == cedar::MidiQueueState::TempoMode::File);
}

TEST_CASE("block table memcpy round-trip", "[state_init_codec]") {
    std::vector<cedar::BlockEntry> table;
    table.push_back(cedar::BlockEntry{1, 8, 3, 1, 0, 16, 4});
    table.push_back(cedar::BlockEntry{9, 12, 2, 2, 0, 32, 8});

    auto buf = akkado::state_init_buffer::pack_block_table(table);
    REQUIRE(buf.size() == 2 * sizeof(cedar::BlockEntry));

    std::vector<cedar::BlockEntry> round(2);
    std::memcpy(round.data(), buf.data(), buf.size());
    REQUIRE(round[0].offset           == 1);
    REQUIRE(round[0].length           == 8);
    REQUIRE(round[0].frame_slot_count == 3);
    REQUIRE(round[0].output_count     == 1);
    REQUIRE(round[0].param_base       == 16);
    REQUIRE(round[0].body_output      == 4);
    REQUIRE(round[1].offset           == 9);
    REQUIRE(round[1].length           == 12);
    REQUIRE(round[1].body_output      == 8);
}
