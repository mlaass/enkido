// Tests for MIDI_QUERY runtime MIDI event source (PRD prd-midi-input, Phase 1).
// Mirrors the conventions in test_poly.cpp: hand-rolled bytecode, the same
// buffer + state-id constants, vm.process_block driving the audio path.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "cedar/opcodes/midi.hpp"
#include "cedar/vm/instruction.hpp"
#include "cedar/vm/vm.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace cedar;

// ============================================================================
// Shared layout — match test_poly.cpp:35-46 so a future merge of the helpers
// is mechanical. POLY-side tests reuse these exact constants.
// ============================================================================

static constexpr std::uint16_t BUF_FREQ = 0;
static constexpr std::uint16_t BUF_GATE = 1;
static constexpr std::uint16_t BUF_VEL  = 2;
static constexpr std::uint16_t BUF_TRIG = 3;
static constexpr std::uint16_t BUF_VOICE_OUT   = 4;
static constexpr std::uint16_t BUF_VOICE_OUT_R = 5;
static constexpr std::uint16_t BUF_MIX   = 6;

static constexpr std::uint32_t POLY_STATE_ID = 0x10000;
static constexpr std::uint32_t OSC_STATE_ID  = 0x20000;
static constexpr std::uint32_t MIDI_STATE_ID = 0x40000;

namespace {

// Build a MIDI_QUERY + POLY (osc-sin body) + OUTPUT program. Direct
// counterpart to test_poly.cpp's build_seq_poly_program, with SEQPAT_QUERY
// swapped for MIDI_QUERY.
std::vector<Instruction> build_midi_poly_program() {
    std::vector<Instruction> program;

    program.push_back(Instruction::make_nullary(
        Opcode::MIDI_QUERY, 0, MIDI_STATE_ID));

    auto poly_begin = Instruction::make_quinary(
        Opcode::POLY_BEGIN, BUF_MIX,
        BUF_FREQ, BUF_GATE, BUF_VEL, BUF_TRIG, BUF_VOICE_OUT,
        POLY_STATE_ID);
    poly_begin.flags = InstructionFlag::STEREO_OUTPUT;
    poly_begin.rate = 2;
    program.push_back(poly_begin);

    program.push_back(Instruction::make_unary(
        Opcode::OSC_SIN, BUF_VOICE_OUT, BUF_FREQ, OSC_STATE_ID));
    program.push_back(Instruction::make_unary(
        Opcode::COPY, BUF_VOICE_OUT_R, BUF_VOICE_OUT));

    program.push_back(Instruction::make_nullary(Opcode::POLY_END, 0));
    program.push_back(Instruction::make_unary(Opcode::OUTPUT, 0, BUF_MIX));

    return program;
}

// Build a program that runs only MIDI_QUERY (no POLY downstream). Useful
// for [midi] tests that just want to drive the drain and inspect
// MidiQueueState directly.
std::vector<Instruction> build_midi_only_program() {
    std::vector<Instruction> program;
    program.push_back(Instruction::make_nullary(
        Opcode::MIDI_QUERY, 0, MIDI_STATE_ID));
    program.push_back(Instruction::make_unary(Opcode::OUTPUT, 0, BUF_MIX));
    return program;
}

}  // namespace

// ============================================================================
// [midi] — state-only unit tests
// ============================================================================

TEST_CASE("MidiQueueState default-initializes held_note_to_event to -1",
          "[midi]") {
    MidiQueueState s;
    for (std::size_t i = 0; i < 128; ++i) {
        REQUIRE(s.held_note_to_event[i] == -1);
    }
}

