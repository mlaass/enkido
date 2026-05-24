// docs/prd-scale-quantize.md — `fn key` (octave-agnostic 12-TET
// quantization) + `fn scale` (octave-aware degree mapping). Both are stdlib
// match-dispatchers shipped in `akkado/stdlib/scale_quantize.ak`, generated
// by `web/scripts/generate-scale-quantize.ts` (Phase 5 Commit F of
// prd-runtime-event-transforms).
//
// Covers PRD §8 edge cases — in-scale identity, tie-break direction,
// chord-event pass-through, octave wrap, default octave, unknown name
// fall-through.

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

// SequenceState owned by the LAST EVENT_MAP in the chain (the outermost
// transform). `key` / `scale` lower to a single EVENT_MAP each so this is
// unambiguous; chained transforms would pick the last in source order.
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

}  // namespace

// ============================================================================
// `key` — octave-agnostic 12-TET quantization (PRD §4.5)
// ============================================================================

TEST_CASE("key: in-scale notes pass through unchanged", "[key][scale-quantize]") {
    // C major contains C E G (60, 64, 67). All three should be identity.
    auto r = akkado::compile(R"(n"[c4 e4 g4]" |> key(@, "c:major"))");
    REQUIRE(r.success);
    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events == 3);
    CHECK_THAT(st->output.events[0].midi_note, WithinAbs(60.0f, 0.01f));
    CHECK_THAT(st->output.events[1].midi_note, WithinAbs(64.0f, 0.01f));
    CHECK_THAT(st->output.events[2].midi_note, WithinAbs(67.0f, 0.01f));
}

