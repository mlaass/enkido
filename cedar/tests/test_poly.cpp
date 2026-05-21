// Tests for POLY infrastructure
// Phase 1: basic execution, XOR state isolation, regression safety
// Phase 3: voice allocation from pattern events

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "cedar/vm/vm.hpp"
#include "cedar/vm/instruction.hpp"
#include "cedar/opcodes/utility.hpp"
#include <array>
#include <cmath>
#include <numeric>
#include <vector>

using namespace cedar;
using Catch::Matchers::WithinAbs;

// ============================================================================
// Helper: Build a POLY program with OSC_SIN body
// ============================================================================

// Buffer layout for poly tests. The per-voice field bank is 11 contiguous
// buffers indexed by PatternPayload field id; voice_out and mix are adjacent
// stereo L/R pairs (R = L+1). POLY_BEGIN carries inputs[0]=bank base,
// inputs[4]=voice_out L, and the VM fills every bank buffer per voice.
//   0..10 = field bank: freq,vel,trig,gate,type,note,dur,chance,time,
//           phase,sample_id
//   11 = voice output L   12 = voice output R (= 11 + 1)
//   13 = mix output L     14 = mix output R (= 13 + 1)

static constexpr std::uint16_t BUF_BANK = 0;
static constexpr std::uint16_t BUF_FREQ = 0;   // bank + FREQ
static constexpr std::uint16_t BUF_VEL  = 1;   // bank + VEL
static constexpr std::uint16_t BUF_TRIG = 2;   // bank + TRIG
static constexpr std::uint16_t BUF_GATE = 3;   // bank + GATE
static constexpr std::uint16_t BUF_TYPE      = 4;
static constexpr std::uint16_t BUF_NOTE      = 5;
static constexpr std::uint16_t BUF_DUR       = 6;
static constexpr std::uint16_t BUF_CHANCE    = 7;
static constexpr std::uint16_t BUF_TIME      = 8;
static constexpr std::uint16_t BUF_PHASE     = 9;
static constexpr std::uint16_t BUF_SAMPLE_ID = 10;
static constexpr std::uint16_t BUF_VOICE_OUT   = 11;
static constexpr std::uint16_t BUF_VOICE_OUT_R = 12;
static constexpr std::uint16_t BUF_MIX   = 13;
static constexpr std::uint16_t BUF_MIX_R = 14;

static constexpr std::uint32_t POLY_STATE_ID = 0x10000;
static constexpr std::uint32_t OSC_STATE_ID  = 0x20000;
static constexpr std::uint32_t SEQ_STATE_ID  = 0x30000;

TEST_CASE("POLY basic execution with active voices", "[poly]") {
    VM vm;
    vm.set_sample_rate(48000.0f);

    // Build program: POLY_BEGIN (body=1: OSC_SIN) + POLY_END + OUTPUT
    // Body: OSC_SIN reads freq from BUF_FREQ, writes to BUF_VOICE_OUT
    std::vector<Instruction> program;

    // POLY_BEGIN: rate=2 (body length), out=BUF_MIX (L; R derived as +1),
    //   in0=BUF_FREQ, in1=BUF_GATE, in2=BUF_VEL, in3=BUF_TRIG, in4=BUF_VOICE_OUT
    auto poly_begin = Instruction::make_quinary(
        Opcode::POLY_BEGIN, BUF_MIX,
        BUF_BANK, 0xFFFF, 0xFFFF, 0xFFFF, BUF_VOICE_OUT,
        POLY_STATE_ID);
    poly_begin.flags = InstructionFlag::STEREO_OUTPUT;
    poly_begin.rate = 2; // body length = 2 instructions
    program.push_back(poly_begin);

    // Body: OSC_SIN(freq=BUF_FREQ) -> BUF_VOICE_OUT, then broadcast mono into R
    program.push_back(Instruction::make_unary(
        Opcode::OSC_SIN, BUF_VOICE_OUT, BUF_FREQ, OSC_STATE_ID));
    program.push_back(Instruction::make_unary(
        Opcode::COPY, BUF_VOICE_OUT_R, BUF_VOICE_OUT));

    // POLY_END
    program.push_back(Instruction::make_nullary(Opcode::POLY_END, 0));

    // OUTPUT: mix -> stereo out
    program.push_back(Instruction::make_unary(Opcode::OUTPUT, 0, BUF_MIX));

    vm.load_program_immediate(std::span<const Instruction>(program));

    // Initialize poly state with 2 active voices
    vm.init_poly_state(POLY_STATE_ID, 0, 8, 0, 0);

    // Manually activate 2 voices
    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    poly.voices[0].active = true;
    poly.voices[0].freq = 440.0f;
    poly.voices[0].gate = 1.0f;
    poly.voices[0].vel = 1.0f;

    poly.voices[1].active = true;
    poly.voices[1].freq = 880.0f;
    poly.voices[1].gate = 1.0f;
    poly.voices[1].vel = 1.0f;

    // Process a block
    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    // The mix buffer should be non-silent (sum of two sines)
    const float* mix = vm.buffers().get(BUF_MIX);
    float max_abs = 0.0f;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        max_abs = std::max(max_abs, std::abs(mix[i]));
    }
    CHECK(max_abs > 0.1f);  // Sum of two sine oscillators should be audible

    // Output should also be non-silent (OUTPUT copies mix to left/right)
    float out_max = 0.0f;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out_max = std::max(out_max, std::abs(left[i]));
    }
    CHECK(out_max > 0.1f);
}

TEST_CASE("POLY XOR state isolation", "[poly]") {
    VM vm;
    vm.set_sample_rate(48000.0f);

    // Same program as above
    std::vector<Instruction> program;

    auto poly_begin = Instruction::make_quinary(
        Opcode::POLY_BEGIN, BUF_MIX,
        BUF_BANK, 0xFFFF, 0xFFFF, 0xFFFF, BUF_VOICE_OUT,
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

    vm.load_program_immediate(std::span<const Instruction>(program));
    vm.init_poly_state(POLY_STATE_ID, 0, 8, 0, 0);

    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    poly.voices[0].active = true;
    poly.voices[0].freq = 440.0f;
    poly.voices[0].gate = 1.0f;
    poly.voices[0].vel = 1.0f;

    poly.voices[1].active = true;
    poly.voices[1].freq = 880.0f;
    poly.voices[1].gate = 1.0f;
    poly.voices[1].vel = 1.0f;

    // Process 10 blocks so oscillators accumulate independent phase
    std::array<float, BLOCK_SIZE> left{}, right{};
    for (int b = 0; b < 10; ++b) {
        vm.process_block(left.data(), right.data());
    }

    // Now check: if isolation works, the mix should NOT be 2*sin(440)
    // because voice 1 is at 880Hz. Compute expected 2*sin(440) signal
    // and verify the actual mix differs from it.
    const float* mix = vm.buffers().get(BUF_MIX);

    // The mix is sum of sin(440) + sin(880). If XOR isolation failed,
    // both voices would share the same OscState, and since voice 1 overwrites
    // freq with 880Hz, the result would be 2*sin(880).
    // With proper isolation, we get sin(440) + sin(880).

    // Check the output is not simply a scaled single-frequency sine.
    // A single-frequency signal has a constant ratio between consecutive samples.
    // A sum of two frequencies does not.
    // We check that the signal has frequency content at both 440 and 880
    // by verifying the absolute values vary in a non-uniform way.
    float sum = 0.0f;
    float sum_sq = 0.0f;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        sum += mix[i];
        sum_sq += mix[i] * mix[i];
    }
    float rms = std::sqrt(sum_sq / BLOCK_SIZE);
    CHECK(rms > 0.1f);  // Should have meaningful energy

    // Additional check: with two different frequencies, the signal should
    // cross zero more often than a pure 440Hz sine.
    int zero_crossings = 0;
    for (std::size_t i = 1; i < BLOCK_SIZE; ++i) {
        if ((mix[i] >= 0.0f) != (mix[i - 1] >= 0.0f)) {
            ++zero_crossings;
        }
    }
    // A pure 440Hz sine at 48kHz has ~128*440/48000 ≈ 1.17 cycles per block
    // → about 2 zero crossings. With 440+880, we expect more.
    CHECK(zero_crossings > 3);
}

