// PRD prd-runtime-event-transforms Phase 1 — akkado-side coverage.
//
// Verifies that transpose() / velocity() lower to runtime EVENT_MAP opcodes
// and that the transformed event stream is correct when rendered through the
// Cedar VM. The opcode itself is exhaustively unit-tested in
// cedar/tests/test_event_map.cpp; here we test the codegen lowering + the
// end-to-end runtime result.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "akkado/akkado.hpp"
#include <cedar/vm/instruction.hpp>
#include <cedar/vm/vm.hpp>
#include <cedar/dsp/constants.hpp>
#include <cedar/opcodes/sequence.hpp>
#include <cedar/opcodes/event_transform_encoding.hpp>
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

// Owns the VM + the SequenceProgram event storage it points into.
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
        } else if (init.type ==
                   akkado::StateInitData::Type::EventTransform) {
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

// The SequenceState the final (outermost) EVENT_MAP publishes its transformed
// OutputEvents into.
const cedar::SequenceState* final_transform_state(RenderHost& host) {
    std::uint32_t id = 0;
    bool found = false;
    for (const auto& i : host.insts) {
        if (i.opcode == cedar::Opcode::EVENT_MAP) {
            id = i.state_id;
            found = true;
        }
    }
    if (!found) return nullptr;
    return host.vm.states().get_if<cedar::SequenceState>(id);
}

// The SequenceState the final EVENT_MAP / EVENT_FILTER publishes into.
const cedar::SequenceState* final_event_state(RenderHost& host) {
    std::uint32_t id = 0;
    bool found = false;
    for (const auto& i : host.insts) {
        if (i.opcode == cedar::Opcode::EVENT_MAP ||
            i.opcode == cedar::Opcode::EVENT_FILTER) {
            id = i.state_id;
            found = true;
        }
    }
    if (!found) return nullptr;
    return host.vm.states().get_if<cedar::SequenceState>(id);
}

// c4 = MIDI 60; equal-tempered frequency.
constexpr float kC4 = 261.6256f;

}  // namespace

TEST_CASE("event-map: transpose() lowers to closure EVENT_MAP (NOTE write-mask)",
          "[event-map]") {
    // Phase 2b: transpose is now a stdlib `fn` over event_map, so it lowers
    // through the closure path (flags |= EVENT_CLOSURE, write-mask bit for
    // NOTE set, rate = closure block_id).
    auto r = akkado::compile(R"(n"c4" |> transpose(@, 12) |> osc("sin", @.freq) |> out(@))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    REQUIRE(count_op(insts, cedar::Opcode::EVENT_MAP) == 1);

    // One SequenceProgram (the source pattern) + one EventTransform (transpose).
    CHECK(count_init(r, akkado::StateInitData::Type::SequenceProgram) == 1);
    CHECK(count_init(r, akkado::StateInitData::Type::EventTransform) == 1);

    for (const auto& i : insts) {
        if (i.opcode != cedar::Opcode::EVENT_MAP) continue;
        CHECK((i.flags & cedar::InstructionFlag::EVENT_CLOSURE) != 0);
        const std::uint8_t mask =
            (i.flags >> cedar::InstructionFlag::EVENT_MASK_SHIFT) & 0x7F;
        CHECK((mask & (1u << cedar::EVENT_OUT_NOTE)) != 0);
        // Upstream state_id reassembles from inputs[2..3] and is non-zero.
        std::uint32_t up = static_cast<std::uint32_t>(i.inputs[2]) |
                           (static_cast<std::uint32_t>(i.inputs[3]) << 16);
        CHECK(up != 0u);
    }
}

TEST_CASE("event-map: transpose(+12) shifts notes up an octave at runtime",
          "[event-map]") {
    auto r = akkado::compile(R"(n"c4".transpose(12))");
    REQUIRE(r.success);
    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events >= 1);
    const auto& e = st->output.events[0];
    // Mono note: the scalar midi_note field carries the pitch (per-voice
    // notes[] is chord-only). values[0] holds frequency — +12 doubles it.
    CHECK_THAT(e.midi_note, WithinAbs(72.0f, 0.01f));
    CHECK_THAT(e.values[0], WithinAbs(kC4 * 2.0f, 1.0f));
}

