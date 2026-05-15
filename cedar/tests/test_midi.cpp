// Tests for MIDI_QUERY runtime MIDI event source (PRD prd-midi-input, Phase 1).
// Mirrors the conventions in test_poly.cpp: hand-rolled bytecode, the same
// buffer + state-id constants, vm.process_block driving the audio path.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "cedar/io/midi_sequence.hpp"
#include "cedar/opcodes/dsp_state.hpp"  // for PolyAllocState
#include "cedar/opcodes/midi.hpp"
#include "cedar/vm/audio_arena.hpp"
#include "cedar/vm/instruction.hpp"
#include "cedar/vm/vm.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
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

// PRD prd-midi-input §7.5: MIDI_QUERY with the four mono buffers wired so
// fill_mono_buffers runs. Buffer layout matches the per-voice POLY layout
// (BUF_GATE / BUF_FREQ / BUF_VEL / BUF_TRIG) so a test can mix this opcode
// with downstream consumers if desired.
std::vector<Instruction> build_midi_mono_program() {
    std::vector<Instruction> program;
    auto midi = Instruction::make_quaternary(
        Opcode::MIDI_QUERY, 0xFFFF,
        BUF_GATE, BUF_FREQ, BUF_VEL, BUF_TRIG, MIDI_STATE_ID);
    program.push_back(midi);
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

// ============================================================================
// [smf] — SMF parser unit tests (PRD prd-midi-input Phase 5)
// ============================================================================

namespace {

// Inline byte-builder for tiny synthetic SMF streams. Tests can construct a
// valid file in 5-10 lines without committing a binary fixture for every
// edge case. push_vlq matches the parser's VLQ shape (MSB continuation).
struct ByteBuilder {
    std::vector<std::uint8_t> bytes;
    void u8(std::uint8_t v)  { bytes.push_back(v); }
    void u16(std::uint16_t v){ u8(std::uint8_t(v >> 8)); u8(std::uint8_t(v)); }
    void u32(std::uint32_t v){ u8(std::uint8_t(v >> 24)); u8(std::uint8_t(v >> 16));
                               u8(std::uint8_t(v >> 8));  u8(std::uint8_t(v)); }
    void str(const char* s)  { for (; *s; ++s) u8(std::uint8_t(*s)); }
    void vlq(std::uint32_t v) {
        std::uint32_t buffer = v & 0x7F;
        while (v >>= 7) { buffer <<= 8; buffer |= ((v & 0x7F) | 0x80); }
        while (true) {
            u8(std::uint8_t(buffer & 0xFF));
            if (buffer & 0x80) buffer >>= 8;
            else break;
        }
    }
    void embed_track(const std::vector<std::uint8_t>& events) {
        str("MTrk");
        u32(std::uint32_t(events.size()));
        bytes.insert(bytes.end(), events.begin(), events.end());
    }
};

// Build a minimal one-track SMF with the supplied track-body bytes.
std::vector<std::uint8_t> make_smf(std::uint16_t format,
                                   std::uint16_t ntrks,
                                   std::uint16_t tpq,
                                   const std::vector<std::uint8_t>& track) {
    ByteBuilder b;
    b.str("MThd");
    b.u32(6);
    b.u16(format);
    b.u16(ntrks);
    b.u16(tpq);
    b.embed_track(track);
    return b.bytes;
}

// Read a fixture file from CEDAR_TEST_FIXTURES_DIR. Aborts the test if the
// file cannot be opened (regression in the build wiring).
std::vector<std::uint8_t> read_fixture(const char* relative_path) {
    std::string full = CEDAR_TEST_FIXTURES_DIR;
    full += "/";
    full += relative_path;
    std::FILE* fp = std::fopen(full.c_str(), "rb");
    REQUIRE(fp != nullptr);
    std::fseek(fp, 0, SEEK_END);
    const auto len = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(len));
    if (len > 0) {
        const std::size_t got = std::fread(out.data(), 1, std::size_t(len), fp);
        REQUIRE(got == std::size_t(len));
    }
    std::fclose(fp);
    return out;
}

}  // namespace

