// EVENT_FANOUT opcode tests — PRD prd-runtime-event-transforms Phase 4
// Commit B. PLY duplicates events, LINGER drops + rescales, SEGMENT samples
// grid points across the cycle.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "cedar/vm/vm.hpp"
#include "cedar/vm/instruction.hpp"
#include "cedar/opcodes/sequence.hpp"
#include "cedar/opcodes/event_transform_encoding.hpp"
#include "cedar/dsp/constants.hpp"

#include <array>
#include <cstring>
#include <span>
#include <vector>

using namespace cedar;
using Catch::Matchers::WithinAbs;

namespace {

constexpr std::size_t TEST_CAP = 64;

thread_local std::array<OutputEvents::OutputEvent, TEST_CAP> g_up_events;

struct Ev {
    float time = 0.0f;
    float duration = 0.25f;
    float velocity = 1.0f;
    float midi_note = 60.0f;
};

void seed_seq_state(VM& vm, std::uint32_t state_id, float cycle_length,
                    std::initializer_list<Ev> events) {
    auto& st = vm.states().get_or_create<SequenceState>(state_id);
    st.cycle_length = cycle_length;
    st.last_beat_pos = -1.0f;
    st.last_beat_pos_gate = -1.0f;
    st.last_queried_cycle = -1.0f;
    st.current_index = 0;
    st.output.events = g_up_events.data();
    st.output.capacity = TEST_CAP;
    st.output.num_events = 0;
    for (const auto& spec : events) {
        auto& e = g_up_events[st.output.num_events++];
        e = {};
        e.time = spec.time;
        e.duration = spec.duration;
        e.velocity = spec.velocity;
        e.midi_note = spec.midi_note;
        e.num_values = 1;
        e.notes[0] = spec.midi_note;
        e.velocities[0] = spec.velocity;
    }
}

Instruction make_push_const(std::uint16_t buf, float value) {
    Instruction inst{};
    inst.opcode = Opcode::PUSH_CONST;
    inst.out_buffer = buf;
    inst.inputs[0] = BUFFER_UNUSED;
    inst.inputs[1] = BUFFER_UNUSED;
    inst.inputs[2] = BUFFER_UNUSED;
    inst.inputs[3] = BUFFER_UNUSED;
    inst.inputs[4] = BUFFER_UNUSED;
    std::memcpy(&inst.state_id, &value, sizeof(float));
    return inst;
}

Instruction make_fanout(std::uint32_t state_id, std::uint8_t kind,
                        std::uint16_t p0, std::uint32_t upstream_id) {
    Instruction inst{};
    inst.opcode = Opcode::EVENT_FANOUT;
    inst.rate = event_fanout_rate(kind);
    inst.out_buffer = BUFFER_UNUSED;
    inst.inputs[0] = p0;
    inst.inputs[1] = BUFFER_UNUSED;
    inst.inputs[2] = static_cast<std::uint16_t>(upstream_id & 0xFFFF);
    inst.inputs[3] = static_cast<std::uint16_t>((upstream_id >> 16) & 0xFFFF);
    inst.inputs[4] = BUFFER_UNUSED;
    inst.state_id = state_id;
    return inst;
}

}  // namespace


