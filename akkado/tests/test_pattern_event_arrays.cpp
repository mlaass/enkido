// Tests for PRD prd-pattern-event-arrays: notes()/freqs() pattern-event chord
// accessors, the DynArray value type, polymorphic len()/indexing, and the
// SEQPAT_VALUES opcode end-to-end.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "akkado/akkado.hpp"
#include <cedar/vm/instruction.hpp>
#include <cedar/vm/vm.hpp>
#include <cedar/dsp/constants.hpp>
#include <array>
#include <cstring>
#include <span>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

std::vector<cedar::Instruction> get_instructions(const akkado::CompileResult& r) {
    std::vector<cedar::Instruction> insts;
    insts.resize(r.bytecode.size() / sizeof(cedar::Instruction));
    std::memcpy(insts.data(), r.bytecode.data(), r.bytecode.size());
    return insts;
}

std::size_t count_op(const std::vector<cedar::Instruction>& insts,
                     cedar::Opcode op, std::uint8_t rate = 0xFF) {
    std::size_t n = 0;
    for (const auto& i : insts) {
        if (i.opcode != op) continue;
        if (rate != 0xFF && i.rate != rate) continue;
        ++n;
    }
    return n;
}

bool diag_has_code(const akkado::CompileResult& r, const std::string& code) {
    for (const auto& d : r.diagnostics) {
        if (d.code == code) return true;
    }
    return false;
}

// Apply the SequenceProgram state inits a compiled pattern program needs.
// seq_storage must outlive every process_block() call.
void apply_inits(cedar::VM& vm, const akkado::CompileResult& r,
                 std::vector<std::vector<cedar::Sequence>>& seq_storage) {
    seq_storage.reserve(r.state_inits.size());
    for (const auto& init : r.state_inits) {
        if (init.type != akkado::StateInitData::Type::SequenceProgram) continue;
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
    }
}

// Run the program for `blocks` blocks and return the L channel of the last.
std::array<float, cedar::BLOCK_SIZE> run_blocks(const akkado::CompileResult& r,
                                                int blocks = 1) {
    auto insts = get_instructions(r);
    cedar::VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);
    vm.set_block_table(r.block_table, r.main_instruction_count);
    REQUIRE(vm.load_program_immediate(std::span<const cedar::Instruction>(insts)));
    std::vector<std::vector<cedar::Sequence>> seq_storage;
    apply_inits(vm, r, seq_storage);
    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    for (int b = 0; b < blocks; ++b) {
        vm.process_block(L.data(), R.data());
    }
    return L;
}

// prd-bus-routing: these tests probe out() / runtime values directly. Bypass
// the master bus so out() stays a transparent device write — no default
// soft-clip @ 0.9, no ±1.0 safety clamp. The master bus has its own tests.
akkado::CompileResult compile_raw(std::string_view src) {
    return akkado::compile(src, "<input>", nullptr, nullptr,
                           /*lint_strict=*/false, /*bypass_master=*/true);
}

}  // namespace

TEST_CASE("event-arrays: notes() emits SEQPAT_VALUES (MIDI) + length opcode",
          "[pattern_event_arrays]") {
    auto r = compile_raw(
        "n\"[c4,e4,g4]\" as e\n"
        "out(len(e.notes), len(e.notes))\n");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    // rate 1 = MIDI conversion for notes() (one per e.notes reference)
    CHECK(count_op(insts, cedar::Opcode::SEQPAT_VALUES, 1) >= 1);
    // length buffer comes from SEQPAT_FIELD num_values selector (rate 5)
    CHECK(count_op(insts, cedar::Opcode::SEQPAT_FIELD, 5) >= 1);
}

TEST_CASE("event-arrays: freqs() emits SEQPAT_VALUES in Hz mode (rate 0)",
          "[pattern_event_arrays]") {
    auto r = compile_raw(
        "n\"[c4,e4,g4]\" as e\n"
        "x = e.freqs\n"
        "out(x[0], x[1])\n");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::SEQPAT_VALUES, 0) == 1);
}

TEST_CASE("event-arrays: len(e.notes) is the runtime chord size",
          "[pattern_event_arrays]") {
    auto r = compile_raw(
        "n\"[c4,e4,g4]\" as e\n"
        "out(len(e.notes), len(e.notes))\n");
    REQUIRE(r.success);
    auto L = run_blocks(r);
    // The single cycle event is a 3-note chord.
    CHECK_THAT(L[0], WithinAbs(3.0f, 1e-4f));
    CHECK_THAT(L[cedar::BLOCK_SIZE - 1], WithinAbs(3.0f, 1e-4f));
}

TEST_CASE("event-arrays: notes() yields the chord's MIDI numbers",
          "[pattern_event_arrays]") {
    auto r = compile_raw(
        "n\"[c4,e4,g4]\" as e\n"
        "x = e.notes\n"
        "out(x[0], x[1])\n");
    REQUIRE(r.success);
    auto L = run_blocks(r);
    // x[0] = c4 = MIDI 60 (freqs round-trip through freq->MIDI).
    CHECK_THAT(L[0], WithinAbs(60.0f, 1e-3f));
}

