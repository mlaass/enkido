// SEQPAT_GATE runtime behavior tests.
//
// The gate must drop to 0 for at least one sample at every event onset
// (per-note retrigger), unless another event is simultaneously sustaining
// at that moment (the legato escape hatch via explicit duration overlap).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "cedar/vm/vm.hpp"
#include "cedar/vm/instruction.hpp"
#include "cedar/opcodes/sequence.hpp"
#include "cedar/opcodes/sequencing.hpp"
#include "cedar/dsp/constants.hpp"

#include <array>
#include <vector>

using namespace cedar;
using Catch::Matchers::WithinAbs;

namespace {

constexpr std::size_t TEST_MAX_OUTPUT_EVENTS = 32;
thread_local std::array<OutputEvents::OutputEvent, TEST_MAX_OUTPUT_EVENTS>
    g_test_output_events;

void install_gate_test_state(VM& vm, std::uint32_t state_id, float cycle_length,
                             std::initializer_list<std::pair<float, float>> events) {
    auto& state = vm.states().get_or_create<SequenceState>(state_id);
    state.cycle_length = cycle_length;
    state.last_beat_pos = -1.0f;
    state.last_beat_pos_gate = -1.0f;
    state.last_queried_cycle = -1.0f;
    state.current_index = 0;
    state.output.events = g_test_output_events.data();
    state.output.capacity = TEST_MAX_OUTPUT_EVENTS;
    state.output.num_events = 0;
    for (auto [t, d] : events) {
        auto& e = g_test_output_events[state.output.num_events++];
        e = {};
        e.time = t;
        e.duration = d;
        e.velocity = 1.0f;
        e.num_values = 1;
        e.values[0] = 440.0f;
        e.velocities[0] = 1.0f;
    }
}

Instruction make_seqpat_gate(std::uint16_t out_buf, std::uint32_t state_id) {
    Instruction inst{};
    inst.opcode = Opcode::SEQPAT_GATE;
    inst.out_buffer = out_buf;
    inst.inputs[0] = BUFFER_UNUSED;
    inst.inputs[1] = BUFFER_UNUSED;
    inst.inputs[2] = BUFFER_UNUSED;
    inst.inputs[3] = BUFFER_UNUSED;
    inst.inputs[4] = BUFFER_UNUSED;
    inst.state_id = state_id;
    return inst;
}

void configure_one_beat_per_block(VM& vm) {
    // 1 beat == BLOCK_SIZE samples → bpm = 60 * sample_rate / BLOCK_SIZE.
    vm.set_sample_rate(DEFAULT_SAMPLE_RATE);
    const float bpm =
        60.0f * DEFAULT_SAMPLE_RATE / static_cast<float>(BLOCK_SIZE);
    vm.set_bpm(bpm);
}

int count_rising_edges(const float* buf, std::size_t n) {
    int count = 0;
    float prev = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        if (prev < 0.5f && buf[i] >= 0.5f) ++count;
        prev = buf[i];
    }
    return count;
}

bool has_drop_in(const float* buf, std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
        if (buf[i] < 0.5f) return true;
    }
    return false;
}

}  // namespace


TEST_CASE("SEQPAT_GATE retriggers between contiguous events",
          "[seqpat_gate][retrigger]") {
    VM vm;
    constexpr std::uint32_t state_id = 0xAB01;
    configure_one_beat_per_block(vm);

    Instruction inst = make_seqpat_gate(/*out*/ 10, state_id);
    REQUIRE(vm.load_program_immediate(std::span{&inst, 1}));

    // Four events tile the 4-beat cycle: c4, e4, g4, b4 at beats 0,1,2,3.
    install_gate_test_state(vm, state_id, /*cycle_length*/ 4.0f, {
        {0.0f, 1.0f}, {1.0f, 1.0f}, {2.0f, 1.0f}, {3.0f, 1.0f}
    });

    std::array<float, BLOCK_SIZE> L{}, R{};
    std::vector<float> gate_history;
    gate_history.reserve(4 * BLOCK_SIZE);
    for (int b = 0; b < 4; ++b) {
        vm.process_block(L.data(), R.data());
        const float* g = vm.buffers().get(10);
        gate_history.insert(gate_history.end(), g, g + BLOCK_SIZE);
    }

    int edges = count_rising_edges(gate_history.data(), gate_history.size());
    CHECK(edges == 4);

    // Boundaries lie at sample BLOCK_SIZE, 2*BLOCK_SIZE, 3*BLOCK_SIZE.
    for (int boundary = 1; boundary < 4; ++boundary) {
        std::size_t mid = static_cast<std::size_t>(boundary) * BLOCK_SIZE;
        std::size_t lo  = mid > 4 ? mid - 4 : 0;
        std::size_t hi  = std::min(mid + 4, gate_history.size());
        CHECK(has_drop_in(gate_history.data(), lo, hi));
    }
}