TEST_CASE("event-map: transpose(-12) shifts notes down an octave",
          "[event-map]") {
    auto r = akkado::compile(R"(n"c4".transpose(-12))");
    REQUIRE(r.success);
    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events >= 1);
    CHECK_THAT(st->output.events[0].midi_note, WithinAbs(48.0f, 0.01f));
    CHECK_THAT(st->output.events[0].values[0], WithinAbs(kC4 * 0.5f, 1.0f));
}

TEST_CASE("event-map: velocity() lowers to closure EVENT_MAP (VEL write-mask) and scales",
          "[event-map]") {
    // Phase 2b: velocity is a stdlib `fn` over event_map; closure form sets
    // the VEL bit in the write mask and rate = closure block_id.
    auto r = akkado::compile(R"(n"[c4 e4]".velocity(0.5))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    REQUIRE(count_op(insts, cedar::Opcode::EVENT_MAP) == 1);
    for (const auto& i : insts) {
        if (i.opcode == cedar::Opcode::EVENT_MAP) {
            CHECK((i.flags & cedar::InstructionFlag::EVENT_CLOSURE) != 0);
            const std::uint8_t mask =
                (i.flags >> cedar::InstructionFlag::EVENT_MASK_SHIFT) & 0x7F;
            CHECK((mask & (1u << cedar::EVENT_OUT_VEL)) != 0);
        }
    }

    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events == 2);
    for (std::uint32_t i = 0; i < st->output.num_events; ++i) {
        // base events carry velocity 1.0; EVENT_MAP scales by 0.5.
        CHECK_THAT(st->output.events[i].velocity, WithinAbs(0.5f, 0.001f));
        CHECK_THAT(st->output.events[i].velocities[0], WithinAbs(0.5f, 0.001f));
    }
}

TEST_CASE("event-map: velocity().transpose() chains two EVENT_MAP opcodes",
          "[event-map]") {
    auto r = akkado::compile(R"(n"c4".velocity(0.7).transpose(7))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    // Fully-chained: one EVENT_MAP per modifier, both runtime.
    REQUIRE(count_op(insts, cedar::Opcode::EVENT_MAP) == 2);
    CHECK(count_init(r, akkado::StateInitData::Type::EventTransform) == 2);

    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events >= 1);
    const auto& e = st->output.events[0];
    // transpose(+7) on top of velocity(0.7): both transforms observed.
    CHECK_THAT(e.midi_note, WithinAbs(67.0f, 0.01f));
    CHECK_THAT(e.velocity, WithinAbs(0.7f, 0.001f));
}

TEST_CASE("event-map: transpose() shifts every voice of a chord",
          "[event-map]") {
    // c"CM" is a 3-voice chord event (C major = MIDI 60,64,67).
    auto r = akkado::compile(R"(c"CM".transpose(12))");
    REQUIRE(r.success);
    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events >= 1);
    const auto& e = st->output.events[0];
    REQUIRE(e.num_values == 3);
    // +12 semitones on every voice; voice count preserved.
    CHECK_THAT(e.notes[0], WithinAbs(72.0f, 0.01f));
    CHECK_THAT(e.notes[1], WithinAbs(76.0f, 0.01f));
    CHECK_THAT(e.notes[2], WithinAbs(79.0f, 0.01f));
    // Frequencies all doubled.
    CHECK_THAT(e.values[0], WithinAbs(kC4 * 2.0f, 1.0f));
}