TEST_CASE("push_midi_event enqueues into ring and op_midi_query drains them",
          "[midi]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_midi_only_program();
    vm.load_program_immediate(std::span<const Instruction>(program));
    vm.init_midi_queue_state(MIDI_STATE_ID, MidiSourceKind::DefaultDevice,
                             nullptr, /*channel_filter*/ 0,
                             /*loop*/ false,
                             MidiQueueState::TempoMode::Follow);

    // Push four note-ons across three pitches.
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 60, 100));
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 64,  90));
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 67,  80));
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 72,  70));

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    auto& s = vm.states().get_or_create<MidiQueueState>(MIDI_STATE_ID);
    REQUIRE(s.output.num_events == 4);

    // Held notes record their event index for note-off patching later.
    CHECK(s.held_note_to_event[60] == 0);
    CHECK(s.held_note_to_event[64] == 1);
    CHECK(s.held_note_to_event[67] == 2);
    CHECK(s.held_note_to_event[72] == 3);

    // Sentinel duration applied; freq from mtof; velocity normalized.
    CHECK(s.output.events[0].duration == MidiQueueState::HELD_DURATION_SENTINEL);
    CHECK(s.output.events[0].midi_note == 60.0f);
    CHECK_THAT(s.output.events[0].velocity,
               Catch::Matchers::WithinAbs(100.0f / 127.0f, 1e-5f));
    CHECK_THAT(s.output.events[0].values[0],
               Catch::Matchers::WithinAbs(440.0f * std::pow(2.0f, (60.0f - 69.0f) / 12.0f), 1e-3f));
}

TEST_CASE("MIDI ring full → drop-newest, overflow counter increments",
          "[midi]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_midi_only_program();
    vm.load_program_immediate(std::span<const Instruction>(program));
    vm.init_midi_queue_state(MIDI_STATE_ID, MidiSourceKind::DefaultDevice,
                             nullptr, 0, false,
                             MidiQueueState::TempoMode::Follow);

    const std::uint32_t cap = MidiQueueState::DEFAULT_RING_CAPACITY;

    // Fill the ring exactly to capacity. Each push uses a different note so
    // we can also assert the output buffer received them all once drained.
    for (std::uint32_t i = 0; i < cap; ++i) {
        // notes 0..127 cycle — we don't drain between pushes so all stay
        // in the ring as raw bytes (the held-note map only kicks in once
        // op_midi_query runs).
        bool ok = vm.push_midi_event(MIDI_STATE_ID, 0x90,
                                     static_cast<std::uint8_t>(i & 0x7F),
                                     100);
        REQUIRE(ok);
    }

    // One past capacity — must drop and bump the counter.
    bool ok = vm.push_midi_event(MIDI_STATE_ID, 0x90, 60, 100);
    CHECK_FALSE(ok);

    auto& s = vm.states().get_or_create<MidiQueueState>(MIDI_STATE_ID);
    CHECK(s.midi_overflow_count == 1);

    // Drain. We expect exactly `cap` events in output (note-ons all
    // accepted; the overflow one was dropped). The held-note map will have
    // overwrites since notes repeat every 128 pushes — fine, we only check
    // the cumulative count here.
    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());
    CHECK(s.output.num_events == cap);
}

TEST_CASE("note-on followed by note-off in same block patches duration",
          "[midi]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_midi_only_program();
    vm.load_program_immediate(std::span<const Instruction>(program));
    vm.init_midi_queue_state(MIDI_STATE_ID, MidiSourceKind::DefaultDevice,
                             nullptr, 0, false,
                             MidiQueueState::TempoMode::Follow);

    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 60, 100));  // note-on
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x80, 60,   0));  // note-off

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    auto& s = vm.states().get_or_create<MidiQueueState>(MIDI_STATE_ID);
    REQUIRE(s.output.num_events == 1);
    // Duration patched away from sentinel (single-sample floor when both
    // events land at the same block boundary).
    CHECK(s.output.events[0].duration < MidiQueueState::HELD_DURATION_SENTINEL);
    CHECK(s.output.events[0].duration > 0.0f);
    CHECK(s.held_note_to_event[60] == -1);
}

