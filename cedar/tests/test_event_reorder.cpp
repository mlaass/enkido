// EVENT_REORDER opcode tests — PRD prd-runtime-event-transforms Phase 4.
//
// The opcode reads an upstream SequenceState's OutputEvents and publishes
// transformed OutputEvents into its own (transform-owned) SequenceState.
// Dispatch is keyed on (inst.rate & 0x0F) per
// cedar/opcodes/event_transform_encoding.hpp::EVENT_REORDER_*.
//
// These tests build a minimal program with a seeded upstream OutputEvents,
// run the opcode for one block, and verify the downstream OutputEvents
// (count, time, duration, midi_note) match the expected per-kind transform.

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

constexpr std::size_t TEST_CAP = 32;

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
        e.values[0] = 0.0f;
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

Instruction make_reorder(std::uint32_t state_id, std::uint8_t kind,
                         std::uint16_t p0, std::uint16_t p1,
                         std::uint32_t upstream_id, std::uint8_t flags = 0) {
    Instruction inst{};
    inst.opcode = Opcode::EVENT_REORDER;
    inst.rate = event_reorder_rate(kind, flags);
    inst.out_buffer = BUFFER_UNUSED;
    inst.inputs[0] = p0;
    inst.inputs[1] = p1;
    inst.inputs[2] = static_cast<std::uint16_t>(upstream_id & 0xFFFF);
    inst.inputs[3] = static_cast<std::uint16_t>((upstream_id >> 16) & 0xFFFF);
    inst.inputs[4] = BUFFER_UNUSED;
    inst.state_id = state_id;
    return inst;
}

}  // namespace


TEST_CASE("EVENT_REORDER REV mirrors event times around cycle_length",
          "[event-reorder]") {
    VM vm;
    constexpr std::uint32_t UP = 0xC010, REV = 0xD010;

    std::vector<Instruction> prog{ make_reorder(REV, EVENT_REORDER_REV,
        BUFFER_UNUSED, BUFFER_UNUSED, UP) };
    REQUIRE(vm.load_program_immediate(
        std::span<const Instruction>(prog.data(), prog.size())));

    // 4 events at t=0, 0.25, 0.5, 0.75 with dur=0.25; cycle_length=1.
    seed_seq_state(vm, UP, 1.0f, {
        Ev{.time=0.0f,  .midi_note=60.0f},
        Ev{.time=0.25f, .midi_note=62.0f},
        Ev{.time=0.5f,  .midi_note=64.0f},
        Ev{.time=0.75f, .midi_note=65.0f},
    });
    vm.init_event_transform_state(REV, 1.0f, false, TEST_CAP);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const auto& out = vm.states().get<SequenceState>(REV).output;
    REQUIRE(out.num_events == 4);
    // After rev: new_time = 1 - t - dur. Sorted by time.
    // (t=0.0,d=0.25) -> 0.75; (t=0.25) -> 0.5; (t=0.5) -> 0.25; (t=0.75) -> 0.0
    CHECK_THAT(out.events[0].time, WithinAbs(0.0f,  0.001f));
    CHECK_THAT(out.events[1].time, WithinAbs(0.25f, 0.001f));
    CHECK_THAT(out.events[2].time, WithinAbs(0.5f,  0.001f));
    CHECK_THAT(out.events[3].time, WithinAbs(0.75f, 0.001f));
    // Notes follow the reversed ordering: 65, 64, 62, 60.
    CHECK_THAT(out.events[0].midi_note, WithinAbs(65.0f, 0.001f));
    CHECK_THAT(out.events[1].midi_note, WithinAbs(64.0f, 0.001f));
    CHECK_THAT(out.events[2].midi_note, WithinAbs(62.0f, 0.001f));
    CHECK_THAT(out.events[3].midi_note, WithinAbs(60.0f, 0.001f));
}