TEST_CASE("event-map: transpose() on a sample pattern is audibly a no-op",
          "[event-map]") {
    // Phase 2b: stdlib `fn transpose` emits an EVENT_MAP unconditionally.
    // For sample patterns the closure rewrites `e.note`, but the downstream
    // sample-play chain reads `sample_id`, so the change is audibly a no-op.
    // (Pre-Phase 2b's `handle_transpose_call` short-circuited and emitted
    // nothing — observable bytecode shape changed; audible output did not.)
    auto r = akkado::compile(R"(s"bd sn".transpose(12))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::EVENT_MAP) == 1);
    CHECK(count_init(r, akkado::StateInitData::Type::EventTransform) == 1);
}

TEST_CASE("event-map: velocity() on a sample pattern emits one SAMPLE_PLAY",
          "[event-map]") {
    auto r = akkado::compile(R"(s"bd sn".velocity(0.5))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    REQUIRE(count_op(insts, cedar::Opcode::EVENT_MAP) == 1);
    // The transform's own readout drives a single SAMPLE_PLAY chain.
    CHECK(count_op(insts, cedar::Opcode::SAMPLE_PLAY) >= 1);
}

TEST_CASE("event-map: signal-valued transpose is rejected in Phase 1",
          "[event-map]") {
    // Phase 1 is constants-only; a signal parameter is an E131 (Phase 3 wires
    // the signal-rate path).
    auto r = akkado::compile(
        R"(lfo = osc("sin", 1)
           n"c4".transpose(lfo))");
    CHECK_FALSE(r.success);
}

// ============================================================================
// Phase 2b — stdlib `fn` migrations on top of closure event_map
// ============================================================================

TEST_CASE("event-map (Phase 2b): dur() multiplies event.duration at runtime",
          "[event-map][phase2b][dur]") {
    auto r = akkado::compile(R"(n"[c4 e4]".dur(2.0))");
    REQUIRE(r.success);
    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events == 2);
    // Source 2-event mini-notation gives each event natural duration 0.5;
    // dur(2.0) doubles to 1.0.
    for (std::uint32_t i = 0; i < st->output.num_events; ++i) {
        CHECK_THAT(st->output.events[i].duration, WithinAbs(1.0f, 0.001f));
    }
}

TEST_CASE("event-map (Phase 2b): bend() writes EVENT_OUT_BEND",
          "[event-map][phase2b][bend]") {
    auto r = akkado::compile(R"(n"[c4 e4 g4]".bend(0.42))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    REQUIRE(count_op(insts, cedar::Opcode::EVENT_MAP) == 1);
    for (const auto& i : insts) {
        if (i.opcode != cedar::Opcode::EVENT_MAP) continue;
        const std::uint8_t mask =
            (i.flags >> cedar::InstructionFlag::EVENT_MASK_SHIFT) & 0x7F;
        CHECK((mask & (1u << cedar::EVENT_OUT_BEND)) != 0);
    }
    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events >= 1);
    // overlay_event_field writes the bend value to prop slot 0.
    for (std::uint32_t i = 0; i < st->output.num_events; ++i) {
        CHECK_THAT(st->output.events[i].prop_vals[0], WithinAbs(0.42f, 0.001f));
    }
}

TEST_CASE("event-map (Phase 2b): aftertouch() writes EVENT_OUT_AT (prop slot 1)",
          "[event-map][phase2b][aftertouch]") {
    auto r = akkado::compile(R"(n"[c4 e4]".aftertouch(0.7))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    REQUIRE(count_op(insts, cedar::Opcode::EVENT_MAP) == 1);
    for (const auto& i : insts) {
        if (i.opcode != cedar::Opcode::EVENT_MAP) continue;
        const std::uint8_t mask =
            (i.flags >> cedar::InstructionFlag::EVENT_MASK_SHIFT) & 0x7F;
        CHECK((mask & (1u << cedar::EVENT_OUT_AT)) != 0);
    }
    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events >= 1);
    // EVENT_OUT_AT writes to prop slot 1.
    for (std::uint32_t i = 0; i < st->output.num_events; ++i) {
        CHECK_THAT(st->output.events[i].prop_vals[1], WithinAbs(0.7f, 0.001f));
    }
}

TEST_CASE("event-map (Phase 2b): bend + aftertouch chain writes both slots",
          "[event-map][phase2b][chain]") {
    auto r = akkado::compile(R"(n"[c4 e4]".bend(0.3).aftertouch(0.8))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::EVENT_MAP) == 2);
    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events >= 1);
    for (std::uint32_t i = 0; i < st->output.num_events; ++i) {
        CHECK_THAT(st->output.events[i].prop_vals[0], WithinAbs(0.3f, 0.001f));  // bend
        CHECK_THAT(st->output.events[i].prop_vals[1], WithinAbs(0.8f, 0.001f));  // aftertouch
    }
}

TEST_CASE("event-map (Phase 2b): late() shifts e.time by +t mod 1",
          "[event-map][phase2b][late]") {
    auto r = akkado::compile(R"(n"[c4 e4 g4 b4]".late(0.25))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    REQUIRE(count_op(insts, cedar::Opcode::EVENT_MAP) == 1);
    for (const auto& i : insts) {
        if (i.opcode != cedar::Opcode::EVENT_MAP) continue;
        const std::uint8_t mask =
            (i.flags >> cedar::InstructionFlag::EVENT_MASK_SHIFT) & 0x7F;
        CHECK((mask & (1u << cedar::EVENT_OUT_TIME)) != 0);
    }
    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events == 4);
    // Source times 0, 0.25, 0.5, 0.75 + 0.25 → 0.25, 0.5, 0.75, 0.0 (wraps).
    // The runtime sort re-orders by time, so post-sort: 0, 0.25, 0.5, 0.75.
    std::vector<float> times;
    for (std::uint32_t i = 0; i < st->output.num_events; ++i)
        times.push_back(st->output.events[i].time);
    REQUIRE(times.size() == 4);
    CHECK_THAT(times[0], WithinAbs(0.0f, 0.001f));
    CHECK_THAT(times[1], WithinAbs(0.25f, 0.001f));
    CHECK_THAT(times[2], WithinAbs(0.5f, 0.001f));
    CHECK_THAT(times[3], WithinAbs(0.75f, 0.001f));
}

TEST_CASE("event-map (Phase 2b): early() shifts e.time by -t with wrap",
          "[event-map][phase2b][early]") {
    // Source times 0, 0.25, 0.5, 0.75 - 0.25 → -0.25, 0, 0.25, 0.5. The
    // stdlib early() adds +1.0 inside fmod to keep the wrap non-negative,
    // landing the wrapped event at 0.75 instead of -0.25.
    auto r = akkado::compile(R"(n"[c4 e4 g4 b4]".early(0.25))");
    REQUIRE(r.success);
    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events == 4);
    std::vector<float> times;
    for (std::uint32_t i = 0; i < st->output.num_events; ++i)
        times.push_back(st->output.events[i].time);
    CHECK_THAT(times[0], WithinAbs(0.0f, 0.001f));
    CHECK_THAT(times[1], WithinAbs(0.25f, 0.001f));
    CHECK_THAT(times[2], WithinAbs(0.5f, 0.001f));
    CHECK_THAT(times[3], WithinAbs(0.75f, 0.001f));
}