TEST_CASE("POLY regression: non-POLY programs still work", "[poly]") {
    VM vm;
    vm.set_sample_rate(48000.0f);

    // Simple program: OSC_SIN(440) -> OUTPUT (no POLY block)
    auto const_inst = make_const_instruction(Opcode::PUSH_CONST, 0, 440.0f);
    auto osc_inst = Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 0x1234);
    auto out_inst = Instruction::make_unary(Opcode::OUTPUT, 0, 1);

    std::array<Instruction, 3> program = {const_inst, osc_inst, out_inst};
    vm.load_program_immediate(std::span<const Instruction>(program));

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    // Output should be a sine wave
    float max_abs = 0.0f;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        max_abs = std::max(max_abs, std::abs(left[i]));
    }
    CHECK(max_abs > 0.01f);  // Should have audio output
    CHECK(max_abs <= 1.0f);   // Sine wave shouldn't exceed amplitude 1
}

TEST_CASE("POLY empty block produces silence", "[poly]") {
    VM vm;
    vm.set_sample_rate(48000.0f);

    // Program: POLY_BEGIN (body=1) + OSC_SIN + POLY_END + OUTPUT
    // But no voices are active, so mix should be silence
    std::vector<Instruction> program;

    auto poly_begin = Instruction::make_quinary(
        Opcode::POLY_BEGIN, BUF_MIX,
        BUF_BANK, 0xFFFF, 0xFFFF, 0xFFFF, BUF_VOICE_OUT,
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

    vm.load_program_immediate(std::span<const Instruction>(program));
    vm.init_poly_state(POLY_STATE_ID, 0, 8, 0, 0);

    // No voices activated — all default to active=false

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    // Both mix channels should be all zeros
    const float* mix = vm.buffers().get(BUF_MIX);
    const float* mix_r = vm.buffers().get(BUF_MIX_R);
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        CHECK(mix[i] == 0.0f);
        CHECK(mix_r[i] == 0.0f);
    }

    // Output should also be silence
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        CHECK(left[i] == 0.0f);
        CHECK(right[i] == 0.0f);
    }
}

// ============================================================================
// Stereo: poly is stereo-native — voice_out and mix are L/R pairs (R = L+1)
// ============================================================================

TEST_CASE("POLY stereo body: distinct L/R channels", "[poly][stereo]") {
    VM vm;
    vm.set_sample_rate(48000.0f);

    // Body writes an oscillator into voice_out L and a constant into voice_out
    // R, so the two mix channels must differ.
    std::vector<Instruction> program;

    auto poly_begin = Instruction::make_quinary(
        Opcode::POLY_BEGIN, BUF_MIX,
        BUF_BANK, 0xFFFF, 0xFFFF, 0xFFFF, BUF_VOICE_OUT,
        POLY_STATE_ID);
    poly_begin.flags = InstructionFlag::STEREO_OUTPUT;
    poly_begin.rate = 2;
    program.push_back(poly_begin);

    program.push_back(Instruction::make_unary(
        Opcode::OSC_SIN, BUF_VOICE_OUT, BUF_FREQ, OSC_STATE_ID));
    program.push_back(make_const_instruction(
        Opcode::PUSH_CONST, BUF_VOICE_OUT_R, 0.3f));

    program.push_back(Instruction::make_nullary(Opcode::POLY_END, 0));

    vm.load_program_immediate(std::span<const Instruction>(program));
    vm.init_poly_state(POLY_STATE_ID, 0, 8, 0, 0);

    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    poly.voices[0].active = true;
    poly.voices[0].freq = 440.0f;
    poly.voices[0].gate = 1.0f;
    poly.voices[0].vel = 1.0f;

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    const float* mix = vm.buffers().get(BUF_MIX);
    const float* mix_r = vm.buffers().get(BUF_MIX_R);

    bool channels_differ = false;
    float l_max = 0.0f, r_max = 0.0f;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        if (std::abs(mix[i] - mix_r[i]) > 1e-4f) channels_differ = true;
        l_max = std::max(l_max, std::abs(mix[i]));
        r_max = std::max(r_max, std::abs(mix_r[i]));
    }
    CHECK(channels_differ);
    CHECK(l_max > 0.1f);
    CHECK(r_max > 0.1f);
    // R is a constant 0.3 (one voice, gate 1).
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        CHECK_THAT(mix_r[i], WithinAbs(0.3f, 1e-4f));
    }
}

TEST_CASE("POLY stereo: multi-voice sums each channel independently",
          "[poly][stereo]") {
    VM vm;
    vm.set_sample_rate(48000.0f);

    // Body copies the per-voice vel into L and the per-voice freq into R, so
    // each mix channel is the independent sum of a distinct per-voice value.
    std::vector<Instruction> program;

    auto poly_begin = Instruction::make_quinary(
        Opcode::POLY_BEGIN, BUF_MIX,
        BUF_BANK, 0xFFFF, 0xFFFF, 0xFFFF, BUF_VOICE_OUT,
        POLY_STATE_ID);
    poly_begin.flags = InstructionFlag::STEREO_OUTPUT;
    poly_begin.rate = 2;
    program.push_back(poly_begin);

    program.push_back(Instruction::make_unary(
        Opcode::COPY, BUF_VOICE_OUT, BUF_VEL));
    program.push_back(Instruction::make_unary(
        Opcode::COPY, BUF_VOICE_OUT_R, BUF_FREQ));

    program.push_back(Instruction::make_nullary(Opcode::POLY_END, 0));

    vm.load_program_immediate(std::span<const Instruction>(program));
    vm.init_poly_state(POLY_STATE_ID, 0, 8, 0, 0);

    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    poly.voices[0].active = true;
    poly.voices[0].freq = 440.0f;
    poly.voices[0].gate = 1.0f;
    poly.voices[0].vel = 0.5f;

    poly.voices[1].active = true;
    poly.voices[1].freq = 880.0f;
    poly.voices[1].gate = 1.0f;
    poly.voices[1].vel = 0.25f;

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    const float* mix = vm.buffers().get(BUF_MIX);
    const float* mix_r = vm.buffers().get(BUF_MIX_R);

    // L = sum of vels = 0.75; R = sum of freqs = 1320. gate is 1 for both.
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        CHECK_THAT(mix[i], WithinAbs(0.75f, 1e-3f));
        CHECK_THAT(mix_r[i], WithinAbs(1320.0f, 0.1f));
    }
}

