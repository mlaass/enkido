// Determinism tests for live-recompile.
//
// The contract: compiling the same source twice must produce byte-equal
// bytecode and event-equal state initialization. If this fails, hot-swap in
// live mode will surface as audibly different output for the same code — the
// symptom that motivated this test.

#include <catch2/catch_test_macros.hpp>
#include "akkado/akkado.hpp"
#include <cedar/vm/vm.hpp>
#include <cedar/vm/state_pool.hpp>
#include <cedar/vm/instruction.hpp>
#include <cedar/vm/audio_arena.hpp>
#include <cedar/dsp/constants.hpp>
#include <cedar/opcodes/sequence.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Diff {
    bool equal = true;
    std::string summary;
};

Diff compare_bytecode(const akkado::CompileResult& a,
                      const akkado::CompileResult& b) {
    Diff d;
    if (a.bytecode.size() != b.bytecode.size()) {
        d.equal = false;
        d.summary = "bytecode size: " + std::to_string(a.bytecode.size()) +
                    " vs " + std::to_string(b.bytecode.size());
        return d;
    }
    if (std::memcmp(a.bytecode.data(), b.bytecode.data(), a.bytecode.size()) != 0) {
        d.equal = false;
        std::ostringstream os;
        // Find first differing instruction.
        const auto n_inst = a.bytecode.size() / sizeof(cedar::Instruction);
        for (std::size_t i = 0; i < n_inst; ++i) {
            cedar::Instruction ia{}, ib{};
            std::memcpy(&ia, a.bytecode.data() + i * sizeof(cedar::Instruction),
                        sizeof(ia));
            std::memcpy(&ib, b.bytecode.data() + i * sizeof(cedar::Instruction),
                        sizeof(ib));
            if (std::memcmp(&ia, &ib, sizeof(ia)) != 0) {
                os << "first diff at instruction " << i
                   << " opcode a=" << static_cast<int>(ia.opcode)
                   << " b=" << static_cast<int>(ib.opcode)
                   << " state_id a=" << ia.state_id << " b=" << ib.state_id;
                break;
            }
        }
        d.summary = os.str();
    }
    return d;
}

Diff compare_state_inits(const akkado::CompileResult& a,
                         const akkado::CompileResult& b) {
    Diff d;
    if (a.state_inits.size() != b.state_inits.size()) {
        d.equal = false;
        d.summary = "state_inits size: " + std::to_string(a.state_inits.size()) +
                    " vs " + std::to_string(b.state_inits.size());
        return d;
    }
    for (std::size_t s = 0; s < a.state_inits.size(); ++s) {
        const auto& ia = a.state_inits[s];
        const auto& ib = b.state_inits[s];
        if (ia.state_id != ib.state_id) {
            d.equal = false;
            d.summary = "state_inits[" + std::to_string(s) +
                        "] state_id mismatch: " + std::to_string(ia.state_id) +
                        " vs " + std::to_string(ib.state_id);
            return d;
        }
        if (ia.sequence_events.size() != ib.sequence_events.size()) {
            d.equal = false;
            d.summary = "state_inits[" + std::to_string(s) +
                        "] sequence count mismatch";
            return d;
        }
        for (std::size_t q = 0; q < ia.sequence_events.size(); ++q) {
            const auto& ea = ia.sequence_events[q];
            const auto& eb = ib.sequence_events[q];
            if (ea.size() != eb.size()) {
                d.equal = false;
                d.summary = "state_inits[" + std::to_string(s) + "].sequence_events[" +
                            std::to_string(q) + "] event count mismatch: " +
                            std::to_string(ea.size()) + " vs " + std::to_string(eb.size());
                return d;
            }
            for (std::size_t e = 0; e < ea.size(); ++e) {
                const auto& A = ea[e];
                const auto& B = eb[e];
                bool same = A.num_values == B.num_values &&
                            A.time == B.time && A.duration == B.duration &&
                            A.velocity == B.velocity && A.midi_note == B.midi_note;
                if (same) {
                    for (std::size_t v = 0; v < A.num_values; ++v) {
                        if (A.values[v] != B.values[v]) { same = false; break; }
                    }
                }
                if (!same) {
                    std::ostringstream os;
                    os << "state_inits[" << s << "].sequence_events[" << q
                       << "][" << e << "] differs: time=" << A.time << "/" << B.time
                       << " midi=" << A.midi_note << "/" << B.midi_note
                       << " num_values=" << static_cast<int>(A.num_values)
                       << "/" << static_cast<int>(B.num_values);
                    if (A.num_values == B.num_values) {
                        os << " values=[";
                        for (std::size_t v = 0; v < A.num_values; ++v) {
                            os << A.values[v];
                            if (v + 1 < A.num_values) os << ",";
                        }
                        os << "] vs [";
                        for (std::size_t v = 0; v < B.num_values; ++v) {
                            os << B.values[v];
                            if (v + 1 < B.num_values) os << ",";
                        }
                        os << "]";
                    }
                    d.equal = false;
                    d.summary = os.str();
                    return d;
                }
            }
        }
    }
    return d;
}

