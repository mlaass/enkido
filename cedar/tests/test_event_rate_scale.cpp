// EVENT_RATE_SCALE opcode tests — PRD prd-runtime-event-transforms Phase 3.
//
// The opcode mutates an upstream SequenceState's `cycle_length` field each
// block to (original_cycle_length / rate). Every SEQPAT_* opcode that
// consults state.cycle_length picks up the change automatically; no
// external-clock plumbing is involved.
//
// These tests build a minimal program with a seeded SequenceState upstream,
// run EVENT_RATE_SCALE for some blocks, and verify the SequenceState's
// cycle_length field tracks the formula.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "cedar/vm/vm.hpp"
#include "cedar/vm/instruction.hpp"
#include "cedar/opcodes/sequence.hpp"
#include "cedar/dsp/constants.hpp"

#include <array>
#include <cstring>
#include <span>
#include <vector>

using namespace cedar;
using Catch::Matchers::WithinAbs;

namespace {

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

Instruction make_rate_scale(std::uint32_t ers_state_id,
                            std::uint32_t upstream_state_id,
                            std::uint16_t factor_buf) {
    Instruction inst{};
    inst.opcode = Opcode::EVENT_RATE_SCALE;
    inst.out_buffer = BUFFER_UNUSED;
    inst.inputs[0] = BUFFER_UNUSED;
    inst.inputs[1] = factor_buf;
    inst.inputs[2] = static_cast<std::uint16_t>(upstream_state_id & 0xFFFF);
    inst.inputs[3] = static_cast<std::uint16_t>((upstream_state_id >> 16) & 0xFFFF);
    inst.inputs[4] = BUFFER_UNUSED;
    inst.state_id = ers_state_id;
    return inst;
}

// Seed a SequenceState with a known initial cycle_length so ERS has
// something to capture on the first block.
void seed_upstream(VM& vm, std::uint32_t state_id, float initial_cycle_length) {
    auto& st = vm.states().get_or_create<SequenceState>(state_id);
    st.cycle_length = initial_cycle_length;
    // OutputEvents not exercised here; ERS only reads/writes cycle_length.
    st.output.num_events = 0;
}

}  // namespace

TEST_CASE("EVENT_RATE_SCALE constant rate=2.0 halves cycle_length",
          "[event-rate-scale]") {
    VM vm;
    constexpr std::uint32_t UP = 0xC001, ERS = 0xD001;

    std::vector<Instruction> prog;
    prog.push_back(make_push_const(10, 2.0f));      // factor = 2
    prog.push_back(make_rate_scale(ERS, UP, 10));
    REQUIRE(vm.load_program_immediate(
        std::span<const Instruction>(prog.data(), prog.size())));

    seed_upstream(vm, UP, 1.0f);
    vm.init_event_rate_scale_state(ERS);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    // After one block, ERS has captured original=1.0 and written
    // cycle_length = 1.0 / 2.0 = 0.5.
    CHECK_THAT(vm.states().get<SequenceState>(UP).cycle_length,
               WithinAbs(0.5f, 0.0001f));
}

TEST_CASE("EVENT_RATE_SCALE constant rate=0.5 doubles cycle_length (slow)",
          "[event-rate-scale]") {
    VM vm;
    constexpr std::uint32_t UP = 0xC002, ERS = 0xD002;

    std::vector<Instruction> prog;
    prog.push_back(make_push_const(10, 0.5f));      // slow factor → 1/2 in codegen
    prog.push_back(make_rate_scale(ERS, UP, 10));
    REQUIRE(vm.load_program_immediate(
        std::span<const Instruction>(prog.data(), prog.size())));

    seed_upstream(vm, UP, 1.0f);
    vm.init_event_rate_scale_state(ERS);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    // 1.0 / 0.5 = 2.0
    CHECK_THAT(vm.states().get<SequenceState>(UP).cycle_length,
               WithinAbs(2.0f, 0.0001f));
}

TEST_CASE("EVENT_RATE_SCALE captures original on first block, not later",
          "[event-rate-scale]") {
    VM vm;
    constexpr std::uint32_t UP = 0xC003, ERS = 0xD003;

    std::vector<Instruction> prog;
    prog.push_back(make_push_const(10, 3.0f));
    prog.push_back(make_rate_scale(ERS, UP, 10));
    REQUIRE(vm.load_program_immediate(
        std::span<const Instruction>(prog.data(), prog.size())));

    // Seed with cycle_length=2 (e.g. inner slow(2) accumulated compile-time).
    seed_upstream(vm, UP, 2.0f);
    vm.init_event_rate_scale_state(ERS);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    // Block 1: original captured = 2.0; cycle_length = 2.0 / 3.0
    auto& upstream = vm.states().get<SequenceState>(UP);
    CHECK_THAT(upstream.cycle_length, WithinAbs(2.0f / 3.0f, 0.0001f));

    // Run another block — ERS should NOT recapture; cycle_length stays.
    vm.process_block(L.data(), R.data());
    CHECK_THAT(upstream.cycle_length, WithinAbs(2.0f / 3.0f, 0.0001f));

    // Even if the upstream's cycle_length got externally clobbered between
    // blocks (e.g. another ERS chain), ERS keeps writing the value derived
    // from its CAPTURED original — not from the upstream's current value.
    upstream.cycle_length = 99.0f;
    vm.process_block(L.data(), R.data());
    CHECK_THAT(upstream.cycle_length, WithinAbs(2.0f / 3.0f, 0.0001f));
}

TEST_CASE("EVENT_RATE_SCALE clamps rate <= 0 to 0.001 (avoids frozen playback)",
          "[event-rate-scale]") {
    VM vm;
    constexpr std::uint32_t UP = 0xC004, ERS = 0xD004;

    std::vector<Instruction> prog;
    prog.push_back(make_push_const(10, -1.0f));     // negative — pathological
    prog.push_back(make_rate_scale(ERS, UP, 10));
    REQUIRE(vm.load_program_immediate(
        std::span<const Instruction>(prog.data(), prog.size())));

    seed_upstream(vm, UP, 1.0f);
    vm.init_event_rate_scale_state(ERS);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    // Clamped to 0.001 → cycle_length = 1.0 / 0.001 = 1000.0
    CHECK_THAT(vm.states().get<SequenceState>(UP).cycle_length,
               WithinAbs(1000.0f, 0.5f));
}

TEST_CASE("EVENT_RATE_SCALE with missing upstream is a safe no-op",
          "[event-rate-scale]") {
    VM vm;
    constexpr std::uint32_t UP = 0xC005, ERS = 0xD005;

    std::vector<Instruction> prog;
    prog.push_back(make_push_const(10, 2.0f));
    prog.push_back(make_rate_scale(ERS, UP, 10));
    REQUIRE(vm.load_program_immediate(
        std::span<const Instruction>(prog.data(), prog.size())));

    // Do NOT seed the upstream — ctx.states->get_if<SequenceState>(UP)
    // returns nullptr; the opcode should return without crashing.
    vm.init_event_rate_scale_state(ERS);

    std::array<float, BLOCK_SIZE> L{}, R{};
    REQUIRE_NOTHROW(vm.process_block(L.data(), R.data()));
}