TEST_CASE("POLY mono-equivalent body broadcasts into both channels",
          "[poly][stereo]") {
    VM vm;
    vm.set_sample_rate(48000.0f);

    // Body writes identical content to both voice-out slots — the dual-mono
    // broadcast real codegen emits for a mono voice body.
    std::vector<Instruction> program;

    auto poly_begin = Instruction::make_quinary(
        Opcode::POLY_BEGIN, BUF_MIX,
        BUF_BANK, 0xFFFF, 0xFFFF, 0xFFFF, BUF_VOICE_OUT,
        POLY_STATE_ID);
    poly_begin.flags = InstructionFlag::STEREO_OUTPUT;
    poly_begin.rate = 2;
    program.push_back(poly_begin);

    program.push_back(Instruction::make_unary(
        Opcode::OSC_SIN, BUF_VOICE_OUT, BUF_FREQ, OSC_STATE_ID));
    program.push_back(Instruction::make_unary(
        Opcode::COPY, BUF_VOICE_OUT_R, BUF_VOICE_OUT));

    program.push_back(Instruction::make_nullary(Opcode::POLY_END, 0));

    vm.load_program_immediate(std::span<const Instruction>(program));
    vm.init_poly_state(POLY_STATE_ID, 0, 8, 0, 0);

    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    poly.voices[0].active = true;
    poly.voices[0].freq = 440.0f;
    poly.voices[0].gate = 1.0f;
    poly.voices[0].vel = 1.0f;

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    const float* mix = vm.buffers().get(BUF_MIX);
    const float* mix_r = vm.buffers().get(BUF_MIX_R);

    float max_abs = 0.0f;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        CHECK(mix[i] == mix_r[i]);  // dual-mono: L and R identical
        max_abs = std::max(max_abs, std::abs(mix[i]));
    }
    CHECK(max_abs > 0.1f);
}

// ============================================================================
// Phase 3: Voice Allocation from Pattern Events
// ============================================================================

// Helper: build a SEQPAT_QUERY + POLY program with OSC_SIN body
static std::vector<Instruction> build_seq_poly_program() {
    std::vector<Instruction> program;

    // SEQPAT_QUERY: queries sequence at block boundaries, fills OutputEvents
    program.push_back(Instruction::make_nullary(
        Opcode::SEQPAT_QUERY, 0, SEQ_STATE_ID));

    // POLY_BEGIN: rate=2 (body length), out=BUF_MIX (L; R derived as +1)
    auto poly_begin = Instruction::make_quinary(
        Opcode::POLY_BEGIN, BUF_MIX,
        BUF_BANK, 0xFFFF, 0xFFFF, 0xFFFF, BUF_VOICE_OUT,
        POLY_STATE_ID);
    poly_begin.flags = InstructionFlag::STEREO_OUTPUT;
    poly_begin.rate = 2;
    program.push_back(poly_begin);

    // Body: OSC_SIN(freq=BUF_FREQ) -> BUF_VOICE_OUT, then broadcast mono into R
    program.push_back(Instruction::make_unary(
        Opcode::OSC_SIN, BUF_VOICE_OUT, BUF_FREQ, OSC_STATE_ID));
    program.push_back(Instruction::make_unary(
        Opcode::COPY, BUF_VOICE_OUT_R, BUF_VOICE_OUT));

    // POLY_END
    program.push_back(Instruction::make_nullary(Opcode::POLY_END, 0));

    // OUTPUT: mix -> stereo out
    program.push_back(Instruction::make_unary(Opcode::OUTPUT, 0, BUF_MIX));

    return program;
}

// Helper: set up a single-event sequence (one note)
// duration_beats: how long the note lasts in beats (output units)
// The sequence system scales: output_duration = e.duration * cycle_length
// So e.duration = duration_beats / cycle_length
static void setup_single_note_sequence(VM& vm, float freq,
                                       float duration_beats = 4.0f,
                                       float velocity = 1.0f) {
    static constexpr float CYCLE_LENGTH = 4.0f;

    Event evt;
    evt.type = EventType::DATA;
    evt.time = 0.0f;
    evt.duration = duration_beats / CYCLE_LENGTH; // fraction of cycle
    evt.chance = 1.0f;
    evt.velocity = velocity;
    evt.num_values = 1;
    evt.values[0] = freq;

    Sequence seq;
    seq.events = &evt;
    seq.num_events = 1;
    seq.capacity = 1;
    seq.duration = CYCLE_LENGTH;
    seq.mode = SequenceMode::NORMAL;

    vm.init_sequence_program_state(SEQ_STATE_ID, &seq, 1, CYCLE_LENGTH, false, 1);
}

// Helper: set up a chord sequence (multi-value event)
static void setup_chord_sequence(VM& vm, const float* freqs,
                                 std::uint8_t num_notes,
                                 float duration_beats = 4.0f) {
    static constexpr float CYCLE_LENGTH = 4.0f;

    Event evt;
    evt.type = EventType::DATA;
    evt.time = 0.0f;
    evt.duration = duration_beats / CYCLE_LENGTH;
    evt.chance = 1.0f;
    evt.velocity = 1.0f;
    evt.num_values = num_notes;
    for (std::uint8_t i = 0; i < num_notes; ++i) {
        evt.values[i] = freqs[i];
    }

    Sequence seq;
    seq.events = &evt;
    seq.num_events = 1;
    seq.capacity = 1;
    seq.duration = CYCLE_LENGTH;
    seq.mode = SequenceMode::NORMAL;

    vm.init_sequence_program_state(SEQ_STATE_ID, &seq, 1, CYCLE_LENGTH, false, 1);
}

TEST_CASE("POLY voice allocation: single note produces audio", "[poly][alloc]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_seq_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    // Set up sequence with one C4 note (261.63 Hz) spanning the full cycle
    setup_single_note_sequence(vm, 261.63f);

    // Init poly state linked to sequence
    vm.init_poly_state(POLY_STATE_ID, SEQ_STATE_ID, 8, 0, 0);

    // Process several blocks
    std::array<float, BLOCK_SIZE> left{}, right{};
    for (int b = 0; b < 5; ++b) {
        vm.process_block(left.data(), right.data());
    }

    // After processing, at least one voice should be active
    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    CHECK(poly.active_voice_count() >= 1);

    // Output should be non-silent
    float max_abs = 0.0f;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        max_abs = std::max(max_abs, std::abs(left[i]));
    }
    CHECK(max_abs > 0.01f);
}