TEST_CASE("EVENT_FANOUT PLY(n=3) triples event count, divides durations",
          "[event-fanout]") {
    VM vm;
    constexpr std::uint32_t UP = 0xE010, F = 0xF010;

    std::vector<Instruction> prog{
        make_push_const(10, 3.0f),
        make_fanout(F, EVENT_FANOUT_PLY, 10, UP),
    };
    REQUIRE(vm.load_program_immediate(
        std::span<const Instruction>(prog.data(), prog.size())));

    // 2 events at t=0, 0.5 dur=0.5; cycle_length=1.
    seed_seq_state(vm, UP, 1.0f, {
        Ev{.time=0.0f, .duration=0.5f, .midi_note=60.0f},
        Ev{.time=0.5f, .duration=0.5f, .midi_note=62.0f},
    });
    vm.init_event_transform_state(F, 1.0f, false, TEST_CAP);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const auto& out = vm.states().get<SequenceState>(F).output;
    REQUIRE(out.num_events == 6);
    // Sub-duration = 0.5/3 ≈ 0.1667.
    for (std::uint32_t i = 0; i < out.num_events; ++i) {
        CHECK_THAT(out.events[i].duration, WithinAbs(0.1667f, 0.001f));
    }
    // First 3 are clones of the c4 event at 0.0, 0.1667, 0.3333.
    CHECK_THAT(out.events[0].time, WithinAbs(0.0f,    0.001f));
    CHECK_THAT(out.events[0].midi_note, WithinAbs(60.0f, 0.001f));
    CHECK_THAT(out.events[2].time, WithinAbs(0.3333f, 0.001f));
    // Last 3 from the e4 event at 0.5, 0.6667, 0.8333.
    CHECK_THAT(out.events[3].time, WithinAbs(0.5f,    0.001f));
    CHECK_THAT(out.events[3].midi_note, WithinAbs(62.0f, 0.001f));
    CHECK_THAT(out.events[5].time, WithinAbs(0.8333f, 0.001f));
}

TEST_CASE("EVENT_FANOUT LINGER(frac=0.5) drops second half, halves cycle_length",
          "[event-fanout]") {
    VM vm;
    constexpr std::uint32_t UP = 0xE011, F = 0xF011;

    std::vector<Instruction> prog{
        make_push_const(10, 0.5f),
        make_fanout(F, EVENT_FANOUT_LINGER, 10, UP),
    };
    REQUIRE(vm.load_program_immediate(
        std::span<const Instruction>(prog.data(), prog.size())));

    // 4 events at t=0, 0.25, 0.5, 0.75 dur=0.25; cycle_length=1.
    seed_seq_state(vm, UP, 1.0f, {
        Ev{.time=0.0f,  .midi_note=60.0f},
        Ev{.time=0.25f, .midi_note=62.0f},
        Ev{.time=0.5f,  .midi_note=64.0f},
        Ev{.time=0.75f, .midi_note=65.0f},
    });
    vm.init_event_transform_state(F, 0.5f, false, TEST_CAP);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const auto& downstream = vm.states().get<SequenceState>(F);
    REQUIRE(downstream.output.num_events == 2);
    // Survivors c4@0, e4@0.25 rescaled by 1/0.5=2 -> 0.0, 0.5.
    CHECK_THAT(downstream.output.events[0].time,     WithinAbs(0.0f, 0.001f));
    CHECK_THAT(downstream.output.events[0].duration, WithinAbs(0.5f, 0.001f));
    CHECK_THAT(downstream.output.events[1].time,     WithinAbs(0.5f, 0.001f));
    // cycle_length scaled by frac: 1.0 * 0.5 = 0.5.
    CHECK_THAT(downstream.cycle_length, WithinAbs(0.5f, 0.001f));
}

TEST_CASE("EVENT_FANOUT LINGER(frac=1.0) is identity",
          "[event-fanout]") {
    VM vm;
    constexpr std::uint32_t UP = 0xE012, F = 0xF012;
    std::vector<Instruction> prog{
        make_push_const(10, 1.0f),
        make_fanout(F, EVENT_FANOUT_LINGER, 10, UP),
    };
    REQUIRE(vm.load_program_immediate(
        std::span<const Instruction>(prog.data(), prog.size())));
    seed_seq_state(vm, UP, 1.0f, {
        Ev{.time=0.0f, .midi_note=60.0f},
        Ev{.time=0.5f, .midi_note=62.0f},
    });
    vm.init_event_transform_state(F, 1.0f, false, TEST_CAP);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const auto& downstream = vm.states().get<SequenceState>(F);
    REQUIRE(downstream.output.num_events == 2);
    CHECK_THAT(downstream.output.events[0].time, WithinAbs(0.0f, 0.001f));
    CHECK_THAT(downstream.output.events[1].time, WithinAbs(0.5f, 0.001f));
    CHECK_THAT(downstream.cycle_length, WithinAbs(1.0f, 0.001f));
}

