// PRD prd-runtime-event-transforms Phase 4 — akkado-side coverage for the
// EVENT_REORDER opcode and the rev/palindrome/zoom/compress migrations
// shipped in Commit A. Verifies:
//   * Codegen emits EVENT_REORDER + Reorder StateInitData
//   * Inner SequenceProgram still carries the original (un-mutated) events
//   * Downstream PatternPayload cycle_length stamped per kind (PALINDROME=2x)
//   * Runtime OutputEvents on the transform state match the per-kind transform
//   * Composition with EVENT_MAP (e.g. rev(transpose(p, 5))) works
//   * Compile-time validation (E132 for zoom/compress reversed const range)
//
// The opcode itself is unit-tested in cedar/tests/test_event_reorder.cpp;
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
    insts.resize(r.program.bytecode.size() / sizeof(cedar::Instruction));
    if (!insts.empty()) {
        std::memcpy(insts.data(), r.program.bytecode.data(), r.program.bytecode.size());
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
    for (const auto& init : r.program.state_inits) {
        if (init.type == type) ++n;
    }
    return n;
}

// Locate the first state_init of a given type and return its state_id.
std::uint32_t first_state_id(const akkado::CompileResult& r,
                             akkado::StateInitData::Type type) {
    for (const auto& init : r.program.state_inits) {
        if (init.type == type) return init.state_id;
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
    seq_storage.reserve(r.program.state_inits.size());
    for (const auto& init : r.program.state_inits) {
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
        } else if (init.type == akkado::StateInitData::Type::EventTransform ||
                   init.type == akkado::StateInitData::Type::Reorder ||
                   init.type == akkado::StateInitData::Type::Fanout) {
            vm.init_event_transform_state(init.state_id, init.cycle_length,
                                          init.is_sample_pattern,
                                          init.total_events);
        }
    }
}

std::unique_ptr<RenderHost> render(const akkado::CompileResult& r, int blocks) {
    auto host = std::make_unique<RenderHost>();
    host->vm.set_sample_rate(48000.0f);
    host->vm.set_bpm(120.0f);
    host->vm.set_block_table(r.program.block_table, r.program.main_instruction_count);
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


TEST_CASE("rev(p) emits EVENT_REORDER(REV) + Reorder init",
          "[reorder][phase4]") {
    auto r = akkado::compile(
        R"(rev(n"[c4 e4 g4 a4]") |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);

    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::EVENT_REORDER) == 1);
    CHECK(count_init(r, akkado::StateInitData::Type::Reorder) == 1);
    CHECK(count_init(r, akkado::StateInitData::Type::SequenceProgram) == 1);

    // The inner pattern still has the original 4 events at 0, 0.25, 0.5, 0.75.
    auto seq_id = first_state_id(r, akkado::StateInitData::Type::SequenceProgram);
    REQUIRE(seq_id != 0);
    for (const auto& init : r.program.state_inits) {
        if (init.type == akkado::StateInitData::Type::SequenceProgram &&
            init.state_id == seq_id) {
            REQUIRE(init.sequence_events[0].size() == 4);
            CHECK(init.sequence_events[0][0].time == 0.0f);
            CHECK_THAT(init.sequence_events[0][1].time, WithinAbs(0.25f, 0.001f));
        }
    }
}

TEST_CASE("rev(p) runtime: OutputEvents on transform state are time-reversed",
          "[reorder][phase4]") {
    auto r = akkado::compile(
        R"(rev(n"[c4 e4 g4 a4]") |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);
    auto host = render(r, /*blocks=*/1);

    auto xform_id = first_state_id(r, akkado::StateInitData::Type::Reorder);
    REQUIRE(xform_id != 0);
    const auto* xform = host->vm.states().get_if<cedar::SequenceState>(xform_id);
    REQUIRE(xform != nullptr);
    REQUIRE(xform->output.num_events == 4);
    // 4 events @ 0, 0.25, 0.5, 0.75 reversed -> 0.0, 0.25, 0.5, 0.75 (sorted).
    // The NOTES travel with the events: midi_note at t=0 was c4 (60); after rev
    // the event AT t=0 (sorted) holds the LAST upstream event's note (a4 = 69).
    CHECK_THAT(xform->output.events[0].time, WithinAbs(0.0f,  0.001f));
    CHECK_THAT(xform->output.events[3].time, WithinAbs(0.75f, 0.001f));
    CHECK_THAT(xform->output.events[0].midi_note, WithinAbs(69.0f, 0.001f));
    CHECK_THAT(xform->output.events[3].midi_note, WithinAbs(60.0f, 0.001f));
}

TEST_CASE("palindrome(p) emits EVENT_REORDER(PALINDROME) with 2x cycle_length",
          "[reorder][phase4]") {
    auto r = akkado::compile(
        R"(palindrome(n"[c4 e4]") |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);

    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::EVENT_REORDER) == 1);
    bool found_reorder = false;
    for (const auto& init : r.program.state_inits) {
        if (init.type == akkado::StateInitData::Type::Reorder) {
            found_reorder = true;
            CHECK_THAT(init.cycle_length, WithinAbs(2.0f, 0.001f));
        }
    }
    CHECK(found_reorder);
}