TEST_CASE("POLY voice allocation: chord produces multiple voices", "[poly][alloc]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_seq_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    // C major chord: C4, E4, G4
    float chord_freqs[] = {261.63f, 329.63f, 392.00f};
    setup_chord_sequence(vm, chord_freqs, 3);

    vm.init_poly_state(POLY_STATE_ID, SEQ_STATE_ID, 8, 0, 0);

    std::array<float, BLOCK_SIZE> left{}, right{};
    for (int b = 0; b < 5; ++b) {
        vm.process_block(left.data(), right.data());
    }

    // Should have 3 active voices
    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    CHECK(poly.active_voice_count() == 3);

    // All three voices should have the chord frequencies
    bool found_c4 = false, found_e4 = false, found_g4 = false;
    for (std::uint8_t i = 0; i < poly.max_voices; ++i) {
        if (!poly.voices[i].active) continue;
        float f = poly.voices[i].freq;
        if (std::abs(f - 261.63f) < 0.1f) found_c4 = true;
        if (std::abs(f - 329.63f) < 0.1f) found_e4 = true;
        if (std::abs(f - 392.00f) < 0.1f) found_g4 = true;
    }
    CHECK(found_c4);
    CHECK(found_e4);
    CHECK(found_g4);

    // Output should be non-silent (sum of 3 oscillators)
    const float* mix = vm.buffers().get(BUF_MIX);
    float rms = 0.0f;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        rms += mix[i] * mix[i];
    }
    rms = std::sqrt(rms / BLOCK_SIZE);
    CHECK(rms > 0.1f);
}

// ============================================================================
// Phase 2: per-voice event-record field bank
// ============================================================================

TEST_CASE("POLY field bank: single note exposes event fields per voice",
          "[poly][fields]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_seq_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    // One C4 note spanning 2 beats, with distinct type_id / midi_note.
    static constexpr float CYCLE_LENGTH = 4.0f;
    Event evt;
    evt.type = EventType::DATA;
    evt.time = 0.0f;
    evt.duration = 2.0f / CYCLE_LENGTH;   // output duration = 2 beats
    evt.chance = 1.0f;                    // 1.0 so the event always plays
    evt.velocity = 0.6f;
    evt.midi_note = 60.0f;                // C4
    evt.type_id = 7;
    evt.num_values = 1;
    evt.values[0] = 261.63f;

    Sequence seq;
    seq.events = &evt;
    seq.num_events = 1;
    seq.capacity = 1;
    seq.duration = CYCLE_LENGTH;
    seq.mode = SequenceMode::NORMAL;
    vm.init_sequence_program_state(SEQ_STATE_ID, &seq, 1, CYCLE_LENGTH, false, 1);

    vm.init_poly_state(POLY_STATE_ID, SEQ_STATE_ID, 8, 0, 0);

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    REQUIRE(poly.active_voice_count() == 1);

    // With one active voice the bank buffers hold that voice's fields.
    CHECK(std::abs(vm.buffers().get(BUF_FREQ)[0] - 261.63f) < 0.01f);
    CHECK(std::abs(vm.buffers().get(BUF_VEL)[0] - 0.6f) < 0.001f);
    CHECK(std::abs(vm.buffers().get(BUF_TYPE)[0] - 7.0f) < 0.001f);
    CHECK(std::abs(vm.buffers().get(BUF_NOTE)[0] - 60.0f) < 0.001f);
    CHECK(std::abs(vm.buffers().get(BUF_DUR)[0] - 2.0f) < 0.01f);
    CHECK(std::abs(vm.buffers().get(BUF_CHANCE)[0] - 1.0f) < 0.001f);
    CHECK(std::abs(vm.buffers().get(BUF_TIME)[0] - 0.0f) < 0.001f);
    // SAMPLE_ID shares the primary value with FREQ.
    CHECK(std::abs(vm.buffers().get(BUF_SAMPLE_ID)[0] - 261.63f) < 0.01f);

    // PHASE is block-constant, in [0,1], and advances as the voice ages.
    const float phase0 = vm.buffers().get(BUF_PHASE)[0];
    CHECK(phase0 >= 0.0f);
    CHECK(phase0 <= 1.0f);
    for (int b = 0; b < 30; ++b) {
        vm.process_block(left.data(), right.data());
    }
    const float phase1 = vm.buffers().get(BUF_PHASE)[0];
    CHECK(phase1 > phase0);
    CHECK(phase1 <= 1.0f);
}

TEST_CASE("POLY field bank: chord exposes per-voice notes", "[poly][fields]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_seq_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    // C major chord — each voice has its own MIDI note in notes[].
    static constexpr float CYCLE_LENGTH = 4.0f;
    Event evt;
    evt.type = EventType::DATA;
    evt.time = 0.0f;
    evt.duration = 4.0f / CYCLE_LENGTH;
    evt.chance = 1.0f;
    evt.velocity = 1.0f;
    evt.midi_note = 60.0f;                 // root
    evt.num_values = 3;
    evt.values[0] = 261.63f; evt.notes[0] = 60.0f;  // C4
    evt.values[1] = 329.63f; evt.notes[1] = 64.0f;  // E4
    evt.values[2] = 392.00f; evt.notes[2] = 67.0f;  // G4

    Sequence seq;
    seq.events = &evt;
    seq.num_events = 1;
    seq.capacity = 1;
    seq.duration = CYCLE_LENGTH;
    seq.mode = SequenceMode::NORMAL;
    vm.init_sequence_program_state(SEQ_STATE_ID, &seq, 1, CYCLE_LENGTH, false, 1);

    vm.init_poly_state(POLY_STATE_ID, SEQ_STATE_ID, 8, 0, 0);

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    REQUIRE(poly.active_voice_count() == 3);

    // Each voice's note_value is its own chord note, not the root.
    bool saw_c = false, saw_e = false, saw_g = false;
    for (std::uint8_t i = 0; i < poly.max_voices; ++i) {
        if (!poly.voices[i].active) continue;
        const float f = poly.voices[i].freq;
        const float nv = poly.voices[i].note_value;
        if (std::abs(f - 261.63f) < 0.1f) { CHECK(std::abs(nv - 60.0f) < 0.01f); saw_c = true; }
        if (std::abs(f - 329.63f) < 0.1f) { CHECK(std::abs(nv - 64.0f) < 0.01f); saw_e = true; }
        if (std::abs(f - 392.00f) < 0.1f) { CHECK(std::abs(nv - 67.0f) < 0.01f); saw_g = true; }
    }
    CHECK(saw_c);
    CHECK(saw_e);
    CHECK(saw_g);
}

