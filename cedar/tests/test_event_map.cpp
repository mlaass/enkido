// EVENT_MAP / EVENT_FILTER opcode tests — PRD prd-runtime-event-transforms
// Phase 1. These exercise the Cedar VM opcodes directly: a seeded upstream
// event source (SequenceState or MidiQueueState) feeds an EVENT_MAP/
// EVENT_FILTER instruction whose transform-owned SequenceState is then
// inspected.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "cedar/vm/vm.hpp"
#include "cedar/vm/instruction.hpp"
#include "cedar/opcodes/sequence.hpp"
#include "cedar/opcodes/midi.hpp"
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

// Per-upstream backing storage for seeded OutputEvents. Two slots so a test
// can seed two independent upstreams if needed.
thread_local std::array<OutputEvents::OutputEvent, TEST_CAP> g_up_events_a;

// A fully specified event for seeding.
struct Ev {
    float time = 0.0f;
    float duration = 1.0f;
    float velocity = 1.0f;
    float chance = 1.0f;
    float midi_note = 0.0f;
    std::uint8_t num_values = 1;
    std::array<float, 4> values = {0, 0, 0, 0};
    std::array<float, 4> notes = {0, 0, 0, 0};
    std::array<float, 4> velocities = {1, 1, 1, 1};
};

void seed_seq_state(VM& vm, std::uint32_t state_id, float cycle_length,
                    std::initializer_list<Ev> events) {
    auto& st = vm.states().get_or_create<SequenceState>(state_id);
    st.cycle_length = cycle_length;
    st.last_beat_pos = -1.0f;
    st.last_beat_pos_gate = -1.0f;
    st.last_queried_cycle = -1.0f;
    st.current_index = 0;
    st.output.events = g_up_events_a.data();
    st.output.capacity = TEST_CAP;
    st.output.num_events = 0;
    for (const auto& spec : events) {
        auto& e = g_up_events_a[st.output.num_events++];
        e = {};
        e.time = spec.time;
        e.duration = spec.duration;
        e.velocity = spec.velocity;
        e.chance = spec.chance;
        e.midi_note = spec.midi_note;
        e.num_values = spec.num_values;
        for (std::size_t i = 0; i < 4; ++i) {
            e.values[i] = spec.values[i];
            e.notes[i] = spec.notes[i];
            e.velocities[i] = spec.velocities[i];
        }
    }
}

// PUSH_CONST instruction filling `buf` with `value` (value bit-packed into
// state_id, matching op_push_const's decode).
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

Instruction make_event_transform(Opcode op, std::uint32_t state_id,
                                  std::uint8_t rate, std::uint16_t param_buf,
                                  std::uint32_t upstream_id) {
    Instruction inst{};
    inst.opcode = op;
    inst.rate = rate;
    inst.out_buffer = BUFFER_UNUSED;
    inst.inputs[0] = param_buf;
    inst.inputs[1] = BUFFER_UNUSED;
    inst.inputs[2] = static_cast<std::uint16_t>(upstream_id & 0xFFFF);
    inst.inputs[3] = static_cast<std::uint16_t>((upstream_id >> 16) & 0xFFFF);
    inst.inputs[4] = BUFFER_UNUSED;
    inst.state_id = state_id;
    return inst;
}

// Build [PUSH_CONST(param), EVENT_*] and load it. param_buf = 10.
void load_transform_program(VM& vm, Opcode op, std::uint32_t xform_id,
                            std::uint8_t rate, float param,
                            std::uint32_t upstream_id) {
    std::vector<Instruction> prog;
    prog.push_back(make_push_const(10, param));
    prog.push_back(make_event_transform(op, xform_id, rate, 10, upstream_id));
    REQUIRE(vm.load_program_immediate(
        std::span<const Instruction>(prog.data(), prog.size())));
}

}  // namespace


TEST_CASE("EVENT_MAP transpose(+12) shifts notes and doubles frequency",
          "[event-transform][event_map]") {
    VM vm;
    constexpr std::uint32_t A = 0xA001, B = 0xB001;
    load_transform_program(vm, Opcode::EVENT_MAP, B,
        event_transform_rate(EVENT_FIELD_NOTE_COUPLED, EVENT_OP_ADD),
        12.0f, A);
    seed_seq_state(vm, A, 1.0f, {
        Ev{.midi_note = 60.0f, .num_values = 1,
           .values = {261.6256f, 0, 0, 0}, .notes = {60, 0, 0, 0}}
    });
    vm.init_event_transform_state(B, 1.0f, false, 32);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const auto& out = vm.states().get<SequenceState>(B).output;
    REQUIRE(out.num_events == 1);
    CHECK_THAT(out.events[0].midi_note, WithinAbs(72.0f, 0.001f));
    CHECK_THAT(out.events[0].notes[0], WithinAbs(72.0f, 0.001f));
    CHECK_THAT(out.events[0].values[0], WithinAbs(523.2512f, 0.5f));
}