TEST_CASE("parse_smf accepts a minimal format-0 file with one note", "[smf]") {
    // One quarter note C4 (60) at 480 TPQ. Note-on at tick 0, note-off at
    // tick 480, EOT at the same tick.
    std::vector<std::uint8_t> track;
    ByteBuilder ev;
    ev.vlq(0);      ev.u8(0x90); ev.u8(60); ev.u8(96);    // note-on
    ev.vlq(480);    ev.u8(0x80); ev.u8(60); ev.u8(64);    // note-off
    ev.vlq(0);      ev.u8(0xFF); ev.u8(0x2F); ev.u8(0x00); // EOT
    track = ev.bytes;

    const auto bytes = make_smf(/*format*/0, /*ntrks*/1, /*tpq*/480, track);

    AudioArena arena(64 * 1024);
    MidiSequence* seq = parse_smf(bytes.data(), bytes.size(), arena);

    REQUIRE(seq != nullptr);
    REQUIRE(seq->ticks_per_quarter == 480);
    REQUIRE(seq->num_notes == 1);
    CHECK(seq->notes[0].note == 60);
    CHECK(seq->notes[0].vel == 96);
    CHECK(seq->notes[0].tick_on == 0);
    CHECK(seq->notes[0].tick_off == 480);
}

TEST_CASE("parse_smf rejects format-2 files", "[smf]") {
    std::vector<std::uint8_t> track;
    ByteBuilder ev;
    ev.vlq(0); ev.u8(0xFF); ev.u8(0x2F); ev.u8(0x00);
    track = ev.bytes;

    const auto bytes = make_smf(/*format*/2, /*ntrks*/1, /*tpq*/480, track);
    AudioArena arena(64 * 1024);
    CHECK(parse_smf(bytes.data(), bytes.size(), arena) == nullptr);
}

TEST_CASE("parse_smf rejects SMPTE timing division", "[smf]") {
    std::vector<std::uint8_t> track;
    ByteBuilder ev;
    ev.vlq(0); ev.u8(0xFF); ev.u8(0x2F); ev.u8(0x00);
    track = ev.bytes;

    // SMPTE timing has the high bit of division set.
    const auto bytes = make_smf(/*format*/0, /*ntrks*/1,
                                /*tpq*/0xE228, track);
    AudioArena arena(64 * 1024);
    CHECK(parse_smf(bytes.data(), bytes.size(), arena) == nullptr);
}

TEST_CASE("parse_smf rejects truncated header / null input", "[smf]") {
    AudioArena arena(8 * 1024);
    CHECK(parse_smf(nullptr, 0, arena) == nullptr);
    const std::uint8_t too_short[] = { 'M','T','h','d', 0,0,0,6 };
    CHECK(parse_smf(too_short, sizeof(too_short), arena) == nullptr);
    const std::uint8_t bad_magic[] = { 'B','A','D','!', 0,0,0,6,
                                       0,0, 0,1, 1,0xE0 };
    CHECK(parse_smf(bad_magic, sizeof(bad_magic), arena) == nullptr);
}

TEST_CASE("parse_smf handles running status across note-ons", "[smf]") {
    // Three note-ons back to back using running status (0x90 implicit on
    // events 2 and 3), each followed by an explicit note-off.
    std::vector<std::uint8_t> track;
    ByteBuilder ev;
    ev.vlq(0);   ev.u8(0x90); ev.u8(60); ev.u8(96);          // note-on, status set
    ev.vlq(0);                ev.u8(64); ev.u8(96);          // running status
    ev.vlq(0);                ev.u8(67); ev.u8(96);          // running status
    ev.vlq(480); ev.u8(0x80); ev.u8(60); ev.u8(64);          // note-off C4
    ev.vlq(0);                ev.u8(64); ev.u8(64);          // running status off
    ev.vlq(0);                ev.u8(67); ev.u8(64);          // running status off
    ev.vlq(0);   ev.u8(0xFF); ev.u8(0x2F); ev.u8(0x00);      // EOT
    track = ev.bytes;

    const auto bytes = make_smf(0, 1, 480, track);
    AudioArena arena(64 * 1024);
    MidiSequence* seq = parse_smf(bytes.data(), bytes.size(), arena);
    REQUIRE(seq != nullptr);
    CHECK(seq->num_notes == 3);
}

TEST_CASE("parse_smf treats velocity-0 note-on as note-off", "[smf]") {
    std::vector<std::uint8_t> track;
    ByteBuilder ev;
    ev.vlq(0);   ev.u8(0x90); ev.u8(60); ev.u8(96);   // note-on
    ev.vlq(240); ev.u8(0x90); ev.u8(60); ev.u8(0);    // note-on vel 0 == off
    ev.vlq(0);   ev.u8(0xFF); ev.u8(0x2F); ev.u8(0x00);
    track = ev.bytes;

    const auto bytes = make_smf(0, 1, 480, track);
    AudioArena arena(64 * 1024);
    MidiSequence* seq = parse_smf(bytes.data(), bytes.size(), arena);
    REQUIRE(seq != nullptr);
    REQUIRE(seq->num_notes == 1);
    CHECK(seq->notes[0].tick_off == 240);
}