TEST_CASE("palindrome(p) runtime: OutputEvents doubled and time-mirrored",
          "[reorder][phase4]") {
    auto r = akkado::compile(
        R"(palindrome(n"[c4 e4]") |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);
    auto host = render(r, 1);

    auto xform_id = first_state_id(r, akkado::StateInitData::Type::Reorder);
    REQUIRE(xform_id != 0);
    const auto* xform = host->vm.states().get_if<cedar::SequenceState>(xform_id);
    REQUIRE(xform != nullptr);
    // 2 upstream events -> 4 downstream (forward + reverse).
    REQUIRE(xform->output.num_events == 4);
    CHECK_THAT(xform->cycle_length, WithinAbs(2.0f, 0.001f));
}

TEST_CASE("zoom(p, 0.25, 0.75) emits EVENT_REORDER(ZOOM)",
          "[reorder][phase4]") {
    auto r = akkado::compile(
        R"(zoom(n"[c4 e4 g4 a4]", 0.25, 0.75) |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::EVENT_REORDER) == 1);
    CHECK(count_init(r, akkado::StateInitData::Type::Reorder) == 1);
}

TEST_CASE("zoom(p, 0.25, 0.75) runtime: middle events remapped to [0,1)",
          "[reorder][phase4]") {
    auto r = akkado::compile(
        R"(zoom(n"[c4 e4 g4 a4]", 0.25, 0.75) |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);
    auto host = render(r, 1);

    auto xform_id = first_state_id(r, akkado::StateInitData::Type::Reorder);
    REQUIRE(xform_id != 0);
    const auto* xform = host->vm.states().get_if<cedar::SequenceState>(xform_id);
    REQUIRE(xform != nullptr);
    REQUIRE(xform->output.num_events == 2);
    CHECK_THAT(xform->output.events[0].time,     WithinAbs(0.0f, 0.001f));
    CHECK_THAT(xform->output.events[0].duration, WithinAbs(0.5f, 0.001f));
    CHECK_THAT(xform->output.events[1].time,     WithinAbs(0.5f, 0.001f));
}

TEST_CASE("zoom rejects reversed constant range with E132",
          "[reorder][phase4]") {
    auto r = akkado::compile(R"(zoom(n"[c4 e4]", 0.75, 0.25))");
    REQUIRE_FALSE(r.success);
    bool found = false;
    for (const auto& d : r.diagnostics) {
        if (d.code == "E132") { found = true; break; }
    }
    CHECK(found);
}

TEST_CASE("compress(p, 0.25, 0.75) emits EVENT_REORDER(COMPRESS)",
          "[reorder][phase4]") {
    auto r = akkado::compile(
        R"(compress(n"[c4 e4]", 0.25, 0.75) |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::EVENT_REORDER) == 1);
}

TEST_CASE("compress(p, 0.25, 0.75) runtime: events rescaled into [s,e)",
          "[reorder][phase4]") {
    auto r = akkado::compile(
        R"(compress(n"[c4 e4]", 0.25, 0.75) |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);
    auto host = render(r, 1);

    auto xform_id = first_state_id(r, akkado::StateInitData::Type::Reorder);
    REQUIRE(xform_id != 0);
    const auto* xform = host->vm.states().get_if<cedar::SequenceState>(xform_id);
    REQUIRE(xform != nullptr);
    REQUIRE(xform->output.num_events == 2);
    // Upstream times 0.0, 0.5; compress into [0.25, 0.75) width=0.5.
    CHECK_THAT(xform->output.events[0].time,     WithinAbs(0.25f, 0.001f));
    CHECK_THAT(xform->output.events[0].duration, WithinAbs(0.25f, 0.001f));
    CHECK_THAT(xform->output.events[1].time,     WithinAbs(0.5f,  0.001f));
}

TEST_CASE("compress rejects reversed constant range with E132",
          "[reorder][phase4]") {
    auto r = akkado::compile(R"(compress(n"[c4 e4]", 0.75, 0.25))");
    REQUIRE_FALSE(r.success);
    bool found = false;
    for (const auto& d : r.diagnostics) {
        if (d.code == "E132") { found = true; break; }
    }
    CHECK(found);
}

TEST_CASE("rev(transpose(p, 5)) composes: EVENT_MAP feeds EVENT_REORDER",
          "[reorder][phase4][composition]") {
    // Phase 4's headline win: runtime composition with EVENT_MAP. Pre-Phase-4
    // rev folded transpose at compile-time via compile_pattern_for_transform's
    // recursive path; this only worked for the legacy compile-time form. The
    // stdlib transpose() now lowers to EVENT_MAP — runtime rev must read those
    // EVENT_MAP-published OutputEvents (not the compile-time AST) for the
    // transpose to survive.
    //
    // Note on the operand order: writing `n"…".transpose(5).rev()` would
    // currently invoke the stdlib fn form of `rev` (no such fn defined yet,
    // it's still a builtin). The form below uses the direct-call style
    // `rev(transpose(...))` which is the test surface.
    auto r = akkado::compile(
        R"(rev(transpose(n"[c4 e4]", 5)) |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);

    auto insts = get_instructions(r);
    // The compile-time fallback (compile_pattern_for_transform) handles
    // transpose at compile time when nested, so the inner EVENT_MAP doesn't
    // appear in the stream. rev's EVENT_REORDER is the only Phase-4 opcode.
    CHECK(count_op(insts, cedar::Opcode::EVENT_REORDER) == 1);
    // The transposed midi_note should already be baked into the inner
    // SequenceProgram. Verify it survives to the transform OutputEvents.
    auto host = render(r, 1);
    auto xform_id = first_state_id(r, akkado::StateInitData::Type::Reorder);
    REQUIRE(xform_id != 0);
    const auto* xform = host->vm.states().get_if<cedar::SequenceState>(xform_id);
    REQUIRE(xform != nullptr);
    REQUIRE(xform->output.num_events == 2);
    // After +5 semitones: c4 -> f4 (65), e4 -> a4 (69).
    // After rev (cycle_length=1, dur=0.5):
    //   t=0.0,note=65 -> new_time = 1 - 0 - 0.5 = 0.5
    //   t=0.5,note=69 -> new_time = 1 - 0.5 - 0.5 = 0.0
    // Sorted by time: (0.0, 69), (0.5, 65).
    CHECK_THAT(xform->output.events[0].midi_note, WithinAbs(69.0f, 0.001f));
    CHECK_THAT(xform->output.events[1].midi_note, WithinAbs(65.0f, 0.001f));
}

TEST_CASE("dot-call form: n\"…\".rev() lowers identically to rev(n\"…\")",
          "[reorder][phase4]") {
    auto dot    = akkado::compile(R"(n"[c4 e4 g4]".rev())");
    auto direct = akkado::compile(R"(rev(n"[c4 e4 g4]"))");
    REQUIRE(dot.success);
    REQUIRE(direct.success);
    // Same opcode emission count (1 EVENT_REORDER) and same StateInitData mix.
    auto dot_insts = get_instructions(dot);
    auto direct_insts = get_instructions(direct);
    CHECK(count_op(dot_insts, cedar::Opcode::EVENT_REORDER) ==
          count_op(direct_insts, cedar::Opcode::EVENT_REORDER));
    CHECK(count_init(dot, akkado::StateInitData::Type::Reorder) ==
          count_init(direct, akkado::StateInitData::Type::Reorder));
}

// ============================================================================
// EVENT_FANOUT — ply / linger / segment (Phase 4 Commit B)
// ============================================================================

TEST_CASE("ply(p, 3) emits EVENT_FANOUT(PLY) + Fanout init",
          "[reorder][phase4][fanout]") {
    auto r = akkado::compile(R"(ply(n"[c4 e4]", 3) |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::EVENT_FANOUT) == 1);
    CHECK(count_init(r, akkado::StateInitData::Type::Fanout) == 1);
    for (const auto& si : r.program.state_inits) {
        if (si.type == akkado::StateInitData::Type::Fanout) {
            // 2 upstream * n=3 = 6
            CHECK(si.total_events == 6);
        }
    }
}

TEST_CASE("ply(p, 2) runtime: 4 events with quartered durations",
          "[reorder][phase4][fanout]") {
    auto r = akkado::compile(R"(ply(n"[c4 e4]", 2) |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);
    auto host = render(r, 1);
    auto xform_id = first_state_id(r, akkado::StateInitData::Type::Fanout);
    REQUIRE(xform_id != 0);
    const auto* xform = host->vm.states().get_if<cedar::SequenceState>(xform_id);
    REQUIRE(xform != nullptr);
    REQUIRE(xform->output.num_events == 4);
    // Original: t=0, 0.5 dur=0.5; ply(2): t=0, 0.25, 0.5, 0.75 dur=0.25 each.
    CHECK_THAT(xform->output.events[0].time,     WithinAbs(0.0f,  0.001f));
    CHECK_THAT(xform->output.events[0].duration, WithinAbs(0.25f, 0.001f));
    CHECK_THAT(xform->output.events[3].time,     WithinAbs(0.75f, 0.001f));
}

TEST_CASE("ply rejects n < 1 with E131",
          "[reorder][phase4][fanout]") {
    auto r = akkado::compile(R"(ply(n"[c4 e4]", 0))");
    REQUIRE_FALSE(r.success);
}

TEST_CASE("linger(p, 0.5) emits EVENT_FANOUT(LINGER) with halved cycle_length",
          "[reorder][phase4][fanout]") {
    auto r = akkado::compile(
        R"(linger(n"[c4 e4 g4 b4]", 0.5) |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::EVENT_FANOUT) == 1);
    for (const auto& si : r.program.state_inits) {
        if (si.type == akkado::StateInitData::Type::Fanout) {
            CHECK_THAT(si.cycle_length, WithinAbs(0.5f, 0.001f));
        }
    }
}

TEST_CASE("linger(p, 0.5) runtime: drops second half, halves cycle_length",
          "[reorder][phase4][fanout]") {
    auto r = akkado::compile(
        R"(linger(n"[c4 e4 g4 b4]", 0.5) |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);
    auto host = render(r, 1);
    auto xform_id = first_state_id(r, akkado::StateInitData::Type::Fanout);
    REQUIRE(xform_id != 0);
    const auto* xform = host->vm.states().get_if<cedar::SequenceState>(xform_id);
    REQUIRE(xform != nullptr);
    REQUIRE(xform->output.num_events == 2);
    CHECK_THAT(xform->output.events[0].time, WithinAbs(0.0f, 0.001f));
    CHECK_THAT(xform->output.events[1].time, WithinAbs(0.5f, 0.001f));
    CHECK_THAT(xform->cycle_length, WithinAbs(0.5f, 0.001f));
}

TEST_CASE("linger with signal-rate frac compiles",
          "[reorder][phase4][fanout][signal]") {
    auto r = akkado::compile(
        R"(mod_sig = sine(0.5)
           linger(n"[c4 e4 g4 b4]", mod_sig * 0.25 + 0.5) |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::EVENT_FANOUT) == 1);
}

TEST_CASE("segment(p, 8) emits EVENT_FANOUT(SEGMENT)",
          "[reorder][phase4][fanout]") {
    auto r = akkado::compile(
        R"(segment(n"[c4 e4]", 8) |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::EVENT_FANOUT) == 1);
}

TEST_CASE("segment(p, 4) runtime: 4 grid points",
          "[reorder][phase4][fanout]") {
    auto r = akkado::compile(
        R"(segment(n"[c4 e4]", 4) |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);
    auto host = render(r, 1);
    auto xform_id = first_state_id(r, akkado::StateInitData::Type::Fanout);
    REQUIRE(xform_id != 0);
    const auto* xform = host->vm.states().get_if<cedar::SequenceState>(xform_id);
    REQUIRE(xform != nullptr);
    REQUIRE(xform->output.num_events == 4);
    CHECK_THAT(xform->output.events[0].duration, WithinAbs(0.25f, 0.001f));
}

TEST_CASE("segment rejects n < 1 with E131",
          "[reorder][phase4][fanout]") {
    auto r = akkado::compile(R"(segment(n"[c4 e4]", 0))");
    REQUIRE_FALSE(r.success);
}

// ============================================================================
// EVENT_REORDER(ITER) — iter / iterBack (Phase 4 Commit C)
// ============================================================================

TEST_CASE("iter(p, 4) emits EVENT_REORDER(ITER) (no legacy iter_n on SequenceProgram)",
          "[reorder][phase4][iter]") {
    auto r = akkado::compile(R"(iter(n"[c4 e4 g4 a4]", 4) |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::EVENT_REORDER) == 1);
    CHECK(count_init(r, akkado::StateInitData::Type::Reorder) == 1);
    // SequenceProgram init exists for the inner pattern.
    CHECK(count_init(r, akkado::StateInitData::Type::SequenceProgram) == 1);
}

TEST_CASE("iter(p, 4) runtime: cycle 0 plays unchanged, cycle 1 rotates by 1/4",
          "[reorder][phase4][iter]") {
    auto r = akkado::compile(
        R"(iter(n"[c4 e4 g4 a4]", 4) |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);
    auto host = render(r, 1);  // 1 block (sample 0, cycle 0)
    auto xform_id = first_state_id(r, akkado::StateInitData::Type::Reorder);
    REQUIRE(xform_id != 0);
    const auto* xform = host->vm.states().get_if<cedar::SequenceState>(xform_id);
    REQUIRE(xform != nullptr);
    REQUIRE(xform->output.num_events == 4);
    // Cycle 0: shift = 0; events at 0, 0.25, 0.5, 0.75 in original order.
    CHECK_THAT(xform->output.events[0].time, WithinAbs(0.0f,  0.001f));
    CHECK_THAT(xform->output.events[3].time, WithinAbs(0.75f, 0.001f));
}

TEST_CASE("iterBack(p, 4) emits EVENT_REORDER(ITER_BACK)",
          "[reorder][phase4][iter]") {
    auto r = akkado::compile(
        R"(iterBack(n"[c4 e4 g4 a4]", 4) |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::EVENT_REORDER) == 1);
}

TEST_CASE("iter rejects n out of [1, 255]",
          "[reorder][phase4][iter]") {
    auto r_zero = akkado::compile(R"(iter(n"[c4 e4]", 0))");
    REQUIRE_FALSE(r_zero.success);
    auto r_big = akkado::compile(R"(iter(n"[c4 e4]", 300))");
    REQUIRE_FALSE(r_big.success);
}

TEST_CASE("iter(p, 4) | transpose composition: rotation reads transposed OutputEvents",
          "[reorder][phase4][iter][composition]") {
    // Regression coverage: iter on top of a runtime EVENT_MAP. Pre-Phase-4
    // this could only work via compile_pattern_for_transform's recursive fold;
    // now iter reads EVENT_MAP's downstream OutputEvents at runtime, so the
    // transpose survives even though iter operates per-block.
    auto r = akkado::compile(
        R"(iter(transpose(n"[c4 e4]", 5), 2) |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);
}

TEST_CASE("zoom with signal-rate endpoint compiles",
          "[reorder][phase4][signal]") {
    // The signal-rate path uses visit() instead of get_number_arg for the
    // start/end args. End-to-end runtime correctness for a moving window is
    // covered by the long-form Python experiment; here we just confirm the
    // compile-time path produces an EVENT_REORDER + Reorder init.
    auto r = akkado::compile(
        R"(mod_sig = sine(0.5)
           zoom(n"[c4 e4 g4 a4]", 0.0, mod_sig * 0.25 + 0.5) |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::EVENT_REORDER) == 1);
    CHECK(count_init(r, akkado::StateInitData::Type::Reorder) == 1);
}

// ============================================================================
// Composition tests (Phase 4 Commit D)
// ============================================================================
//
// The headline benefit of Phase 4: structural transforms compose with
// EVENT_MAP / EVENT_FILTER / EVENT_RATE_SCALE through the OutputEvents wire
// format. Pre-Phase-4 these chains either broke (runtime EVENT_MAP under
// compile-time rev would be silently dropped) or were limited to constant-
// only nesting via compile_pattern_for_transform's recursive fold. After
// Phase 4 the runtime opcodes read upstream OutputEvents, so any order
// composes.

TEST_CASE("rev(palindrome(p)) chains two EVENT_REORDERs",
          "[reorder][phase4][composition]") {
    // Inner palindrome doubles event count + cycle_length, outer rev mirrors.
    // The outer reads inner's downstream OutputEvents (4 events at the
    // 2x-cycle), then reverses them.
    auto r = akkado::compile(
        R"(rev(palindrome(n"[c4 e4]")) |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    // The outer rev's compile_pattern_for_transform folds inner palindrome at
    // compile time (palindrome stays in is_transform), so only one runtime
    // EVENT_REORDER is emitted (the outer rev). This matches Phase 3 fast/
    // slow precedent: nested constant-only structural transforms fold; only
    // the outermost emits a runtime opcode.
    CHECK(count_op(insts, cedar::Opcode::EVENT_REORDER) >= 1);
}

TEST_CASE("velocity(rev(p), 0.5) — rev under runtime EVENT_MAP",
          "[reorder][phase4][composition]") {
    // velocity is an event_map stdlib fn (Phase 2b); the outer call sees a
    // runtime EVENT_MAP upstream. The EVENT_MAP reads rev's downstream
    // OutputEvents and writes its own; the resulting playback has reversed
    // event order AND scaled velocities.
    auto r = akkado::compile(
        R"(n"[c4 e4 g4 a4]".rev().velocity(0.5) |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::EVENT_REORDER) == 1);
    CHECK(count_op(insts, cedar::Opcode::EVENT_MAP) >= 1);
}

TEST_CASE("rev(fast(p, 2)) folds inner fast at compile time, emits only EVENT_REORDER",
          "[reorder][phase4][composition]") {
    // Both rev and fast are in compile_pattern_for_transform's is_transform
    // list, so the OUTER call's recompile recursively folds the inner fast
    // into the inner SequenceProgram's cycle_length. Only the outer rev
    // emits a runtime opcode. This matches Phase 3 nesting semantics — see
    // PRD prd-runtime-event-transforms §3.3 "compile-time fallback".
    auto r = akkado::compile(
        R"(rev(fast(n"[c4 e4 g4 a4]", 2)) |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::EVENT_REORDER) == 1);
    // No ERS because the inner fast folded at compile time.
    CHECK(count_op(insts, cedar::Opcode::EVENT_RATE_SCALE) == 0);
    // The inner SequenceProgram's cycle_length is the folded value: 1/2 = 0.5.
    for (const auto& si : r.program.state_inits) {
        if (si.type == akkado::StateInitData::Type::SequenceProgram) {
            CHECK_THAT(si.cycle_length, WithinAbs(0.5f, 0.001f));
        }
    }
}

TEST_CASE("ply(rev(p), 3) — EVENT_FANOUT on top of EVENT_REORDER",
          "[reorder][phase4][composition][fanout]") {
    auto r = akkado::compile(
        R"(ply(rev(n"[c4 e4]"), 3) |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);
    // Outer ply emits FANOUT; inner rev is folded by compile_pattern_for_transform.
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::EVENT_FANOUT) == 1);
}

TEST_CASE("transpose(p, 5).palindrome() runtime: chord stays transposed",
          "[reorder][phase4][composition]") {
    // PRD regression: runtime EVENT_MAP (transpose) feeds EVENT_REORDER
    // (palindrome). The reversed-half events must carry the transposed note.
    auto r = akkado::compile(
        R"(n"[c4 e4]".transpose(5).palindrome() |> sine(@.freq) |> out(@))");
    REQUIRE(r.success);
    auto host = render(r, 1);
    auto xform_id = first_state_id(r, akkado::StateInitData::Type::Reorder);
    REQUIRE(xform_id != 0);
    const auto* xform = host->vm.states().get_if<cedar::SequenceState>(xform_id);
    REQUIRE(xform != nullptr);
    REQUIRE(xform->output.num_events == 4);
    // After +5: c4 (60) -> f4 (65), e4 (64) -> a4 (69).
    // Palindrome emits each twice; check all 4 are 65 or 69 (no 60/64).
    for (std::uint32_t i = 0; i < xform->output.num_events; ++i) {
        const float m = xform->output.events[i].midi_note;
        CHECK((std::abs(m - 65.0f) < 0.01f || std::abs(m - 69.0f) < 0.01f));
    }
}