TEST_CASE("EVENT_MAP transpose(-12) halves frequency",
          "[event-transform][event_map]") {
    VM vm;
    constexpr std::uint32_t A = 0xA002, B = 0xB002;
    load_transform_program(vm, Opcode::EVENT_MAP, B,
        event_transform_rate(EVENT_FIELD_NOTE_COUPLED, EVENT_OP_ADD),
        -12.0f, A);
    seed_seq_state(vm, A, 1.0f, {
        Ev{.midi_note = 69.0f, .num_values = 1,
           .values = {440.0f, 0, 0, 0}, .notes = {69, 0, 0, 0}}
    });
    vm.init_event_transform_state(B, 1.0f, false, 32);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const auto& out = vm.states().get<SequenceState>(B).output;
    REQUIRE(out.num_events == 1);
    CHECK_THAT(out.events[0].midi_note, WithinAbs(57.0f, 0.001f));
    CHECK_THAT(out.events[0].values[0], WithinAbs(220.0f, 0.1f));
}

TEST_CASE("EVENT_MAP transpose accepts fractional semitones",
          "[event-transform][event_map]") {
    VM vm;
    constexpr std::uint32_t A = 0xA003, B = 0xB003;
    load_transform_program(vm, Opcode::EVENT_MAP, B,
        event_transform_rate(EVENT_FIELD_NOTE_COUPLED, EVENT_OP_ADD),
        7.0f, A);  // a perfect fifth
    seed_seq_state(vm, A, 1.0f, {
        Ev{.midi_note = 60.0f, .num_values = 1,
           .values = {261.6256f, 0, 0, 0}, .notes = {60, 0, 0, 0}}
    });
    vm.init_event_transform_state(B, 1.0f, false, 32);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const auto& out = vm.states().get<SequenceState>(B).output;
    REQUIRE(out.num_events == 1);
    CHECK_THAT(out.events[0].midi_note, WithinAbs(67.0f, 0.001f));
    // +7 semitones = ratio 2^(7/12) ≈ 1.4983.
    CHECK_THAT(out.events[0].values[0], WithinAbs(261.6256f * 1.49831f, 0.5f));
}

TEST_CASE("EVENT_MAP velocity(x0.5) scales event and per-voice velocity",
          "[event-transform][event_map]") {
    VM vm;
    constexpr std::uint32_t A = 0xA004, B = 0xB004;
    load_transform_program(vm, Opcode::EVENT_MAP, B,
        event_transform_rate(EVENT_FIELD_VEL, EVENT_OP_MUL), 0.5f, A);
    seed_seq_state(vm, A, 1.0f, {
        Ev{.velocity = 1.0f, .num_values = 1, .values = {440.0f, 0, 0, 0},
           .velocities = {1.0f, 1, 1, 1}},
        Ev{.time = 0.5f, .velocity = 0.8f, .num_values = 1,
           .values = {550.0f, 0, 0, 0}, .velocities = {0.8f, 1, 1, 1}}
    });
    vm.init_event_transform_state(B, 1.0f, false, 32);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const auto& out = vm.states().get<SequenceState>(B).output;
    REQUIRE(out.num_events == 2);
    CHECK_THAT(out.events[0].velocity, WithinAbs(0.5f, 0.001f));
    CHECK_THAT(out.events[0].velocities[0], WithinAbs(0.5f, 0.001f));
    CHECK_THAT(out.events[1].velocity, WithinAbs(0.4f, 0.001f));
    CHECK_THAT(out.events[1].velocities[0], WithinAbs(0.4f, 0.001f));
}