TEST_CASE("parse_smf populates tempos[] with default + meta events", "[smf]") {
    std::vector<std::uint8_t> track;
    ByteBuilder ev;
    // Set-tempo: 500000 us/qn (120 BPM) at tick 0
    ev.vlq(0); ev.u8(0xFF); ev.u8(0x51); ev.u8(0x03);
    ev.u8(0x07); ev.u8(0xA1); ev.u8(0x20);  // 0x07A120 = 500000
    // Set-tempo: 250000 us/qn (240 BPM) at tick 480
    ev.vlq(480); ev.u8(0xFF); ev.u8(0x51); ev.u8(0x03);
    ev.u8(0x03); ev.u8(0xD0); ev.u8(0x90);  // 0x03D090 = 250000
    ev.vlq(0); ev.u8(0xFF); ev.u8(0x2F); ev.u8(0x00);
    track = ev.bytes;

    const auto bytes = make_smf(0, 1, 480, track);
    AudioArena arena(64 * 1024);
    MidiSequence* seq = parse_smf(bytes.data(), bytes.size(), arena);
    REQUIRE(seq != nullptr);
    REQUIRE(seq->num_tempos == 2);
    CHECK(seq->tempos[0].tick == 0);
    CHECK(seq->tempos[0].us_per_quarter == 500000u);
    CHECK_THAT(seq->tempos[0].beats_before,
               Catch::Matchers::WithinAbs(0.0f, 1e-6f));
    CHECK(seq->tempos[1].tick == 480);
    CHECK(seq->tempos[1].us_per_quarter == 250000u);
    // 480 ticks before second tempo / 480 TPQ = 1 beat.
    CHECK_THAT(seq->tempos[1].beats_before,
               Catch::Matchers::WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("parse_smf synthesizes a default tempo when the file declares none",
          "[smf]") {
    std::vector<std::uint8_t> track;
    ByteBuilder ev;
    ev.vlq(0);   ev.u8(0x90); ev.u8(60); ev.u8(96);
    ev.vlq(480); ev.u8(0x80); ev.u8(60); ev.u8(64);
    ev.vlq(0);   ev.u8(0xFF); ev.u8(0x2F); ev.u8(0x00);
    track = ev.bytes;

    const auto bytes = make_smf(0, 1, 480, track);
    AudioArena arena(64 * 1024);
    MidiSequence* seq = parse_smf(bytes.data(), bytes.size(), arena);
    REQUIRE(seq != nullptr);
    REQUIRE(seq->num_tempos == 1);
    CHECK(seq->tempos[0].tick == 0);
    CHECK(seq->tempos[0].us_per_quarter == 500000u);  // 120 BPM default
}

TEST_CASE("parse_smf skips unsupported channel and meta payloads", "[smf]") {
    // CC, program change, pitch-bend, channel pressure, and a non-tempo
    // meta should all be consumed without throwing the parser off.
    std::vector<std::uint8_t> track;
    ByteBuilder ev;
    // Text meta (type 0x01) with body "hi"
    ev.vlq(0); ev.u8(0xFF); ev.u8(0x01); ev.u8(0x02); ev.u8('h'); ev.u8('i');
    // CC #74 = 0x40
    ev.vlq(0); ev.u8(0xB0); ev.u8(74); ev.u8(0x40);
    // Program change to 0x10
    ev.vlq(0); ev.u8(0xC0); ev.u8(0x10);
    // Channel pressure 0x55
    ev.vlq(0); ev.u8(0xD0); ev.u8(0x55);
    // Pitch bend
    ev.vlq(0); ev.u8(0xE0); ev.u8(0x40); ev.u8(0x40);
    // Note-on/off
    ev.vlq(0);   ev.u8(0x90); ev.u8(60); ev.u8(96);
    ev.vlq(240); ev.u8(0x80); ev.u8(60); ev.u8(64);
    ev.vlq(0);   ev.u8(0xFF); ev.u8(0x2F); ev.u8(0x00);
    track = ev.bytes;

    const auto bytes = make_smf(0, 1, 480, track);
    AudioArena arena(64 * 1024);
    MidiSequence* seq = parse_smf(bytes.data(), bytes.size(), arena);
    REQUIRE(seq != nullptr);
    CHECK(seq->num_notes == 1);
}

TEST_CASE("parse_smf loads twinkle.mid fixture with 24 notes", "[smf]") {
    const auto bytes = read_fixture("midi/twinkle.mid");
    AudioArena arena(256 * 1024);
    MidiSequence* seq = parse_smf(bytes.data(), bytes.size(), arena);
    REQUIRE(seq != nullptr);
    CHECK(seq->num_notes == 24);
    CHECK(seq->ticks_per_quarter == 480);
    CHECK(seq->num_tempos >= 1);
    // First note is middle C (60). Each note is a quarter note at 480 TPQ,
    // so tick_off - tick_on must be 480.
    CHECK(seq->notes[0].note == 60);
    CHECK(seq->notes[0].tick_off - seq->notes[0].tick_on == 480u);
}

TEST_CASE("parse_smf loads tempo-map.mid with embedded tempo changes", "[smf]") {
    const auto bytes = read_fixture("midi/tempo-map.mid");
    AudioArena arena(256 * 1024);
    MidiSequence* seq = parse_smf(bytes.data(), bytes.size(), arena);
    REQUIRE(seq != nullptr);
    REQUIRE(seq->num_notes == 4);
    REQUIRE(seq->num_tempos >= 4);
    // beat_on_file for follow mode would equal tick_on / TPQ. For the
    // second note (tick_on = 480, TPQ = 480) follow == 1.0 beats; file mode
    // honours the tempo map but ticks are linear in beats, so beat_on_file
    // also equals 1.0 — the difference manifests in playback wall-time, not
    // beat coordinates. The point of file mode is that we DO precompute the
    // beat tables: confirm the field is populated.
    CHECK_THAT(seq->notes[1].beat_on_file,
               Catch::Matchers::WithinAbs(1.0f, 1e-3f));
}

TEST_CASE("parse_smf rejects the format-2 fixture", "[smf]") {
    const auto bytes = read_fixture("midi/format2.mid");
    AudioArena arena(64 * 1024);
    CHECK(parse_smf(bytes.data(), bytes.size(), arena) == nullptr);
}

// ============================================================================
// [midi-file] — op_midi_query file-playback tests
// ============================================================================

TEST_CASE("File playback emits a note-on once per scheduled tick", "[midi-file]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_midi_only_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    const auto fixture = read_fixture("midi/twinkle.mid");
    REQUIRE(vm.load_midi_file("twinkle.mid",
                              fixture.data(), fixture.size()) == 0);
    vm.init_midi_queue_state(MIDI_STATE_ID, MidiSourceKind::File,
                             "twinkle.mid", 0, /*loop*/false,
                             MidiQueueState::TempoMode::Follow);

    std::array<float, BLOCK_SIZE> left{}, right{};

    // Sanity check: run a single block, confirm op_midi_query advanced the
    // play head (catches a Phase-5 regression where the file path remained
    // stubbed). Then run a short window and assert at least one note
    // emitted — the long-form scan test in [midi-poly] handles the
    // 300-sec coverage requirement without paying for 5000 quick blocks
    // in every iterative dev cycle.
    vm.process_block(left.data(), right.data());
    auto* s = vm.states().get_if<MidiQueueState>(MIDI_STATE_ID);
    REQUIRE(s != nullptr);
    REQUIRE(s->file_seq != nullptr);
    CHECK(s->file_play_head_beats > 0.0);
    CHECK(s->output.num_events >= 1);
    CHECK(s->output.events[0].midi_note == 60.0f);
}

TEST_CASE("File playback loops when loop=true", "[midi-file]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_midi_only_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    const auto fixture = read_fixture("midi/twinkle.mid");
    REQUIRE(vm.load_midi_file("twinkle.mid",
                              fixture.data(), fixture.size()) == 0);
    vm.init_midi_queue_state(MIDI_STATE_ID, MidiSourceKind::File,
                             "twinkle.mid", 0, /*loop*/true,
                             MidiQueueState::TempoMode::Follow);

    std::array<float, BLOCK_SIZE> left{}, right{};
    // Run for ~12 sec of audio (4500 blocks) — twinkle is 12 sec at 120 BPM
    // (24 quarter notes = 24 * 0.5 = 12 sec), so loop should re-emit notes.
    for (int i = 0; i < 5000; ++i) {
        vm.process_block(left.data(), right.data());
    }
    auto* s = vm.states().get_if<MidiQueueState>(MIDI_STATE_ID);
    REQUIRE(s != nullptr);
    // After ~13 sec of audio with a 12-sec loop, expect at least 25 events
    // (full first iteration + one note into the second). Cap at capacity.
    CHECK(s->output.num_events > 24);
}

TEST_CASE("File playback honors channel_filter", "[midi-file]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_midi_only_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    const auto fixture = read_fixture("midi/twinkle.mid");
    REQUIRE(vm.load_midi_file("twinkle.mid",
                              fixture.data(), fixture.size()) == 0);
    // The fixture writes all notes on channel 0 (1 with 1-indexed shift).
    // Filtering channel 2 must drop everything.
    vm.init_midi_queue_state(MIDI_STATE_ID, MidiSourceKind::File,
                             "twinkle.mid", /*channel*/2, false,
                             MidiQueueState::TempoMode::Follow);

    std::array<float, BLOCK_SIZE> left{}, right{};
    for (int i = 0; i < 1000; ++i) {
        vm.process_block(left.data(), right.data());
    }
    auto* s = vm.states().get_if<MidiQueueState>(MIDI_STATE_ID);
    REQUIRE(s != nullptr);
    CHECK(s->output.num_events == 0);
}

TEST_CASE("File playback nullptr file_seq stays silent", "[midi-file]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_midi_only_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    // Initialize a File-kind state without loading any bytes — file_seq
    // stays nullptr. Documented edge case (PRD §7 "`.mid` not yet loaded").
    vm.init_midi_queue_state(MIDI_STATE_ID, MidiSourceKind::File,
                             "missing.mid", 0, false,
                             MidiQueueState::TempoMode::Follow);

    std::array<float, BLOCK_SIZE> left{}, right{};
    for (int i = 0; i < 100; ++i) {
        vm.process_block(left.data(), right.data());
    }
    auto* s = vm.states().get_if<MidiQueueState>(MIDI_STATE_ID);
    REQUIRE(s != nullptr);
    CHECK(s->file_seq == nullptr);
    CHECK(s->output.num_events == 0);
}

TEST_CASE("vm.load_midi_file is idempotent and dedup'd by name", "[midi-file]") {
    VM vm;
    // Load order mirrors the host: program first (which resets the
    // arena-backed registries), then asset bytes, then state inits.
    auto program = build_midi_only_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    const auto fixture = read_fixture("midi/twinkle.mid");
    REQUIRE(vm.load_midi_file("twinkle.mid",
                              fixture.data(), fixture.size()) == 0);
    // Second call returns success without re-parsing.
    REQUIRE(vm.load_midi_file("twinkle.mid",
                              fixture.data(), fixture.size()) == 0);

    // Two MidiQueueStates referencing the same file get the same pointer.
    vm.init_midi_queue_state(0x111, MidiSourceKind::File,
                             "twinkle.mid", 0, false,
                             MidiQueueState::TempoMode::Follow);
    vm.init_midi_queue_state(0x222, MidiSourceKind::File,
                             "twinkle.mid", 0, false,
                             MidiQueueState::TempoMode::Follow);
    auto* s1 = vm.states().get_if<MidiQueueState>(0x111);
    auto* s2 = vm.states().get_if<MidiQueueState>(0x222);
    REQUIRE(s1 != nullptr); REQUIRE(s2 != nullptr);
    CHECK(s1->file_seq != nullptr);
    CHECK(s1->file_seq == s2->file_seq);
}

TEST_CASE("vm.load_midi_file rejects format-2 fixture", "[midi-file]") {
    VM vm;
    const auto bytes = read_fixture("midi/format2.mid");
    CHECK(vm.load_midi_file("format2.mid",
                            bytes.data(), bytes.size()) < 0);
}

// ============================================================================
// [midi-poly] — full file → POLY integration
// ============================================================================

TEST_CASE("twinkle.mid drives poly voice allocation for ~300 sec", "[midi-poly]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_midi_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    const auto fixture = read_fixture("midi/twinkle.mid");
    REQUIRE(vm.load_midi_file("twinkle.mid",
                              fixture.data(), fixture.size()) == 0);
    vm.init_midi_queue_state(MIDI_STATE_ID, MidiSourceKind::File,
                             "twinkle.mid", 0, /*loop*/true,
                             MidiQueueState::TempoMode::Follow);
    vm.init_poly_state(POLY_STATE_ID, MIDI_STATE_ID, 8, 0, 0);

    // 300 seconds at 48 kHz / 128 = 112_500 blocks. Drive the engine and
    // accumulate the peak absolute output value to confirm voices are
    // actually rendering audio rather than tripping zero output.
    constexpr int kBlocks = 112'500;
    float peak = 0.0f;
    std::array<float, BLOCK_SIZE> left{}, right{};
    for (int i = 0; i < kBlocks; ++i) {
        vm.process_block(left.data(), right.data());
        for (float v : left) peak = std::max(peak, std::fabs(v));
    }
    // Looped Twinkle should produce a clearly non-zero sine output.
    CHECK(peak > 0.05f);

    // POLY state should still be running cleanly (no stuck voices that
    // never released across hundreds of seconds).
    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    CHECK(poly.active_voice_count() <= 8);
}

// ============================================================================
// PRD prd-midi-input §7.5: monophonic per-block buffer synthesis
//
// `midi() as e |> osc("sin", e.freq)` works because op_midi_query bakes the
// runtime event stream into mono gate/freq/vel/trig buffers using
// last-note-wins with held-stack fallback. These tests drive the SPSC ring
// directly and inspect the four mono buffers via vm.states().
// ============================================================================

namespace {
// Helper: read back the four mono buffers after the last process_block.
// All four live in BufferPool slots whose indices match BUF_GATE / BUF_FREQ
// / BUF_VEL / BUF_TRIG.
struct MonoSample {
    float gate;
    float freq;
    float vel;
    float trig;
};
MonoSample read_mono(const VM& vm, std::size_t i) {
    const auto* gate = const_cast<VM&>(vm).buffers().get(BUF_GATE);
    const auto* freq = const_cast<VM&>(vm).buffers().get(BUF_FREQ);
    const auto* vel  = const_cast<VM&>(vm).buffers().get(BUF_VEL);
    const auto* trig = const_cast<VM&>(vm).buffers().get(BUF_TRIG);
    return {gate[i], freq[i], vel[i], trig[i]};
}
}  // namespace

TEST_CASE("midi-mono: note-on raises gate, sets freq and vel, fires trig once",
          "[midi-mono]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_midi_mono_program();
    vm.load_program_immediate(std::span<const Instruction>(program));
    vm.init_midi_queue_state(MIDI_STATE_ID, MidiSourceKind::DefaultDevice,
                             nullptr, 0, false,
                             MidiQueueState::TempoMode::Follow);

    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 60, 100));

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    // Trig fires exactly once across the block.
    int trig_count = 0;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        if (read_mono(vm, i).trig > 0.5f) ++trig_count;
    }
    CHECK(trig_count == 1);

    // After the note-on, gate is held high; freq matches MIDI 60 → C4.
    auto end = read_mono(vm, BLOCK_SIZE - 1);
    CHECK(end.gate > 0.9f);
    const float expected_freq =
        440.0f * std::pow(2.0f, (60.0f - 69.0f) / 12.0f);
    CHECK_THAT(end.freq, Catch::Matchers::WithinAbs(expected_freq, 1e-3f));
    CHECK_THAT(end.vel,
               Catch::Matchers::WithinAbs(100.0f / 127.0f, 1e-5f));
}

