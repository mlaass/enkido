// Tests for the bus-routing master bus — prd-bus-routing Phase 1.
//
// Unlike the probe tests elsewhere (which compile with bypass_master), these
// exercise the master bus itself: out()/bus() lowering, the per-block bus
// epilogue (default soft-clip @ 0.9 + forced ±1.0 safety clamp), bus
// summing, and the new diagnostics.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "akkado/akkado.hpp"
#include <cedar/vm/instruction.hpp>
#include <cedar/vm/vm.hpp>
#include <cedar/dsp/constants.hpp>
#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <vector>

namespace {

std::vector<cedar::Instruction> get_instructions(const akkado::CompileResult& r) {
    std::vector<cedar::Instruction> insts;
    insts.resize(r.bytecode.size() / sizeof(cedar::Instruction));
    if (!insts.empty()) {
        std::memcpy(insts.data(), r.bytecode.data(), r.bytecode.size());
    }
    return insts;
}

std::size_t count_op(const std::vector<cedar::Instruction>& insts, cedar::Opcode op) {
    std::size_t n = 0;
    for (const auto& i : insts) if (i.opcode == op) ++n;
    return n;
}

bool has_code(const akkado::CompileResult& r, const std::string& code) {
    for (const auto& d : r.diagnostics) if (d.code == code) return true;
    return false;
}

// Compile (master bus active) and render one block; returns the L channel.
std::array<float, cedar::BLOCK_SIZE> render_left(const akkado::CompileResult& r) {
    auto insts = get_instructions(r);
    cedar::VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);
    REQUIRE(vm.load_program_immediate(
        std::span<const cedar::Instruction>(insts)));
    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());
    return L;
}

// Render one block, returning both channels.
struct StereoBlock {
    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
};
StereoBlock render_lr(const akkado::CompileResult& r) {
    auto insts = get_instructions(r);
    cedar::VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);
    REQUIRE(vm.load_program_immediate(
        std::span<const cedar::Instruction>(insts)));
    StereoBlock b;
    vm.process_block(b.L.data(), b.R.data());
    return b;
}

}  // namespace

TEST_CASE("bus-routing: out(@) and bus(0, @) are byte-identical", "[bus]") {
    auto a = akkado::compile("out(0.5)");
    auto b = akkado::compile("bus(0, 0.5)");
    REQUIRE(a.success);
    REQUIRE(b.success);
    REQUIRE(a.bytecode.size() == b.bytecode.size());
    CHECK(std::memcmp(a.bytecode.data(), b.bytecode.data(),
                      a.bytecode.size()) == 0);
}

TEST_CASE("bus-routing: out() writer carries BUS_WRITE and a real buffer",
          "[bus]") {
    auto r = akkado::compile("out(0.5)");
    REQUIRE(r.success);
    auto insts = get_instructions(r);

    // Every OUTPUT writer must have been fixed up — no bus placeholder
    // (0xFF00+) may survive into the final bytecode.
    bool found_bus_writer = false;
    for (const auto& i : insts) {
        if (i.opcode != cedar::Opcode::OUTPUT) continue;
        // No bus placeholder (0xFF00..0xFFFE) may survive; a real bus buffer
        // (< 0xFF00) or the device sentinel (0xFFFF) is fine.
        CHECK((i.out_buffer < 0xFF00 || i.out_buffer == 0xFFFF));
        if ((i.flags & cedar::InstructionFlag::BUS_WRITE) != 0) {
            found_bus_writer = true;
        }
    }
    CHECK(found_bus_writer);
}

TEST_CASE("bus-routing: epilogue emits the default tone + safety chain",
          "[bus]") {
    auto r = akkado::compile("out(0.5)");
    REQUIRE(r.success);
    auto insts = get_instructions(r);

    // Default tone chain: exactly one soft-clip.
    CHECK(count_op(insts, cedar::Opcode::DISTORT_SOFT) == 1);
    // Forced safety: two CLAMP (one per channel).
    CHECK(count_op(insts, cedar::Opcode::CLAMP) == 2);
    // Prologue: bus 0 cleared with two COPYs from BUFFER_ZERO.
    std::size_t bus_clears = 0;
    for (const auto& i : insts) {
        if (i.opcode == cedar::Opcode::COPY &&
            i.inputs[0] == cedar::BUFFER_ZERO) {
            ++bus_clears;
        }
    }
    CHECK(bus_clears == 2);
    // The prologue runs first.
    CHECK(insts.front().opcode == cedar::Opcode::COPY);
}