TEST_CASE("note-off arriving blocks later patches the original note-on",
          "[midi]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_midi_only_program();
    vm.load_program_immediate(std::span<const Instruction>(program));
    vm.init_midi_queue_state(MIDI_STATE_ID, MidiSourceKind::DefaultDevice,
                             nullptr, 0, false,
                             MidiQueueState::TempoMode::Follow);

    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 60, 100));

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    auto& s = vm.states().get_or_create<MidiQueueState>(MIDI_STATE_ID);
    REQUIRE(s.output.num_events == 1);
    const float on_time = s.output.events[0].time;
    CHECK(s.output.events[0].duration == MidiQueueState::HELD_DURATION_SENTINEL);

    // Process a few silent blocks while the note is held.
    for (int b = 0; b < 4; ++b) {
        vm.process_block(left.data(), right.data());
    }
    REQUIRE(s.output.num_events == 1);
    CHECK(s.output.events[0].duration == MidiQueueState::HELD_DURATION_SENTINEL);

    // Note-off arrives in a later block.
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x80, 60, 0));
    vm.process_block(left.data(), right.data());

    REQUIRE(s.output.num_events == 1);
    CHECK(s.output.events[0].duration < MidiQueueState::HELD_DURATION_SENTINEL);
    CHECK(s.output.events[0].duration > 0.0f);
    // The patched duration should cover ~5 blocks at 120 BPM / 48 kHz
    // (5 * 128 / 24000 ≈ 0.027 beats). Bound loosely — exact value depends
    // on float math in block_start_beats.
    const float expected = 5.0f * static_cast<float>(BLOCK_SIZE) /
                           ((60.0f / 120.0f) * 48000.0f);
    CHECK_THAT(s.output.events[0].duration,
               Catch::Matchers::WithinAbs(expected, expected * 0.5f));
    CHECK(s.output.events[0].time == on_time);
}

TEST_CASE("channel_filter rejects events on non-matching MIDI channels",
          "[midi]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_midi_only_program();
    vm.load_program_immediate(std::span<const Instruction>(program));
    // Only accept channel 2 (1-indexed → status nibble 0x91).
    vm.init_midi_queue_state(MIDI_STATE_ID, MidiSourceKind::DefaultDevice,
                             nullptr, /*channel_filter*/ 2, false,
                             MidiQueueState::TempoMode::Follow);

    // Channel 1 (status 0x90) — should be filtered out.
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 60, 100));
    // Channel 2 (status 0x91) — should pass.
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x91, 64, 110));
    // Channel 3 (status 0x92) — filtered out.
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x92, 67,  90));

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    auto& s = vm.states().get_or_create<MidiQueueState>(MIDI_STATE_ID);
    REQUIRE(s.output.num_events == 1);
    CHECK(s.output.events[0].midi_note == 64.0f);
}

TEST_CASE("non-note-status MIDI bytes are silently discarded in Phase 1",
          "[midi]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_midi_only_program();
    vm.load_program_immediate(std::span<const Instruction>(program));
    vm.init_midi_queue_state(MIDI_STATE_ID, MidiSourceKind::DefaultDevice,
                             nullptr, 0, false,
                             MidiQueueState::TempoMode::Follow);

    // CC#74 = 0xB0, pitch-bend = 0xE0, channel pressure = 0xD0, poly AT =
    // 0xA0, program change = 0xC0. None should appear in output.
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0xB0, 74, 100));
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0xE0,  0,  64));
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0xD0,  60,  0));
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0xA0,  60, 100));
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0xC0,   5,   0));
    // Real note-on after the noise.
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 60, 100));

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    auto& s = vm.states().get_or_create<MidiQueueState>(MIDI_STATE_ID);
    REQUIRE(s.output.num_events == 1);
    CHECK(s.output.events[0].midi_note == 60.0f);
}

TEST_CASE("velocity-0 note-on is treated as note-off (MIDI spec)",
          "[midi]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_midi_only_program();
    vm.load_program_immediate(std::span<const Instruction>(program));
    vm.init_midi_queue_state(MIDI_STATE_ID, MidiSourceKind::DefaultDevice,
                             nullptr, 0, false,
                             MidiQueueState::TempoMode::Follow);

    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 60, 100));
    // Velocity-0 note-on: behaves as note-off per MIDI spec.
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 60,   0));

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    auto& s = vm.states().get_or_create<MidiQueueState>(MIDI_STATE_ID);
    REQUIRE(s.output.num_events == 1);
    CHECK(s.output.events[0].duration < MidiQueueState::HELD_DURATION_SENTINEL);
    CHECK(s.held_note_to_event[60] == -1);
}