TEST_CASE("POLY voice allocation: voice release on event end", "[poly][alloc]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_seq_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    // Short note: 0.5 beats out of 4-beat cycle
    setup_single_note_sequence(vm, 440.0f, 0.5f);

    vm.init_poly_state(POLY_STATE_ID, SEQ_STATE_ID, 8, 0, 0);

    // At 120 BPM, 48kHz: samples_per_beat = 24000
    // 0.5 beats = 12000 samples = ~93.75 blocks of 128
    // Process blocks past the event end
    std::array<float, BLOCK_SIZE> left{}, right{};
    for (int b = 0; b < 100; ++b) {
        vm.process_block(left.data(), right.data());
    }

    // After the event ends and release timeout expires, the voice should be freed
    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    bool any_active = false;
    for (std::uint8_t i = 0; i < poly.max_voices; ++i) {
        if (poly.voices[i].active) {
            any_active = true;
        }
    }
    CHECK_FALSE(any_active);
}

TEST_CASE("POLY voice allocation: mono mode uses single voice", "[poly][alloc]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_seq_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    // Two sequential notes in a 4-beat cycle (each 2 beats)
    // e.duration is a fraction: output_duration = e.duration * cycle_length
    // For 2 beats out of 4: e.duration = 0.5
    // e.time is in beats (when seq.duration == cycle_length)
    Event events[2];
    events[0].type = EventType::DATA;
    events[0].time = 0.0f;
    events[0].duration = 0.5f;     // 0.5 * 4.0 = 2.0 beats
    events[0].chance = 1.0f;
    events[0].velocity = 1.0f;
    events[0].num_values = 1;
    events[0].values[0] = 261.63f;  // C4

    events[1].type = EventType::DATA;
    events[1].time = 2.0f;
    events[1].duration = 0.5f;     // 0.5 * 4.0 = 2.0 beats
    events[1].chance = 1.0f;
    events[1].velocity = 1.0f;
    events[1].num_values = 1;
    events[1].values[0] = 329.63f;  // E4

    Sequence seq;
    seq.events = events;
    seq.num_events = 2;
    seq.capacity = 2;
    seq.duration = 4.0f;
    seq.mode = SequenceMode::NORMAL;

    vm.init_sequence_program_state(SEQ_STATE_ID, &seq, 1, 4.0f, false, 2);

    // Mode 1 = mono
    vm.init_poly_state(POLY_STATE_ID, SEQ_STATE_ID, 8, 1, 0);

    // Process enough blocks to cover the first note region
    std::array<float, BLOCK_SIZE> left{}, right{};
    for (int b = 0; b < 10; ++b) {
        vm.process_block(left.data(), right.data());
    }

    // In mono mode, only voice 0 should ever be active
    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    CHECK(poly.active_voice_count() <= 1);
    // Verify only slot 0 is used
    for (std::uint8_t i = 1; i < poly.max_voices; ++i) {
        CHECK_FALSE(poly.voices[i].active);
    }
}

TEST_CASE("POLY voice allocation: voice stealing when full", "[poly][alloc]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_seq_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    // Create a 4-note chord — but limit to 2 voices
    float chord_freqs[] = {261.63f, 329.63f, 392.00f, 493.88f};
    setup_chord_sequence(vm, chord_freqs, 4);

    // Only 2 max voices — must steal
    vm.init_poly_state(POLY_STATE_ID, SEQ_STATE_ID, 2, 0, 0);

    std::array<float, BLOCK_SIZE> left{}, right{};
    for (int b = 0; b < 5; ++b) {
        vm.process_block(left.data(), right.data());
    }

    // Should have exactly 2 active voices (stolen from 4 chord notes)
    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    CHECK(poly.active_voice_count() == 2);

    // Output should be non-silent
    float max_abs = 0.0f;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        max_abs = std::max(max_abs, std::abs(left[i]));
    }
    CHECK(max_abs > 0.01f);
}

TEST_CASE("POLY voice allocation: velocity is passed to voices", "[poly][alloc]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_seq_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    // Note with specific velocity
    setup_single_note_sequence(vm, 440.0f, 4.0f, 0.5f);

    vm.init_poly_state(POLY_STATE_ID, SEQ_STATE_ID, 8, 0, 0);

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    // Check that the voice got the correct velocity
    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    bool found = false;
    for (std::uint8_t i = 0; i < poly.max_voices; ++i) {
        if (poly.voices[i].active) {
            CHECK_THAT(poly.voices[i].vel, WithinAbs(0.5f, 0.01f));
            found = true;
        }
    }
    CHECK(found);
}

// Helper: set up a 3-chord sequence that tiles the full cycle
// Used for cycle-boundary and voice-reuse tests
static void setup_three_chord_sequence(VM& vm) {
    static constexpr float CYCLE_LENGTH = 4.0f;

    // Three contiguous chord events, each 1/3 of cycle
    // Event 0: [A4, C5, E5]  Event 1: [A4, C5, B5]  Event 2: [A4, E5, D5]
    // Note: a4=440 is shared across all 3 events
    static Event events[3];

    events[0].type = EventType::DATA;
    events[0].time = 0.0f;
    events[0].duration = 1.0f / 3.0f;
    events[0].chance = 1.0f;
    events[0].velocity = 1.0f;
    events[0].num_values = 3;
    events[0].values[0] = 440.00f;   // A4
    events[0].values[1] = 523.25f;   // C5
    events[0].values[2] = 659.25f;   // E5

    events[1].type = EventType::DATA;
    events[1].time = 1.0f / 3.0f;
    events[1].duration = 1.0f / 3.0f;
    events[1].chance = 1.0f;
    events[1].velocity = 1.0f;
    events[1].num_values = 3;
    events[1].values[0] = 440.00f;   // A4 (shared)
    events[1].values[1] = 523.25f;   // C5 (shared)
    events[1].values[2] = 987.77f;   // B5

    events[2].type = EventType::DATA;
    events[2].time = 2.0f / 3.0f;
    events[2].duration = 1.0f / 3.0f;  // ends at 1.0 = cycle boundary
    events[2].chance = 1.0f;
    events[2].velocity = 1.0f;
    events[2].num_values = 3;
    events[2].values[0] = 440.00f;   // A4 (shared)
    events[2].values[1] = 659.25f;   // E5
    events[2].values[2] = 587.33f;   // D5

    Sequence seq;
    seq.events = events;
    seq.num_events = 3;
    seq.capacity = 3;
    seq.duration = CYCLE_LENGTH;
    seq.mode = SequenceMode::NORMAL;

    vm.init_sequence_program_state(SEQ_STATE_ID, &seq, 1, CYCLE_LENGTH, false, 3);
}

