// PRD prd-runtime-event-transforms Phase 3 — akkado-side coverage for
// fast() / slow() runtime rate scaling. Verifies:
//   * Codegen emits EVENT_RATE_SCALE + RateScale StateInitData
//   * Constant factor: SequenceProgram cycle_length stays at the inner-only
//     value; ERS adjusts SequenceState.cycle_length per block
//   * Signal-rate factor compiles (signal-rate fast/slow paths)
//   * slow's reciprocal: codegen emits DIV for signal-rate, folds 1/x for
//     constants
//   * Composition: fast(slow(...)) / nested chains
//   * E185 rejects non-positive constant factors
//
// The opcode itself is unit-tested in cedar/tests/test_event_rate_scale.cpp;
// here we test the codegen lowering + end-to-end runtime behavior.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "akkado/akkado.hpp"
#include <cedar/vm/instruction.hpp>
#include <cedar/vm/vm.hpp>
#include <cedar/dsp/constants.hpp>
#include <cedar/opcodes/sequence.hpp>
#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

std::vector<cedar::Instruction> get_instructions(const akkado::CompileResult& r) {
    std::vector<cedar::Instruction> insts;
    insts.resize(r.bytecode.size() / sizeof(cedar::Instruction));
    if (!insts.empty()) {
        std::memcpy(insts.data(), r.bytecode.data(), r.bytecode.size());
    }
    return insts;
}

std::size_t count_op(const std::vector<cedar::Instruction>& insts,
                     cedar::Opcode op) {
    std::size_t n = 0;
    for (const auto& i : insts) {
        if (i.opcode == op) ++n;
    }
    return n;
}

std::size_t count_init(const akkado::CompileResult& r,
                       akkado::StateInitData::Type type) {
    std::size_t n = 0;
    for (const auto& init : r.state_inits) {
        if (init.type == type) ++n;
    }
    return n;
}

// Find the SequenceProgram init's state_id — the upstream SequenceState that
// the EVENT_RATE_SCALE instruction targets.
std::uint32_t first_seq_program_state_id(const akkado::CompileResult& r) {
    for (const auto& init : r.state_inits) {
        if (init.type == akkado::StateInitData::Type::SequenceProgram) {
            return init.state_id;
        }
    }
    return 0;
}

struct RenderHost {
    cedar::VM vm;
    std::vector<std::vector<cedar::Sequence>> seq_storage;
    std::vector<cedar::Instruction> insts;
};

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
        } else if (init.type == akkado::StateInitData::Type::RateScale) {
            vm.init_event_rate_scale_state(init.state_id);
        }
    }
}

std::unique_ptr<RenderHost> render(const akkado::CompileResult& r, int blocks) {
    auto host = std::make_unique<RenderHost>();
    host->vm.set_sample_rate(48000.0f);
    host->vm.set_bpm(120.0f);
    host->vm.set_block_table(r.block_table, r.main_instruction_count);
    host->insts = get_instructions(r);
    REQUIRE(host->vm.load_program_immediate(
        std::span<const cedar::Instruction>(host->insts)));
    apply_inits(host->vm, r, host->seq_storage);
    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    for (int b = 0; b < blocks; ++b) {
        host->vm.process_block(L.data(), R.data());
    }
    return host;
}

}  // namespace


TEST_CASE("fast(p, 2) emits ERS + RateScale init; cycle_length runtime = 0.5",
          "[fast-slow][phase3]") {
    auto r = akkado::compile(R"(n"c4 e4".fast(2) |> osc("sin", @.freq) |> out(@))");
    REQUIRE(r.success);

    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::EVENT_RATE_SCALE) == 1);
    CHECK(count_init(r, akkado::StateInitData::Type::RateScale) == 1);

    auto host = render(r, /*blocks=*/1);
    const auto* seq = host->vm.states().get_if<cedar::SequenceState>(
        first_seq_program_state_id(r));
    REQUIRE(seq != nullptr);
    // ERS captures original=1.0, writes 1.0 / 2.0 = 0.5 per block.
    CHECK_THAT(seq->cycle_length, WithinAbs(0.5f, 0.001f));
}


TEST_CASE("slow(p, 2) emits ERS + RateScale init; cycle_length runtime = 2.0",
          "[fast-slow][phase3]") {
    auto r = akkado::compile(R"(n"c4 e4".slow(2) |> osc("sin", @.freq) |> out(@))");
    REQUIRE(r.success);

    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::EVENT_RATE_SCALE) == 1);

    auto host = render(r, 1);
    const auto* seq = host->vm.states().get_if<cedar::SequenceState>(
        first_seq_program_state_id(r));
    REQUIRE(seq != nullptr);
    // slow folds 1/factor at compile time → rate=0.5. ERS writes 1/0.5 = 2.0.
    CHECK_THAT(seq->cycle_length, WithinAbs(2.0f, 0.001f));
}


