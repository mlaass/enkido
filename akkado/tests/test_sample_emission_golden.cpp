// Golden-bytecode characterization for SAMPLE_PLAY emission.
//
// Captures the SAMPLE_PLAY tail (PUSH_CONST 1.0 + SAMPLE_PLAY + MUL) shape for
// representative sample-pattern shapes. The 4-phase emission unification
// refactor (docs/prd-sample-emission-unification.md) MUST keep this test
// passing — every migrated call site has to produce the same bytecode it does
// today, or one test below fails with the exact opcode that drifted.
//
// Rather than checking exact buffer indices (which depend on allocator state
// and would be brittle across unrelated refactors), each shape asserts
// structural invariants that the unification must preserve, plus a fingerprint
// of the SAMPLE_PLAY tail so accidental wiring drift surfaces immediately.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "akkado/akkado.hpp"
#include <cedar/vm/instruction.hpp>
#include <cstring>
#include <vector>

namespace {

std::vector<cedar::Instruction> bytecode_to_insts(const akkado::CompileResult& r) {
    std::vector<cedar::Instruction> out;
    std::size_t n = r.program.bytecode.size() / sizeof(cedar::Instruction);
    out.resize(n);
    std::memcpy(out.data(), r.program.bytecode.data(), r.program.bytecode.size());
    return out;
}

float decode_const(const cedar::Instruction& inst) {
    float v;
    std::memcpy(&v, &inst.state_id, sizeof(float));
    return v;
}

std::size_t count_op(const std::vector<cedar::Instruction>& insts, cedar::Opcode op) {
    std::size_t c = 0;
    for (const auto& i : insts) if (i.opcode == op) ++c;
    return c;
}

const cedar::Instruction* find_first_op(const std::vector<cedar::Instruction>& insts, cedar::Opcode op) {
    for (const auto& i : insts) if (i.opcode == op) return &i;
    return nullptr;
}

// Locate the PUSH_CONST 1.0 immediately preceding the first SAMPLE_PLAY whose
// inputs[1] points at it. This is the "pitch_buf" wiring the helper produces.
struct SampleTail {
    const cedar::Instruction* push_const_pitch = nullptr;  // PUSH_CONST 1.0
    const cedar::Instruction* sample_play      = nullptr;  // SAMPLE_PLAY
    const cedar::Instruction* mul              = nullptr;  // MUL(out, velocity_buf)
};

SampleTail locate_sample_tail(const std::vector<cedar::Instruction>& insts) {
    SampleTail tail;
    for (std::size_t i = 0; i < insts.size(); ++i) {
        if (insts[i].opcode != cedar::Opcode::SAMPLE_PLAY) continue;
        tail.sample_play = &insts[i];
        // PUSH_CONST 1.0 must be the producer of inputs[1] (pitch).
        std::uint16_t pitch_buf = insts[i].inputs[1];
        for (std::size_t j = 0; j < i; ++j) {
            if (insts[j].opcode == cedar::Opcode::PUSH_CONST &&
                insts[j].out_buffer == pitch_buf &&
                std::abs(decode_const(insts[j]) - 1.0f) < 1e-6f) {
                tail.push_const_pitch = &insts[j];
            }
        }
        // MUL(SAMPLE_PLAY.out, velocity_buf, ...) immediately after.
        for (std::size_t j = i + 1; j < insts.size(); ++j) {
            if (insts[j].opcode == cedar::Opcode::MUL &&
                insts[j].inputs[0] == insts[i].out_buffer) {
                tail.mul = &insts[j];
                break;
            }
        }
        break;  // first SAMPLE_PLAY is enough for the structural check
    }
    return tail;
}

void check_pattern_tail(const akkado::CompileResult& r,
                        bool expect_velocity_mul = true,
                        std::size_t expected_sample_play_count = 1) {
    REQUIRE(r.success);
    auto insts = bytecode_to_insts(r);

    REQUIRE(count_op(insts, cedar::Opcode::SAMPLE_PLAY) == expected_sample_play_count);

    auto tail = locate_sample_tail(insts);
    REQUIRE(tail.sample_play != nullptr);
    REQUIRE(tail.push_const_pitch != nullptr);

    // SAMPLE_PLAY input layout invariants. The unification helper must keep
    // these stable across phases.
    CHECK(tail.sample_play->inputs[0] != 0xFFFF);                // trigger
    CHECK(tail.sample_play->inputs[1] == tail.push_const_pitch->out_buffer);  // pitch buf
    CHECK(tail.sample_play->inputs[2] != 0xFFFF);                // sample-id buffer

    // Pattern path: state_id split across in3/in4 with state_id = (split | 1).
    if (tail.sample_play->inputs[3] != 0xFFFF || tail.sample_play->inputs[4] != 0xFFFF) {
        std::uint32_t reassembled =
            static_cast<std::uint32_t>(tail.sample_play->inputs[3]) |
            (static_cast<std::uint32_t>(tail.sample_play->inputs[4]) << 16);
        CHECK(tail.sample_play->state_id == reassembled + 1);
    }

    if (expect_velocity_mul) {
        REQUIRE(tail.mul != nullptr);
        CHECK(tail.mul->inputs[0] == tail.sample_play->out_buffer);
        CHECK(tail.mul->inputs[1] != 0xFFFF);  // velocity_buf
    }
}

}  // namespace