TEST_CASE("bus-routing: a program with no sink emits no epilogue", "[bus]") {
    auto r = akkado::compile("0.5");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::DISTORT_SOFT) == 0);
    CHECK(count_op(insts, cedar::Opcode::OUTPUT) == 0);
    CHECK(count_op(insts, cedar::Opcode::CLAMP) == 0);
}

TEST_CASE("bus-routing: a non-literal bus index is E260", "[bus][diag]") {
    auto r = akkado::compile(
        "b = param(\"b\", 1, 0, 4)\n"
        "osc(\"sin\", 440) |> bus(b, @)\n");
    CHECK_FALSE(r.success);
    CHECK(has_code(r, "E260"));
}

TEST_CASE("bus-routing: a mono bus() signal warns W202", "[bus][diag]") {
    auto r = akkado::compile("bus(1, 0.5)");
    REQUIRE(r.success);
    CHECK(has_code(r, "W202"));
}

TEST_CASE("bus-routing: the safety stage clamps an aggressive master",
          "[bus][runtime]") {
    // saw(110) * 100 is wildly over unity; the forced ±1.0 rail must hold.
    auto r = akkado::compile("osc(\"saw\", 110) |> @ * 100 |> out(@)");
    REQUIRE(r.success);
    auto L = render_left(r);
    float peak = 0.0f;
    for (float v : L) peak = std::max(peak, std::abs(v));
    CHECK(peak <= 1.0001f);   // device never exceeds the hard rail
    CHECK(peak > 0.5f);       // and the signal is still present
}

TEST_CASE("bus-routing: non-zero buses sum into bus 0", "[bus][runtime]") {
    // bus 1 = 0.3, bus 2 = 0.4 → bus 0 sees 0.7, then the default soft-clip.
    auto r = akkado::compile("bus(1, 0.3)\nbus(2, 0.4)");
    REQUIRE(r.success);
    auto L = render_left(r);
    // Both buses contributed: the output is well above either bus alone
    // (0.3 / 0.4) and below the raw sum 0.7 (the soft-clip attenuates).
    CHECK(L[0] > 0.45f);
    CHECK(L[0] < 0.7f);
}

TEST_CASE("bus-routing: out() still produces audio through the master",
          "[bus][runtime]") {
    auto r = akkado::compile("osc(\"saw\", 220) |> @ * 0.4 |> out(@)");
    REQUIRE(r.success);
    auto L = render_left(r);
    float peak = 0.0f;
    for (float v : L) peak = std::max(peak, std::abs(v));
    CHECK(peak > 0.01f);
}

// --- Phase 2: per-bus FX (mixer / master) --------------------------------

TEST_CASE("bus-routing: master identity closure skips the default soft-clip",
          "[bus][mixer]") {
    auto r = akkado::compile("master((s) -> s)\nout(0.5)");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    // The default tone chain is replaced by the (identity) closure — no
    // soft-clip is emitted. The forced safety clamp still runs.
    CHECK(count_op(insts, cedar::Opcode::DISTORT_SOFT) == 0);
    CHECK(count_op(insts, cedar::Opcode::CLAMP) == 2);
    auto L = render_left(r);
    // 0.5 passes through untouched (only the ±1.0 rail, no soft-clip pull).
    CHECK_THAT(L[0], Catch::Matchers::WithinAbs(0.5f, 1e-4f));
}

TEST_CASE("bus-routing: an arity-1 master closure processes bus 0",
          "[bus][mixer]") {
    auto r = akkado::compile("master((s) -> s |> @ * 0.5)\nout(0.8)");
    REQUIRE(r.success);
    auto L = render_left(r);
    CHECK_THAT(L[0], Catch::Matchers::WithinAbs(0.4f, 1e-3f));
}

TEST_CASE("bus-routing: an arity-2 mixer closure drives L and R separately",
          "[bus][mixer]") {
    auto r = akkado::compile(
        "mixer(0, (l, r) -> stereo(l * 0.5, r * 0.25))\nout(0.8)");
    REQUIRE(r.success);
    auto b = render_lr(r);
    CHECK_THAT(b.L[0], Catch::Matchers::WithinAbs(0.4f, 1e-3f));
    CHECK_THAT(b.R[0], Catch::Matchers::WithinAbs(0.2f, 1e-3f));
}