TEST_CASE("fast with signal-rate factor compiles + emits ERS",
          "[fast-slow][phase3]") {
    // The factor here is a runtime signal (osc * 1.5 + 2).
    auto r = akkado::compile(R"(
        rate = osc("sin", 0.2) * 1.5 + 2
        n"c4 e4".fast(rate) |> osc("sin", @.freq) |> out(@)
    )");
    REQUIRE(r.success);

    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::EVENT_RATE_SCALE) == 1);
    // No DIV — fast passes the signal through as-is.
    auto host = render(r, 4);
    const auto* seq = host->vm.states().get_if<cedar::SequenceState>(
        first_seq_program_state_id(r));
    REQUIRE(seq != nullptr);
    // Signal-rate factor: cycle_length is finite and positive (ERS clamps
    // rate >= 0.001 so cycle_length never explodes nor goes negative).
    CHECK(seq->cycle_length > 0.0f);
    CHECK(std::isfinite(seq->cycle_length));
}


TEST_CASE("slow with signal-rate factor compiles + emits ERS + DIV",
          "[fast-slow][phase3]") {
    // slow(p, sg) → rate = 1.0 / sg at runtime → codegen emits a DIV.
    auto r = akkado::compile(R"(
        rate = osc("sin", 0.2) + 2
        n"c4 e4".slow(rate) |> osc("sin", @.freq) |> out(@)
    )");
    REQUIRE(r.success);

    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::EVENT_RATE_SCALE) == 1);
    // slow inserts a runtime 1/sg DIV for the rate input.
    CHECK(count_op(insts, cedar::Opcode::DIV) >= 1);
}


TEST_CASE("fast(slow(p, 2), 3) composes — runtime cycle_length ≈ 0.667 (1.5x)",
          "[fast-slow][phase3]") {
    // compile_pattern_for_transform's recursive case applies the inner slow's
    // compile-time `*= 2` mutation: cycle_length = 2. Outer fast's ERS runs
    // at runtime: cycle_length = 2 / 3.
    auto r = akkado::compile(R"(fast(slow(n"c4 e4", 2), 3) |> osc("sin", @.freq) |> out(@))");
    REQUIRE(r.success);

    // Only ONE EVENT_RATE_SCALE — the outer one. The inner slow used the
    // compile-time fallback path.
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::EVENT_RATE_SCALE) == 1);

    auto host = render(r, 1);
    const auto* seq = host->vm.states().get_if<cedar::SequenceState>(
        first_seq_program_state_id(r));
    REQUIRE(seq != nullptr);
    // 2.0 / 3.0 ≈ 0.667
    CHECK_THAT(seq->cycle_length, WithinAbs(2.0f / 3.0f, 0.001f));
}


TEST_CASE("fast with 0 or negative constant factor → E185",
          "[fast-slow][phase3]") {
    SECTION("zero factor") {
        auto r = akkado::compile(R"(fast(n"c4 e4", 0))");
        CHECK_FALSE(r.success);
        bool saw_e185 = false;
        for (const auto& d : r.diagnostics) {
            if (d.code == "E185") saw_e185 = true;
        }
        CHECK(saw_e185);
    }
    SECTION("negative factor") {
        auto r = akkado::compile(R"(slow(n"c4 e4", -1))");
        CHECK_FALSE(r.success);
        bool saw_e185 = false;
        for (const auto& d : r.diagnostics) {
            if (d.code == "E185") saw_e185 = true;
        }
        CHECK(saw_e185);
    }
}


