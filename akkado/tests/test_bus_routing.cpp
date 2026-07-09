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

TEST_CASE("bus-routing: bus_buffers exposes the per-bus buffer index map",
          "[bus]") {
    // Master (bus 0) + two numbered buses → one BusBufferMapping per bus,
    // ascending by bus_index (std::map iteration order in emit_bus_epilogue).
    auto r = akkado::compile("out(0.5)\nbus(1, 0.3)\nbus(2, 0.4)");
    REQUIRE(r.success);
    REQUIRE(r.bus_buffers.size() == 3);

    CHECK(r.bus_buffers[0].bus_index == 0u);
    CHECK(r.bus_buffers[1].bus_index == 1u);
    CHECK(r.bus_buffers[2].bus_index == 2u);

    for (const auto& bb : r.bus_buffers) {
        // Right channel is always the left index + 1 (adjacency invariant).
        CHECK(bb.right_buffer == static_cast<std::uint16_t>(bb.left_buffer + 1));
        // Every bus buffer lies inside the pool the host is told to allocate.
        CHECK(bb.left_buffer < r.required_buffers);
        CHECK(bb.right_buffer < r.required_buffers);
    }

    // Left-buffer indices are pairwise distinct (no two buses share a slot).
    for (std::size_t i = 0; i < r.bus_buffers.size(); ++i) {
        for (std::size_t j = i + 1; j < r.bus_buffers.size(); ++j) {
            CHECK(r.bus_buffers[i].left_buffer != r.bus_buffers[j].left_buffer);
        }
    }
}