TEST_CASE("EVENT_MAP transpose shifts every voice of a chord",
          "[event-transform][event_map]") {
    VM vm;
    constexpr std::uint32_t A = 0xA005, B = 0xB005;
    load_transform_program(vm, Opcode::EVENT_MAP, B,
        event_transform_rate(EVENT_FIELD_NOTE_COUPLED, EVENT_OP_ADD),
        12.0f, A);
    // C major triad: MIDI 60/64/67.
    seed_seq_state(vm, A, 1.0f, {
        Ev{.midi_note = 60.0f, .num_values = 3,
           .values = {261.6256f, 329.6276f, 391.9954f, 0},
           .notes = {60, 64, 67, 0}}
    });
    vm.init_event_transform_state(B, 1.0f, false, 32);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const auto& out = vm.states().get<SequenceState>(B).output;
    REQUIRE(out.num_events == 1);
    REQUIRE(out.events[0].num_values == 3);  // voice count preserved
    CHECK_THAT(out.events[0].notes[0], WithinAbs(72.0f, 0.001f));
    CHECK_THAT(out.events[0].notes[1], WithinAbs(76.0f, 0.001f));
    CHECK_THAT(out.events[0].notes[2], WithinAbs(79.0f, 0.001f));
    CHECK_THAT(out.events[0].values[0], WithinAbs(261.6256f * 2.0f, 0.5f));
    CHECK_THAT(out.events[0].values[2], WithinAbs(391.9954f * 2.0f, 0.5f));
}

TEST_CASE("EVENT_MAP propagates upstream cycle_length unchanged",
          "[event-transform][event_map]") {
    VM vm;
    constexpr std::uint32_t A = 0xA006, B = 0xB006;
    load_transform_program(vm, Opcode::EVENT_MAP, B,
        event_transform_rate(EVENT_FIELD_NOTE_COUPLED, EVENT_OP_ADD),
        12.0f, A);
    seed_seq_state(vm, A, 2.5f, {
        Ev{.midi_note = 60.0f, .num_values = 1, .values = {261.6256f, 0, 0, 0}}
    });
    vm.init_event_transform_state(B, 1.0f, false, 32);  // placeholder 1.0

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    // EVENT_MAP copies the upstream's cycle_length each block.
    CHECK_THAT(vm.states().get<SequenceState>(B).cycle_length,
               WithinAbs(2.5f, 0.001f));
}

TEST_CASE("EVENT_MAP with an empty upstream produces no events",
          "[event-transform][event_map]") {
    VM vm;
    constexpr std::uint32_t A = 0xA007, B = 0xB007;
    load_transform_program(vm, Opcode::EVENT_MAP, B,
        event_transform_rate(EVENT_FIELD_NOTE_COUPLED, EVENT_OP_ADD),
        12.0f, A);
    seed_seq_state(vm, A, 1.0f, {});  // zero events
    vm.init_event_transform_state(B, 1.0f, false, 32);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const auto& st = vm.states().get<SequenceState>(B);
    CHECK(st.output.num_events == 0);
    CHECK(st.current_index == 0);
}

TEST_CASE("EVENT_MAP reads a MidiQueueState upstream (MIDI parity)",
          "[event-transform][event_map][midi]") {
    VM vm;
    constexpr std::uint32_t A = 0xA008, B = 0xB008;
    load_transform_program(vm, Opcode::EVENT_MAP, B,
        event_transform_rate(EVENT_FIELD_NOTE_COUPLED, EVENT_OP_ADD),
        12.0f, A);

    // Seed a MidiQueueState — resolve_output_events must handle it just like
    // a SequenceState.
    auto& midi = vm.states().get_or_create<MidiQueueState>(A);
    midi.cycle_length = 4.0f;
    midi.output.events = g_up_events_a.data();
    midi.output.capacity = TEST_CAP;
    midi.output.num_events = 1;
    g_up_events_a[0] = {};
    g_up_events_a[0].midi_note = 60.0f;
    g_up_events_a[0].num_values = 1;
    g_up_events_a[0].values[0] = 261.6256f;
    g_up_events_a[0].notes[0] = 60.0f;

    vm.init_event_transform_state(B, 1.0f, false, 32);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const auto& out = vm.states().get<SequenceState>(B).output;
    REQUIRE(out.num_events == 1);
    CHECK_THAT(out.events[0].midi_note, WithinAbs(72.0f, 0.001f));
}

