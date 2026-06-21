// Leg 3 of the memory-integrity harness (docs/prd-memory-integrity-tests.md):
// mechanically enforce the "no allocations in the audio path" invariant.
//
// Pattern (per §9.5): load a representative program → run warmup blocks so all
// lazily-allocated DSP state (oscillator phase, SVF state, delay line) is in
// place → arm the ZeroAllocGuard → run measured blocks → disarm → assert zero
// allocations occurred while armed.
//
// The guard's overrides are compiled out under AddressSanitizer (ASan owns
// operator new), so this test passes trivially in the sanitize build; its
// authoritative run is the release build (scripts/memory/run_all.sh,
// scripts/check-release.sh).

#include <catch2/catch_test_macros.hpp>

#include "cedar/vm/vm.hpp"
#include "cedar/vm/instruction.hpp"
#include "cedar/dsp/constants.hpp"
#include "zero_alloc_guard.hpp"

#include <array>
#include <bit>
#include <vector>

using namespace cedar;

namespace {

Instruction push_const(std::uint16_t out, float value) {
    Instruction inst{};
    inst.opcode = Opcode::PUSH_CONST;
    inst.out_buffer = out;
    inst.inputs[0] = inst.inputs[1] = inst.inputs[2] =
        inst.inputs[3] = inst.inputs[4] = BUFFER_UNUSED;
    inst.state_id = std::bit_cast<std::uint32_t>(value);
    return inst;
}

// A representative signal chain that exercises three distinct stateful opcode
// families: an oscillator (OscState), a state-variable filter (filter state),
// and a delay (a heap/arena-backed delay line allocated on first block).
std::vector<Instruction> representative_program() {
    constexpr std::uint16_t BUF_FREQ = 0;
    constexpr std::uint16_t BUF_OSC  = 1;
    constexpr std::uint16_t BUF_CUT  = 2;
    constexpr std::uint16_t BUF_Q    = 3;
    constexpr std::uint16_t BUF_FILT = 4;
    constexpr std::uint16_t BUF_TIME = 5;
    constexpr std::uint16_t BUF_FB   = 6;
    constexpr std::uint16_t BUF_DRY  = 7;
    constexpr std::uint16_t BUF_WET  = 8;
    constexpr std::uint16_t BUF_DLY  = 9;
    constexpr std::uint16_t BUF_OUT  = 10;

    Instruction osc =
        Instruction::make_unary(Opcode::OSC_SIN, BUF_OSC, BUF_FREQ, 0xA0C0'0001u);

    Instruction lp{};
    lp.opcode = Opcode::FILTER_SVF_LP;
    lp.out_buffer = BUF_FILT;
    lp.inputs[0] = BUF_OSC;
    lp.inputs[1] = BUF_CUT;
    lp.inputs[2] = BUF_Q;
    lp.inputs[3] = BUFFER_UNUSED;  // dry → default 0
    lp.inputs[4] = BUFFER_UNUSED;  // wet → default 1
    lp.state_id = 0xA0C0'0002u;

    Instruction delay{};
    delay.opcode = Opcode::DELAY;
    delay.out_buffer = BUF_DLY;
    delay.inputs[0] = BUF_FILT;
    delay.inputs[1] = BUF_TIME;
    delay.inputs[2] = BUF_FB;
    delay.inputs[3] = BUF_DRY;
    delay.inputs[4] = BUF_WET;
    delay.rate = 0;  // seconds
    delay.state_id = 0xA0C0'0003u;

    return {
        push_const(BUF_FREQ, 220.0f),
        osc,
        push_const(BUF_CUT, 1200.0f),
        push_const(BUF_Q, 0.707f),
        lp,
        push_const(BUF_TIME, 0.1f),
        push_const(BUF_FB, 0.3f),
        push_const(BUF_DRY, 1.0f),
        push_const(BUF_WET, 0.5f),
        delay,
        Instruction::make_unary(Opcode::OUTPUT, BUF_OUT, BUF_DLY),
    };
}

}  // namespace

TEST_CASE("zero-alloc: VM block processing does not allocate", "[zero_alloc]") {
    VM vm;
    vm.set_crossfade_blocks(0);
    auto program = representative_program();
    REQUIRE(vm.load_program(std::span(program)) == VM::LoadResult::Success);

    std::array<float, BLOCK_SIZE> L{}, R{};

    // Warmup: first blocks allocate the delay line and create all DSP states.
    // 128 blocks ≈ 0.34 s @ 48 kHz/128 — well past any first-block setup.
    for (int b = 0; b < 128; ++b) {
        vm.process_block(L.data(), R.data());
    }

    {
        cedar::testing::ZeroAllocGuard guard;
        for (int b = 0; b < 512; ++b) {
            vm.process_block(L.data(), R.data());
        }
    }

    const std::size_t violations = cedar::testing::zero_alloc_violations();
    if (violations != 0) {
        UNSCOPED_INFO("first allocation while armed was "
                      << cedar::testing::zero_alloc_first_bytes() << " bytes");
    }
    CHECK(violations == 0);
}

TEST_CASE("zero-alloc: guard actually traps an allocation", "[zero_alloc]") {
    // Sensibility check (§11.1): prove the trap catches a real allocation so a
    // pass on the VM test above is meaningful and not vacuous. Skipped under
    // ASan, where the overrides are compiled out.
    if (!cedar::testing::zero_alloc_active()) {
        SUCCEED("zero-alloc hooks compiled out (ASan build) — trap inert");
        return;
    }

    volatile int* leak = nullptr;
    {
        cedar::testing::ZeroAllocGuard guard;
        leak = new int(42);  // deliberate allocation inside the armed window
    }
    CHECK(cedar::testing::zero_alloc_violations() >= 1);
    delete leak;
}