void check_deterministic(std::string_view source, std::size_t reps = 4) {
    auto first = akkado::compile(source);
    if (!first.success) {
        std::ostringstream os;
        for (const auto& diag : first.diagnostics) {
            os << "  " << diag.code << ": " << diag.message << "\n";
        }
        INFO("Compile diagnostics:\n" << os.str());
    }
    REQUIRE(first.success);
    for (std::size_t i = 1; i < reps; ++i) {
        auto again = akkado::compile(source);
        REQUIRE(again.success);
        auto bd = compare_bytecode(first, again);
        INFO("Bytecode diff (rep " << i << "): " << bd.summary);
        CHECK(bd.equal);
        auto sd = compare_state_inits(first, again);
        INFO("State init diff (rep " << i << "): " << sd.summary);
        CHECK(sd.equal);
    }
}

}  // namespace

// Chord patterns must be piped to a polyphony-aware UGen (soundfont) or wrapped
// in poly() — that's an Akkado language requirement (E410). All chord-pattern
// determinism tests therefore use the soundfont pipeline.
TEST_CASE("Recompile is deterministic: chord pattern", "[determinism][hot_swap]") {
    // The exact user-reported program. Voicings observed to change between
    // recompiles when triggered "at the right time" in live mode.
    SECTION("full user pipeline with soundfont") {
        check_deterministic(
            R"(c"<[Am Am7] [Dm7 Dm] G CM >" |> soundfont(@, "gm", 0) |> out(@*.85))");
    }
}

TEST_CASE("Recompile is deterministic: simpler chord patterns", "[determinism][hot_swap]") {
    SECTION("single chord") {
        check_deterministic(R"(c"Am" |> soundfont(@, "gm", 0) |> out(@*.85))");
    }
    SECTION("flat sequence of chords") {
        check_deterministic(R"(c"Am Dm G C" |> soundfont(@, "gm", 0) |> out(@*.85))");
    }
    SECTION("slowcat of chords") {
        check_deterministic(R"(c"<Am Dm G C>" |> soundfont(@, "gm", 0) |> out(@*.85))");
    }
    SECTION("grouped then slowcat") {
        check_deterministic(R"(c"<[Am Dm] [G C]>" |> soundfont(@, "gm", 0) |> out(@*.85))");
    }
    SECTION("seventh chords") {
        check_deterministic(R"(c"Am7 Dm7 G7 Cmaj7" |> soundfont(@, "gm", 0) |> out(@*.85))");
    }
}

TEST_CASE("Recompile is deterministic: non-chord patterns", "[determinism][hot_swap]") {
    SECTION("note pattern") {
        check_deterministic(R"(n"c4 e4 g4" |> osc("sin", %.freq) |> out(%, %))");
    }
    SECTION("alternation in note pattern") {
        check_deterministic(R"(n"<c4 e4 g4 b4>" |> osc("sin", %.freq) |> out(%, %))");
    }
}

// ============================================================================
// End-to-end: hot-swap must preserve `<...>` alternation across recompile.
// ============================================================================

namespace {

// Apply a compile result's sequence state inits to the VM. Mirrors the
// host-side logic in web/wasm/nkido_wasm.cpp:911 and
// tools/nkido-cli/program_loader.cpp:235. The seq_storage buffer keeps
// the source sequences alive while init_sequence_program copies their
// events into arena memory.
void apply_seq_state_inits(cedar::VM& vm, const akkado::CompileResult& cr,
                           std::vector<std::vector<cedar::Sequence>>& seq_storage) {
    for (const auto& init : cr.state_inits) {
        if (init.type != akkado::StateInitData::Type::SequenceProgram) continue;
        std::vector<cedar::Sequence> seq_copy = init.sequences;
        for (std::size_t i = 0; i < seq_copy.size() && i < init.sequence_events.size(); ++i) {
            if (!init.sequence_events[i].empty()) {
                seq_copy[i].events = const_cast<cedar::Event*>(init.sequence_events[i].data());
                seq_copy[i].num_events = static_cast<std::uint32_t>(init.sequence_events[i].size());
                seq_copy[i].capacity = static_cast<std::uint32_t>(init.sequence_events[i].size());
            }
        }
        seq_storage.push_back(std::move(seq_copy));
        auto& stored = seq_storage.back();
        vm.init_sequence_program_state(
            init.state_id, stored.data(), stored.size(),
            init.cycle_length, init.is_sample_pattern, init.total_events);
    }
}

// Run the live-coding load sequence: queue the new program for swap, then
// apply state inits. Mirrors the WASM `cedar_load_program` +
// `cedar_apply_state_inits` pair, which is what the web client calls on
// every recompile.
void live_load(cedar::VM& vm, const akkado::CompileResult& cr,
               std::vector<std::vector<cedar::Sequence>>& seq_storage) {
    REQUIRE(cr.success);
    const std::size_t n_inst = cr.bytecode.size() / sizeof(cedar::Instruction);
    std::vector<cedar::Instruction> insts(n_inst);
    std::memcpy(insts.data(), cr.bytecode.data(), cr.bytecode.size());
    REQUIRE(vm.load_program(insts) == cedar::VM::LoadResult::Success);
    apply_seq_state_inits(vm, cr, seq_storage);
}

// Walk every SequenceState in the pool and return the highest seq.step
// observed. The simplest cross-pattern progress indicator: as long as
// playback is advancing the alternation counter, this is monotonically
// non-decreasing over time, and stays stable across structurally-identical
// recompiles.
std::uint32_t max_step_across_all_seqs(cedar::VM& vm) {
    std::uint32_t max_step = 0;
    auto& pool = vm.states();
    // Iterate all known state ids. StatePool exposes get_state_ids implicitly
    // through ProgramSlot, but here we walk the internal slots: use a helper
    // by probing every SequenceState that exists.
    for (std::uint32_t i = 0; i < cedar::MAX_STATES; ++i) {
        // The slot layout isn't exposed; instead iterate by checking common
        // state IDs we expect from compile. Since we don't know IDs, use the
        // pool's iteration helper if available — fall back to a no-op.
        (void)i;
        (void)pool;
        break;
    }
    return max_step;
}

}  // namespace

