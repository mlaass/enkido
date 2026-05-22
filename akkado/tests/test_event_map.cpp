// PRD prd-runtime-event-transforms Phase 1 — akkado-side coverage.
//
// Verifies that transpose() / velocity() lower to runtime EVENT_MAP opcodes
// and that the transformed event stream is correct when rendered through the
// Cedar VM. The opcode itself is exhaustively unit-tested in
// cedar/tests/test_event_map.cpp; here we test the codegen lowering + the
// end-to-end runtime result.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "akkado/akkado.hpp"
#include <cedar/vm/instruction.hpp>
#include <cedar/vm/vm.hpp>
#include <cedar/dsp/constants.hpp>
#include <cedar/opcodes/sequence.hpp>
#include <cedar/opcodes/event_transform_encoding.hpp>
#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

std::vector<cedar::Instruction> get_instructions(const akkado::CompileResult& r) {
    std::vector<cedar::Instruction> insts;
    insts.resize(r.bytecode.size() / sizeof(cedar::Instruction));
    if (!insts.empty()) {
        std::memcpy(insts.data(), r.bytecode.data(), r.bytecode.size());
    }
    return insts;
}

std::size_t count_op(const std::vector<cedar::Instruction>& insts,
                     cedar::Opcode op) {
    std::size_t n = 0;
    for (const auto& i : insts) {
        if (i.opcode == op) ++n;
    }
    return n;
}

std::size_t count_init(const akkado::CompileResult& r,
                       akkado::StateInitData::Type type) {
    std::size_t n = 0;
    for (const auto& init : r.state_inits) {
        if (init.type == type) ++n;
    }
    return n;
}

// Owns the VM + the SequenceProgram event storage it points into.
struct RenderHost {
    cedar::VM vm;
    std::vector<std::vector<cedar::Sequence>> seq_storage;
    std::vector<cedar::Instruction> insts;
};

void apply_inits(cedar::VM& vm, const akkado::CompileResult& r,
                 std::vector<std::vector<cedar::Sequence>>& seq_storage) {
    seq_storage.reserve(r.state_inits.size());
    for (const auto& init : r.state_inits) {
        if (init.type == akkado::StateInitData::Type::SequenceProgram) {
            std::vector<cedar::Sequence> seq_copy = init.sequences;
            for (std::size_t i = 0;
                 i < seq_copy.size() && i < init.sequence_events.size(); ++i) {
                if (!init.sequence_events[i].empty()) {
                    seq_copy[i].events = const_cast<cedar::Event*>(
                        init.sequence_events[i].data());
                    seq_copy[i].num_events = static_cast<std::uint32_t>(
                        init.sequence_events[i].size());
                    seq_copy[i].capacity = seq_copy[i].num_events;
                }
            }
            seq_storage.push_back(std::move(seq_copy));
            auto& stored = seq_storage.back();
            vm.init_sequence_program_state(init.state_id, stored.data(),
                                           stored.size(), init.cycle_length,
                                           init.is_sample_pattern,
                                           init.total_events);
        } else if (init.type ==
                   akkado::StateInitData::Type::EventTransform) {
            vm.init_event_transform_state(init.state_id, init.cycle_length,
                                          init.is_sample_pattern,
                                          init.total_events);
        }
    }
}

std::unique_ptr<RenderHost> render(const akkado::CompileResult& r, int blocks) {
    auto host = std::make_unique<RenderHost>();
    host->vm.set_sample_rate(48000.0f);
    host->vm.set_bpm(120.0f);
    host->vm.set_block_table(r.block_table, r.main_instruction_count);
    host->insts = get_instructions(r);
    REQUIRE(host->vm.load_program_immediate(
        std::span<const cedar::Instruction>(host->insts)));
    apply_inits(host->vm, r, host->seq_storage);
    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    for (int b = 0; b < blocks; ++b) {
        host->vm.process_block(L.data(), R.data());
    }
    return host;
}

// The SequenceState the final (outermost) EVENT_MAP publishes its transformed
// OutputEvents into.
const cedar::SequenceState* final_transform_state(RenderHost& host) {
    std::uint32_t id = 0;
    bool found = false;
    for (const auto& i : host.insts) {
        if (i.opcode == cedar::Opcode::EVENT_MAP) {
            id = i.state_id;
            found = true;
        }
    }
    if (!found) return nullptr;
    return host.vm.states().get_if<cedar::SequenceState>(id);
}

// c4 = MIDI 60; equal-tempered frequency.
constexpr float kC4 = 261.6256f;

}  // namespace

