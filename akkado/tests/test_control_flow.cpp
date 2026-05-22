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

// prd-bus-routing: these tests probe out() / runtime values directly. Bypass
// the master bus so out() stays a transparent device write — no default
// soft-clip @ 0.9, no ±1.0 safety clamp. The master bus has its own tests.
akkado::CompileResult compile_raw(std::string_view src) {
    return akkado::compile(src, "<input>", nullptr, nullptr,
                           /*lint_strict=*/false, /*bypass_master=*/true);
}

}  // namespace

// =============================================================================
// Codegen structure
// =============================================================================

TEST_CASE("when(): lowers to one SKIP_IF_ZERO and one SKIP_IF_NONZERO",
          "[control_flow][codegen]") {
    auto r = compile_raw("out(when(1, saw(440), saw(220)))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);

    CHECK(count_op(insts, cedar::Opcode::SKIP_IF_ZERO) == 1);
    CHECK(count_op(insts, cedar::Opcode::SKIP_IF_NONZERO) == 1);
}

TEST_CASE("when(): skip offsets land on the correct instructions",
          "[control_flow][codegen]") {
    auto r = compile_raw("out(when(1, saw(440), saw(220)))");
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
    auto r = compile_raw("out(when(1, saw(440), saw(440)))");
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
    auto r = compile_raw("out(when(1, pan(0.5, 0.0), 0.2))");
    CHECK_FALSE(r.success);
    CHECK(has_diag(r, "E247"));
}

TEST_CASE("when(): wrong argument count is rejected",
          "[control_flow][codegen][error]") {
    // The analyzer validates arity against the BuiltinInfo entry (3 required)
    // before codegen runs, so an under-arity call is caught with E006.
    auto r = compile_raw("out(when(1, 0.5))");
    CHECK_FALSE(r.success);
    CHECK(has_diag(r, "E006"));
}

// =============================================================================
// Runtime behaviour
// =============================================================================

TEST_CASE("when(): true condition runs the true branch",
          "[control_flow][runtime]") {
    auto r = compile_raw("out(when(1, 0.7, 0.2))");
    REQUIRE(r.success);
    CHECK(run_one_block_left(r) == Catch::Approx(0.7f));
}

TEST_CASE("when(): false condition runs the false branch",
          "[control_flow][runtime]") {
    auto r = compile_raw("out(when(0, 0.7, 0.2))");
    REQUIRE(r.success);
    CHECK(run_one_block_left(r) == Catch::Approx(0.2f));
}

TEST_CASE("when(): bit-exact to select() for a constant condition",
          "[control_flow][runtime]") {
    SECTION("condition true") {
        auto rw = compile_raw("out(when(1, 0.7, 0.2))");
        auto rs = compile_raw("out(select(1, 0.7, 0.2))");
        REQUIRE(rw.success);
        REQUIRE(rs.success);
        CHECK(run_one_block_left(rw) == run_one_block_left(rs));
    }
    SECTION("condition false") {
        auto rw = compile_raw("out(when(0, 0.7, 0.2))");
        auto rs = compile_raw("out(select(0, 0.7, 0.2))");
        REQUIRE(rw.success);
        REQUIRE(rs.success);
        CHECK(run_one_block_left(rw) == run_one_block_left(rs));
    }
}

TEST_CASE("when(): piped input flows into both branches via @",
          "[control_flow][runtime]") {
    // The hole @ resolves to the piped value (0.5) in both branches.
    auto r = compile_raw("0.5 |> when(1, @ * 2, @) |> out(@)");
    REQUIRE(r.success);
    CHECK(run_one_block_left(r) == Catch::Approx(1.0f));
}

// =============================================================================
// loop() — bounded static iteration
// =============================================================================

TEST_CASE("loop(): lowers to a LOOP_STATIC header", "[control_flow][codegen]") {
    auto r = compile_raw("0.1 |> loop(4) { @ + 0.1 } |> out(@)");
    REQUIRE(r.success);
    auto insts = get_instructions(r);

    REQUIRE(count_op(insts, cedar::Opcode::LOOP_STATIC) == 1);
    for (const auto& i : insts) {
        if (i.opcode == cedar::Opcode::LOOP_STATIC) {
            // out_buffer carries the iteration count; rate is the body length
            // (PUSH_CONST 0.1, ADD, COPY-thread = 3 instructions).
            CHECK(i.out_buffer == 4);
            CHECK(i.rate == 3);
        }
    }
}

TEST_CASE("loop(): threads the accumulator through N iterations",
          "[control_flow][runtime]") {
    SECTION("additive") {
        // 0.1, then +0.1 four times -> 0.5
        auto r = compile_raw("0.1 |> loop(4) { @ + 0.1 } |> out(@)");
        REQUIRE(r.success);
        CHECK(run_one_block_left(r) == Catch::Approx(0.5f));
    }
    SECTION("multiplicative") {
        // 1.0, then *0.5 three times -> 0.125
        auto r = compile_raw("1.0 |> loop(3) { @ * 0.5 } |> out(@)");
        REQUIRE(r.success);
        CHECK(run_one_block_left(r) == Catch::Approx(0.125f));
    }
}

TEST_CASE("loop(): zero count leaves the seed unchanged",
          "[control_flow][runtime]") {
    auto r = compile_raw("0.42 |> loop(0) { @ + 1 } |> out(@)");
    REQUIRE(r.success);
    CHECK(run_one_block_left(r) == Catch::Approx(0.42f));
}

TEST_CASE("loop(): composes in a pipe chain", "[control_flow][runtime]") {
    // 0 -> +1 three times = 3 -> *10 twice = 300
    auto r = compile_raw(
        "0.0 |> loop(3) { @ + 1 } |> loop(2) { @ * 10 } |> out(@)");
    REQUIRE(r.success);
    CHECK(run_one_block_left(r) == Catch::Approx(300.0f));
}

TEST_CASE("loop(): const-folded count expression", "[control_flow][runtime]") {
    // The count must constant-fold; 2 + 2 folds to 4.
    auto r = compile_raw("0.1 |> loop(2 + 2) { @ + 0.1 } |> out(@)");
    REQUIRE(r.success);
    CHECK(run_one_block_left(r) == Catch::Approx(0.5f));
}

TEST_CASE("loop(): non-constant count is E243",
          "[control_flow][codegen][error]") {
    // A count that depends on a runtime signal cannot constant-fold.
    auto r = compile_raw("0.1 |> loop(saw(2)) { @ + 0.1 } |> out(@)");
    CHECK_FALSE(r.success);
    CHECK(has_diag(r, "E243"));
}

TEST_CASE("loop(): fractional count is E243",
          "[control_flow][codegen][error]") {
    auto r = compile_raw("0.1 |> loop(2.5) { @ + 0.1 } |> out(@)");
    CHECK_FALSE(r.success);
    CHECK(has_diag(r, "E243"));
}

TEST_CASE("loop(): bare identifier `loop` is not the loop form",
          "[control_flow][parser]") {
    // `loop` followed by `(...)` without a trailing `{` is an ordinary call,
    // so this resolves to an unknown-function error, not a parse of a loop.
    auto r = compile_raw("out(loop(4))");
    CHECK_FALSE(r.success);
    CHECK_FALSE(has_diag(r, "E243"));  // never reached the loop lowering
}