TEST_CASE("event-map (Phase 2b): late() with t > 1 wraps to the same position",
          "[event-map][phase2b][late]") {
    auto r = akkado::compile(R"(n"[c4 e4 g4 b4]".late(1.25))");
    REQUIRE(r.success);
    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events == 4);
    for (std::uint32_t i = 0; i < st->output.num_events; ++i) {
        const float t = st->output.events[i].time;
        CHECK(t >= 0.0f);
        CHECK(t < 1.0f);
    }
}

TEST_CASE("event-map (Phase 2b): transpose then dur chains as two EVENT_MAPs",
          "[event-map][phase2b][chain]") {
    auto r = akkado::compile(R"(n"c4".transpose(7).dur(2.0))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::EVENT_MAP) == 2);
    CHECK(count_init(r, akkado::StateInitData::Type::EventTransform) == 2);
    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events >= 1);
    const auto& e = st->output.events[0];
    CHECK_THAT(e.midi_note, WithinAbs(67.0f, 0.01f));        // +7 from c4
    CHECK_THAT(e.duration, WithinAbs(2.0f, 0.001f));         // *2.0 from 1.0
}

// ============================================================================
// Phase 2a — closure-taking event_map / event_filter builtins
// ============================================================================

TEST_CASE("event-map: event_map() lowers to a closure EVENT_MAP + subprogram",
          "[event-map]") {
    auto r = akkado::compile(
        R"(event_map(n"c4", (e) -> {note: e.note + 12}) |> osc("sin", @.freq) |> out(@))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    REQUIRE(count_op(insts, cedar::Opcode::EVENT_MAP) == 1);
    // The closure body compiled into one subprogram block.
    CHECK(r.block_table.size() >= 1);
    CHECK(count_init(r, akkado::StateInitData::Type::EventTransform) == 1);

    for (const auto& i : insts) {
        if (i.opcode != cedar::Opcode::EVENT_MAP) continue;
        // Closure form: EVENT_CLOSURE flag set, `rate` carries the block_id.
        CHECK((i.flags & cedar::InstructionFlag::EVENT_CLOSURE) != 0);
        CHECK(i.rate < r.block_table.size());
        // Write mask (flags bits 4+) records that the `note` field was set.
        std::uint8_t mask = static_cast<std::uint8_t>(
            (i.flags >> cedar::InstructionFlag::EVENT_MASK_SHIFT)
            & ((1u << cedar::EVENT_OUT_COUNT) - 1u));
        CHECK((mask & (1u << cedar::EVENT_OUT_NOTE)) != 0);
        std::uint32_t up = static_cast<std::uint32_t>(i.inputs[2]) |
                           (static_cast<std::uint32_t>(i.inputs[3]) << 16);
        CHECK(up != 0u);
    }
}

TEST_CASE("event-map: event_map closure transposes notes at runtime",
          "[event-map]") {
    auto r = akkado::compile(R"(event_map(n"c4", (e) -> {note: e.note + 12}))");
    REQUIRE(r.success);
    auto h = render(r, 1);
    const auto* st = final_event_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events >= 1);
    const auto& e = st->output.events[0];
    CHECK_THAT(e.midi_note, WithinAbs(72.0f, 0.01f));
    CHECK_THAT(e.values[0], WithinAbs(kC4 * 2.0f, 1.0f));
}