TEST_CASE("EVENT_REORDER PALINDROME doubles event count and cycle_length",
          "[event-reorder]") {
    VM vm;
    constexpr std::uint32_t UP = 0xC011, PAL = 0xD011;

    std::vector<Instruction> prog{ make_reorder(PAL, EVENT_REORDER_PALINDROME,
        BUFFER_UNUSED, BUFFER_UNUSED, UP) };
    REQUIRE(vm.load_program_immediate(
        std::span<const Instruction>(prog.data(), prog.size())));

    // 2 events at t=0, 0.5 with dur=0.5; cycle_length=1.
    seed_seq_state(vm, UP, 1.0f, {
        Ev{.time=0.0f, .duration=0.5f, .midi_note=60.0f},
        Ev{.time=0.5f, .duration=0.5f, .midi_note=62.0f},
    });
    vm.init_event_transform_state(PAL, 2.0f, false, TEST_CAP);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const auto& downstream = vm.states().get<SequenceState>(PAL);
    REQUIRE(downstream.output.num_events == 4);
    // Downstream cycle_length is 2x upstream.
    CHECK_THAT(downstream.cycle_length, WithinAbs(2.0f, 0.001f));
    // Forward: t/2,dur/2 -> (0.0, 0.25), (0.25, 0.25).
    // Reverse: 1 - 0.5*t - 0.5*dur -> (0.75, 0.25), (0.5, 0.25).
    // Sorted by time: 0.0, 0.25, 0.5, 0.75.
    CHECK_THAT(downstream.output.events[0].time, WithinAbs(0.0f,  0.001f));
    CHECK_THAT(downstream.output.events[1].time, WithinAbs(0.25f, 0.001f));
    CHECK_THAT(downstream.output.events[2].time, WithinAbs(0.5f,  0.001f));
    CHECK_THAT(downstream.output.events[3].time, WithinAbs(0.75f, 0.001f));
}

TEST_CASE("EVENT_REORDER ZOOM keeps middle window remapped to [0,1)",
          "[event-reorder]") {
    VM vm;
    constexpr std::uint32_t UP = 0xC012, ZM = 0xD012;

    std::vector<Instruction> prog{
        make_push_const(10, 0.25f),
        make_push_const(11, 0.75f),
        make_reorder(ZM, EVENT_REORDER_ZOOM, 10, 11, UP),
    };
    REQUIRE(vm.load_program_immediate(
        std::span<const Instruction>(prog.data(), prog.size())));

    // 4 events at t=0, 0.25, 0.5, 0.75 dur=0.25. Window [0.25, 0.75) keeps
    // the middle two; remap to [0, 1).
    seed_seq_state(vm, UP, 1.0f, {
        Ev{.time=0.0f,  .midi_note=60.0f},
        Ev{.time=0.25f, .midi_note=62.0f},
        Ev{.time=0.5f,  .midi_note=64.0f},
        Ev{.time=0.75f, .midi_note=65.0f},
    });
    vm.init_event_transform_state(ZM, 1.0f, false, TEST_CAP);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const auto& out = vm.states().get<SequenceState>(ZM).output;
    REQUIRE(out.num_events == 2);
    CHECK_THAT(out.events[0].time,     WithinAbs(0.0f,  0.001f));
    CHECK_THAT(out.events[0].duration, WithinAbs(0.5f,  0.001f));
    CHECK_THAT(out.events[0].midi_note, WithinAbs(62.0f, 0.001f));
    CHECK_THAT(out.events[1].time,     WithinAbs(0.5f,  0.001f));
    CHECK_THAT(out.events[1].duration, WithinAbs(0.5f,  0.001f));
    CHECK_THAT(out.events[1].midi_note, WithinAbs(64.0f, 0.001f));
}

TEST_CASE("EVENT_REORDER ZOOM with end <= start yields empty output",
          "[event-reorder]") {
    VM vm;
    constexpr std::uint32_t UP = 0xC013, ZM = 0xD013;

    std::vector<Instruction> prog{
        make_push_const(10, 0.5f),
        make_push_const(11, 0.5f),  // degenerate: end == start
        make_reorder(ZM, EVENT_REORDER_ZOOM, 10, 11, UP),
    };
    REQUIRE(vm.load_program_immediate(
        std::span<const Instruction>(prog.data(), prog.size())));
    seed_seq_state(vm, UP, 1.0f, {Ev{.time=0.0f}, Ev{.time=0.5f}});
    vm.init_event_transform_state(ZM, 1.0f, false, TEST_CAP);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    CHECK(vm.states().get<SequenceState>(ZM).output.num_events == 0);
}