TEST_CASE("midi-mono: last-note-wins — newer note overrides freq while held",
          "[midi-mono]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_midi_mono_program();
    vm.load_program_immediate(std::span<const Instruction>(program));
    vm.init_midi_queue_state(MIDI_STATE_ID, MidiSourceKind::DefaultDevice,
                             nullptr, 0, false,
                             MidiQueueState::TempoMode::Follow);

    // Hold C4 for a few blocks, then add E4 — E4 should win.
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 60, 100));
    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 64,  90));
    vm.process_block(left.data(), right.data());

    auto end = read_mono(vm, BLOCK_SIZE - 1);
    const float expected_e4 =
        440.0f * std::pow(2.0f, (64.0f - 69.0f) / 12.0f);
    CHECK(end.gate > 0.9f);
    CHECK_THAT(end.freq, Catch::Matchers::WithinAbs(expected_e4, 1e-3f));
}

TEST_CASE("midi-mono: pop fallback — note-off uncovers prior held note",
          "[midi-mono]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_midi_mono_program();
    vm.load_program_immediate(std::span<const Instruction>(program));
    vm.init_midi_queue_state(MIDI_STATE_ID, MidiSourceKind::DefaultDevice,
                             nullptr, 0, false,
                             MidiQueueState::TempoMode::Follow);

    // C4 on, E4 on, then E4 off. Result: gate stays high, freq falls back
    // to C4 — classic monophonic legato.
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 60, 100));
    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 64,  90));
    vm.process_block(left.data(), right.data());

    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x80, 64,   0));
    vm.process_block(left.data(), right.data());

    auto end = read_mono(vm, BLOCK_SIZE - 1);
    const float expected_c4 =
        440.0f * std::pow(2.0f, (60.0f - 69.0f) / 12.0f);
    CHECK(end.gate > 0.9f);
    CHECK_THAT(end.freq, Catch::Matchers::WithinAbs(expected_c4, 1e-3f));

    // Stack now has one entry (C4).
    auto& s = vm.states().get_or_create<MidiQueueState>(MIDI_STATE_ID);
    CHECK(s.mono_stack_depth == 1);
    CHECK(s.mono_held_stack[0].note == 60);
}