TEST_CASE("event-map: transpose() lowers to EVENT_MAP (NOTE_COUPLED/ADD)",
          "[event-map]") {
    auto r = akkado::compile(R"(n"c4" |> transpose(@, 12) |> osc("sin", @.freq) |> out(@))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    REQUIRE(count_op(insts, cedar::Opcode::EVENT_MAP) == 1);

    // One SequenceProgram (the source pattern) + one EventTransform (transpose).
    CHECK(count_init(r, akkado::StateInitData::Type::SequenceProgram) == 1);
    CHECK(count_init(r, akkado::StateInitData::Type::EventTransform) == 1);

    for (const auto& i : insts) {
        if (i.opcode != cedar::Opcode::EVENT_MAP) continue;
        CHECK(i.rate == cedar::event_transform_rate(
                            cedar::EVENT_FIELD_NOTE_COUPLED, cedar::EVENT_OP_ADD));
        CHECK(i.inputs[0] != 0xFFFF);  // PUSH_CONST param buffer
        // Upstream state_id reassembles from inputs[2..3] and is non-zero.
        std::uint32_t up = static_cast<std::uint32_t>(i.inputs[2]) |
                           (static_cast<std::uint32_t>(i.inputs[3]) << 16);
        CHECK(up != 0u);
    }
}

TEST_CASE("event-map: transpose(+12) shifts notes up an octave at runtime",
          "[event-map]") {
    auto r = akkado::compile(R"(n"c4".transpose(12))");
    REQUIRE(r.success);
    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events >= 1);
    const auto& e = st->output.events[0];
    // Mono note: the scalar midi_note field carries the pitch (per-voice
    // notes[] is chord-only). values[0] holds frequency — +12 doubles it.
    CHECK_THAT(e.midi_note, WithinAbs(72.0f, 0.01f));
    CHECK_THAT(e.values[0], WithinAbs(kC4 * 2.0f, 1.0f));
}

TEST_CASE("event-map: transpose(-12) shifts notes down an octave",
          "[event-map]") {
    auto r = akkado::compile(R"(n"c4".transpose(-12))");
    REQUIRE(r.success);
    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events >= 1);
    CHECK_THAT(st->output.events[0].midi_note, WithinAbs(48.0f, 0.01f));
    CHECK_THAT(st->output.events[0].values[0], WithinAbs(kC4 * 0.5f, 1.0f));
}

TEST_CASE("event-map: velocity() lowers to EVENT_MAP (VEL/MUL) and scales",
          "[event-map]") {
    auto r = akkado::compile(R"(n"[c4 e4]".velocity(0.5))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    REQUIRE(count_op(insts, cedar::Opcode::EVENT_MAP) == 1);
    for (const auto& i : insts) {
        if (i.opcode == cedar::Opcode::EVENT_MAP) {
            CHECK(i.rate == cedar::event_transform_rate(
                                cedar::EVENT_FIELD_VEL, cedar::EVENT_OP_MUL));
        }
    }

    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events == 2);
    for (std::uint32_t i = 0; i < st->output.num_events; ++i) {
        // base events carry velocity 1.0; EVENT_MAP scales by 0.5.
        CHECK_THAT(st->output.events[i].velocity, WithinAbs(0.5f, 0.001f));
        CHECK_THAT(st->output.events[i].velocities[0], WithinAbs(0.5f, 0.001f));
    }
}

TEST_CASE("event-map: velocity().transpose() chains two EVENT_MAP opcodes",
          "[event-map]") {
    auto r = akkado::compile(R"(n"c4".velocity(0.7).transpose(7))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    // Fully-chained: one EVENT_MAP per modifier, both runtime.
    REQUIRE(count_op(insts, cedar::Opcode::EVENT_MAP) == 2);
    CHECK(count_init(r, akkado::StateInitData::Type::EventTransform) == 2);

    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events >= 1);
    const auto& e = st->output.events[0];
    // transpose(+7) on top of velocity(0.7): both transforms observed.
    CHECK_THAT(e.midi_note, WithinAbs(67.0f, 0.01f));
    CHECK_THAT(e.velocity, WithinAbs(0.7f, 0.001f));
}

TEST_CASE("event-map: transpose() shifts every voice of a chord",
          "[event-map]") {
    // c"CM" is a 3-voice chord event (C major = MIDI 60,64,67).
    auto r = akkado::compile(R"(c"CM".transpose(12))");
    REQUIRE(r.success);
    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events >= 1);
    const auto& e = st->output.events[0];
    REQUIRE(e.num_values == 3);
    // +12 semitones on every voice; voice count preserved.
    CHECK_THAT(e.notes[0], WithinAbs(72.0f, 0.01f));
    CHECK_THAT(e.notes[1], WithinAbs(76.0f, 0.01f));
    CHECK_THAT(e.notes[2], WithinAbs(79.0f, 0.01f));
    // Frequencies all doubled.
    CHECK_THAT(e.values[0], WithinAbs(kC4 * 2.0f, 1.0f));
}

TEST_CASE("event-map: transpose() is a no-op on sample patterns",
          "[event-map]") {
    auto r = akkado::compile(R"(s"bd sn".transpose(12))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    // transpose on a sample pattern emits no EVENT_MAP — it is a pitch op.
    CHECK(count_op(insts, cedar::Opcode::EVENT_MAP) == 0);
    CHECK(count_init(r, akkado::StateInitData::Type::EventTransform) == 0);
}

TEST_CASE("event-map: velocity() on a sample pattern emits one SAMPLE_PLAY",
          "[event-map]") {
    auto r = akkado::compile(R"(s"bd sn".velocity(0.5))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    REQUIRE(count_op(insts, cedar::Opcode::EVENT_MAP) == 1);
    // query-only upstream emission: the source pattern contributes no readout,
    // so exactly one SAMPLE_PLAY chain exists (the transform's own).
    CHECK(count_op(insts, cedar::Opcode::SAMPLE_PLAY) == 1);
}

TEST_CASE("event-map: signal-valued transpose is rejected in Phase 1",
          "[event-map]") {
    // Phase 1 is constants-only; a signal parameter is an E131 (Phase 3 wires
    // the signal-rate path).
    auto r = akkado::compile(
        R"(lfo = osc("sin", 1)
           n"c4".transpose(lfo))");
    CHECK_FALSE(r.success);
}