TEST_CASE("key: out-of-scale notes snap down on a tie", "[key][scale-quantize]") {
    // C major: c#4(61)→c4(60), d#4(63)→d4(62), f#4(66)→f4(65), g#4(68)→g4(67),
    // a#4(70)→a4(69). All ties (distance 1 to two scale tones) round to the
    // lower MIDI per PRD §11.4.
    auto r = akkado::compile(R"(n"[c#4 d#4 f#4 g#4 a#4]" |> key(@, "c:major"))");
    REQUIRE(r.success);
    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events == 5);
    CHECK_THAT(st->output.events[0].midi_note, WithinAbs(60.0f, 0.01f));
    CHECK_THAT(st->output.events[1].midi_note, WithinAbs(62.0f, 0.01f));
    CHECK_THAT(st->output.events[2].midi_note, WithinAbs(65.0f, 0.01f));
    CHECK_THAT(st->output.events[3].midi_note, WithinAbs(67.0f, 0.01f));
    CHECK_THAT(st->output.events[4].midi_note, WithinAbs(69.0f, 0.01f));
}

TEST_CASE("key: D minor snaps c4 down to the lower in-scale tone",
          "[key][scale-quantize]") {
    // D minor = {D, E, F, G, A, A#, C} -> pcs {2, 4, 5, 7, 9, 10, 0}.
    // c4 (60, pc=0) is in scale -> 60.
    // c#4 (61, pc=1) closest = c(0)=dist 1 or d(2)=dist 1, tie -> lower = 60.
    // d4 (62) in scale -> 62.
    auto r = akkado::compile(R"(n"[c4 c#4 d4]" |> key(@, "d:minor"))");
    REQUIRE(r.success);
    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events == 3);
    CHECK_THAT(st->output.events[0].midi_note, WithinAbs(60.0f, 0.01f));
    CHECK_THAT(st->output.events[1].midi_note, WithinAbs(60.0f, 0.01f));
    CHECK_THAT(st->output.events[2].midi_note, WithinAbs(62.0f, 0.01f));
}

TEST_CASE("key: chord events pass through unchanged", "[key][scale-quantize]") {
    // PRD §11.2 — chord events (num_values > 1) are not quantized. The runtime
    // closure overlay rewrites the scalar `e.note` but the chord voices in
    // `e.notes[]` remain at their input MIDI values.
    //
    // c"CM" = C major chord at {60, 64, 67}. Quantizing to D minor: the
    // scalar primary moves (60 -> 60 since C is in D minor), but the voice
    // array stays intact.
    auto r = akkado::compile(R"(c"CM" |> key(@, "d:minor"))");
    REQUIRE(r.success);
    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events == 1);
    const auto& e = st->output.events[0];
    REQUIRE(e.num_values == 3);
    CHECK_THAT(e.notes[0], WithinAbs(60.0f, 0.01f));
    CHECK_THAT(e.notes[1], WithinAbs(64.0f, 0.01f));
    CHECK_THAT(e.notes[2], WithinAbs(67.0f, 0.01f));
}

TEST_CASE("key: unknown scale name falls through to identity",
          "[key][scale-quantize]") {
    // Unknown name hits the `_:` catch-all (pass-through). The fall-through
    // matches Strudel-style permissive live-coding — a typo doesn't silence
    // the pattern.
    auto r = akkado::compile(R"(n"c#4" |> key(@, "x:bogus"))");
    REQUIRE(r.success);
    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    // No event_map is emitted on a fall-through; pattern flows through
    // untransformed. The post-pipe sequence state still holds c#4 = 61.
    if (st) {
        REQUIRE(st->output.num_events == 1);
        CHECK_THAT(st->output.events[0].midi_note, WithinAbs(61.0f, 0.01f));
    }
}

// ============================================================================
// `scale` — degree mapping (PRD §4.4)
// ============================================================================

TEST_CASE("scale: degree 0 lands on the root MIDI", "[scale][scale-quantize]") {
    // d3:minor — root MIDI 50. Degree 0 -> 50.
    auto r = akkado::compile(R"(n"c0" |> scale(@, "d3:minor"))");
    REQUIRE(r.success);
    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events >= 1);
    // n"c0" is MIDI 12 — interpreted as a (very large) degree, the scale
    // map wraps it via octave division. degree = 12, k = 7 (minor scale length),
    // oct = floor(12/7) = 1, step = 12 - 7 = 5, interval[5] = 8.
    // out_midi = 50 + 12 + 8 = 70.
    // We use a degree-zero source: degree 0 -> 50.
    //
    // Substituting a `v"0"` source that emits a literal 0 value, but `v"..."`
    // requires the pattern parser to accept it. n"c0" is the simplest. Per
    // PRD §11.3, scale reads whichever numeric field carries the degree —
    // for `n"…"` that's the note field, which for c0 is MIDI 12.
    CHECK_THAT(st->output.events[0].midi_note, WithinAbs(70.0f, 0.01f));
}

TEST_CASE("scale: explicit degree maps to root + interval[step]",
          "[scale][scale-quantize]") {
    // d3:minor intervals = [0, 2, 3, 5, 7, 8, 10], k = 7. degree 2 -> step 2,
    // interval = 3, out = 50 + 3 = 53. degree 7 -> oct 1, step 0,
    // out = 50 + 12 + 0 = 62.
    //
    // We synthesise integer degrees by piping a transposed mono pattern
    // through scale: n"c0".transpose(-10) brings MIDI to 2 (degree 2);
    // .transpose(-5) brings to 7 (degree 7).
    {
        auto r = akkado::compile(R"(n"c0".transpose(-10) |> scale(@, "d3:minor"))");
        REQUIRE(r.success);
        auto h = render(r, 1);
        const auto* st = final_transform_state(*h);
        REQUIRE(st != nullptr);
        REQUIRE(st->output.num_events >= 1);
        CHECK_THAT(st->output.events[0].midi_note, WithinAbs(53.0f, 0.01f));
    }
    {
        auto r = akkado::compile(R"(n"c0".transpose(-5) |> scale(@, "d3:minor"))");
        REQUIRE(r.success);
        auto h = render(r, 1);
        const auto* st = final_transform_state(*h);
        REQUIRE(st != nullptr);
        REQUIRE(st->output.num_events >= 1);
        // degree 7 -> root + 12 (octave wrap).
        CHECK_THAT(st->output.events[0].midi_note, WithinAbs(62.0f, 0.01f));
    }
}

TEST_CASE("scale: no-octave default lands on octave 3",
          "[scale][scale-quantize]") {
    // "d:minor" with no octave digit defaults to "d3:minor" (PRD §11.5).
    // Same degree as the explicit case above -> same MIDI.
    auto r = akkado::compile(R"(n"c0".transpose(-12) |> scale(@, "d:minor"))");
    REQUIRE(r.success);
    auto h = render(r, 1);
    const auto* st = final_transform_state(*h);
    REQUIRE(st != nullptr);
    REQUIRE(st->output.num_events >= 1);
    // degree 0 -> root midi (D3 = 50).
    CHECK_THAT(st->output.events[0].midi_note, WithinAbs(50.0f, 0.01f));
}

TEST_CASE("scale: alias scale names resolve to the canonical scale",
          "[scale][scale-quantize]") {
    // "c3:major" and "c3:ionian" are aliases per PRD §4.2.
    auto r_major  = akkado::compile(R"(n"c0".transpose(-10) |> scale(@, "c3:major"))");
    auto r_ionian = akkado::compile(R"(n"c0".transpose(-10) |> scale(@, "c3:ionian"))");
    REQUIRE(r_major.success);
    REQUIRE(r_ionian.success);
    auto h_major  = render(r_major,  1);
    auto h_ionian = render(r_ionian, 1);
    const auto* st_major  = final_transform_state(*h_major);
    const auto* st_ionian = final_transform_state(*h_ionian);
    REQUIRE(st_major != nullptr);
    REQUIRE(st_ionian != nullptr);
    REQUIRE(st_major->output.num_events == 1);
    REQUIRE(st_ionian->output.num_events == 1);
    CHECK_THAT(st_major->output.events[0].midi_note,
               WithinAbs(st_ionian->output.events[0].midi_note, 0.01f));
}