TEST_CASE("fast(p, N) for N>1 fires the right number of triggers (wrap-detect regression)",
          "[fast-slow][phase3][regression]") {
    // Regression: SEQPAT_STEP's wrap-detect heuristic was hardcoded to
    // `last_beat_pos - 0.5f`, assuming cycle_length=1.0. For fast(N) with
    // cycle_length<1 (e.g. fast(2) → 0.5), wraps from beat_pos≈0.49 → 0.01
    // produced a backward jump of -0.48 which fell SHORT of the -0.5
    // threshold — so wraps were never detected and only the first event in
    // each upstream alternation cycle ever fired. This test exercises the
    // *runtime trigger count* for fast(4) over 1 second to confirm the
    // scaled threshold (`cycle_length * 0.5f`) detects every wrap.
    //
    // Cedar-side unit tests only verified that ERS writes cycle_length —
    // they didn't render the pattern + check actual onset count, missing
    // this bug entirely. This test renders + counts.
    auto r = akkado::compile(
        R"(n"c4 e4 g4 a4".fast(4) |> osc("sin", @.freq) * ar(@.trig, 0.001, 0.005) |> out(@))");
    REQUIRE(r.success);
    auto host = render(r, /*blocks=*/static_cast<int>(48000 / cedar::BLOCK_SIZE));

    // Read the master-bus output buffer indirectly via process_block sum
    // would require accumulating L into a buffer; for a precise check we
    // inspect the SequenceState's step counter, which advances once per
    // upstream query (= once per cycle). 1 second at 120 BPM is 2 beats.
    // fast(4) with cycle_length=0.25 → 8 cycles in 2 beats → step should
    // have advanced 8 times from its initial value.
    const auto* seq = host->vm.states().get_if<cedar::SequenceState>(
        first_seq_program_state_id(r));
    REQUIRE(seq != nullptr);
    // SEQPAT_QUERY updates state.cycle_index = current_cycle every time it
    // crosses a cycle boundary. Over 2 beats with cycle_length=0.25, the
    // final cycle reached is 7 (cycles 0..7). With the wrap-detect bug,
    // the upstream alternation step never advanced past 1, so we'd never
    // even reach cycle 2. The cycle_index field is the SEQPAT_QUERY-side
    // counter and is a direct read of the wrap detection's effect.
    // Pre-fix: cycle_index stuck near 0/1.
    // Post-fix: cycle_index near 7 (last cycle started in the 1s window).
    CHECK(seq->cycle_index >= 6u);
    CHECK(seq->cycle_index <= 9u);
}

TEST_CASE("fast()/slow() on euclid() emits targeted hint about dur parameter",
          "[fast-slow][euclid]") {
    // Regression: prior to the euclid dur-param fix, users hit a generic
    // 'E133 first argument must be a pattern' when writing
    // `euclid(3,8).slow(2)`. The signal-vs-pattern type mismatch is real but
    // the error gave no hint about the correct way to slow euclid (the new
    // `dur` parameter). This test pins the targeted message in place.
    SECTION("slow on euclid produces hint mentioning dur") {
        auto r = akkado::compile(R"(
            euclid(3, 8).slow(2) |> out(@)
        )");
        CHECK_FALSE(r.success);
        bool saw_dur_hint = false;
        for (const auto& d : r.diagnostics) {
            if (d.code == "E133" &&
                d.message.find("dur") != std::string::npos &&
                d.message.find("euclid") != std::string::npos) {
                saw_dur_hint = true;
            }
        }
        CHECK(saw_dur_hint);
    }

    SECTION("fast on euclid produces hint mentioning dur") {
        auto r = akkado::compile(R"(
            euclid(3, 8).fast(2) |> out(@)
        )");
        CHECK_FALSE(r.success);
        bool saw_dur_hint = false;
        for (const auto& d : r.diagnostics) {
            if (d.code == "E133" &&
                d.message.find("dur") != std::string::npos &&
                d.message.find("euclid") != std::string::npos) {
                saw_dur_hint = true;
            }
        }
        CHECK(saw_dur_hint);
    }

    SECTION("euclid with explicit dur arg compiles cleanly") {
        auto r = akkado::compile(R"(
            osc("sin", 110) * ar(euclid(3, 8, 0, 8), 0.005, 0.4) |> out(@)
        )");
        if (!r.success) {
            for (const auto& d : r.diagnostics) {
                INFO(d.code << ": " << d.message);
            }
        }
        CHECK(r.success);
    }
}

TEST_CASE("fast applied to a Pattern-bound identifier emits ERS",
          "[fast-slow][phase3]") {
    // Multi-use scenario: a pattern defined once, transformed two different
    // ways. Each call gets its OWN recompiled SequenceState (via
    // compile_pattern_for_transform's identifier resolution) + its own ERS,
    // so the two rates don't interfere.
    auto r = akkado::compile(R"(
        f = n"c4 e4"
        slow(f, 2) |> osc("sin", @.freq) |> out(@)
        fast(f, 3) |> osc("sin", @.freq) |> out(@)
    )");
    REQUIRE(r.success);

    auto insts = get_instructions(r);
    // Two independent rate transforms → two ERS instructions.
    CHECK(count_op(insts, cedar::Opcode::EVENT_RATE_SCALE) == 2);
    // Two distinct SequenceProgram inits (one per call).
    CHECK(count_init(r, akkado::StateInitData::Type::SequenceProgram) == 2);
    CHECK(count_init(r, akkado::StateInitData::Type::RateScale) == 2);
}