TEST_CASE("bus-routing: a mixer closure processes a non-zero bus before the "
          "sum into bus 0", "[bus][mixer]") {
    auto r = akkado::compile(
        "bus(1, 0.6)\nmixer(1, (s) -> s |> @ * 0.5)");
    REQUIRE(r.success);
    auto L = render_left(r);
    // bus 1 = 0.6, halved by the mixer → 0.3 into bus 0; the default
    // soft-clip @ 0.9 is near-linear at 0.3.
    CHECK(L[0] > 0.25f);
    CHECK(L[0] < 0.34f);
}

TEST_CASE("bus-routing: multiple masters — last wins, W203 on the dropped one",
          "[bus][mixer][diag]") {
    auto r = akkado::compile(
        "master((s) -> s |> @ * 0.5)\nmaster((s) -> s)\nout(0.8)");
    REQUIRE(r.success);
    CHECK(has_code(r, "W203"));
    auto L = render_left(r);
    // The last master (identity) wins — 0.8 passes through.
    CHECK_THAT(L[0], Catch::Matchers::WithinAbs(0.8f, 1e-3f));
}

TEST_CASE("bus-routing: a mono closure return broadcasts L = R with W204",
          "[bus][mixer][diag]") {
    auto r = akkado::compile("master((s) -> 0.3)\nout(0.8)");
    REQUIRE(r.success);
    CHECK(has_code(r, "W204"));
    auto b = render_lr(r);
    CHECK_THAT(b.L[0], Catch::Matchers::WithinAbs(0.3f, 1e-3f));
    CHECK_THAT(b.R[0], Catch::Matchers::WithinAbs(0.3f, 1e-3f));
}

TEST_CASE("bus-routing: a sink inside a mixer closure is E261",
          "[bus][mixer][diag]") {
    auto a = akkado::compile("master((s) -> out(s))\nout(0.5)");
    CHECK_FALSE(a.success);
    CHECK(has_code(a, "E261"));

    auto b = akkado::compile("mixer(1, (s) -> bus(2, s))\nbus(1, 0.5)");
    CHECK_FALSE(b.success);
    CHECK(has_code(b, "E261"));
}

TEST_CASE("bus-routing: a mixer on a bus with no writers warns W205",
          "[bus][mixer][diag]") {
    auto r = akkado::compile("out(0.5)\nmixer(3, (s) -> s)");
    REQUIRE(r.success);
    CHECK(has_code(r, "W205"));
}

TEST_CASE("bus-routing: a mixer closure with bad arity is E262",
          "[bus][mixer][diag]") {
    auto a = akkado::compile("master(() -> 0.5)\nout(0.5)");
    CHECK_FALSE(a.success);
    CHECK(has_code(a, "E262"));

    auto b = akkado::compile("master((a, b, c) -> 0.5)\nout(0.5)");
    CHECK_FALSE(b.success);
    CHECK(has_code(b, "E262"));
}

TEST_CASE("bus-routing: the safety stage clamps an aggressive master closure",
          "[bus][mixer][runtime]") {
    auto r = akkado::compile("master((s) -> s |> @ * 100)\nout(0.5)");
    REQUIRE(r.success);
    auto L = render_left(r);
    float peak = 0.0f;
    for (float v : L) peak = std::max(peak, std::abs(v));
    CHECK(peak <= 1.0001f);   // the forced ±1.0 rail still holds
    CHECK(peak > 0.5f);       // and the signal is present
}

TEST_CASE("bus-routing: bypass_master leaves mixer/master inert",
          "[bus][mixer]") {
    auto r = akkado::compile("master((s) -> s |> @ * 0.5)\nout(0.8)",
                             "<input>", nullptr, nullptr, false,
                             /*bypass_master=*/true);
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::DISTORT_SOFT) == 0);
    auto L = render_left(r);
    // No epilogue: out() writes 0.8 straight to the device, mixer ignored.
    CHECK_THAT(L[0], Catch::Matchers::WithinAbs(0.8f, 1e-3f));
}