TEST_CASE("event-map: event_map closure scales velocity at runtime",
          "[event-map]") {
    auto r = akkado::compile(R"(event_map(n"[c4 e4]", (e) -> {vel: e.vel * 0.5}))");
    REQUIRE(r.success);
    auto h = render(r, 1);
    const auto* st = final_event_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events == 2);
    for (std::uint32_t i = 0; i < st->output.num_events; ++i) {
        CHECK_THAT(st->output.events[i].velocity, WithinAbs(0.5f, 0.001f));
        CHECK_THAT(st->output.events[i].velocities[0], WithinAbs(0.5f, 0.001f));
    }
}

TEST_CASE("event-map: event_map closure overlays multiple fields",
          "[event-map]") {
    auto r = akkado::compile(
        R"(event_map(n"c4", (e) -> {note: e.note + 7, vel: e.vel * 0.5}))");
    REQUIRE(r.success);
    auto h = render(r, 1);
    const auto* st = final_event_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events >= 1);
    const auto& e = st->output.events[0];
    CHECK_THAT(e.midi_note, WithinAbs(67.0f, 0.01f));
    CHECK_THAT(e.velocity, WithinAbs(0.5f, 0.001f));
}

TEST_CASE("event-map: empty record closure is an identity transform",
          "[event-map]") {
    auto r = akkado::compile(R"(event_map(n"c4", (e) -> {}))");
    REQUIRE(r.success);
    auto h = render(r, 1);
    const auto* st = final_event_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events >= 1);
    const auto& e = st->output.events[0];
    // Nothing assigned — the event passes through untouched.
    CHECK_THAT(e.midi_note, WithinAbs(60.0f, 0.01f));
    CHECK_THAT(e.velocity, WithinAbs(1.0f, 0.001f));
}