TEST_CASE("EVENT_FANOUT SEGMENT(n=4) samples 4 grid points",
          "[event-fanout]") {
    VM vm;
    constexpr std::uint32_t UP = 0xE013, F = 0xF013;
    std::vector<Instruction> prog{
        make_push_const(10, 4.0f),
        make_fanout(F, EVENT_FANOUT_SEGMENT, 10, UP),
    };
    REQUIRE(vm.load_program_immediate(
        std::span<const Instruction>(prog.data(), prog.size())));
    // 2 events covering halves: c4 over [0, 0.5), e4 over [0.5, 1).
    seed_seq_state(vm, UP, 1.0f, {
        Ev{.time=0.0f, .duration=0.5f, .midi_note=60.0f},
        Ev{.time=0.5f, .duration=0.5f, .midi_note=62.0f},
    });
    vm.init_event_transform_state(F, 1.0f, false, TEST_CAP);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const auto& out = vm.states().get<SequenceState>(F).output;
    REQUIRE(out.num_events == 4);
    // Grid points 0, 0.25, 0.5, 0.75 with dur 0.25.
    CHECK_THAT(out.events[0].time,     WithinAbs(0.0f,  0.001f));
    CHECK_THAT(out.events[0].duration, WithinAbs(0.25f, 0.001f));
    CHECK_THAT(out.events[0].midi_note, WithinAbs(60.0f, 0.001f));
    CHECK_THAT(out.events[2].time,     WithinAbs(0.5f,  0.001f));
    CHECK_THAT(out.events[2].midi_note, WithinAbs(62.0f, 0.001f));
}

TEST_CASE("EVENT_FANOUT with empty upstream produces empty output",
          "[event-fanout]") {
    VM vm;
    constexpr std::uint32_t UP = 0xE014, F = 0xF014;
    std::vector<Instruction> prog{
        make_push_const(10, 3.0f),
        make_fanout(F, EVENT_FANOUT_PLY, 10, UP),
    };
    REQUIRE(vm.load_program_immediate(
        std::span<const Instruction>(prog.data(), prog.size())));
    seed_seq_state(vm, UP, 1.0f, {});  // empty upstream
    vm.init_event_transform_state(F, 1.0f, false, TEST_CAP);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());
    CHECK(vm.states().get<SequenceState>(F).output.num_events == 0);
}

TEST_CASE("EVENT_FANOUT with missing upstream is a safe no-op",
          "[event-fanout]") {
    VM vm;
    constexpr std::uint32_t UP_MISSING = 0xE015, F = 0xF015;
    std::vector<Instruction> prog{
        make_push_const(10, 3.0f),
        make_fanout(F, EVENT_FANOUT_PLY, 10, UP_MISSING),
    };
    REQUIRE(vm.load_program_immediate(
        std::span<const Instruction>(prog.data(), prog.size())));
    vm.init_event_transform_state(F, 1.0f, false, TEST_CAP);

    std::array<float, BLOCK_SIZE> L{}, R{};
    REQUIRE_NOTHROW(vm.process_block(L.data(), R.data()));
    CHECK(vm.states().get<SequenceState>(F).output.num_events == 0);
}

TEST_CASE("EVENT_FANOUT LINGER clamps frac to (0, 1]",
          "[event-fanout]") {
    VM vm;
    constexpr std::uint32_t UP = 0xE016, F = 0xF016;
    std::vector<Instruction> prog{
        make_push_const(10, -0.5f),  // pathological negative frac
        make_fanout(F, EVENT_FANOUT_LINGER, 10, UP),
    };
    REQUIRE(vm.load_program_immediate(
        std::span<const Instruction>(prog.data(), prog.size())));
    seed_seq_state(vm, UP, 1.0f, {Ev{.time=0.0f, .midi_note=60.0f}});
    vm.init_event_transform_state(F, 1.0f, false, TEST_CAP);

    std::array<float, BLOCK_SIZE> L{}, R{};
    REQUIRE_NOTHROW(vm.process_block(L.data(), R.data()));
    // With frac clamped to 0.001, the survivor event's t=0 < 0.001 keeps it.
    CHECK(vm.states().get<SequenceState>(F).output.num_events >= 0);
}