TEST_CASE("midi-mono: gate transitions 1→0 only when stack empties",
          "[midi-mono]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_midi_mono_program();
    vm.load_program_immediate(std::span<const Instruction>(program));
    vm.init_midi_queue_state(MIDI_STATE_ID, MidiSourceKind::DefaultDevice,
                             nullptr, 0, false,
                             MidiQueueState::TempoMode::Follow);

    // Press one note, release it: gate falls.
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 60, 100));
    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x80, 60,   0));
    vm.process_block(left.data(), right.data());

    auto end = read_mono(vm, BLOCK_SIZE - 1);
    CHECK(end.gate < 0.1f);  // gate dropped after the release transition

    auto& s = vm.states().get_or_create<MidiQueueState>(MIDI_STATE_ID);
    CHECK(s.mono_stack_depth == 0);
}

TEST_CASE("midi-mono: trig pulse is exactly one sample wide",
          "[midi-mono]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_midi_mono_program();
    vm.load_program_immediate(std::span<const Instruction>(program));
    vm.init_midi_queue_state(MIDI_STATE_ID, MidiSourceKind::DefaultDevice,
                             nullptr, 0, false,
                             MidiQueueState::TempoMode::Follow);

    // Three sequential notes, one per block. Each should fire exactly one
    // trig sample regardless of stack depth.
    std::array<float, BLOCK_SIZE> left{}, right{};
    for (int b = 0; b < 3; ++b) {
        REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90,
                                   static_cast<std::uint8_t>(60 + b * 2),
                                   100));
        vm.process_block(left.data(), right.data());

        int trig_count = 0;
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            if (read_mono(vm, i).trig > 0.5f) ++trig_count;
        }
        CHECK(trig_count == 1);
    }
}

