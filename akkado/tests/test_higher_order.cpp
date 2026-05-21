// Higher-order DSL + FOREACH_EVENT codegen tests —
// PRD prd-runtime-functions-control-flow L3.

#include <catch2/catch_test_macros.hpp>

#include "akkado/akkado.hpp"
#include <cedar/vm/vm.hpp>
#include <cedar/vm/instruction.hpp>
#include <cedar/dsp/constants.hpp>

#include <array>
#include <cmath>
#include <span>
#include <vector>

namespace {

std::vector<cedar::Instruction> get_instructions(const akkado::CompileResult& r) {
    std::vector<cedar::Instruction> insts(
        r.bytecode.size() / sizeof(cedar::Instruction));
    std::memcpy(insts.data(), r.bytecode.data(), r.bytecode.size());
    return insts;
}

std::size_t count_op(const std::vector<cedar::Instruction>& insts, cedar::Opcode op) {
    std::size_t n = 0;
    for (const auto& i : insts) if (i.opcode == op) ++n;
    return n;
}

bool has_diag(const akkado::CompileResult& r, const std::string& code) {
    for (const auto& d : r.diagnostics) if (d.code == code) return true;
    return false;
}

// Apply the SequenceProgram + ForeachAlloc state inits a compiled FOREACH_EVENT
// program needs. seq_storage must outlive every process_block() call.
void apply_inits(cedar::VM& vm, const akkado::CompileResult& r,
                 std::vector<std::vector<cedar::Sequence>>& seq_storage) {
    seq_storage.reserve(r.state_inits.size());
    for (const auto& init : r.state_inits) {
        if (init.type == akkado::StateInitData::Type::SequenceProgram) {
            std::vector<cedar::Sequence> seq_copy = init.sequences;
            for (std::size_t i = 0;
                 i < seq_copy.size() && i < init.sequence_events.size(); ++i) {
                if (!init.sequence_events[i].empty()) {
                    seq_copy[i].events = const_cast<cedar::Event*>(
                        init.sequence_events[i].data());
                    seq_copy[i].num_events = static_cast<std::uint32_t>(
                        init.sequence_events[i].size());
                    seq_copy[i].capacity = seq_copy[i].num_events;
                }
            }
            seq_storage.push_back(std::move(seq_copy));
            auto& stored = seq_storage.back();
            vm.init_sequence_program_state(init.state_id, stored.data(),
                                           stored.size(), init.cycle_length,
                                           init.is_sample_pattern,
                                           init.total_events);
        } else if (init.type == akkado::StateInitData::Type::ForeachAlloc) {
            vm.init_foreach_state(init.state_id, init.foreach_allocator_kind,
                                  init.foreach_block_id,
                                  init.foreach_event_src_state_id,
                                  init.foreach_max_iterations,
                                  init.poly_max_voices, init.poly_mode,
                                  init.poly_steal_strategy,
                                  init.poly_release_seconds);
        }
    }
}

// Compile, load (staging the block table), apply state inits, render `blocks`
// blocks, and return the peak absolute sample on the left channel.
float render_peak(const akkado::CompileResult& r, int blocks) {
    auto insts = get_instructions(r);
    cedar::VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);
    vm.set_block_table(r.block_table, r.main_instruction_count);
    REQUIRE(vm.load_program_immediate(std::span<const cedar::Instruction>(insts)));
    std::vector<std::vector<cedar::Sequence>> seq_storage;
    apply_inits(vm, r, seq_storage);
    float peak = 0.0f;
    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    for (int b = 0; b < blocks; ++b) {
        vm.process_block(L.data(), R.data());
        for (float s : L) peak = std::max(peak, std::abs(s));
    }
    return peak;
}

}  // namespace

// =============================================================================
// POLY migration — poly() emits FOREACH_EVENT, not POLY_BEGIN
// =============================================================================

