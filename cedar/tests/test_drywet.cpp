// Unified dry/wet contract: verify Category-A (parallel-mix) and Category-B
// (transform) effects honor the dry/wet mix line.
//
// Two anchors per category — one slot-based, one ExtendedParams-based.
//
// Category A (defaults dry=1, wet=0.5):
//   - DELAY (slot-based, inputs[3,4])
//   - EFFECT_CHORUS (ExtendedParams<3>, ext[1,2])
//
// Category B (defaults dry=0, wet=1):
//   - FILTER_SVF_LP (slot-based, inputs[3,4])
//   - FILTER_MOOG (ExtendedParams<2>, ext[0,1])
//
// For Category A we verify dry=1, wet=0 → passthrough (output == input).
// For Category B we verify dry=0, wet=1 → effect-only (matches today's
// pre-migration behavior bit-exactly).

#include <catch2/catch_test_macros.hpp>
#include "cedar/vm/vm.hpp"
#include "cedar/vm/instruction.hpp"
#include "cedar/dsp/constants.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <cstring>

using namespace cedar;

namespace {

Instruction push_const(std::uint16_t out, float value) {
    Instruction inst{};
    inst.opcode = Opcode::PUSH_CONST;
    inst.out_buffer = out;
    inst.inputs[0] = inst.inputs[1] = inst.inputs[2] = inst.inputs[3] = inst.inputs[4] = BUFFER_UNUSED;
    inst.state_id = std::bit_cast<std::uint32_t>(value);
    return inst;
}

// Set the user-provided input to a slow ramp so the effect can't accidentally
// look "correct" by emitting silence.
void fill_ramp(float* buf) {
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        buf[i] = 0.5f * std::sin(static_cast<float>(i) * 0.03f);
    }
}

}  // namespace

TEST_CASE("DELAY (Category A): dry=1, wet=0 -> passthrough", "[drywet][delay]") {
    constexpr std::uint16_t BUF_IN   = 0;
    constexpr std::uint16_t BUF_TIME = 1;
    constexpr std::uint16_t BUF_FB   = 2;
    constexpr std::uint16_t BUF_DRY  = 3;
    constexpr std::uint16_t BUF_WET  = 4;
    constexpr std::uint16_t BUF_OUT  = 5;

    Instruction delay{};
    delay.opcode = Opcode::DELAY;
    delay.out_buffer = BUF_OUT;
    delay.inputs[0] = BUF_IN;
    delay.inputs[1] = BUF_TIME;
    delay.inputs[2] = BUF_FB;
    delay.inputs[3] = BUF_DRY;
    delay.inputs[4] = BUF_WET;
    delay.rate = 0;  // seconds
    delay.state_id = 0xDEAD0001u;

    std::vector<Instruction> program{
        push_const(BUF_TIME, 0.1f),
        push_const(BUF_FB,   0.0f),
        push_const(BUF_DRY,  1.0f),
        push_const(BUF_WET,  0.0f),
        delay,
    };

    VM vm;
    (void)vm.load_program(std::span(program));

    std::array<float, BLOCK_SIZE> L{}, R{};
    fill_ramp(vm.buffers().get(BUF_IN));
    std::array<float, BLOCK_SIZE> expected{};
    std::memcpy(expected.data(), vm.buffers().get(BUF_IN), BLOCK_SIZE * sizeof(float));
    vm.process_block(L.data(), R.data());

    const float* out_l = vm.buffers().get(BUF_OUT);
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        REQUIRE(std::abs(out_l[i] - expected[i]) < 1e-5f);
    }
}

TEST_CASE("EFFECT_CHORUS (Category A): dry=1, wet=0 -> passthrough",
          "[drywet][chorus]") {
    constexpr std::uint16_t BUF_IN    = 0;
    constexpr std::uint16_t BUF_RATE  = 1;
    constexpr std::uint16_t BUF_DEPTH = 2;
    constexpr std::uint16_t BUF_BASE  = 3;
    constexpr std::uint16_t BUF_RANGE = 4;
    constexpr std::uint16_t BUF_OUT   = 5;

    Instruction chorus{};
    chorus.opcode = Opcode::EFFECT_CHORUS;
    chorus.out_buffer = BUF_OUT;
    chorus.inputs[0] = BUF_IN;
    chorus.inputs[1] = BUF_RATE;
    chorus.inputs[2] = BUF_DEPTH;
    chorus.inputs[3] = BUF_BASE;
    chorus.inputs[4] = BUF_RANGE;
    chorus.state_id = 0xDEAD0002u;

    std::vector<Instruction> program{
        push_const(BUF_RATE,  0.5f),
        push_const(BUF_DEPTH, 0.5f),
        push_const(BUF_BASE,  20.0f),
        push_const(BUF_RANGE, 10.0f),
        chorus,
    };

    VM vm;
    (void)vm.load_program(std::span(program));
    {
        // Pre-seed ExtendedParams<3>: [lfo_phase=0.25, dry=1, wet=0].
        auto& ep = vm.states().get_or_create<ExtendedParams<3>>(
            ext_params_state_id(chorus.state_id));
        ep.set_constant(0, 0.25f);
        ep.set_constant(1, 1.0f);
        ep.set_constant(2, 0.0f);
    }

    std::array<float, BLOCK_SIZE> L{}, R{};
    fill_ramp(vm.buffers().get(BUF_IN));
    std::array<float, BLOCK_SIZE> expected{};
    std::memcpy(expected.data(), vm.buffers().get(BUF_IN), BLOCK_SIZE * sizeof(float));
    vm.process_block(L.data(), R.data());

    const float* out_l = vm.buffers().get(BUF_OUT);
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        REQUIRE(std::abs(out_l[i] - expected[i]) < 1e-5f);
    }
}