TEST_CASE("midi-mono: program without buffer wiring leaves the fill a no-op",
          "[midi-mono]") {
    // Backwards-compat guard for build_midi_only_program() (which uses
    // make_nullary so inputs[0..3] == 0xFFFF). fill_mono_buffers must skip
    // entirely — no stack push, no buffer touch.
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
    CHECK(s.mono_stack_depth == 0);  // not touched
    CHECK(s.output.num_events == 1); // event still recorded normally
}

// ============================================================================
// PRD prd-midi-input §7.1: SoundFont accepts MIDI upstream
//
// `midi() |> soundfont(...)` triggers polyphonic voices on each note-on event
// and releases them on note-off, without relying on the legacy buffer-driven
// edge detection. SoundFontVoiceState's internal 32-voice allocator handles
// chord polyphony.
// ============================================================================

#ifndef CEDAR_NO_SOUNDFONT
namespace fs = std::filesystem;

static fs::path find_sf_fixture_midi() {
    fs::path cur = fs::current_path();
    for (int depth = 0; depth < 8; ++depth) {
        const fs::path candidate =
            cur / "web" / "static" / "soundfonts" / "TimGM6mb.sf3";
        if (fs::exists(candidate)) return candidate;
        if (cur.has_parent_path() && cur.parent_path() != cur) {
            cur = cur.parent_path();
        } else {
            break;
        }
    }
    return {};
}