TEST_CASE("POLY cycle-boundary: no voice leak at aligned BPM", "[poly][boundary]") {
    // At 120 BPM, cycle_length_in_samples (96000) is exactly divisible by
    // BLOCK_SIZE (128), causing block_end_pos == cycle_length at the boundary.
    // The last event (evt_end == cycle_length) must be properly released.
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_seq_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    setup_three_chord_sequence(vm);
    vm.init_poly_state(POLY_STATE_ID, SEQ_STATE_ID, 12, 0, 0);

    // At 120 BPM, 1 cycle = 750 blocks. Run 3 full cycles.
    std::array<float, BLOCK_SIZE> left{}, right{};
    for (int b = 0; b < 750 * 3; ++b) {
        vm.process_block(left.data(), right.data());
    }

    // After 3 full cycles, voice count should NOT have grown beyond
    // the normal working set (3 active + some releasing)
    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    int active_count = 0;
    for (std::uint8_t i = 0; i < poly.max_voices; ++i) {
        if (poly.voices[i].active) active_count++;
    }
    // At a cycle boundary, we expect 3 active voices (current chord)
    // plus up to 3 releasing from the previous chord = max 6
    // Without the fix, leaked voices would accumulate to 12 (max_voices)
    CHECK(active_count <= 6);
}

TEST_CASE("POLY voice reuse: same frequency reuses voice slot", "[poly][reuse]") {
    // When a note (A4=440Hz) appears in consecutive events, the poly system
    // should reuse the same voice slot instead of allocating a new one.
    // This preserves oscillator phase continuity.
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_seq_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    setup_three_chord_sequence(vm);
    vm.init_poly_state(POLY_STATE_ID, SEQ_STATE_ID, 12, 0, 0);

    // Process enough blocks to get past the first event and into the second
    // Event 0 covers 0 to 1/3 cycle. At 120 BPM, 1/3 cycle = 250 blocks.
    std::array<float, BLOCK_SIZE> left{}, right{};
    for (int b = 0; b < 260; ++b) {
        vm.process_block(left.data(), right.data());
    }

    // A4 (440 Hz) should be on a voice that's still active (reused from Event 0)
    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    int a4_count = 0;
    for (std::uint8_t i = 0; i < poly.max_voices; ++i) {
        if (poly.voices[i].active && !poly.voices[i].releasing &&
            std::abs(poly.voices[i].freq - 440.0f) < 0.5f) {
            a4_count++;
        }
    }
    // With voice reuse, A4 should appear on exactly 1 voice
    // Without reuse, it could be on 2 (one releasing from Event 0, one from Event 1)
    CHECK(a4_count == 1);
}

TEST_CASE("POLY long-running stability: no voice accumulation over 10 cycles", "[poly][stability]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_seq_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    setup_three_chord_sequence(vm);
    vm.init_poly_state(POLY_STATE_ID, SEQ_STATE_ID, 12, 0, 0);

    // Run 10 full cycles (7500 blocks at 120 BPM)
    std::array<float, BLOCK_SIZE> left{}, right{};
    int max_active_seen = 0;
    for (int b = 0; b < 750 * 10; ++b) {
        vm.process_block(left.data(), right.data());
        if (b % 750 == 749) {
            // Check at end of each cycle
            auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
            int active = 0;
            for (std::uint8_t i = 0; i < poly.max_voices; ++i) {
                if (poly.voices[i].active) active++;
            }
            max_active_seen = std::max(max_active_seen, active);
        }
    }
    // Voice count should stay bounded (never exceed 6: 3 active + 3 releasing)
    CHECK(max_active_seen <= 6);
}

// ============================================================================
// Bug repro: chord("C Em Am G") |> poly(...) — incomplete chord onsets
// User report: every few bars, a chord plays with a missing note.
// Reproduces the user's "C Em Am G" loop with shared notes between adjacent
// chords and a release tail that overlaps the next chord onset.
// ============================================================================

// Set up 4-chord sequence (C, Em, Am, G), each 1 beat of a 4-beat cycle.
// Adjacent chords share notes (drives the freq-match reuse path):
//   C  = [C4, E4, G4]   shared with Em on E4 and G4
//   Em = [E4, G4, B4]   shared with Am on E4 (octave: see below)
//   Am = [A3, C4, E4]   shared with G on D4-via-G4? no — but C transition shares C4
//   G  = [G3, B3, D4]   shares B3 with Em? no — different octaves
// Use closer voicings to maximize reuse:
//   C  = [261.63 (C4), 329.63 (E4), 392.00 (G4)]
//   Em = [329.63 (E4), 392.00 (G4), 493.88 (B4)]
//   Am = [261.63 (C4), 329.63 (E4), 440.00 (A4)]
//   G  = [246.94 (B3), 392.00 (G4), 493.88 (B4)]
static void setup_c_em_am_g_sequence(VM& vm) {
    static constexpr float CYCLE_LENGTH = 4.0f;
    static Event events[4];

    auto fill = [&](Event& e, float t, float f0, float f1, float f2) {
        e.type = EventType::DATA;
        e.time = t;
        e.duration = 0.25f;  // 0.25 * 4.0 = 1 beat
        e.chance = 1.0f;
        e.velocity = 1.0f;
        e.num_values = 3;
        e.values[0] = f0;
        e.values[1] = f1;
        e.values[2] = f2;
    };

    fill(events[0], 0.0f, 261.63f, 329.63f, 392.00f);  // C
    fill(events[1], 1.0f, 329.63f, 392.00f, 493.88f);  // Em
    fill(events[2], 2.0f, 261.63f, 329.63f, 440.00f);  // Am
    fill(events[3], 3.0f, 246.94f, 392.00f, 493.88f);  // G

    Sequence seq;
    seq.events = events;
    seq.num_events = 4;
    seq.capacity = 4;
    seq.duration = CYCLE_LENGTH;
    seq.mode = SequenceMode::NORMAL;

    vm.init_sequence_program_state(SEQ_STATE_ID, &seq, 1, CYCLE_LENGTH, false, 4);
}

