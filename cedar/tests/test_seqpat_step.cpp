// SEQPAT_STEP runtime behavior tests.
//
// The trigger output fires a 1-sample pulse at every *note* onset. Rest
// events (`~` in mini-notation, encoded as num_values == 0) must never
// fire a trigger — including at a cycle wrap when the rest is the first
// event of the pattern.

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

namespace {

constexpr std::size_t TEST_MAX_OUTPUT_EVENTS = 32;
thread_local std::array<OutputEvents::OutputEvent, TEST_MAX_OUTPUT_EVENTS>
    g_step_test_output_events;

// Event spec: {time, duration, num_values}. num_values == 0 marks a rest.
struct EventSpec {
    float time;
    float duration;
    std::uint8_t num_values;
};

void install_step_test_state(VM& vm, std::uint32_t state_id, float cycle_length,
                              std::initializer_list<EventSpec> events) {
    auto& state = vm.states().get_or_create<SequenceState>(state_id);
    state.cycle_length = cycle_length;
    state.last_beat_pos = -1.0f;
    state.last_beat_pos_gate = -1.0f;
    state.last_queried_cycle = -1.0f;
    state.current_index = 0;
    state.active_source_offset = 0;
    state.active_source_length = 0;
    state.output.events = g_step_test_output_events.data();
    state.output.capacity = TEST_MAX_OUTPUT_EVENTS;
    state.output.num_events = 0;
    for (auto spec : events) {
        auto& e = g_step_test_output_events[state.output.num_events++];
        e = {};
        e.time = spec.time;
        e.duration = spec.duration;
        e.velocity = 1.0f;
        e.num_values = spec.num_values;
        if (spec.num_values > 0) {
            e.values[0] = 440.0f;
            e.velocities[0] = 1.0f;
        }
    }
}

Instruction make_seqpat_step(std::uint16_t value_buf, std::uint16_t trig_buf,
                             std::uint32_t state_id) {
    Instruction inst{};
    inst.opcode = Opcode::SEQPAT_STEP;
    inst.out_buffer = value_buf;
    inst.inputs[0] = BUFFER_UNUSED;   // velocity output (skipped)
    inst.inputs[1] = trig_buf;        // trigger output
    inst.inputs[2] = BUFFER_UNUSED;   // voice index → default 0
    inst.inputs[3] = BUFFER_UNUSED;   // external clock (internal)
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

bool any_high_in(const float* buf, std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
        if (buf[i] >= 0.5f) return true;
    }
    return false;
}

}  // namespace


TEST_CASE("SEQPAT_STEP fires one trigger per note onset",
          "[seqpat_step][trigger]") {
    VM vm;
    constexpr std::uint32_t state_id = 0xCD01;
    configure_one_beat_per_block(vm);

    Instruction inst = make_seqpat_step(/*value*/ 10, /*trig*/ 11, state_id);
    REQUIRE(vm.load_program_immediate(std::span{&inst, 1}));

    // Four notes tile the 4-beat cycle at beats 0,1,2,3.
    install_step_test_state(vm, state_id, /*cycle_length*/ 4.0f, {
        {0.0f, 1.0f, 1}, {1.0f, 1.0f, 1}, {2.0f, 1.0f, 1}, {3.0f, 1.0f, 1}
    });

    std::array<float, BLOCK_SIZE> L{}, R{};
    std::vector<float> trig_history;
    // Two full cycles.
    for (int b = 0; b < 8; ++b) {
        vm.process_block(L.data(), R.data());
        const float* t = vm.buffers().get(11);
        trig_history.insert(trig_history.end(), t, t + BLOCK_SIZE);
    }

    // 4 notes × 2 cycles = 8 triggers.
    CHECK(count_rising_edges(trig_history.data(), trig_history.size()) == 8);
}


TEST_CASE("SEQPAT_STEP rest as first event does not fire trigger on cycle wrap",
          "[seqpat_step][rest][wrap]") {
    VM vm;
    constexpr std::uint32_t state_id = 0xCD02;
    configure_one_beat_per_block(vm);

    Instruction inst = make_seqpat_step(/*value*/ 10, /*trig*/ 11, state_id);
    REQUIRE(vm.load_program_immediate(std::span{&inst, 1}));

    // Pattern "~ c4 e4 g4": a rest at beat 0, then three notes. The rest is
    // event[0] at time 0 — exactly what the cycle-wrap fallback inspects.
    install_step_test_state(vm, state_id, /*cycle_length*/ 4.0f, {
        {0.0f, 1.0f, 0},  // rest
        {1.0f, 1.0f, 1}, {2.0f, 1.0f, 1}, {3.0f, 1.0f, 1}
    });

    std::array<float, BLOCK_SIZE> L{}, R{};
    std::vector<float> trig_history;
    // Two full cycles — the cycle wrap occurs at block 4.
    for (int b = 0; b < 8; ++b) {
        vm.process_block(L.data(), R.data());
        const float* t = vm.buffers().get(11);
        trig_history.insert(trig_history.end(), t, t + BLOCK_SIZE);
    }

    // 3 notes × 2 cycles = 6 triggers — the rest contributes none.
    CHECK(count_rising_edges(trig_history.data(), trig_history.size()) == 6);

    // No trigger at the rest position: cycle starts (sample 0 and the wrap
    // at sample 4*BLOCK_SIZE) must stay low through the rest's first beat.
    CHECK_FALSE(any_high_in(trig_history.data(), 0, BLOCK_SIZE));
    CHECK_FALSE(any_high_in(trig_history.data(),
                            4 * BLOCK_SIZE, 5 * BLOCK_SIZE));
}


TEST_CASE("SEQPAT_STEP rest mid-pattern does not fire trigger",
          "[seqpat_step][rest]") {
    VM vm;
    constexpr std::uint32_t state_id = 0xCD03;
    configure_one_beat_per_block(vm);

    Instruction inst = make_seqpat_step(/*value*/ 10, /*trig*/ 11, state_id);
    REQUIRE(vm.load_program_immediate(std::span{&inst, 1}));

    // Pattern "c4 ~ e4 ~": notes at beats 0 and 2, rests at beats 1 and 3.
    install_step_test_state(vm, state_id, /*cycle_length*/ 4.0f, {
        {0.0f, 1.0f, 1}, {1.0f, 1.0f, 0}, {2.0f, 1.0f, 1}, {3.0f, 1.0f, 0}
    });

    std::array<float, BLOCK_SIZE> L{}, R{};
    std::vector<float> trig_history;
    for (int b = 0; b < 8; ++b) {
        vm.process_block(L.data(), R.data());
        const float* t = vm.buffers().get(11);
        trig_history.insert(trig_history.end(), t, t + BLOCK_SIZE);
    }

    // 2 notes × 2 cycles = 4 triggers.
    CHECK(count_rising_edges(trig_history.data(), trig_history.size()) == 4);

    // The rest beats (block 1 and block 3 of each cycle) stay low.
    for (int cycle = 0; cycle < 2; ++cycle) {
        std::size_t base = static_cast<std::size_t>(cycle) * 4 * BLOCK_SIZE;
        CHECK_FALSE(any_high_in(trig_history.data(),
                                base + 1 * BLOCK_SIZE, base + 2 * BLOCK_SIZE));
        CHECK_FALSE(any_high_in(trig_history.data(),
                                base + 3 * BLOCK_SIZE, base + 4 * BLOCK_SIZE));
    }
}