TEST_CASE("event-map: event_map closure transposes every voice of a chord",
          "[event-map]") {
    auto r = akkado::compile(R"(event_map(c"CM", (e) -> {note: e.note + 12}))");
    REQUIRE(r.success);
    auto h = render(r, 1);
    const auto* st = final_event_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events >= 1);
    const auto& e = st->output.events[0];
    REQUIRE(e.num_values == 3);
    CHECK_THAT(e.notes[0], WithinAbs(72.0f, 0.01f));
    CHECK_THAT(e.notes[1], WithinAbs(76.0f, 0.01f));
    CHECK_THAT(e.notes[2], WithinAbs(79.0f, 0.01f));
    CHECK_THAT(e.values[0], WithinAbs(kC4 * 2.0f, 1.0f));
}

TEST_CASE("event-map: chained event_map calls compose at runtime",
          "[event-map]") {
    auto r = akkado::compile(
        R"(event_map(event_map(n"c4", (e) -> {note: e.note + 5}),
                     (e) -> {note: e.note + 7}))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    REQUIRE(count_op(insts, cedar::Opcode::EVENT_MAP) == 2);
    CHECK(count_init(r, akkado::StateInitData::Type::EventTransform) == 2);
    auto h = render(r, 1);
    const auto* st = final_event_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events >= 1);
    // 60 + 5 + 7 = 72.
    CHECK_THAT(st->output.events[0].midi_note, WithinAbs(72.0f, 0.01f));
}

TEST_CASE("event-map: event_filter lowers to a closure EVENT_FILTER",
          "[event-map]") {
    auto r = akkado::compile(
        R"(event_filter(n"[c4 e4 g4]", (e) -> e.note > 63))");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    REQUIRE(count_op(insts, cedar::Opcode::EVENT_FILTER) == 1);
    for (const auto& i : insts) {
        if (i.opcode != cedar::Opcode::EVENT_FILTER) continue;
        CHECK((i.flags & cedar::InstructionFlag::EVENT_CLOSURE) != 0);
        CHECK(i.inputs[4] != 0xFFFF);  // predicate result buffer
    }
}

TEST_CASE("event-map: event_filter drops events failing the predicate",
          "[event-map]") {
    // c4=60, e4=64, g4=67. Keep only notes above 63 → e4 and g4 survive.
    auto r = akkado::compile(
        R"(event_filter(n"[c4 e4 g4]", (e) -> e.note > 63))");
    REQUIRE(r.success);
    auto h = render(r, 1);
    const auto* st = final_event_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events == 2);
    CHECK_THAT(st->output.events[0].midi_note, WithinAbs(64.0f, 0.01f));
    CHECK_THAT(st->output.events[1].midi_note, WithinAbs(67.0f, 0.01f));
}

TEST_CASE("event-map: event_map closure with wrong arity is rejected",
          "[event-map]") {
    auto r = akkado::compile(
        R"(event_map(n"c4", (e, x) -> {note: e.note}))");
    CHECK_FALSE(r.success);  // E404 — closure must have exactly 1 parameter
}

TEST_CASE("event-map: event_map closure not returning a record is rejected",
          "[event-map]") {
    auto r = akkado::compile(R"(event_map(n"c4", (e) -> e.note + 1))");
    CHECK_FALSE(r.success);  // E174 — closure must return a record literal
}

TEST_CASE("event-map: event_map on a non-pattern argument is rejected",
          "[event-map]") {
    auto r = akkado::compile(
        R"(event_map(osc("sin", 440), (e) -> {note: e.note}))");
    CHECK_FALSE(r.success);  // E242 — first argument must be an event source
}