TEST_CASE("POLY chord completeness: every chord onset allocates all notes",
          "[poly][regression][chord]") {
    // Regression test: drive a 4-chord loop with shared notes between adjacent
    // chords. After every chord onset, verify that the expected number of
    // voices have a freq matching one of the chord's notes (within 0.5 Hz)
    // AND have gate > 0.5 OR a pending gate-on this block.
    //
    // The bug under test: in some cycles, a voice that was reused for a new
    // chord note still has the previous event's pending gate-off scheduled,
    // or the same-block reuse-then-release race leaves a chord note silent.
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_seq_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    setup_c_em_am_g_sequence(vm);

    // Use plenty of voices so we never hit the steal path
    vm.init_poly_state(POLY_STATE_ID, SEQ_STATE_ID, 21, 0, 0);

    // Chord frequency table for verification
    const std::array<std::array<float, 3>, 4> CHORDS = {{
        {261.63f, 329.63f, 392.00f},  // C
        {329.63f, 392.00f, 493.88f},  // Em
        {261.63f, 329.63f, 440.00f},  // Am
        {246.94f, 392.00f, 493.88f},  // G
    }};

    // 120 BPM, 48 kHz, BLOCK_SIZE 128 → 1 beat = 187.5 blocks.
    // 4 beats per cycle × 4 chord changes per cycle = chord onsets every ~187 blocks.
    // Run 16 cycles (= 64 chord onsets) — bug should manifest within this.
    constexpr int CYCLES = 16;
    constexpr int BLOCKS_PER_CYCLE = 750;  // 96000 samples / 128
    constexpr int TOTAL_BLOCKS = CYCLES * BLOCKS_PER_CYCLE;

    std::array<float, BLOCK_SIZE> left{}, right{};

    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);

    int incomplete_chord_count = 0;
    int chord_onsets_inspected = 0;

    int last_chord_idx = -1;

    for (int b = 0; b < TOTAL_BLOCKS; ++b) {
        // Determine which chord *should* be active at the start of this block
        // 1 beat = 187.5 blocks; chord_idx changes at boundaries
        const float block_beats = (b * BLOCK_SIZE) / (48000.0f * 60.0f / 120.0f);
        const float cycle_pos = std::fmod(block_beats, 4.0f);
        const int chord_idx = static_cast<int>(std::floor(cycle_pos));

        vm.process_block(left.data(), right.data());

        // Detect a chord change: this block contains the gate-on of a new chord
        if (chord_idx != last_chord_idx && last_chord_idx != -1) {
            // Inspect 2 blocks after the change to let voices settle
            // (allocation happens in the block where the event starts)
        }

        // Every block, just AFTER the chord onset settles (a few blocks in),
        // count active voices matching the current chord's notes.
        // Pick a sample point well inside the chord's beat: 50 blocks after onset.
        const int blocks_into_chord = static_cast<int>((cycle_pos - chord_idx) * 187.5f);
        if (blocks_into_chord == 50) {
            chord_onsets_inspected++;

            const auto& chord = CHORDS[chord_idx];
            int matched = 0;
            for (float target : chord) {
                bool found = false;
                for (std::uint8_t i = 0; i < poly.max_voices; ++i) {
                    const auto& v = poly.voices[i];
                    if (v.active && !v.releasing && v.gate > 0.5f &&
                        std::abs(v.freq - target) < 0.5f) {
                        found = true;
                        break;
                    }
                }
                if (found) ++matched;
            }

            if (matched < 3) {
                ++incomplete_chord_count;
                INFO("Chord " << chord_idx << " at block " << b
                     << " matched " << matched << "/3 notes");
            }
        }

        last_chord_idx = chord_idx;
    }

    INFO("Inspected " << chord_onsets_inspected << " chord onsets, "
         << incomplete_chord_count << " were incomplete");
    CHECK(chord_onsets_inspected >= 60);  // sanity: we sampled enough
    CHECK(incomplete_chord_count == 0);
}

// ============================================================================
// PRD prd-midi-input §7.2: Gate-multiplied release fix
// ============================================================================
//
// Default behavior (release_seconds == 0) zeros the voice output as soon as
// the gate steps 1→0 at note-off. With a per-instance release window > 0,
// the voice keeps mixing into the output for `release_seconds * sample_rate`
// samples after note-off so the body's own ADSR (which sees the actual
// gate edge) can run its release tail.

TEST_CASE("POLY release window: legacy zero-release silences voice immediately",
          "[poly-release]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_seq_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    // 0.5-beat note out of 4-beat cycle. At 120 BPM/48 kHz the note ends at
    // sample 12000 (block ~93).
    setup_single_note_sequence(vm, 440.0f, 0.5f);
    vm.init_poly_state(POLY_STATE_ID, SEQ_STATE_ID, 8, 0, 0, /*release_s*/ 0.0f);

    std::array<float, BLOCK_SIZE> left{}, right{};

    // Run past the note. Capture peak amplitude in the immediate post-note
    // window (blocks 95-100, after note-off but within the legacy
    // RELEASE_TIMEOUT=4 reaping window).
    float post_peak = 0.0f;
    for (int b = 0; b < 100; ++b) {
        vm.process_block(left.data(), right.data());
        if (b >= 95) {
            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
                post_peak = std::max(post_peak, std::abs(left[i]));
            }
        }
    }

    // With release=0 the gate-multiplied mix zeros the voice as soon as
    // gate steps 0. Voices reap a few blocks later. Output in this window
    // should be effectively silent.
    CHECK(post_peak < 0.01f);

    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    CHECK_THAT(poly.release_window_samples, WithinAbs(0.0f, 0.5f));
}

TEST_CASE("POLY release window: positive release_seconds keeps voice mixing",
          "[poly-release]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_seq_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    // Same 0.5-beat note as above.
    setup_single_note_sequence(vm, 440.0f, 0.5f);
    // 0.5s release window = 24000 samples = ~187 blocks.
    vm.init_poly_state(POLY_STATE_ID, SEQ_STATE_ID, 8, 0, 0, /*release_s*/ 0.5f);

    std::array<float, BLOCK_SIZE> left{}, right{};

    // The body here is an OSC_SIN with no envelope, so the voice keeps
    // generating a sine after note-off. The mix-side gate is held at 1.0
    // while release_countdown > 0, so the sine should remain audible until
    // the release window expires. Sample the same post-note window.
    float post_peak = 0.0f;
    for (int b = 0; b < 100; ++b) {
        vm.process_block(left.data(), right.data());
        if (b >= 95) {
            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
                post_peak = std::max(post_peak, std::abs(left[i]));
            }
        }
    }

    // Sine at the same amplitude as during the hold should still be present
    // — we're well inside the release window (< 12800 samples / ~100 blocks
    // out of the 24000-sample window).
    CHECK(post_peak > 0.3f);

    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    CHECK_THAT(poly.release_window_samples, WithinAbs(24000.0f, 1.0f));
}

TEST_CASE("POLY release window: voice reaped after countdown expires",
          "[poly-release]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_seq_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    // 0.5-beat note ends around block 93.
    setup_single_note_sequence(vm, 440.0f, 0.5f);
    // Tiny 1 ms release window (48 samples — under one block).
    vm.init_poly_state(POLY_STATE_ID, SEQ_STATE_ID, 8, 0, 0, /*release_s*/ 0.001f);

    std::array<float, BLOCK_SIZE> left{}, right{};
    for (int b = 0; b < 105; ++b) {
        vm.process_block(left.data(), right.data());
    }

    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    bool any_active = false;
    for (std::uint8_t i = 0; i < poly.max_voices; ++i) {
        if (poly.voices[i].active) any_active = true;
    }
    CHECK_FALSE(any_active);  // Released and reaped well within the window.
}