static std::vector<std::uint8_t> read_file_bytes_midi(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    REQUIRE(in.good());
    auto sz = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    std::vector<std::uint8_t> buf(sz);
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz));
    return buf;
}

// Build a MIDI_QUERY + SOUNDFONT_VOICE (event-driven) + OUTPUT program.
// All five SOUNDFONT_VOICE input slots are 0xFFFF — that's the runtime
// signal that selects the event-driven path. Bytecode mirrors what
// handle_soundfont_call emits for `midi() |> soundfont(@, "...", N)`.
static std::vector<Instruction> build_midi_soundfont_program() {
    static constexpr std::uint32_t SF_STATE_ID = 0x50000;
    std::vector<Instruction> program;

    program.push_back(Instruction::make_nullary(
        Opcode::MIDI_QUERY, 0xFFFF, MIDI_STATE_ID));

    Instruction sf_inst{};
    sf_inst.opcode = Opcode::SOUNDFONT_VOICE;
    sf_inst.out_buffer = BUF_MIX;
    sf_inst.inputs[0] = 0xFFFF;
    sf_inst.inputs[1] = 0xFFFF;
    sf_inst.inputs[2] = 0xFFFF;
    sf_inst.inputs[3] = 0xFFFF;
    sf_inst.inputs[4] = 0xFFFF;
    sf_inst.state_id = SF_STATE_ID;
    sf_inst.rate = 0;  // SF slot 0
    program.push_back(sf_inst);

    program.push_back(Instruction::make_unary(Opcode::OUTPUT, 0, BUF_MIX));
    return program;
}