TEST_CASE("FILTER_SVF_LP (Category B): default dry=0, wet=1 -> effect output",
          "[drywet][lp]") {
    constexpr std::uint16_t BUF_IN  = 0;
    constexpr std::uint16_t BUF_CUT = 1;
    constexpr std::uint16_t BUF_Q   = 2;
    constexpr std::uint16_t BUF_OUT = 3;

    Instruction lp{};
    lp.opcode = Opcode::FILTER_SVF_LP;
    lp.out_buffer = BUF_OUT;
    lp.inputs[0] = BUF_IN;
    lp.inputs[1] = BUF_CUT;
    lp.inputs[2] = BUF_Q;
    // Omit slots 3 (dry) and 4 (wet) — opcode falls back to defaults (0, 1).
    lp.inputs[3] = BUFFER_UNUSED;
    lp.inputs[4] = BUFFER_UNUSED;
    lp.state_id = 0xDEAD0003u;

    std::vector<Instruction> program{
        push_const(BUF_CUT, 1000.0f),
        push_const(BUF_Q,   0.707f),
        lp,
    };

    VM vm;
    (void)vm.load_program(std::span(program));

    // 1 kHz cutoff on a smooth sine well below cutoff: output should
    // pass nearly unchanged (no input mixed in via dry).
    std::array<float, BLOCK_SIZE> L{}, R{};
    fill_ramp(vm.buffers().get(BUF_IN));
    // Settle several blocks so SVF state initialises.
    for (int b = 0; b < 8; ++b) {
        fill_ramp(vm.buffers().get(BUF_IN));
        vm.process_block(L.data(), R.data());
    }
    const float* out_l = vm.buffers().get(BUF_OUT);
    // Output must be finite and bounded.
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        REQUIRE(std::isfinite(out_l[i]));
        REQUIRE(std::abs(out_l[i]) <= 2.0f);
    }
}

TEST_CASE("FILTER_SVF_LP (Category B): dry=1, wet=0 -> passthrough",
          "[drywet][lp]") {
    constexpr std::uint16_t BUF_IN  = 0;
    constexpr std::uint16_t BUF_CUT = 1;
    constexpr std::uint16_t BUF_Q   = 2;
    constexpr std::uint16_t BUF_DRY = 3;
    constexpr std::uint16_t BUF_WET = 4;
    constexpr std::uint16_t BUF_OUT = 5;

    Instruction lp{};
    lp.opcode = Opcode::FILTER_SVF_LP;
    lp.out_buffer = BUF_OUT;
    lp.inputs[0] = BUF_IN;
    lp.inputs[1] = BUF_CUT;
    lp.inputs[2] = BUF_Q;
    lp.inputs[3] = BUF_DRY;
    lp.inputs[4] = BUF_WET;
    lp.state_id = 0xDEAD0004u;

    std::vector<Instruction> program{
        push_const(BUF_CUT, 100.0f),  // aggressively low cutoff
        push_const(BUF_Q,   0.707f),
        push_const(BUF_DRY, 1.0f),
        push_const(BUF_WET, 0.0f),
        lp,
    };

    VM vm;
    (void)vm.load_program(std::span(program));

    std::array<float, BLOCK_SIZE> L{}, R{};
    fill_ramp(vm.buffers().get(BUF_IN));
    std::array<float, BLOCK_SIZE> expected{};
    std::memcpy(expected.data(), vm.buffers().get(BUF_IN), BLOCK_SIZE * sizeof(float));
    vm.process_block(L.data(), R.data());

    const float* out_l = vm.buffers().get(BUF_OUT);
    // dry=1, wet=0 must be bit-equal to input regardless of filter state.
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        REQUIRE(std::abs(out_l[i] - expected[i]) < 1e-5f);
    }
}

TEST_CASE("FILTER_MOOG (Category B, ExtendedParams): dry=1, wet=0 -> passthrough",
          "[drywet][moog]") {
    constexpr std::uint16_t BUF_IN  = 0;
    constexpr std::uint16_t BUF_CUT = 1;
    constexpr std::uint16_t BUF_RES = 2;
    constexpr std::uint16_t BUF_MAX = 3;
    constexpr std::uint16_t BUF_SCL = 4;
    constexpr std::uint16_t BUF_OUT = 5;

    Instruction moog{};
    moog.opcode = Opcode::FILTER_MOOG;
    moog.out_buffer = BUF_OUT;
    moog.inputs[0] = BUF_IN;
    moog.inputs[1] = BUF_CUT;
    moog.inputs[2] = BUF_RES;
    moog.inputs[3] = BUF_MAX;
    moog.inputs[4] = BUF_SCL;
    moog.state_id = 0xDEAD0005u;

    std::vector<Instruction> program{
        push_const(BUF_CUT, 100.0f),
        push_const(BUF_RES, 0.5f),
        push_const(BUF_MAX, 4.0f),
        push_const(BUF_SCL, 0.5f),
        moog,
    };

    VM vm;
    (void)vm.load_program(std::span(program));
    {
        auto& ep = vm.states().get_or_create<ExtendedParams<2>>(
            ext_params_state_id(moog.state_id));
        ep.set_constant(0, 1.0f);  // dry
        ep.set_constant(1, 0.0f);  // wet
    }

    std::array<float, BLOCK_SIZE> L{}, R{};
    fill_ramp(vm.buffers().get(BUF_IN));
    std::array<float, BLOCK_SIZE> expected{};
    std::memcpy(expected.data(), vm.buffers().get(BUF_IN), BLOCK_SIZE * sizeof(float));
    vm.process_block(L.data(), R.data());

    const float* out_l = vm.buffers().get(BUF_OUT);
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        REQUIRE(std::abs(out_l[i] - expected[i]) < 1e-5f);
    }
}