TEST_CASE("SEQPAT_GATE stays high across explicit duration overlap (legato)",
          "[seqpat_gate][legato]") {
    VM vm;
    constexpr std::uint32_t state_id = 0xAB02;
    configure_one_beat_per_block(vm);

    Instruction inst = make_seqpat_gate(/*out*/ 10, state_id);
    REQUIRE(vm.load_program_immediate(std::span{&inst, 1}));

    // Two events at beats 0 and 1, each with duration 2 → overlap by 1 beat.
    install_gate_test_state(vm, state_id, 4.0f, {{0.0f, 2.0f}, {1.0f, 2.0f}});

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());  // block 0: beats 0..1
    vm.process_block(L.data(), R.data());  // block 1: beats 1..2 (overlap region)
    const float* g1 = vm.buffers().get(10);
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        CHECK_THAT(g1[i], WithinAbs(1.0f, 1e-6f));
    }
}


TEST_CASE("SEQPAT_GATE is zero during gaps between events",
          "[seqpat_gate][gap]") {
    VM vm;
    constexpr std::uint32_t state_id = 0xAB03;
    configure_one_beat_per_block(vm);

    Instruction inst = make_seqpat_gate(/*out*/ 10, state_id);
    REQUIRE(vm.load_program_immediate(std::span{&inst, 1}));

    // One event at beat 0 with duration 0.5; rest of cycle is empty.
    install_gate_test_state(vm, state_id, 4.0f, {{0.0f, 0.5f}});

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());
    const float* g = vm.buffers().get(10);

    bool any_high_first = false;
    for (std::size_t i = 0; i < BLOCK_SIZE / 2 - 4; ++i) {
        if (g[i] > 0.5f) { any_high_first = true; break; }
    }
    CHECK(any_high_first);

    for (std::size_t i = BLOCK_SIZE / 2 + 4; i < BLOCK_SIZE; ++i) {
        CHECK_THAT(g[i], WithinAbs(0.0f, 1e-6f));
    }
}


TEST_CASE("SEQPAT_GATE retriggers across cycle wrap",
          "[seqpat_gate][wrap]") {
    VM vm;
    constexpr std::uint32_t state_id = 0xAB04;
    configure_one_beat_per_block(vm);

    Instruction inst = make_seqpat_gate(/*out*/ 10, state_id);
    REQUIRE(vm.load_program_immediate(std::span{&inst, 1}));

    // Single event filling the cycle. Within a cycle the gate stays at 1.0,
    // but at the wrap (cycle 0 → cycle 1) the event's start time is crossed
    // again → retrigger drop.
    install_gate_test_state(vm, state_id, 4.0f, {{0.0f, 4.0f}});

    std::array<float, BLOCK_SIZE> L{}, R{};
    std::vector<float> gate_history;
    gate_history.reserve(5 * BLOCK_SIZE);
    for (int b = 0; b < 5; ++b) {
        vm.process_block(L.data(), R.data());
        const float* g = vm.buffers().get(10);
        gate_history.insert(gate_history.end(), g, g + BLOCK_SIZE);
    }

    int edges = count_rising_edges(gate_history.data(), gate_history.size());
    CHECK(edges >= 2);
}