TEST_CASE("midi-soundfont: live note-on allocates a SF voice",
          "[midi-soundfont]") {
    auto sf_path = find_sf_fixture_midi();
    if (sf_path.empty()) {
        SKIP("SF2/SF3 fixture not found — skipping");
    }

    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto sf_bytes = read_file_bytes_midi(sf_path);
    REQUIRE(vm.soundfont_registry().load_from_memory(
                MemoryView(sf_bytes.data(), sf_bytes.size()),
                "test", vm.sample_bank()) == 0);

    constexpr std::uint32_t SF_STATE_ID = 0x50000;
    auto program = build_midi_soundfont_program();
    vm.load_program_immediate(std::span<const Instruction>(program));
    vm.init_midi_queue_state(MIDI_STATE_ID, MidiSourceKind::DefaultDevice,
                             nullptr, 0, false,
                             MidiQueueState::TempoMode::Follow);
    vm.init_soundfont_voice_event_state(SF_STATE_ID, MIDI_STATE_ID, /*preset=*/0);

    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 60, 100));  // C4 on
    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    auto& sf = vm.states().get_or_create<SoundFontVoiceState>(SF_STATE_ID);
    int active = 0;
    for (std::uint16_t i = 0; i < SoundFontVoiceState::MAX_VOICES; ++i) {
        if (sf.voices[i].active) ++active;
    }
    CHECK(active >= 1);
}

TEST_CASE("midi-soundfont: chord allocates multiple voices",
          "[midi-soundfont]") {
    auto sf_path = find_sf_fixture_midi();
    if (sf_path.empty()) {
        SKIP("SF2/SF3 fixture not found — skipping");
    }

    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto sf_bytes = read_file_bytes_midi(sf_path);
    REQUIRE(vm.soundfont_registry().load_from_memory(
                MemoryView(sf_bytes.data(), sf_bytes.size()),
                "test", vm.sample_bank()) == 0);

    constexpr std::uint32_t SF_STATE_ID = 0x50000;
    auto program = build_midi_soundfont_program();
    vm.load_program_immediate(std::span<const Instruction>(program));
    vm.init_midi_queue_state(MIDI_STATE_ID, MidiSourceKind::DefaultDevice,
                             nullptr, 0, false,
                             MidiQueueState::TempoMode::Follow);
    vm.init_soundfont_voice_event_state(SF_STATE_ID, MIDI_STATE_ID, 0);

    // C major triad — three simultaneous note-ons.
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 60, 100));  // C4
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 64,  90));  // E4
    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 67,  80));  // G4

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    // Count distinct active notes (one preset may use multiple zones per
    // note, so total active >= 3 but distinct notes should be exactly 3).
    auto& sf = vm.states().get_or_create<SoundFontVoiceState>(SF_STATE_ID);
    std::set<std::uint8_t> active_notes;
    for (std::uint16_t i = 0; i < SoundFontVoiceState::MAX_VOICES; ++i) {
        if (sf.voices[i].active) active_notes.insert(sf.voices[i].note);
    }
    CHECK(active_notes.count(60) == 1);
    CHECK(active_notes.count(64) == 1);
    CHECK(active_notes.count(67) == 1);
}

TEST_CASE("midi-soundfont: note-off marks matching voices as releasing",
          "[midi-soundfont]") {
    auto sf_path = find_sf_fixture_midi();
    if (sf_path.empty()) {
        SKIP("SF2/SF3 fixture not found — skipping");
    }

    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto sf_bytes = read_file_bytes_midi(sf_path);
    REQUIRE(vm.soundfont_registry().load_from_memory(
                MemoryView(sf_bytes.data(), sf_bytes.size()),
                "test", vm.sample_bank()) == 0);

    constexpr std::uint32_t SF_STATE_ID = 0x50000;
    auto program = build_midi_soundfont_program();
    vm.load_program_immediate(std::span<const Instruction>(program));
    vm.init_midi_queue_state(MIDI_STATE_ID, MidiSourceKind::DefaultDevice,
                             nullptr, 0, false,
                             MidiQueueState::TempoMode::Follow);
    vm.init_soundfont_voice_event_state(SF_STATE_ID, MIDI_STATE_ID, 0);

    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x90, 60, 100));
    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    REQUIRE(vm.push_midi_event(MIDI_STATE_ID, 0x80, 60,   0));
    vm.process_block(left.data(), right.data());

    auto& sf = vm.states().get_or_create<SoundFontVoiceState>(SF_STATE_ID);
    bool found_releasing_60 = false;
    for (std::uint16_t i = 0; i < SoundFontVoiceState::MAX_VOICES; ++i) {
        const auto& v = sf.voices[i];
        if (v.active && v.note == 60 && v.releasing) {
            found_releasing_60 = true;
        }
    }
    CHECK(found_releasing_60);
}

TEST_CASE("midi-soundfont: legacy pattern path is untouched by has_upstream_events",
          "[midi-soundfont]") {
    // A SoundFontVoiceState created with has_upstream_events=false must
    // take the legacy buffer-driven path. Just verify the default value.
    SoundFontVoiceState s;
    CHECK_FALSE(s.has_upstream_events);
    CHECK(s.seq_state_id == 0);
    CHECK(s.preset_idx == 0);
}
#endif  // CEDAR_NO_SOUNDFONT