TEST_CASE("VM hot-swap preserves alternation across recompile", "[determinism][hot_swap]") {
    // End-to-end version of the user's symptom: render a chord-pattern
    // program for a while, then load the same source again. After the swap,
    // playback must continue from where it left off — not restart the `<...>`
    // alternation counter.
    constexpr const char* kSrc =
        R"(n"<c4 e4 g4 b4>" |> osc("sin", %.freq) |> out(%, %))";

    auto cr = akkado::compile(kSrc);
    REQUIRE(cr.success);

    cedar::VM vm;
    vm.set_crossfade_blocks(0);  // skip crossfade for faster, cleaner test
    std::vector<std::vector<cedar::Sequence>> seq_storage;
    live_load(vm, cr, seq_storage);

    std::array<float, cedar::BLOCK_SIZE> left{}, right{};

    // First block triggers the swap. Then render enough blocks to advance
    // the alternation counter several times. Default cps gives ~2 seconds
    // per cycle; 4 cycles ≈ 8 seconds ≈ 3000 blocks. Overshoot to be safe.
    for (int i = 0; i < 4000; ++i) {
        vm.process_block(left.data(), right.data());
    }

    // Find the SequenceState created for the pattern. We don't know its id
    // up front, so probe the state_inits to recover it.
    std::uint32_t seq_state_id = 0;
    for (const auto& init : cr.state_inits) {
        if (init.type == akkado::StateInitData::Type::SequenceProgram) {
            seq_state_id = init.state_id;
            break;
        }
    }
    REQUIRE(seq_state_id != 0);

    auto* before = vm.states().get_if<cedar::SequenceState>(seq_state_id);
    REQUIRE(before != nullptr);
    REQUIRE(before->num_sequences >= 1);
    // Find a sequence with ALTERNATE mode and report its step.
    std::int32_t alt_seq_idx = -1;
    for (std::uint32_t i = 0; i < before->num_sequences; ++i) {
        if (before->sequences[i].mode == cedar::SequenceMode::ALTERNATE) {
            alt_seq_idx = static_cast<std::int32_t>(i);
            break;
        }
    }
    INFO("num_sequences: " << before->num_sequences
         << " alt_seq_idx: " << alt_seq_idx
         << " cycle_index: " << before->cycle_index
         << " last_beat_pos: " << before->last_beat_pos);
    REQUIRE(alt_seq_idx >= 0);
    const std::uint32_t step_before = before->sequences[alt_seq_idx].step;
    INFO("step before recompile: " << step_before);
    REQUIRE(step_before > 0);  // playback must have advanced at all

    // Live-coding recompile: same source, separate compile, hot-swap path.
    auto cr2 = akkado::compile(kSrc);
    REQUIRE(cr2.success);
    live_load(vm, cr2, seq_storage);

    auto* after = vm.states().get_if<cedar::SequenceState>(seq_state_id);
    REQUIRE(after != nullptr);
    REQUIRE(after->num_sequences >= 1);
    CHECK(after->sequences[alt_seq_idx].step == step_before);

    // Process one more block to apply the queued swap. The state must
    // remain preserved after rebind_states + gc_sweep complete.
    vm.process_block(left.data(), right.data());

    auto* after_swap = vm.states().get_if<cedar::SequenceState>(seq_state_id);
    REQUIRE(after_swap != nullptr);
    CHECK(after_swap->sequences[alt_seq_idx].step >= step_before);
}