TEST_CASE("EVENT_REORDER COMPRESS rescales [0,1) into [s,e)",
          "[event-reorder]") {
    VM vm;
    constexpr std::uint32_t UP = 0xC014, CMP = 0xD014;

    std::vector<Instruction> prog{
        make_push_const(10, 0.25f),
        make_push_const(11, 0.75f),
        make_reorder(CMP, EVENT_REORDER_COMPRESS, 10, 11, UP),
    };
    REQUIRE(vm.load_program_immediate(
        std::span<const Instruction>(prog.data(), prog.size())));

    // 2 events at t=0, 0.5 dur=0.5; compress to [0.25, 0.75) width=0.5.
    // Result: (0.25, 0.25), (0.5, 0.25).
    seed_seq_state(vm, UP, 1.0f, {
        Ev{.time=0.0f, .duration=0.5f, .midi_note=60.0f},
        Ev{.time=0.5f, .duration=0.5f, .midi_note=62.0f},
    });
    vm.init_event_transform_state(CMP, 1.0f, false, TEST_CAP);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const auto& out = vm.states().get<SequenceState>(CMP).output;
    REQUIRE(out.num_events == 2);
    CHECK_THAT(out.events[0].time,     WithinAbs(0.25f, 0.001f));
    CHECK_THAT(out.events[0].duration, WithinAbs(0.25f, 0.001f));
    CHECK_THAT(out.events[0].midi_note, WithinAbs(60.0f, 0.001f));
    CHECK_THAT(out.events[1].time,     WithinAbs(0.5f,  0.001f));
    CHECK_THAT(out.events[1].duration, WithinAbs(0.25f, 0.001f));
    CHECK_THAT(out.events[1].midi_note, WithinAbs(62.0f, 0.001f));
}

TEST_CASE("EVENT_REORDER with empty upstream produces empty output",
          "[event-reorder]") {
    VM vm;
    constexpr std::uint32_t UP = 0xC015, R = 0xD015;
    std::vector<Instruction> prog{ make_reorder(R, EVENT_REORDER_REV,
        BUFFER_UNUSED, BUFFER_UNUSED, UP) };
    REQUIRE(vm.load_program_immediate(
        std::span<const Instruction>(prog.data(), prog.size())));
    seed_seq_state(vm, UP, 1.0f, {});  // no events
    vm.init_event_transform_state(R, 1.0f, false, TEST_CAP);

    std::array<float, BLOCK_SIZE> L{}, R_buf{};
    vm.process_block(L.data(), R_buf.data());

    CHECK(vm.states().get<SequenceState>(R).output.num_events == 0);
}

TEST_CASE("EVENT_REORDER with missing upstream is a safe no-op",
          "[event-reorder]") {
    VM vm;
    constexpr std::uint32_t UP_MISSING = 0xC016, R = 0xD016;
    std::vector<Instruction> prog{ make_reorder(R, EVENT_REORDER_REV,
        BUFFER_UNUSED, BUFFER_UNUSED, UP_MISSING) };
    REQUIRE(vm.load_program_immediate(
        std::span<const Instruction>(prog.data(), prog.size())));
    // Do NOT seed UP_MISSING — resolve_output_events returns null.
    vm.init_event_transform_state(R, 1.0f, false, TEST_CAP);

    std::array<float, BLOCK_SIZE> L{}, R_buf{};
    REQUIRE_NOTHROW(vm.process_block(L.data(), R_buf.data()));
    CHECK(vm.states().get<SequenceState>(R).output.num_events == 0);
}

TEST_CASE("EVENT_REORDER ITER(n=4) rotates events forward by 1/n per cycle",
          "[event-reorder][iter]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);
    constexpr std::uint32_t UP = 0xC018, IT = 0xD018;
    std::vector<Instruction> prog{
        make_push_const(10, 4.0f),
        make_reorder(IT, EVENT_REORDER_ITER, 10, BUFFER_UNUSED, UP),
    };
    REQUIRE(vm.load_program_immediate(
        std::span<const Instruction>(prog.data(), prog.size())));

    // 4 events at t=0, 0.25, 0.5, 0.75 with distinct midi_notes 60..63;
    // upstream cycle_length=1.
    seed_seq_state(vm, UP, 1.0f, {
        Ev{.time=0.0f,  .duration=0.25f, .midi_note=60.0f},
        Ev{.time=0.25f, .duration=0.25f, .midi_note=61.0f},
        Ev{.time=0.5f,  .duration=0.25f, .midi_note=62.0f},
        Ev{.time=0.75f, .duration=0.25f, .midi_note=63.0f},
    });
    vm.init_event_transform_state(IT, 1.0f, false, TEST_CAP);

    std::array<float, BLOCK_SIZE> L{}, R{};
    // Block 0: cycle_index=0 -> shift=0; events unchanged (sorted).
    vm.process_block(L.data(), R.data());
    {
        const auto& out = vm.states().get<SequenceState>(IT).output;
        REQUIRE(out.num_events == 4);
        CHECK_THAT(out.events[0].midi_note, WithinAbs(60.0f, 0.001f));
        CHECK_THAT(out.events[3].midi_note, WithinAbs(63.0f, 0.001f));
    }

    // Run enough blocks to enter cycle 1. spb = 0.5s * 48000 = 24000 samples;
    // cycle_length=1.0 cycle so 24000 samples = 1 cycle. BLOCK_SIZE=128, so
    // ~188 blocks per cycle.
    const int blocks_per_cycle = 24000 / BLOCK_SIZE + 1;
    for (int i = 0; i < blocks_per_cycle; ++i) {
        vm.process_block(L.data(), R.data());
    }
    {
        const auto& out = vm.states().get<SequenceState>(IT).output;
        REQUIRE(out.num_events == 4);
        // Cycle 1: shift = -1/4 = -0.25; t' = (t - 0.25) mod 1.
        // Original (0, 60), (0.25, 61), (0.5, 62), (0.75, 63) -->
        // (-0.25→0.75, 60), (0, 61), (0.25, 62), (0.5, 63). Sorted by time:
        // (0, 61), (0.25, 62), (0.5, 63), (0.75, 60).
        CHECK_THAT(out.events[0].midi_note, WithinAbs(61.0f, 0.001f));
        CHECK_THAT(out.events[1].midi_note, WithinAbs(62.0f, 0.001f));
        CHECK_THAT(out.events[2].midi_note, WithinAbs(63.0f, 0.001f));
        CHECK_THAT(out.events[3].midi_note, WithinAbs(60.0f, 0.001f));
    }
}

