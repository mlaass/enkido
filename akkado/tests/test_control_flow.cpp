// Tests for forward control flow — PRD prd-runtime-functions-control-flow L1.
// Covers the when() builtin and its SKIP_IF_ZERO / SKIP_IF_NONZERO lowering.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "akkado/akkado.hpp"
#include <cedar/vm/instruction.hpp>
#include <cedar/vm/vm.hpp>
#include <cedar/dsp/constants.hpp>
#include <array>
#include <cstring>
#include <span>
#include <vector>

namespace {

std::vector<cedar::Instruction> get_instructions(const akkado::CompileResult& result) {
    std::vector<cedar::Instruction> instructions;
    size_t count = result.bytecode.size() / sizeof(cedar::Instruction);
    instructions.resize(count);
    std::memcpy(instructions.data(), result.bytecode.data(), result.bytecode.size());
    return instructions;
}

size_t count_op(const std::vector<cedar::Instruction>& insts, cedar::Opcode op) {
    size_t n = 0;
    for (const auto& i : insts) if (i.opcode == op) ++n;
    return n;
}

bool has_diag(const akkado::CompileResult& r, const std::string& code) {
    for (const auto& d : r.diagnostics) if (d.code == code) return true;
    return false;
}

// Compile + run one block, return the last sample of the left output.
float run_one_block_left(const akkado::CompileResult& r) {
    cedar::VM vm;
    REQUIRE(vm.load_program_immediate(
        std::span<const cedar::Instruction>(
            reinterpret_cast<const cedar::Instruction*>(r.bytecode.data()),
            r.bytecode.size() / sizeof(cedar::Instruction))));
    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());
    return L[cedar::BLOCK_SIZE - 1];
}

}  // namespace

// =============================================================================
// Codegen structure
// =============================================================================

TEST_CASE("when(): lowers to one SKIP_IF_ZERO and one SKIP_IF_NONZERO",
          "[control_flow][codegen]") {
    auto r = akkado::compile("out(when(1, saw(440), saw(220)))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);

    CHECK(count_op(insts, cedar::Opcode::SKIP_IF_ZERO) == 1);
    CHECK(count_op(insts, cedar::Opcode::SKIP_IF_NONZERO) == 1);
}

TEST_CASE("when(): skip offsets land on the correct instructions",
          "[control_flow][codegen]") {
    auto r = akkado::compile("out(when(1, saw(440), saw(220)))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);

    size_t skip1 = insts.size(), skip2 = insts.size();
    for (size_t i = 0; i < insts.size(); ++i) {
        if (insts[i].opcode == cedar::Opcode::SKIP_IF_ZERO) skip1 = i;
        if (insts[i].opcode == cedar::Opcode::SKIP_IF_NONZERO) skip2 = i;
    }
    REQUIRE(skip1 < insts.size());
    REQUIRE(skip2 < insts.size());
    REQUIRE(skip1 < skip2);

    // SKIP_IF_ZERO (cond==0) must jump to the first false-branch instruction,
    // i.e. the instruction immediately after SKIP_IF_NONZERO.
    CHECK(skip1 + insts[skip1].rate + 1 == skip2 + 1);
    // SKIP_IF_NONZERO (cond!=0) must jump past the false branch (and its COPY).
    CHECK(skip2 + insts[skip2].rate + 1 <= insts.size());
}

TEST_CASE("when(): branches get distinct state IDs", "[control_flow][codegen]") {
    // Both branches contain a saw oscillator; the "true"/"false" path
    // components must make their state IDs differ so they don't share state.
    auto r = akkado::compile("out(when(1, saw(440), saw(440)))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);

    std::vector<std::uint32_t> osc_states;
    for (const auto& i : insts) {
        if (i.opcode == cedar::Opcode::OSC_SAW) osc_states.push_back(i.state_id);
    }
    REQUIRE(osc_states.size() == 2);
    CHECK(osc_states[0] != osc_states[1]);
}

// =============================================================================
// Diagnostics
// =============================================================================

TEST_CASE("when(): mismatched branch channel count is E247",
          "[control_flow][codegen][error]") {
    // True branch is stereo (pan), false branch is mono.
    auto r = akkado::compile("out(when(1, pan(0.5, 0.0), 0.2))");
    CHECK_FALSE(r.success);
    CHECK(has_diag(r, "E247"));
}

TEST_CASE("when(): wrong argument count is rejected",
          "[control_flow][codegen][error]") {
    // The analyzer validates arity against the BuiltinInfo entry (3 required)
    // before codegen runs, so an under-arity call is caught with E006.
    auto r = akkado::compile("out(when(1, 0.5))");
    CHECK_FALSE(r.success);
    CHECK(has_diag(r, "E006"));
}

// =============================================================================
// Runtime behaviour
// =============================================================================

TEST_CASE("when(): true condition runs the true branch",
          "[control_flow][runtime]") {
    auto r = akkado::compile("out(when(1, 0.7, 0.2))");
    REQUIRE(r.success);
    CHECK(run_one_block_left(r) == Catch::Approx(0.7f));
}

TEST_CASE("when(): false condition runs the false branch",
          "[control_flow][runtime]") {
    auto r = akkado::compile("out(when(0, 0.7, 0.2))");
    REQUIRE(r.success);
    CHECK(run_one_block_left(r) == Catch::Approx(0.2f));
}

TEST_CASE("when(): bit-exact to select() for a constant condition",
          "[control_flow][runtime]") {
    SECTION("condition true") {
        auto rw = akkado::compile("out(when(1, 0.7, 0.2))");
        auto rs = akkado::compile("out(select(1, 0.7, 0.2))");
        REQUIRE(rw.success);
        REQUIRE(rs.success);
        CHECK(run_one_block_left(rw) == run_one_block_left(rs));
    }
    SECTION("condition false") {
        auto rw = akkado::compile("out(when(0, 0.7, 0.2))");
        auto rs = akkado::compile("out(select(0, 0.7, 0.2))");
        REQUIRE(rw.success);
        REQUIRE(rs.success);
        CHECK(run_one_block_left(rw) == run_one_block_left(rs));
    }
}

TEST_CASE("when(): piped input flows into both branches via @",
          "[control_flow][runtime]") {
    // The hole @ resolves to the piped value (0.5) in both branches.
    auto r = akkado::compile("0.5 |> when(1, @ * 2, @) |> out(@)");
    REQUIRE(r.success);
    CHECK(run_one_block_left(r) == Catch::Approx(1.0f));
}