TEST_CASE("event-arrays: indexing wraps past the chord length",
          "[pattern_event_arrays]") {
    // x[3] on a 3-note chord wraps to x[0].
    auto r = compile_raw(
        "n\"[c4,e4,g4]\" as e\n"
        "x = e.notes\n"
        "out(x[3], x[3])\n");
    REQUIRE(r.success);
    auto L = run_blocks(r);
    CHECK_THAT(L[0], WithinAbs(60.0f, 1e-3f));  // wrapped back to c4
}

TEST_CASE("event-arrays: e.notes can be indexed directly without a binding",
          "[pattern_event_arrays]") {
    // The parser allows `[i]` directly after a field access.
    auto r = compile_raw(
        "n\"[c4,e4,g4]\" as e\n"
        "out(e.notes[1], e.notes[1])\n");
    REQUIRE(r.success);
    auto L = run_blocks(r);
    CHECK_THAT(L[0], WithinAbs(64.0f, 1e-3f));  // e.notes[1] = e4 = MIDI 64
}

TEST_CASE("event-arrays: freqs() yields the chord frequencies in Hz",
          "[pattern_event_arrays]") {
    auto r = compile_raw(
        "n\"[c4,e4,g4]\" as e\n"
        "x = e.freqs\n"
        "out(x[0], x[0])\n");
    REQUIRE(r.success);
    auto L = run_blocks(r);
    // c4 = 261.63 Hz
    CHECK_THAT(L[0], WithinAbs(261.63f, 0.5f));
}

TEST_CASE("event-arrays: monophonic event is a length-1 dynamic array",
          "[pattern_event_arrays]") {
    auto r = compile_raw(
        "n\"c4\" as e\n"
        "out(len(e.notes), len(e.notes))\n");
    REQUIRE(r.success);
    auto L = run_blocks(r);
    CHECK_THAT(L[0], WithinAbs(1.0f, 1e-4f));
}

TEST_CASE("event-arrays: notes(pattern-literal) accepts a literal directly",
          "[pattern_event_arrays]") {
    // PRD Q4: notes() accepts any Pattern, not just an `as`-bound variable.
    auto r = compile_raw(
        "x = notes(n\"[c4,e4,g4]\")\n"
        "out(x[0], x[1])\n");
    REQUIRE(r.success);
}

TEST_CASE("event-arrays: e.notes UFCS method call composes with step",
          "[pattern_event_arrays]") {
    auto r = compile_raw(
        "fn step (arr, trig) -> arr[counter(trig)]\n"
        "n\"[c4,e4,g4] g3\" as e\n"
        "e.notes.step(trigger(8)) |> mtof(@) |> osc(\"sin\", @) |> out(@, @)\n");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    // The DynArray reaches step() and is indexed via ARRAY_INDEX.
    CHECK(count_op(insts, cedar::Opcode::SEQPAT_VALUES) >= 1);
    CHECK(count_op(insts, cedar::Opcode::ARRAY_INDEX) >= 1);
}

TEST_CASE("event-arrays: stateful UGen over a dynamic array is rejected",
          "[pattern_event_arrays]") {
    auto r = compile_raw(
        "n\"[c4,e4,g4]\" as e\n"
        "osc(\"sin\", e.freqs) |> out(@, @)\n");
    CHECK_FALSE(r.success);
    CHECK(diag_has_code(r, "E181"));
}

TEST_CASE("event-arrays: map() over a dynamic array stays dynamic",
          "[pattern_event_arrays]") {
    // map(e.notes, (v) -> v + 7) transposes every chord note; the result is
    // still a DynArray, indexable through step().
    auto r = compile_raw(
        "fn step (arr, trig) -> arr[counter(trig)]\n"
        "n\"[c4,e4,g4] g3\" as e\n"
        "map(e.notes, (v) -> v + 7).step(trigger(8))\n"
        "  |> mtof(@) |> osc(\"sin\", @) |> out(@, @)\n");
    REQUIRE(r.success);
}

TEST_CASE("event-arrays: map() over a dynamic array rejects the index form",
          "[pattern_event_arrays]") {
    auto r = compile_raw(
        "n\"[c4,e4,g4]\" as e\n"
        "x = map(e.notes, (v, i) -> v + i)\n"
        "out(x[0], x[0])\n");
    CHECK_FALSE(r.success);
    CHECK(diag_has_code(r, "E183"));
}

TEST_CASE("event-arrays: scalar e.freq accessor is unchanged",
          "[pattern_event_arrays]") {
    // Regression: the existing first-note scalar accessor still compiles and
    // does not emit SEQPAT_VALUES.
    auto r = compile_raw(
        "n\"[c4,e4,g4]\" as e\n"
        "osc(\"sin\", e.freq) |> out(@, @)\n");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::SEQPAT_VALUES) == 0);
}

TEST_CASE("event-arrays: notes is a normal (non-reserved) identifier",
          "[pattern_event_arrays]") {
    // `notes` is a common variable name — unlike state/get/set it is NOT
    // reserved; a user binding shadows the builtin like len/map.
    auto r = compile_raw("notes = [60, 64, 67]\nout(notes[0], notes[1])\n");
    REQUIRE(r.success);
}