// ============================================================================
// [midi-poly] — full MIDI_QUERY → POLY_BEGIN integration tests
// ============================================================================

TEST_CASE("MIDI note-on allocates a POLY voice and produces audio",
          "[midi-poly]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_midi_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    vm.init_midi_queue_state(MIDI_STATE_ID, MidiSourceKind::DefaultDevice,
                             nullptr, 0, false,
                             MidiQueueState::TempoMode::Follow);
    vm.init_poly_state(POLY_STATE_ID, MIDI_STATE_ID, 8, 0, 0);

    // Triad: C4, E4, G4.
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 60, 100));
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 64, 100));
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 67, 100));

    std::array<float, BLOCK_SIZE> left{}, right{};
    // First block: drain + allocate.
    vm.process_block(left.data(), right.data());
    // Second block: voices fully active across the block.
    vm.process_block(left.data(), right.data());

    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    CHECK(poly.active_voice_count() == 3);

    float max_abs = 0.0f;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        max_abs = std::max(max_abs, std::abs(left[i]));
    }
    CHECK(max_abs > 0.01f);
}

TEST_CASE("MIDI note-off releases the matching POLY voice",
          "[midi-poly]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_midi_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    vm.init_midi_queue_state(MIDI_STATE_ID, MidiSourceKind::DefaultDevice,
                             nullptr, 0, false,
                             MidiQueueState::TempoMode::Follow);
    vm.init_poly_state(POLY_STATE_ID, MIDI_STATE_ID, 8, 0, 0);

    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 60, 100));

    std::array<float, BLOCK_SIZE> left{}, right{};
    // Drive a few blocks with the note held.
    for (int b = 0; b < 4; ++b) {
        vm.process_block(left.data(), right.data());
    }

    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    REQUIRE(poly.active_voice_count() == 1);

    // Note-off — POLY's release path runs in the block where evt.time +
    // evt.duration falls (i.e., this next block). The voice deactivates
    // after PolyAllocState::RELEASE_TIMEOUT (= 4) ticks.
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x80, 60, 0));
    for (int b = 0; b < 10; ++b) {
        vm.process_block(left.data(), right.data());
    }
    CHECK(poly.active_voice_count() == 0);
}

TEST_CASE("MIDI channel filter end-to-end gates POLY voice allocation",
          "[midi-poly]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_midi_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    // Only channel 2 events should produce voices.
    vm.init_midi_queue_state(MIDI_STATE_ID, MidiSourceKind::DefaultDevice,
                             nullptr, /*channel_filter*/ 2, false,
                             MidiQueueState::TempoMode::Follow);
    vm.init_poly_state(POLY_STATE_ID, MIDI_STATE_ID, 8, 0, 0);

    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 60, 100));  // ch 1 — drop
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x91, 64, 100));  // ch 2 — keep
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x92, 67, 100));  // ch 3 — drop

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());
    vm.process_block(left.data(), right.data());

    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    CHECK(poly.active_voice_count() == 1);
}

TEST_CASE("MIDI same-block note-on + note-off fires a one-shot voice",
          "[midi-poly]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_midi_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    vm.init_midi_queue_state(MIDI_STATE_ID, MidiSourceKind::DefaultDevice,
                             nullptr, 0, false,
                             MidiQueueState::TempoMode::Follow);
    vm.init_poly_state(POLY_STATE_ID, MIDI_STATE_ID, 8, 0, 0);

    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 60, 100));
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x80, 60,   0));

    std::array<float, BLOCK_SIZE> left{}, right{};
    // Allocate + release within this single op_midi_query call:
    vm.process_block(left.data(), right.data());

    // Settle: the voice goes through its release tail (RELEASE_TIMEOUT
    // blocks), no stuck voice.
    for (int b = 0; b < 10; ++b) {
        vm.process_block(left.data(), right.data());
    }

    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    CHECK(poly.active_voice_count() == 0);
}