TEST_CASE("POLY release window: retrigger resets countdown so silenced voice "
          "speaks again", "[poly-release]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_seq_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    // Two notes on the same pitch (440 Hz). Second note retriggers the
    // same voice slot — its release_countdown must reset to 0 so the new
    // note's natural gate path runs.
    static constexpr float CYCLE_LENGTH = 4.0f;
    Event events[2];
    events[0].type = EventType::DATA;
    events[0].time = 0.0f;
    events[0].duration = 0.25f;   // 1 beat
    events[0].chance = 1.0f;
    events[0].velocity = 1.0f;
    events[0].num_values = 1;
    events[0].values[0] = 440.0f;
    events[1] = events[0];
    events[1].time = 2.0f;        // second beat-2 note

    Sequence seq;
    seq.events = events;
    seq.num_events = 2;
    seq.capacity = 2;
    seq.duration = CYCLE_LENGTH;
    seq.mode = SequenceMode::NORMAL;
    vm.init_sequence_program_state(SEQ_STATE_ID, &seq, 1, CYCLE_LENGTH, false, 2);

    // 5 s release window so the first note's countdown is still running when
    // the second note arrives.
    vm.init_poly_state(POLY_STATE_ID, SEQ_STATE_ID, 8, 0, 0, /*release_s*/ 5.0f);

    std::array<float, BLOCK_SIZE> left{}, right{};
    // Process up to and past the second note-on (beat 2 = 48000 samples
    // = 375 blocks).
    for (int b = 0; b < 400; ++b) {
        vm.process_block(left.data(), right.data());
    }

    // Same voice slot should be active and not releasing — second note-on
    // reused it and reset the release state.
    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    int active_440 = 0;
    for (std::uint8_t i = 0; i < poly.max_voices; ++i) {
        const auto& v = poly.voices[i];
        if (v.active && std::abs(v.freq - 440.0f) < 0.5f && !v.releasing) {
            ++active_440;
            CHECK(v.release_countdown == 0);
        }
    }
    CHECK(active_440 == 1);
}

// ============================================================================
// Phase 3: custom record-suffix property fields
// (prd-poly-callback-event-record)
// ============================================================================

namespace {
// Field bank with one custom prop slot: 0..10 fixed, 11 = custom slot 0.
constexpr std::uint16_t BUF_CUSTOM = 11;
constexpr std::uint16_t BUF_VOUT_L = 12;
constexpr std::uint16_t BUF_VOUT_R = 13;
constexpr std::uint16_t BUF_PROP_MIX_L = 14;

// SEQPAT_QUERY + POLY program whose voice body copies the custom prop-slot
// bank buffer (bank_base + 11) into the voice output — so the post-block
// bank buffer is observable and the body actually consumes the custom field.
std::vector<Instruction> build_poly_prop_program() {
    std::vector<Instruction> program;
    program.push_back(Instruction::make_nullary(
        Opcode::SEQPAT_QUERY, 0, SEQ_STATE_ID));
    auto poly_begin = Instruction::make_quinary(
        Opcode::POLY_BEGIN, BUF_PROP_MIX_L,
        BUF_BANK, 0xFFFF, 0xFFFF, 0xFFFF, BUF_VOUT_L,
        POLY_STATE_ID);
    poly_begin.flags = InstructionFlag::STEREO_OUTPUT;
    poly_begin.rate = 2;
    program.push_back(poly_begin);
    program.push_back(Instruction::make_unary(
        Opcode::COPY, BUF_VOUT_L, BUF_CUSTOM));
    program.push_back(Instruction::make_unary(
        Opcode::COPY, BUF_VOUT_R, BUF_CUSTOM));
    program.push_back(Instruction::make_nullary(Opcode::POLY_END, 0));
    program.push_back(Instruction::make_unary(
        Opcode::OUTPUT, 0, BUF_PROP_MIX_L));
    return program;
}
}  // namespace

TEST_CASE("POLY field bank: custom prop value surfaces per voice",
          "[poly][fields][phase3]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_poly_prop_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    static constexpr float CYCLE_LENGTH = 4.0f;
    Event evt;
    evt.type = EventType::DATA;
    evt.time = 0.0f;
    evt.duration = 4.0f / CYCLE_LENGTH;
    evt.chance = 1.0f;
    evt.velocity = 1.0f;
    evt.num_values = 1;
    evt.values[0] = 261.63f;
    evt.prop_vals[0] = 0.9f;
    evt.prop_set_mask = 0x1;            // slot 0 explicitly declared

    Sequence seq;
    seq.events = &evt;
    seq.num_events = 1;
    seq.capacity = 1;
    seq.duration = CYCLE_LENGTH;
    seq.mode = SequenceMode::NORMAL;
    vm.init_sequence_program_state(SEQ_STATE_ID, &seq, 1, CYCLE_LENGTH, false, 1);

    // prop_count=1, default 0.5 — must be ignored since the event sets it.
    const float defaults[MAX_PROPS_PER_EVENT] = {0.5f, 0.0f, 0.0f, 0.0f};
    vm.init_poly_state(POLY_STATE_ID, SEQ_STATE_ID, 8, 0, 0,
                       /*release*/ 0.0f, /*prop_count*/ 1, defaults);

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    REQUIRE(poly.active_voice_count() == 1);
    for (std::uint8_t i = 0; i < poly.max_voices; ++i) {
        if (!poly.voices[i].active) continue;
        CHECK((poly.voices[i].prop_set_mask & 0x1) != 0);
        CHECK(std::abs(poly.voices[i].prop_vals[0] - 0.9f) < 0.001f);
    }
    // The custom field-bank slot holds the event's value, not the default.
    CHECK(std::abs(vm.buffers().get(BUF_CUSTOM)[0] - 0.9f) < 0.001f);
}

TEST_CASE("POLY field bank: custom prop default applies when event omits it",
          "[poly][fields][phase3]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_poly_prop_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    static constexpr float CYCLE_LENGTH = 4.0f;
    Event evt;
    evt.type = EventType::DATA;
    evt.time = 0.0f;
    evt.duration = 4.0f / CYCLE_LENGTH;
    evt.chance = 1.0f;
    evt.velocity = 1.0f;
    evt.num_values = 1;
    evt.values[0] = 261.63f;
    // prop_set_mask left 0 — the event declares no custom property.

    Sequence seq;
    seq.events = &evt;
    seq.num_events = 1;
    seq.capacity = 1;
    seq.duration = CYCLE_LENGTH;
    seq.mode = SequenceMode::NORMAL;
    vm.init_sequence_program_state(SEQ_STATE_ID, &seq, 1, CYCLE_LENGTH, false, 1);

    const float defaults[MAX_PROPS_PER_EVENT] = {0.5f, 0.0f, 0.0f, 0.0f};
    vm.init_poly_state(POLY_STATE_ID, SEQ_STATE_ID, 8, 0, 0,
                       0.0f, 1, defaults);

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    REQUIRE(poly.active_voice_count() == 1);
    for (std::uint8_t i = 0; i < poly.max_voices; ++i) {
        if (!poly.voices[i].active) continue;
        CHECK((poly.voices[i].prop_set_mask & 0x1) == 0);
    }
    // The event omits the property → the field bank carries the callback
    // destructure default.
    CHECK(std::abs(vm.buffers().get(BUF_CUSTOM)[0] - 0.5f) < 0.001f);
}