TEST_CASE("EVENT_FILTER (GTE) drops events below the velocity threshold",
          "[event-transform][event_filter]") {
    VM vm;
    constexpr std::uint32_t A = 0xA009, B = 0xB009;
    load_transform_program(vm, Opcode::EVENT_FILTER, B,
        event_transform_rate(EVENT_FIELD_VEL, EVENT_CMP_GTE), 0.5f, A);
    seed_seq_state(vm, A, 1.0f, {
        Ev{.time = 0.0f, .velocity = 0.9f, .num_values = 1,
           .values = {440.0f, 0, 0, 0}},
        Ev{.time = 0.25f, .velocity = 0.2f, .num_values = 1,
           .values = {550.0f, 0, 0, 0}},
        Ev{.time = 0.5f, .velocity = 0.5f, .num_values = 1,
           .values = {660.0f, 0, 0, 0}},
        Ev{.time = 0.75f, .velocity = 0.1f, .num_values = 1,
           .values = {770.0f, 0, 0, 0}}
    });
    vm.init_event_transform_state(B, 1.0f, false, 32);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const auto& out = vm.states().get<SequenceState>(B).output;
    // Only the 0.9 and 0.5 events clear the >= 0.5 threshold.
    REQUIRE(out.num_events == 2);
    CHECK_THAT(out.events[0].velocity, WithinAbs(0.9f, 0.001f));
    CHECK_THAT(out.events[1].velocity, WithinAbs(0.5f, 0.001f));
    // Surviving events are copied through intact.
    CHECK_THAT(out.events[0].values[0], WithinAbs(440.0f, 0.001f));
    CHECK_THAT(out.events[1].values[0], WithinAbs(660.0f, 0.001f));
}

TEST_CASE("EVENT_FILTER can drop every event",
          "[event-transform][event_filter]") {
    VM vm;
    constexpr std::uint32_t A = 0xA00A, B = 0xB00A;
    load_transform_program(vm, Opcode::EVENT_FILTER, B,
        event_transform_rate(EVENT_FIELD_VEL, EVENT_CMP_GTE), 2.0f, A);
    seed_seq_state(vm, A, 1.0f, {
        Ev{.velocity = 0.9f, .num_values = 1, .values = {440.0f, 0, 0, 0}},
        Ev{.time = 0.5f, .velocity = 1.0f, .num_values = 1,
           .values = {550.0f, 0, 0, 0}}
    });
    vm.init_event_transform_state(B, 1.0f, false, 32);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const auto& st = vm.states().get<SequenceState>(B);
    CHECK(st.output.num_events == 0);
    CHECK(st.current_index == 0);
}

TEST_CASE("EVENT_FILTER (LTE) keeps events at or below the threshold",
          "[event-transform][event_filter]") {
    VM vm;
    constexpr std::uint32_t A = 0xA00B, B = 0xB00B;
    load_transform_program(vm, Opcode::EVENT_FILTER, B,
        event_transform_rate(EVENT_FIELD_VEL, EVENT_CMP_LTE), 0.5f, A);
    seed_seq_state(vm, A, 1.0f, {
        Ev{.velocity = 0.9f, .num_values = 1, .values = {440.0f, 0, 0, 0}},
        Ev{.time = 0.5f, .velocity = 0.3f, .num_values = 1,
           .values = {550.0f, 0, 0, 0}}
    });
    vm.init_event_transform_state(B, 1.0f, false, 32);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const auto& out = vm.states().get<SequenceState>(B).output;
    REQUIRE(out.num_events == 1);
    CHECK_THAT(out.events[0].velocity, WithinAbs(0.3f, 0.001f));
}

TEST_CASE("EVENT_MAP re-runs every block, staying stable over time",
          "[event-transform][event_map]") {
    VM vm;
    constexpr std::uint32_t A = 0xA00C, B = 0xB00C;
    load_transform_program(vm, Opcode::EVENT_MAP, B,
        event_transform_rate(EVENT_FIELD_NOTE_COUPLED, EVENT_OP_ADD),
        12.0f, A);
    seed_seq_state(vm, A, 1.0f, {
        Ev{.midi_note = 60.0f, .num_values = 1,
           .values = {261.6256f, 0, 0, 0}, .notes = {60, 0, 0, 0}}
    });
    vm.init_event_transform_state(B, 1.0f, false, 32);

    std::array<float, BLOCK_SIZE> L{}, R{};
    // Many blocks: the transform must not accumulate (idempotent rebuild).
    for (int i = 0; i < 256; ++i) {
        vm.process_block(L.data(), R.data());
        const auto& out = vm.states().get<SequenceState>(B).output;
        REQUIRE(out.num_events == 1);
        REQUIRE_THAT(out.events[0].midi_note, WithinAbs(72.0f, 0.001f));
    }
}
