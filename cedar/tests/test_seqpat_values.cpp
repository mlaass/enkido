// Tests for SEQPAT_VALUES — the pattern-event-array opcode (PRD
// prd-pattern-event-arrays). SEQPAT_VALUES packs the active chord's notes
// into a buffer; SEQPAT_FIELD field=5 reports the matching length. Both run
// the identical "collect events at the active onset" scan.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "cedar/vm/vm.hpp"
#include "cedar/vm/instruction.hpp"
#include <array>
#include <span>
#include <vector>

using namespace cedar;
using Catch::Matchers::WithinAbs;

namespace {

constexpr std::uint32_t SEQ_STATE = 0x5EEDu;
constexpr std::uint16_t BUF_OUT = 0;

Event data_event(float time, std::initializer_list<float> values);

// Run a 3-instruction program: SEQPAT_QUERY, the opcode under test, OUTPUT.
// load_program_immediate clears the state pool, so the sequence must be
// installed *after* the program is loaded. Returns the first block's L.
std::array<float, BLOCK_SIZE> run(const Instruction& probe,
                                  std::vector<Event>& events) {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    std::vector<Instruction> program = {
        Instruction::make_nullary(Opcode::SEQPAT_QUERY, 0, SEQ_STATE),
        probe,
        Instruction::make_unary(Opcode::OUTPUT, 0, BUF_OUT),
    };
    REQUIRE(vm.load_program_immediate(std::span<const Instruction>(program)));

    Sequence seq{};
    seq.events = events.data();
    seq.num_events = static_cast<std::uint32_t>(events.size());
    seq.capacity = seq.num_events;
    seq.duration = 1.0f;
    seq.mode = SequenceMode::NORMAL;
    vm.init_sequence_program_state(SEQ_STATE, &seq, 1, /*cycle_length=*/1.0f,
                                   /*is_sample=*/false,
                                   /*total_events=*/std::max<std::uint32_t>(
                                       1, seq.num_events));

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());
    return L;
}

Instruction seqpat_values(std::uint8_t rate) {
    Instruction inst{};
    inst.opcode = Opcode::SEQPAT_VALUES;
    inst.out_buffer = BUF_OUT;
    inst.rate = rate;
    inst.inputs[0] = BUFFER_UNUSED;
    inst.inputs[1] = BUFFER_UNUSED;  // internal clock
    inst.inputs[2] = BUFFER_UNUSED;
    inst.inputs[3] = BUFFER_UNUSED;
    inst.inputs[4] = BUFFER_UNUSED;
    inst.state_id = SEQ_STATE;
    return inst;
}

Instruction seqpat_numvalues() {
    Instruction inst{};
    inst.opcode = Opcode::SEQPAT_FIELD;
    inst.out_buffer = BUF_OUT;
    inst.rate = 5;  // num_values selector
    inst.inputs[0] = 0;              // voice 0
    inst.inputs[1] = BUFFER_UNUSED;  // internal clock
    inst.inputs[2] = BUFFER_UNUSED;
    inst.inputs[3] = BUFFER_UNUSED;
    inst.inputs[4] = BUFFER_UNUSED;
    inst.state_id = SEQ_STATE;
    return inst;
}

Event data_event(float time, std::initializer_list<float> values) {
    Event e{};
    e.type = EventType::DATA;
    e.time = time;
    e.duration = 1.0f;
    e.chance = 1.0f;
    e.velocity = 1.0f;
    e.num_values = static_cast<std::uint8_t>(values.size());
    std::uint8_t i = 0;
    for (float v : values) e.values[i++] = v;
    return e;
}

}  // namespace

TEST_CASE("seqpat_values: multi-value chord event packs all notes", "[seqpat_values]") {
    // One event carrying a 3-note chord (chord-symbol representation).
    std::vector<Event> events = {data_event(0.0f, {261.63f, 329.63f, 392.0f})};
    auto hz = run(seqpat_values(0), events);  // rate 0 = raw Hz
    CHECK_THAT(hz[0], WithinAbs(261.63f, 0.1f));
    CHECK_THAT(hz[1], WithinAbs(329.63f, 0.1f));
    CHECK_THAT(hz[2], WithinAbs(392.0f, 0.1f));
    CHECK_THAT(hz[3], WithinAbs(0.0f, 1e-4f));  // beyond the chord = 0
}

TEST_CASE("seqpat_values: rate 1 converts to MIDI numbers", "[seqpat_values]") {
    std::vector<Event> events = {data_event(0.0f, {261.63f, 329.63f, 392.0f})};
    auto midi = run(seqpat_values(1), events);  // rate 1 = MIDI
    CHECK_THAT(midi[0], WithinAbs(60.0f, 0.01f));  // C4
    CHECK_THAT(midi[1], WithinAbs(64.0f, 0.01f));  // E4
    CHECK_THAT(midi[2], WithinAbs(67.0f, 0.01f));  // G4
}

TEST_CASE("seqpat_values: simultaneous single-value events form one chord",
          "[seqpat_values]") {
    // Polyrhythm `[c4,e4,g4]` representation: three events sharing time 0.
    std::vector<Event> events = {
        data_event(0.0f, {261.63f}),
        data_event(0.0f, {329.63f}),
        data_event(0.0f, {392.0f}),
    };
    auto hz = run(seqpat_values(0), events);
    CHECK_THAT(hz[0], WithinAbs(261.63f, 0.1f));
    CHECK_THAT(hz[1], WithinAbs(329.63f, 0.1f));
    CHECK_THAT(hz[2], WithinAbs(392.0f, 0.1f));
}

TEST_CASE("seqpat_values: num_values length matches the packed chord size",
          "[seqpat_values]") {
    std::vector<Event> events = {
        data_event(0.0f, {261.63f}),
        data_event(0.0f, {329.63f}),
        data_event(0.0f, {392.0f}),
    };
    auto len = run(seqpat_numvalues(), events);
    CHECK_THAT(len[0], WithinAbs(3.0f, 1e-4f));
    CHECK_THAT(len[BLOCK_SIZE - 1], WithinAbs(3.0f, 1e-4f));
}

TEST_CASE("seqpat_values: empty sequence yields a zero-length array",
          "[seqpat_values]") {
    std::vector<Event> empty1;
    std::vector<Event> empty2;
    auto data = run(seqpat_values(1), empty1);
    auto len = run(seqpat_numvalues(), empty2);
    CHECK_THAT(data[0], WithinAbs(0.0f, 1e-4f));
    CHECK_THAT(len[0], WithinAbs(0.0f, 1e-4f));
}