TEST_CASE("EVENT_REORDER ITER_BACK rotates backward (opposite direction)",
          "[event-reorder][iter]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);
    constexpr std::uint32_t UP = 0xC019, IT = 0xD019;
    std::vector<Instruction> prog{
        make_push_const(10, 4.0f),
        make_reorder(IT, EVENT_REORDER_ITER_BACK, 10, BUFFER_UNUSED, UP,
                     /*flags=*/EVENT_REORDER_ITER_DIR_BACK),
    };
    REQUIRE(vm.load_program_immediate(
        std::span<const Instruction>(prog.data(), prog.size())));
    seed_seq_state(vm, UP, 1.0f, {
        Ev{.time=0.0f,  .duration=0.25f, .midi_note=60.0f},
        Ev{.time=0.25f, .duration=0.25f, .midi_note=61.0f},
        Ev{.time=0.5f,  .duration=0.25f, .midi_note=62.0f},
        Ev{.time=0.75f, .duration=0.25f, .midi_note=63.0f},
    });
    vm.init_event_transform_state(IT, 1.0f, false, TEST_CAP);

    std::array<float, BLOCK_SIZE> L{}, R{};
    const int blocks_per_cycle = 24000 / BLOCK_SIZE + 1;
    // Advance into cycle 1.
    for (int i = 0; i < blocks_per_cycle + 1; ++i) {
        vm.process_block(L.data(), R.data());
    }
    const auto& out = vm.states().get<SequenceState>(IT).output;
    REQUIRE(out.num_events == 4);
    // Cycle 1, dir=-1: shift = +1/4 = +0.25; t' = (t + 0.25) mod 1.
    // (0,60)->0.25, (0.25,61)->0.5, (0.5,62)->0.75, (0.75,63)->0.
    // Sorted: (0, 63), (0.25, 60), (0.5, 61), (0.75, 62).
    CHECK_THAT(out.events[0].midi_note, WithinAbs(63.0f, 0.001f));
    CHECK_THAT(out.events[1].midi_note, WithinAbs(60.0f, 0.001f));
    CHECK_THAT(out.events[2].midi_note, WithinAbs(61.0f, 0.001f));
    CHECK_THAT(out.events[3].midi_note, WithinAbs(62.0f, 0.001f));
}

TEST_CASE("EVENT_REORDER PALINDROME captures upstream cycle_length on first block",
          "[event-reorder]") {
    VM vm;
    constexpr std::uint32_t UP = 0xC017, PAL = 0xD017;
    std::vector<Instruction> prog{ make_reorder(PAL, EVENT_REORDER_PALINDROME,
        BUFFER_UNUSED, BUFFER_UNUSED, UP) };
    REQUIRE(vm.load_program_immediate(
        std::span<const Instruction>(prog.data(), prog.size())));

    // Seed cycle_length=2 (e.g. inner slow(2)).
    seed_seq_state(vm, UP, 2.0f, {Ev{.time=0.0f, .duration=1.0f}});
    vm.init_event_transform_state(PAL, 1.0f, false, TEST_CAP);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    // Downstream cycle_length = original * 2 = 2.0 * 2.0 = 4.0
    CHECK_THAT(vm.states().get<SequenceState>(PAL).cycle_length,
               WithinAbs(4.0f, 0.001f));

    // Subsequent blocks reuse the captured original.
    vm.process_block(L.data(), R.data());
    CHECK_THAT(vm.states().get<SequenceState>(PAL).cycle_length,
               WithinAbs(4.0f, 0.001f));
}