TEST_CASE("poly() compiles to FOREACH_EVENT + a subprogram block", "[L3][poly]") {
    auto r = akkado::compile(R"(
        fn lead({freq, gate, vel}) -> osc("sin", freq)
        n"c4" |> poly(@, lead, 4) |> out(@)
    )");
    REQUIRE(r.success);
    auto insts = get_instructions(r);

    CHECK(count_op(insts, cedar::Opcode::FOREACH_EVENT) == 1);
    CHECK(count_op(insts, cedar::Opcode::POLY_BEGIN) == 0);
    CHECK(count_op(insts, cedar::Opcode::POLY_END) == 0);

    // One subprogram block — the instrument body.
    REQUIRE(r.block_table.size() == 1);
    CHECK(r.block_table[0].length > 0);
    CHECK(r.block_table[0].output_count == 2);
    // The block body lives after the main program.
    CHECK(r.block_table[0].offset >= r.main_instruction_count);

    // A ForeachAlloc state init with VOICE_POOL allocator.
    bool found = false;
    for (const auto& s : r.state_inits) {
        if (s.type == akkado::StateInitData::Type::ForeachAlloc) {
            CHECK(s.foreach_allocator_kind == 0);  // VOICE_POOL
            CHECK(s.poly_max_voices == 4);
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("poly() still renders audio through the FOREACH_EVENT path", "[L3][poly]") {
    auto r = akkado::compile(R"(
        fn lead({freq, gate, vel}) -> osc("sin", freq) * ar(gate, 0.01, 0.3) * vel
        n"c4 e4 g4" |> poly(@, lead, 8) |> out(@)
    )");
    REQUIRE(r.success);
    CHECK(render_peak(r, 200) > 0.01f);
}

// =============================================================================
// each_voice — higher-order per-event instrument (PER_ITERATION)
// =============================================================================

TEST_CASE("each_voice() emits FOREACH_EVENT with the PER_ITERATION allocator",
          "[L3][each_voice]") {
    auto r = akkado::compile(
        "n\"c4 e4 g4\" |> each_voice(@, (v) -> osc(\"sin\", v) * 0.3) |> out(@)");
    REQUIRE(r.success);
    auto insts = get_instructions(r);

    CHECK(count_op(insts, cedar::Opcode::FOREACH_EVENT) == 1);
    REQUIRE(r.block_table.size() == 1);
    CHECK(r.block_table[0].length > 0);

    bool found = false;
    for (const auto& s : r.state_inits) {
        if (s.type == akkado::StateInitData::Type::ForeachAlloc) {
            CHECK(s.foreach_allocator_kind == 1);  // PER_ITERATION
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("each_voice() renders audio", "[L3][each_voice]") {
    auto r = akkado::compile(
        "n\"c4 e4 g4\" |> each_voice(@, (v) -> osc(\"sin\", v) * 0.3) |> out(@)");
    REQUIRE(r.success);
    CHECK(render_peak(r, 200) > 0.01f);
}

TEST_CASE("each_voice() rejects a non-event-stream operand with E242",
          "[L3][each_voice][error]") {
    auto r = akkado::compile(
        "each_voice(osc(\"sin\", 440), (v) -> osc(\"sin\", v)) |> out(@)");
    CHECK_FALSE(r.success);
    CHECK(has_diag(r, "E242"));
}

// =============================================================================
// Event-record lambda parameters (PRD L3 — n.freq / n.vel / n.gate / ...)
// =============================================================================

TEST_CASE("each_voice() lambda parameter exposes event-record fields",
          "[L3][each_voice][record]") {
    auto r = akkado::compile(
        "n\"c4 e4 g4\" |> each_voice(@, (n) -> osc(\"sin\", n.freq) * 0.3) "
        "|> out(@)");
    REQUIRE(r.success);
    CHECK(render_peak(r, 200) > 0.01f);
}

TEST_CASE("each_voice() synthesizes n.trig to drive a per-event envelope",
          "[L3][each_voice][record]") {
    auto r = akkado::compile(
        "n\"c4 e4 g4\" |> each_voice(@, "
        "(n) -> osc(\"sin\", n.freq) * ar(n.trig, 0.01, 0.2)) |> out(@)");
    REQUIRE(r.success);
    CHECK(render_peak(r, 200) > 0.01f);
}

TEST_CASE("each_voice() rejects a field absent from the event record (E408)",
          "[L3][each_voice][error]") {
    auto r = akkado::compile(
        "n\"c4\" |> each_voice(@, (n) -> osc(\"sin\", n.phase))");
    CHECK_FALSE(r.success);
    CHECK(has_diag(r, "E408"));
}

// =============================================================================
// each — side-effecting per-event sink (PER_ITERATION, no mix)
// =============================================================================

TEST_CASE("each() emits FOREACH_EVENT with the PER_ITERATION allocator",
          "[L3][each]") {
    auto r = akkado::compile(
        "n\"c4 e4 g4\" |> each(@, (n) -> osc(\"sin\", n.freq) * 0.3 |> out(@))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::FOREACH_EVENT) == 1);
    REQUIRE(r.block_table.size() == 1);

    bool found = false;
    for (const auto& s : r.state_inits) {
        if (s.type == akkado::StateInitData::Type::ForeachAlloc) {
            CHECK(s.foreach_allocator_kind == 1);  // PER_ITERATION
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("each() body out() calls accumulate audio", "[L3][each]") {
    auto r = akkado::compile(
        "n\"c4 e4 g4\" |> each(@, (n) -> osc(\"sin\", n.freq) * 0.3 |> out(@))");
    REQUIRE(r.success);
    CHECK(render_peak(r, 200) > 0.01f);
}

// =============================================================================
// reduce — event-stream accumulator (SHARED), unified with array reduce()
// =============================================================================

TEST_CASE("reduce() over a pattern emits FOREACH_EVENT with the SHARED allocator",
          "[L3][reduce]") {
    auto r = akkado::compile(
        "n\"c4 e4 g4\" |> reduce(@, (acc, n) -> acc + n.freq, 0.0) |> out(@)");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::FOREACH_EVENT) == 1);

    bool found = false;
    for (const auto& s : r.state_inits) {
        if (s.type == akkado::StateInitData::Type::ForeachAlloc) {
            CHECK(s.foreach_allocator_kind == 2);  // SHARED
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("reduce() over a pattern renders a finite accumulator signal",
          "[L3][reduce]") {
    auto r = akkado::compile(
        "n\"c4 e4 g4\" |> reduce(@, (acc, n) -> acc + n.freq, 0.0) |> out(@)");
    REQUIRE(r.success);
    float peak = render_peak(r, 200);
    CHECK(std::isfinite(peak));
}

TEST_CASE("reduce() over an array still unrolls at compile time (no FOREACH_EVENT)",
          "[L3][reduce]") {
    auto r = akkado::compile(
        "osc(\"sin\", reduce([110.0, 110.0, 110.0], (a, b) -> a + b, 0.0)) "
        "|> out(@)");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::FOREACH_EVENT) == 0);
}

TEST_CASE("reduce() over a pattern rejects a 1-parameter lambda (E409)",
          "[L3][reduce][error]") {
    auto r = akkado::compile(
        "n\"c4\" |> reduce(@, (acc) -> acc, 0.0) |> out(@)");
    CHECK_FALSE(r.success);
    CHECK(has_diag(r, "E409"));
}