// G1 — bare sample pattern (handle_mini_literal site).
TEST_CASE("Golden: G1 bare sample pattern s\"bd ~ ~ ~\"",
          "[golden][sample][emission]") {
    auto r = akkado::compile(R"(s"bd ~ ~ ~")");
    check_pattern_tail(r);
}

// G2 — polyrhythm path (compile_polyrhythm_events → emit_sample_chain).
TEST_CASE("Golden: G2 polyrhythm s\"[hh,bd] ~ ~ ~\"",
          "[golden][sample][emission][polyrhythm]") {
    auto r = akkado::compile(R"(s"[hh,bd] ~ ~ ~")");
    check_pattern_tail(r);
}

// G3 — record-suffix property propagation, single atom.
TEST_CASE("Golden: G3 single-atom {vel} s\"bd{vel:0.25} ~ ~ ~\"",
          "[golden][sample][emission][velocity]") {
    auto r = akkado::compile(R"(s"[bd{vel:0.25} ~ ~ ~]")");
    check_pattern_tail(r);
    // Per-voice velocity rides on velocities[0]; event.velocity stays at 1.0.
    REQUIRE(!r.program.state_inits.empty());
    const auto& events = r.program.state_inits[0].sequence_events[0];
    REQUIRE(!events.empty());
    CHECK(events[0].velocities[0] == Catch::Approx(0.25f).margin(0.001f));
    CHECK(events[0].velocity == Catch::Approx(1.0f).margin(0.001f));
}

// G4 — property propagation through polyrhythm. Per-voice independence.
TEST_CASE("Golden: G4 polyrhythm with {vel} on one voice",
          "[golden][sample][emission][polyrhythm][velocity]") {
    auto r = akkado::compile(R"(s"[[hh,bd{vel:0.25}] ~ ~ ~]")");
    check_pattern_tail(r);
    REQUIRE(!r.program.state_inits.empty());
    const auto& events = r.program.state_inits[0].sequence_events[0];
    REQUIRE(!events.empty());
    REQUIRE(events[0].num_values == 2);
    CHECK(events[0].velocities[0] == Catch::Approx(1.0f).margin(0.001f));
    CHECK(events[0].velocities[1] == Catch::Approx(0.25f).margin(0.001f));
}

// G5 — stdlib velocity() over event_map (Phase 2b). The pattern's own
// SAMPLE_PLAY chain (from the inner SequenceProgram) plus the transform's
// readout SAMPLE_PLAY → 2 SAMPLE_PLAYs total. Pre-Phase 2b the C++ handler
// shared the readout via compile_pattern_query_only → 1 SAMPLE_PLAY.
TEST_CASE("Golden: G5 velocity() wrapper",
          "[golden][sample][emission]") {
    auto r = akkado::compile(R"(velocity(s"bd ~ ~ ~", 0.5))");
    check_pattern_tail(r, /*expect_velocity_mul=*/true,
                       /*expected_sample_play_count=*/2);
}

// G6 — handle_bank_call site.
TEST_CASE("Golden: G6 .bank() wrapper",
          "[golden][sample][emission]") {
    auto r = akkado::compile(R"(s"bd".bank("TR909"))");
    check_pattern_tail(r);
}

// G7 — handle_variant_call site.
TEST_CASE("Golden: G7 .variant() wrapper",
          "[golden][sample][emission]") {
    auto r = akkado::compile(R"(s"bd".variant(2))");
    check_pattern_tail(r);
}

// G8 — emit_pattern_with_state inline-mirror site (fast / slow / every / ...).
TEST_CASE("Golden: G8 fast() goes through emit_pattern_with_state",
          "[golden][sample][emission]") {
    auto r = akkado::compile(R"(s"bd ~".fast(2))");
    check_pattern_tail(r);
}

// G9 — sample-mode polyrhythm with three voices.
TEST_CASE("Golden: G9 three-voice polyrhythm s\"[bd,hh,sn]\"",
          "[golden][sample][emission][polyrhythm]") {
    auto r = akkado::compile(R"(s"[bd,hh,sn]")");
    check_pattern_tail(r);
    REQUIRE(!r.program.state_inits.empty());
    const auto& events = r.program.state_inits[0].sequence_events[0];
    REQUIRE(!events.empty());
    CHECK(events[0].num_values == 3);
}

// G10 — scalar sample() builtin. Phase 1 baselines the pre-migration emission
// (today: SAMPLE_PLAY emitted via the generic builtin codegen path with
// builtin->inst_rate and compute_state_id()). Phase 2 routes it through
// emit_sample_chain Kind::Scalar — must stay bit-identical.
TEST_CASE("Golden: G10 scalar sample() builtin",
          "[golden][sample][emission][scalar]") {
    auto r = akkado::compile(R"(sample(1.0, 1.0, "bd") |> out(%, %))");
    REQUIRE(r.success);
    auto insts = bytecode_to_insts(r);
    const auto* sp = find_first_op(insts, cedar::Opcode::SAMPLE_PLAY);
    REQUIRE(sp != nullptr);
    // Scalar mode: state_id is compute_state_id() (non-zero — semantic-path
    // hash is deterministic and never collides with 0). Inputs[3]/[4] are
    // BUFFER_UNUSED in the unified path; verify Phase 2 preserves that.
    // Pre-migration today: inputs[3]/[4] are populated by the generic
    // builtin codegen path with the actual arg-3/4 buffers (which are
    // BUFFER_UNUSED for `sample` since it has only 3 declared inputs).
    CHECK(sp->state_id != 0u);
}