TEST_CASE("bus-routing: a program with no sink has empty bus_buffers",
          "[bus]") {
    // Pure computation, no out()/bus() writer → no bus epilogue, no mapping.
    auto r = akkado::compile("x = 1 + 1");
    REQUIRE(r.success);
    CHECK(r.bus_buffers.empty());
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

// ---------------------------------------------------------------------------
// Phase 3 — the diamond operator `<>` / `<>(N)`.
// ---------------------------------------------------------------------------

namespace {
// True if two programs compile to byte-identical bytecode.
bool bytecode_identical(const char* src_a, const char* src_b) {
    auto a = akkado::compile(src_a);
    auto b = akkado::compile(src_b);
    REQUIRE(a.success);
    REQUIRE(b.success);
    REQUIRE(a.bytecode.size() == b.bytecode.size());
    return std::memcmp(a.bytecode.data(), b.bytecode.data(),
                       a.bytecode.size()) == 0;
}
}  // namespace

TEST_CASE("bus-routing: `<>` is byte-identical to `|> out(@)`", "[bus]") {
    CHECK(bytecode_identical("0.5 <>", "0.5 |> out(@)"));
}

TEST_CASE("bus-routing: `<>(N)` is byte-identical to `|> bus(N, @)`", "[bus]") {
    CHECK(bytecode_identical("0.5 <>(1)", "0.5 |> bus(1, @)"));
    CHECK(bytecode_identical("0.5 <>(3)", "0.5 |> bus(3, @)"));
}

TEST_CASE("bus-routing: `<>(0)` equals bare `<>` (and `out`)", "[bus]") {
    CHECK(bytecode_identical("0.5 <>(0)", "0.5 <>"));
    CHECK(bytecode_identical("0.5 <>(0)", "0.5 |> out(@)"));
}

TEST_CASE("bus-routing: `<>` binds looser than `|>` — captures the whole chain",
          "[bus]") {
    CHECK(bytecode_identical(
        "osc(\"saw\", 220) |> lp(@, 800) <>",
        "osc(\"saw\", 220) |> lp(@, 800) |> out(@)"));
    CHECK(bytecode_identical(
        "osc(\"saw\", 220) |> lp(@, 800) <>(2)",
        "osc(\"saw\", 220) |> lp(@, 800) |> bus(2, @)"));
}

TEST_CASE("bus-routing: `<> (N)` tolerates whitespace before the paren",
          "[bus]") {
    CHECK(bytecode_identical("0.5 <> (1)", "0.5 |> bus(1, @)"));
    CHECK(bytecode_identical("0.5 <>( 1 )", "0.5 |> bus(1, @)"));
}

TEST_CASE("bus-routing: `<>(N)` with a non-literal index is rejected (E260)",
          "[bus][diag]") {
    auto r = akkado::compile("drive = param(\"d\", 1, 0, 4)\n0.5 <>(drive)");
    CHECK(!r.success);
    CHECK(has_code(r, "E260"));
}

TEST_CASE("bus-routing: `<>` with no LHS after a binary operator is rejected "
          "(E263)", "[bus][diag]") {
    // `<>` is now a pipe-precedence infix operator; in prefix position
    // (e.g. directly after a binary operator that needs an RHS) it has
    // no left-hand side and the parser emits E263.
    //
    // Note: a "leading `<>`" at the very start of user code is *not* a
    // robust test for E263 once stdlib is prepended — `<>` will be
    // consumed as an infix on the trailing expression of the last
    // stdlib statement, exactly as `|>` would be. That is the same
    // cross-statement behavior any pipe-precedence operator has under
    // this language's whitespace-insensitive statement model.
    auto r = akkado::compile("1 + <>");
    CHECK(!r.success);
    CHECK(has_code(r, "E263"));
}

TEST_CASE("bus-routing: `<>` works anywhere `|> out(@)` works", "[bus]") {
    // The diamond is now a pipe-precedence infix operator: it should
    // produce byte-identical bytecode to the explicit `|> out(@)` rewrite
    // wherever `|> out(@)` is allowed — including assignment RHS and
    // sub-expressions, not just bare expression statements.
    SECTION("assignment RHS") {
        CHECK(bytecode_identical("x = 0.5 <>", "x = 0.5 |> out(@)"));
        CHECK(bytecode_identical("x = 0.5 <>(2)", "x = 0.5 |> bus(2, @)"));
    }
    SECTION("sub-expression inside parens") {
        CHECK(bytecode_identical("out((0.5 <>))", "out((0.5 |> out(@)))"));
    }
    SECTION("regression: trailing `<>` on a multi-line assignment chain") {
        // The reported program: `<>` on its own line after a multi-line
        // pipe chain inside an assignment must compile and produce the
        // same bytecode as the explicit `|> out(@)` rewrite.
        const char* with_diamond =
            "dry = n\"c4 eb4 g4 bb4 c5 bb4 g4 eb4\"\n"
            "    |> saw(@freq) * ar(@trig, 0.005, 0.1) * 0.5\n"
            "    |> lp(@, 700)\n"
            "    |> delay(@, 0.5, 0.7, _)\n"
            "    |> reverb(@, ..{dry:0, wet:0})\n"
            "  <>";
        const char* with_pipe_out =
            "dry = n\"c4 eb4 g4 bb4 c5 bb4 g4 eb4\"\n"
            "    |> saw(@freq) * ar(@trig, 0.005, 0.1) * 0.5\n"
            "    |> lp(@, 700)\n"
            "    |> delay(@, 0.5, 0.7, _)\n"
            "    |> reverb(@, ..{dry:0, wet:0})\n"
            "  |> out(@)";
        CHECK(bytecode_identical(with_diamond, with_pipe_out));
    }
}

TEST_CASE("bus-routing: `<` still lexes as a comparison, not a diamond",
          "[bus]") {
    // `1 < 3` must parse as a comparison — adding the `<>` token must not
    // disturb a bare `<`.
    auto r = akkado::compile("out(1 < 3)");
    REQUIRE(r.success);
}

// ---------------------------------------------------------------------------
// Regression: mixer-closure stereo copy-back must not clobber bus_l before
// bus_r reads it. The closure parameter is bound directly to bus_l (and
// bus_r is its stereo pair), so `left(sg)` / `right(sg)` references inside
// the closure body alias the destination bus buffers. The naive
// `bus_l ← rl; bus_r ← rr` ordering creates a read-after-write hazard.
// ---------------------------------------------------------------------------

namespace {
// Find bus 0's L/R buffer indices from the prologue clears. The prologue
// emits exactly two `COPY <bus> ← BUFFER_ZERO` instructions for bus 0 — the
// first targets bus_0_l, the second bus_0_r.
struct BusPair { std::uint16_t l = 0xFFFF, r = 0xFFFF; };
BusPair find_bus0(const std::vector<cedar::Instruction>& insts) {
    BusPair p;
    int seen = 0;
    for (const auto& i : insts) {
        if (i.opcode == cedar::Opcode::COPY &&
            i.inputs[0] == cedar::BUFFER_ZERO) {
            if (seen == 0) p.l = i.out_buffer;
            else if (seen == 1) p.r = i.out_buffer;
            if (++seen == 2) break;
        }
    }
    return p;
}
}  // namespace

TEST_CASE("mixer-closure: stereo(0, left(sg)) puts signal on R, silence on L",
          "[bus][mixer][regression]") {
    // The reported bug: this exact closure produced silence on both channels
    // because the copy-back order was `bus_l ← 0; bus_r ← bus_l`, clobbering
    // bus_l (containing the original signal) before bus_r could read it.
    auto r = akkado::compile(
        "mixer(0, (sg) -> stereo(0, left(sg)))\nout(stereo(0.4, 0.6))");
    REQUIRE(r.success);
    auto b = render_lr(r);
    CHECK_THAT(b.L[0], Catch::Matchers::WithinAbs(0.0f, 1e-4f));
    // sg has L=0.4; left(sg) returns the L channel; goes to R output.
    CHECK_THAT(b.R[0], Catch::Matchers::WithinAbs(0.4f, 1e-4f));
}

TEST_CASE("mixer-closure: stereo(right(sg), 0) symmetric case — silence on R",
          "[bus][mixer][regression]") {
    // Symmetric to the reported pattern. rl = bus_r (an unrelated alias —
    // current order is already safe), rr = const_0 (no alias). The naive
    // order works here, but we keep the test to lock the symmetric variant.
    auto r = akkado::compile(
        "mixer(0, (sg) -> stereo(right(sg), 0))\nout(stereo(0.4, 0.6))");
    REQUIRE(r.success);
    auto b = render_lr(r);
    CHECK_THAT(b.L[0], Catch::Matchers::WithinAbs(0.6f, 1e-4f));
    CHECK_THAT(b.R[0], Catch::Matchers::WithinAbs(0.0f, 1e-4f));
}

TEST_CASE("mixer-closure: stereo(right(sg), left(sg)) swaps channels",
          "[bus][mixer][regression]") {
    // Full swap: rl = bus_r, rr = bus_l. The naive two-COPY order leaves
    // both channels holding the original right value. The fix uses a temp
    // buffer to break the cycle.
    auto r = akkado::compile(
        "mixer(0, (sg) -> stereo(right(sg), left(sg)))\n"
        "out(stereo(0.3, 0.7))");
    REQUIRE(r.success);
    auto b = render_lr(r);
    CHECK_THAT(b.L[0], Catch::Matchers::WithinAbs(0.7f, 1e-4f));
    CHECK_THAT(b.R[0], Catch::Matchers::WithinAbs(0.3f, 1e-4f));
}

TEST_CASE("mixer-closure: stereo(left(sg), right(sg)) identity is preserved",
          "[bus][mixer][regression]") {
    // Identity rebuild. rl == bus_l, rr == bus_r — both COPYs are skipped.
    // Behavior must match `master((s) -> s)`.
    auto r = akkado::compile(
        "mixer(0, (sg) -> stereo(left(sg), right(sg)))\n"
        "out(stereo(0.3, 0.7))");
    REQUIRE(r.success);
    auto b = render_lr(r);
    CHECK_THAT(b.L[0], Catch::Matchers::WithinAbs(0.3f, 1e-4f));
    CHECK_THAT(b.R[0], Catch::Matchers::WithinAbs(0.7f, 1e-4f));
}

TEST_CASE("mixer-closure: stereo(left(sg) * 2, left(sg)) computed + aliased",
          "[bus][mixer][regression]") {
    // rl is a fresh buffer (left(sg) * 2 allocates), rr aliases bus_l.
    // Same conflict class as `stereo(0, left(sg))`.
    auto r = akkado::compile(
        "mixer(0, (sg) -> stereo(left(sg) * 2, left(sg)))\nout(0.3)");
    REQUIRE(r.success);
    auto b = render_lr(r);
    CHECK_THAT(b.L[0], Catch::Matchers::WithinAbs(0.6f, 1e-4f));
    CHECK_THAT(b.R[0], Catch::Matchers::WithinAbs(0.3f, 1e-4f));
}

TEST_CASE("mixer-closure: swap closure uses a scratch buffer for the temp",
          "[bus][mixer][regression]") {
    // Codegen-level precise check: the swap case (rl=bus_r, rr=bus_l) must
    // emit three COPYs at the copy-back site — one to a scratch buffer that
    // is NEITHER bus_l nor bus_r — not a direct bus_r ← bus_l (which would
    // be the buggy direct read).
    auto r = akkado::compile(
        "mixer(0, (sg) -> stereo(right(sg), left(sg)))\nout(0.5)");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    auto bus = find_bus0(insts);
    REQUIRE(bus.l != 0xFFFF);
    REQUIRE(bus.r != 0xFFFF);

    // The fix emits: COPY tmp ← bus_l; COPY bus_l ← bus_r; COPY bus_r ← tmp.
    // The buggy order would have been: COPY bus_l ← bus_r; COPY bus_r ← bus_l.
    // Precise contract: no COPY exists with dst==bus_r and src==bus_l.
    bool has_direct_swap = false;
    for (const auto& i : insts) {
        if (i.opcode == cedar::Opcode::COPY &&
            i.out_buffer == bus.r && i.inputs[0] == bus.l) {
            has_direct_swap = true;
            break;
        }
    }
    CHECK_FALSE(has_direct_swap);
}

TEST_CASE("mixer-closure: conflict case emits bus_r write before bus_l write",
          "[bus][mixer][regression]") {
    // Codegen-level precise check for the reorder case. The fix moves the
    // `bus_r ← bus_l` COPY ahead of the `bus_l ← <zero>` COPY so bus_l is
    // read before being overwritten.
    auto r = akkado::compile(
        "mixer(0, (sg) -> stereo(0, left(sg)))\nout(0.5)");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    auto bus = find_bus0(insts);
    REQUIRE(bus.l != 0xFFFF);
    REQUIRE(bus.r != 0xFFFF);

    // Find the closure-emitted COPYs into bus_l and bus_r. Both prologue
    // clears (src == BUFFER_ZERO) come first and are skipped here. We look
    // for the first non-prologue COPY into each bus buffer.
    auto find_first_post_prologue_copy_to = [&](std::uint16_t dst) -> int {
        int prologue_seen = 0;
        for (int idx = 0; idx < static_cast<int>(insts.size()); ++idx) {
            const auto& i = insts[idx];
            if (i.opcode == cedar::Opcode::COPY &&
                i.inputs[0] == cedar::BUFFER_ZERO &&
                prologue_seen < 2) {
                ++prologue_seen;
                continue;
            }
            if (i.opcode == cedar::Opcode::COPY && i.out_buffer == dst) {
                return idx;
            }
        }
        return -1;
    };
    int idx_l = find_first_post_prologue_copy_to(bus.l);
    int idx_r = find_first_post_prologue_copy_to(bus.r);
    REQUIRE(idx_l >= 0);
    REQUIRE(idx_r >= 0);
    // The COPY that reads bus_l (writing into bus_r) must come before the
    // COPY that overwrites bus_l with the zero constant.
    CHECK(idx_r < idx_l);
    // And the bus_r COPY's source must still be bus_l (not the clobbered
    // value via some indirection).
    CHECK(insts[idx_r].inputs[0] == bus.l);
}

// ---------------------------------------------------------------------------
// Variant coverage: the alias bug class also affects arity-2 closures, the
// `master(...)` parse form, and non-zero bus indices. The mechanism is the
// same — closure params bind directly to bus_l/bus_r — so each entry below
// would have failed before the fix exactly as the arity-1 cases did.
// ---------------------------------------------------------------------------

TEST_CASE("mixer-closure: arity-2 stereo(0, l) — silence on L, signal on R",
          "[bus][mixer][regression]") {
    // Arity-2 closure: param_l → bus_l directly, so `l` aliases bus_l. Same
    // conflict class as `stereo(0, left(sg))` in arity-1 form.
    auto r = akkado::compile(
        "mixer(0, (l, r) -> stereo(0, l))\nout(stereo(0.4, 0.6))");
    REQUIRE(r.success);
    auto b = render_lr(r);
    CHECK_THAT(b.L[0], Catch::Matchers::WithinAbs(0.0f, 1e-4f));
    CHECK_THAT(b.R[0], Catch::Matchers::WithinAbs(0.4f, 1e-4f));
}

TEST_CASE("mixer-closure: arity-2 stereo(r, l) channel swap",
          "[bus][mixer][regression]") {
    // Arity-2 swap. Without the fix the two COPYs collide via bus_l.
    auto r = akkado::compile(
        "mixer(0, (l, r) -> stereo(r, l))\nout(stereo(0.3, 0.7))");
    REQUIRE(r.success);
    auto b = render_lr(r);
    CHECK_THAT(b.L[0], Catch::Matchers::WithinAbs(0.7f, 1e-4f));
    CHECK_THAT(b.R[0], Catch::Matchers::WithinAbs(0.3f, 1e-4f));
}

TEST_CASE("mixer-closure: master((sg) -> stereo(0, left(sg))) parse-path",
          "[bus][mixer][regression]") {
    // `master(...)` is a separate parser form for `mixer(0, ...)`. Exercise
    // it once to lock the alias fix on this path too.
    auto r = akkado::compile(
        "master((sg) -> stereo(0, left(sg)))\nout(stereo(0.4, 0.6))");
    REQUIRE(r.success);
    auto b = render_lr(r);
    CHECK_THAT(b.L[0], Catch::Matchers::WithinAbs(0.0f, 1e-4f));
    CHECK_THAT(b.R[0], Catch::Matchers::WithinAbs(0.4f, 1e-4f));
}

TEST_CASE("mixer-closure: non-zero bus — the reported `mixer(2, ...)` shape",
          "[bus][mixer][regression]") {
    // The reproduction the user reported used `mixer(2, ...)` after a
    // `<>(2)` send. Bus 2 then sums into bus 0 in the epilogue. The closure
    // mechanism is identical to bus 0, but lock the user-reported topology.
    auto r = akkado::compile(
        "osc(\"saw\", 220) * 0.3 <>(2)\n"
        "mixer(2, (sg) -> stereo(0, left(sg)))");
    REQUIRE(r.success);
    auto b = render_lr(r);
    // L stays at the bus-0 idle level (silence — no other writers).
    float peak_l = 0.0f;
    for (float v : b.L) peak_l = std::max(peak_l, std::abs(v));
    CHECK(peak_l < 1e-3f);
    // R carries the saw — peak well above noise.
    float peak_r = 0.0f;
    for (float v : b.R) peak_r = std::max(peak_r, std::abs(v));
    CHECK(peak_r > 0.05f);
}

TEST_CASE("mixer-closure: stereo(left(sg) + right(sg), right(sg)) sum + alias",
          "[bus][mixer][regression]") {
    // rl is a fresh sum buffer (left+right allocates), rr == bus_r. The
    // naive order would skip the right COPY (rr == bus_r) and overwrite
    // bus_l with the precomputed sum — already safe. Lock the behavior:
    // L = L_in + R_in (sum-to-mono on left), R unchanged.
    auto r = akkado::compile(
        "mixer(0, (sg) -> stereo(left(sg) + right(sg), right(sg)))\n"
        "out(stereo(0.2, 0.3))");
    REQUIRE(r.success);
    auto b = render_lr(r);
    CHECK_THAT(b.L[0], Catch::Matchers::WithinAbs(0.5f, 1e-4f));
    CHECK_THAT(b.R[0], Catch::Matchers::WithinAbs(0.3f, 1e-4f));
}

TEST_CASE("mixer-closure: stereo(right(sg), left(sg) + right(sg)) sum + swap",
          "[bus][mixer][regression]") {
    // rl == bus_r, rr is a fresh sum buffer. Not a true swap (rr != bus_l)
    // but rl == bus_r still requires care: with the default order, the
    // first COPY writes bus_l, then the second COPY writes bus_r — bus_r
    // is the source for rl, but the first COPY only reads it (no write to
    // bus_r yet), so it's safe.
    auto r = akkado::compile(
        "mixer(0, (sg) -> stereo(right(sg), left(sg) + right(sg)))\n"
        "out(stereo(0.2, 0.3))");
    REQUIRE(r.success);
    auto b = render_lr(r);
    CHECK_THAT(b.L[0], Catch::Matchers::WithinAbs(0.3f, 1e-4f));
    CHECK_THAT(b.R[0], Catch::Matchers::WithinAbs(0.5f, 1e-4f));
}

TEST_CASE("mixer-closure: pipe-form closure body — `sg |> stereo(0, left(@))`",
          "[bus][mixer][regression]") {
    // Closure body written as a pipe expression. `@` inside the pipe binds
    // to `sg`; same alias mechanics as the direct call form.
    auto r = akkado::compile(
        "mixer(0, (sg) -> sg |> stereo(0, left(@)))\n"
        "out(stereo(0.4, 0.6))");
    REQUIRE(r.success);
    auto b = render_lr(r);
    CHECK_THAT(b.L[0], Catch::Matchers::WithinAbs(0.0f, 1e-4f));
    CHECK_THAT(b.R[0], Catch::Matchers::WithinAbs(0.4f, 1e-4f));
}

// --- Per-bus mixer trim (studio daw-core OQ5) --------------------------------
// The host pokes "__bus_trim_<N>" and the bus epilogue multiplies bus N by it
// (bus 0 = master fader), so a host mixer fader reaches the real master. Unpoked
// buses are unity (nondestructive). Identity master keeps the arithmetic exact.
TEST_CASE("bus-routing: per-bus trim scales the master (OQ5)", "[bus][trim]") {
    auto r = akkado::compile("master((s) -> s)\nbus(1, 0.5)\nbus(2, 0.3)");
    REQUIRE(r.success);
    const auto insts = get_instructions(r);

    // BUS_TRIM emitted per bus (2 non-master + master = 3).
    CHECK(count_op(insts, cedar::Opcode::BUS_TRIM) == 3);

    auto render = [&](const char* name, float val) {
        cedar::VM vm;
        vm.set_sample_rate(48000.0f);
        vm.set_bpm(120.0f);
        REQUIRE(vm.load_program_immediate(
            std::span<const cedar::Instruction>(insts)));
        if (name != nullptr) vm.set_param(name, val);  // poked before first block
        std::array<float, cedar::BLOCK_SIZE> L{}, R{};
        vm.process_block(L.data(), R.data());
        return L[64];  // mid-block
    };

    using Catch::Matchers::WithinAbs;
    // Unpoked → unity: master = 0.5 + 0.3 = 0.8 (bit-exact vs a no-trim build).
    CHECK_THAT(render(nullptr, 0.0f), WithinAbs(0.8f, 1e-5f));
    // Mute bus 1 → 0.3.
    CHECK_THAT(render("__bus_trim_1", 0.0f), WithinAbs(0.3f, 1e-5f));
    // Halve bus 1 → 0.25 + 0.3 = 0.55.
    CHECK_THAT(render("__bus_trim_1", 0.5f), WithinAbs(0.55f, 1e-5f));
    // Master fader 0.5 → 0.8 * 0.5 = 0.4.
    CHECK_THAT(render("__bus_trim_0", 0.5f), WithinAbs(0.4f, 1e-5f));
}

// --- Friendly bus labels (studio daw-core OQ4) -------------------------------
// bus(N, sig, "name") / mixer(N, closure, "name") / master(closure, "name")
// attach a label to the bus, surfaced on BusBufferMapping so a host names stem
// files + mixer strips from the code. Empty ⇒ host falls back to bus<N>/master.
TEST_CASE("bus-routing: bus()/mixer() accept a friendly label (OQ4)",
          "[bus][label]") {
    auto label_of = [](const akkado::CompileResult& r, std::uint32_t bus) {
        for (const auto& bb : r.bus_buffers)
            if (bb.bus_index == bus) return bb.label;
        return std::string{"<missing>"};
    };

    SECTION("bus() trailing label") {
        auto r = akkado::compile("bus(1, 0.5, \"kick\")\nbus(2, 0.3, \"pads\")");
        REQUIRE(r.success);
        CHECK(label_of(r, 1) == "kick");
        CHECK(label_of(r, 2) == "pads");
        CHECK(label_of(r, 0).empty());  // master unlabeled → host default
    }
    SECTION("mixer()/master() trailing label") {
        auto r = akkado::compile(
            "bus(1, 0.5)\n"
            "mixer(1, (s) -> s |> @ * 0.5, \"drums\")\n"
            "master((s) -> s, \"mix\")");
        REQUIRE(r.success);
        CHECK(label_of(r, 1) == "drums");
        CHECK(label_of(r, 0) == "mix");
    }
    SECTION("unlabeled buses stay empty (nondestructive)") {
        auto r = akkado::compile("bus(1, 0.5)\nout(0.2)");
        REQUIRE(r.success);
        CHECK(label_of(r, 1).empty());
        CHECK(label_of(r, 0).empty());
    }
    SECTION("a label alone is not a signal") {
        auto r = akkado::compile("bus(1, \"kick\")");
        CHECK_FALSE(r.success);  // label popped, no signal left → E260
    }
}

// --- Per-bus hot-swap crossfade (studio daw-core OQ2) ------------------------
// The master has always been crossfaded on a hot-swap; the per-bus scratch pairs
// (what a host taps for stems) used to switch at a hard block edge. Both
// programs now execute during the swap window, the outgoing program's stems are
// copied out before the incoming one overwrites the shared pool, and the stems
// are blended with the same equal-power law and written back into the incoming
// program's bus buffers.
namespace {
std::uint16_t bus_left_of(const akkado::CompileResult& r, std::uint32_t bus) {
    for (const auto& b : r.bus_buffers)
        if (b.bus_index == bus) return b.left_buffer;
    return 0xFFFF;
}
}  // namespace

TEST_CASE("bus-routing: per-bus stems crossfade on hot-swap (OQ2)",
          "[bus][crossfade]") {
    using Catch::Matchers::WithinAbs;

    SECTION("a changed bus blends instead of jumping at the block edge") {
        auto A = akkado::compile("master((s) -> s)\nbus(1, 0.8)");
        auto B = akkado::compile("master((s) -> s)\nbus(1, 0.2)");
        REQUIRE(A.success);
        REQUIRE(B.success);
        const auto ia = get_instructions(A), ib = get_instructions(B);
        const std::uint16_t b1 = bus_left_of(B, 1);
        REQUIRE(b1 != 0xFFFF);

        cedar::VM vm;
        vm.set_sample_rate(48000.0f);
        vm.set_bpm(120.0f);
        REQUIRE(vm.load_program_immediate(std::span<const cedar::Instruction>(ia)));
        std::array<float, cedar::BLOCK_SIZE> L{}, R{};
        vm.process_block(L.data(), R.data());
        CHECK_THAT(vm.buffers().get(bus_left_of(A, 1))[0], WithinAbs(0.8f, 1e-5f));

        REQUIRE(vm.load_program(std::span<const cedar::Instruction>(ib)) ==
                cedar::VM::LoadResult::Success);

        // Walk the crossfade: the stem must pass through intermediate values
        // between the old (0.8) and new (0.2) levels, tracking the master.
        bool saw_intermediate = false;
        for (int b = 0; b < 6; ++b) {
            vm.process_block(L.data(), R.data());
            const float stem = vm.buffers().get(b1)[0];
            // Identity master + a single bus ⇒ the stem equals the master mix.
            CHECK_THAT(stem, WithinAbs(L[0], 1e-5f));
            CHECK(stem <= 0.8f + 1e-5f);
            CHECK(stem >= 0.2f - 1e-5f);
            if (stem > 0.2f + 1e-3f && stem < 0.8f - 1e-3f) saw_intermediate = true;
        }
        CHECK(saw_intermediate);  // would be false with a hard block-edge switch
        // Settled on the new program.
        vm.process_block(L.data(), R.data());
        CHECK_THAT(vm.buffers().get(b1)[0], WithinAbs(0.2f, 1e-5f));
    }

    SECTION("a bus only the new program has fades up from silence") {
        auto A = akkado::compile("master((s) -> s)\nout(0.5)");
        auto B = akkado::compile("master((s) -> s)\nout(0.5)\nbus(1, 0.6)");
        REQUIRE(A.success);
        REQUIRE(B.success);
        const auto ia = get_instructions(A), ib = get_instructions(B);
        const std::uint16_t b1 = bus_left_of(B, 1);
        REQUIRE(b1 != 0xFFFF);

        cedar::VM vm;
        vm.set_sample_rate(48000.0f);
        vm.set_bpm(120.0f);
        REQUIRE(vm.load_program_immediate(std::span<const cedar::Instruction>(ia)));
        std::array<float, cedar::BLOCK_SIZE> L{}, R{};
        vm.process_block(L.data(), R.data());
        REQUIRE(vm.load_program(std::span<const cedar::Instruction>(ib)) ==
                cedar::VM::LoadResult::Success);

        vm.process_block(L.data(), R.data());
        // First crossfade block is all-old: the new bus is still silent, not 0.6.
        CHECK(vm.buffers().get(b1)[0] < 0.6f - 1e-3f);
        for (int b = 0; b < 5; ++b) vm.process_block(L.data(), R.data());
        CHECK_THAT(vm.buffers().get(b1)[0], WithinAbs(0.6f, 1e-5f));  // settled
    }
}
