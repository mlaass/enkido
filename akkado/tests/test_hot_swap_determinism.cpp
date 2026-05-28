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
#include <cedar/opcodes/dsp_state.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
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

// Apply a compile result's state inits to the VM. Mirrors the host-side logic
// in web/wasm/nkido_wasm.cpp:1152 (`cedar_apply_state_inits`) and
// tools/nkido/program_loader.cpp:288 (`apply_state_inits`). Covers the init
// types reached by the patches in this file: SequenceProgram, PolyAlloc,
// ForeachAlloc, EventTransform, RateScale, ExtendedParams. The seq_storage
// buffer keeps the source sequences alive while init_sequence_program copies
// their events into arena memory.
void apply_seq_state_inits(cedar::VM& vm, const akkado::CompileResult& cr,
                           std::vector<std::vector<cedar::Sequence>>& seq_storage) {
    for (const auto& init : cr.state_inits) {
        if (init.type == akkado::StateInitData::Type::SequenceProgram) {
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
        } else if (init.type == akkado::StateInitData::Type::PolyAlloc) {
            vm.init_poly_state(init.state_id, init.poly_seq_state_id,
                               init.poly_max_voices, init.poly_mode,
                               init.poly_steal_strategy,
                               init.poly_release_seconds,
                               init.poly_prop_count, init.poly_prop_defaults);
        } else if (init.type == akkado::StateInitData::Type::ForeachAlloc) {
            vm.init_foreach_state(init.state_id, init.foreach_allocator_kind,
                                  init.foreach_block_id,
                                  init.foreach_event_src_state_id,
                                  init.foreach_max_iterations,
                                  init.poly_max_voices, init.poly_mode,
                                  init.poly_steal_strategy,
                                  init.poly_release_seconds,
                                  init.poly_prop_count,
                                  init.poly_prop_defaults);
        } else if (init.type == akkado::StateInitData::Type::EventTransform ||
                   init.type == akkado::StateInitData::Type::Reorder ||
                   init.type == akkado::StateInitData::Type::Fanout) {
            vm.init_event_transform_state(init.state_id, init.cycle_length,
                                          init.is_sample_pattern,
                                          init.total_events);
        } else if (init.type == akkado::StateInitData::Type::RateScale) {
            vm.init_event_rate_scale_state(init.state_id);
        } else if (init.type == akkado::StateInitData::Type::ExtendedParams) {
            vm.init_extended_params(init.state_id,
                                    init.ext_constants.data(),
                                    init.ext_buffer_indices.data(),
                                    init.ext_count);
        }
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

// Per-event voice-fire counter. The "voice retriggered or released" check in
// the existing tests is too coarse: it passes if any one voice fires. This
// helper verifies the stronger property — the count of voice allocations per
// block equals the number of NoteOn onsets the pattern emits in that block's
// sample window, summing the chord notes (`evt.num_values`) for chord events.
//
// Detection rule for "voice fired this block": after process_block returns,
// any voice with `active && age == 1` was just allocated this block (poly's
// tick() increments age once per block; allocate_voice sets age=0, so a fresh
// allocation lands at age=1 post-tick).
//
// Caveat: pattern queries (SEQPAT_QUERY) refresh `output.events` per cycle.
// For chord patterns (the focus of these tests) the events are stable across
// cycles, so reading output.events after each block is safe. ALTERNATE
// patterns would need a different approach.
struct VoiceFireCounter {
    cedar::VM* vm = nullptr;
    std::uint32_t poly_id = 0;
    std::uint32_t seq_id = 0;
    // Counter state.
    std::uint32_t total_expected_fires = 0;
    std::uint32_t total_observed_fires = 0;
    std::uint32_t blocks_with_underfire = 0;
    std::uint32_t blocks_with_overfire = 0;
    std::ostringstream first_underfire;
    std::ostringstream first_overfire;

    void step() {
        const auto* poly = vm->states().template get_if<cedar::PolyAllocState>(poly_id);
        const auto* seq  = vm->states().template get_if<cedar::SequenceState>(seq_id);
        if (!poly || !poly->voices || !seq || seq->output.num_events == 0) {
            return;
        }
        // Count fires this block: active voices with age==1 (one tick since
        // allocate_voice set age=0).
        std::uint32_t observed = 0;
        for (std::uint16_t v = 0; v < poly->max_voices; ++v) {
            const auto& voice = poly->voices[v];
            if (voice.active && voice.age == 1) ++observed;
        }
        total_observed_fires += observed;

        // Expected fires: walk output.events, sum num_values for events whose
        // onset (e.time, beats from cycle start) falls inside this block's
        // sample window. Mirrors cedar/src/vm/vm.cpp:compute_block_timing —
        // double-precision beat math + boundary-epsilon snap so cycle-end
        // events fire in the SAME block as run_voice_pool decided to fire
        // them. Drift between this counter and the VM would produce false
        // "underfire" reports.
        constexpr double CYCLE_BOUNDARY_EPSILON = 1e-9;
        const double sr_d = static_cast<double>(vm->context().sample_rate);
        const double bpm_d = static_cast<double>(vm->context().bpm);
        const double spb_d = (60.0 / bpm_d) * sr_d;
        const double cycle_length_d = static_cast<double>(seq->cycle_length);
        if (spb_d <= 0.0 || cycle_length_d <= 0.0) return;
        const std::uint64_t global = vm->context().global_sample_counter;
        if (global < cedar::BLOCK_SIZE) return;  // not enough history yet
        const std::uint64_t block_start_sample = global - cedar::BLOCK_SIZE;
        const double beat_start_d =
            static_cast<double>(block_start_sample) / spb_d;
        double cycle_pos_raw = std::fmod(beat_start_d, cycle_length_d);
        if (cycle_pos_raw < CYCLE_BOUNDARY_EPSILON) cycle_pos_raw = 0.0;
        const double cycle_pos_d = cycle_pos_raw;
        const double block_beats_d =
            static_cast<double>(cedar::BLOCK_SIZE) / spb_d;
        double block_end_pos_raw = cycle_pos_d + block_beats_d;
        if (std::abs(block_end_pos_raw - cycle_length_d) < CYCLE_BOUNDARY_EPSILON) {
            block_end_pos_raw = cycle_length_d;
        }
        // Narrow to float to match the VM's event-time comparison precision.
        const float cycle_pos = static_cast<float>(cycle_pos_d);
        const float block_end_pos = static_cast<float>(block_end_pos_raw);
        const float cycle_length = static_cast<float>(cycle_length_d);
        std::uint32_t expected = 0;
        for (std::uint32_t i = 0; i < seq->output.num_events; ++i) {
            const auto& e = seq->output.events[i];
            const bool in_block = (e.time >= cycle_pos && e.time < block_end_pos);
            const bool in_wrap = (block_end_pos > cycle_length) &&
                                 (e.time < block_end_pos - cycle_length);
            if (in_block || in_wrap) {
                expected += e.num_values > 0 ? e.num_values : 1;
            }
        }
        total_expected_fires += expected;
        if (observed < expected) {
            ++blocks_with_underfire;
            if (first_underfire.tellp() == 0) {
                first_underfire << "underfire @block_start_sample=" << block_start_sample
                                << " cycle_pos=" << cycle_pos
                                << " expected=" << expected
                                << " observed=" << observed;
            }
        }
        if (observed > expected) {
            ++blocks_with_overfire;
            if (first_overfire.tellp() == 0) {
                first_overfire << "overfire @block_start_sample=" << block_start_sample
                               << " cycle_pos=" << cycle_pos
                               << " expected=" << expected
                               << " observed=" << observed;
            }
        }
    }
};

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

// ============================================================================
// Regression: hot-swap inside a poly() instrument body must not stick voices
// ============================================================================
//
// User report: editing the `delay()` line in the patch below — even fully
// commenting it out — caused the audio to lock into a stable tone that
// persisted until the page was reloaded. Pattern events stopped firing,
// existing voices never received note-off, and the AR envelope's edge
// detector never saw a new rising edge to retrigger from.
//
// The contract this test pins down: after a live recompile that changes the
// body of a poly()'s instrument closure but leaves the pattern source
// untouched, (a) the upstream SequenceState must keep advancing through
// cycles, (b) at least one voice in the PolyAllocState must retrigger or
// release in the post-swap window, and (c) the audio output must not be
// frozen (consecutive blocks identical sample-for-sample, RMS variance
// vanishingly small).
//
// Run with `[hot_swap]` or `[regression]` tag. Both crossfade=0 and the
// default crossfade path are exercised — the bug is not crossfade-specific.

namespace {

std::uint32_t find_state_init_id(const akkado::CompileResult& cr,
                                 akkado::StateInitData::Type t) {
    for (const auto& init : cr.state_inits) {
        if (init.type == t) return init.state_id;
    }
    return 0;
}

// poly() lowers to either a legacy PolyAlloc or a ForeachAlloc(VOICE_POOL)
// state init depending on the path the compiler took. Both back PolyAllocState
// in the state pool — same struct, same opcode handler — so for the test we
// just want the state_id, whichever kind it is.
std::uint32_t find_poly_state_id(const akkado::CompileResult& cr) {
    for (const auto& init : cr.state_inits) {
        if (init.type == akkado::StateInitData::Type::PolyAlloc) {
            return init.state_id;
        }
        if (init.type == akkado::StateInitData::Type::ForeachAlloc &&
            init.foreach_allocator_kind == 0) {
            return init.state_id;
        }
    }
    return 0;
}

float block_rms(std::span<const float> samples) {
    if (samples.empty()) return 0.0f;
    double acc = 0.0;
    for (float s : samples) acc += static_cast<double>(s) * s;
    return static_cast<float>(std::sqrt(acc / samples.size()));
}

}  // namespace

TEST_CASE("Hot-swap inside poly body must not stick voices",
          "[hot_swap][regression][drone]") {
    // Full user patch (program A) and the same patch with the delay line
    // removed (program B) — exactly the edit the user performed.
    constexpr const char* kSrcA =
        "bpm = 110\n"
        "stab = ({freq, gate, vel}) ->\n"
        "    saw(freq) * ar(gate, 0.05, 0.4) * vel\n"
        "    |> lp(@, 1200)\n"
        "    |> delay(@, 0.3, 0.1)\n"
        "\n"
        "c\"C Em Am G\" |> poly(@, stab) * 0.3 |> out(@)\n";
    constexpr const char* kSrcB =
        "bpm = 110\n"
        "stab = ({freq, gate, vel}) ->\n"
        "    saw(freq) * ar(gate, 0.05, 0.4) * vel\n"
        "    |> lp(@, 1200)\n"
        "\n"
        "c\"C Em Am G\" |> poly(@, stab) * 0.3 |> out(@)\n";

    auto run_scenario = [&](std::uint32_t crossfade_blocks) {
        auto cr_a = akkado::compile(kSrcA);
        if (!cr_a.success) {
            std::ostringstream os;
            for (const auto& d : cr_a.diagnostics) os << "  " << d.code << ": " << d.message << "\n";
            INFO("Compile A diagnostics:\n" << os.str());
        }
        REQUIRE(cr_a.success);
        auto cr_b = akkado::compile(kSrcB);
        if (!cr_b.success) {
            std::ostringstream os;
            for (const auto& d : cr_b.diagnostics) os << "  " << d.code << ": " << d.message << "\n";
            INFO("Compile B diagnostics:\n" << os.str());
        }
        REQUIRE(cr_b.success);

        const std::uint32_t seq_state_id =
            find_state_init_id(cr_a, akkado::StateInitData::Type::SequenceProgram);
        const std::uint32_t poly_state_id = find_poly_state_id(cr_a);
        REQUIRE(seq_state_id != 0);
        REQUIRE(poly_state_id != 0);
        // Stable state_ids across the edit: the chord pattern source is
        // unchanged, and the poly() call site has not moved. If these shift,
        // the bug analysis changes (H5 in the plan), so capture explicitly.
        CHECK(find_state_init_id(cr_b, akkado::StateInitData::Type::SequenceProgram)
              == seq_state_id);
        CHECK(find_poly_state_id(cr_b) == poly_state_id);

        cedar::VM vm;
        vm.set_crossfade_blocks(crossfade_blocks);
        std::vector<std::vector<cedar::Sequence>> seq_storage;
        live_load(vm, cr_a, seq_storage);

        std::array<float, cedar::BLOCK_SIZE> left{}, right{};

        // Program A: render long enough for the pattern to advance several
        // cycles. At 110 BPM, cycle_length=1 beat → ~0.545 s per cycle.
        // 2 seconds covers ~3.7 cycles, more than enough to see voices
        // allocated and released.
        const std::size_t blocks_per_sec =
            static_cast<std::size_t>(vm.context().sample_rate) / cedar::BLOCK_SIZE;
        const std::size_t blocks_a = blocks_per_sec * 2;
        for (std::size_t i = 0; i < blocks_a; ++i) {
            vm.process_block(left.data(), right.data());
        }

        auto* seq_pre = vm.states().get_if<cedar::SequenceState>(seq_state_id);
        REQUIRE(seq_pre != nullptr);
        const std::uint32_t cycle_pre = seq_pre->cycle_index;
        INFO("crossfade_blocks=" << crossfade_blocks
             << " cycle_index after A: " << cycle_pre);
        REQUIRE(cycle_pre > 0);

        auto* poly_pre = vm.states().get_if<cedar::PolyAllocState>(poly_state_id);
        REQUIRE(poly_pre != nullptr);
        REQUIRE(poly_pre->voices != nullptr);

        // Snapshot per-voice age + active mask before the swap. After the
        // swap we want to see at least one of: (a) a voice's age has reset
        // (it was retriggered or reused for a new note), (b) a voice was
        // observed releasing, (c) a previously-active voice has gone inactive.
        std::array<std::uint32_t, cedar::PolyAllocState::MAX_VOICES> pre_age{};
        std::array<bool, cedar::PolyAllocState::MAX_VOICES> pre_active{};
        for (std::uint16_t v = 0; v < poly_pre->max_voices; ++v) {
            pre_age[v] = poly_pre->voices[v].age;
            pre_active[v] = poly_pre->voices[v].active;
        }

        // Live recompile to program B.
        live_load(vm, cr_b, seq_storage);

        // Program B: render another 2 seconds and watch playback.
        const std::size_t blocks_b = blocks_per_sec * 2;
        std::vector<float> mono_out;
        mono_out.reserve(blocks_b * cedar::BLOCK_SIZE);
        bool observed_voice_retrigger_or_release = false;
        bool observed_pattern_advance = false;
        bool observed_releasing_flag = false;
        VoiceFireCounter fires{&vm, poly_state_id, seq_state_id};
        for (std::size_t i = 0; i < blocks_b; ++i) {
            vm.process_block(left.data(), right.data());
            for (float s : left) mono_out.push_back(s);
            fires.step();

            auto* seq_now = vm.states().get_if<cedar::SequenceState>(seq_state_id);
            if (seq_now && seq_now->cycle_index > cycle_pre) {
                observed_pattern_advance = true;
            }
            auto* poly_now = vm.states().get_if<cedar::PolyAllocState>(poly_state_id);
            if (poly_now && poly_now->voices) {
                for (std::uint16_t v = 0; v < poly_now->max_voices; ++v) {
                    const auto& voice = poly_now->voices[v];
                    if (voice.releasing) observed_releasing_flag = true;
                    // Age reset: voice was reallocated/retriggered.
                    if (pre_active[v] && voice.active && voice.age < pre_age[v]) {
                        observed_voice_retrigger_or_release = true;
                    }
                    // Active → inactive: voice was released and timed out.
                    if (pre_active[v] && !voice.active) {
                        observed_voice_retrigger_or_release = true;
                    }
                }
            }
        }
        if (observed_releasing_flag) observed_voice_retrigger_or_release = true;

        // Diagnostic readout for failure analysis.
        const std::uint32_t live_xor = vm.states().state_id_xor();
        INFO("post-B state_id_xor: " << live_xor
             << " seq_state_id=" << seq_state_id
             << " poly_state_id=" << poly_state_id
             << " pool_size=" << vm.states().size());
        // Dump all state ids in cr_b for cross-reference.
        std::ostringstream all_inits_os;
        for (const auto& init : cr_b.state_inits) {
            all_inits_os << "init{type=" << static_cast<int>(init.type)
                         << " id=" << init.state_id << "} ";
        }
        INFO("post-B cr_b state_inits: " << all_inits_os.str());
        // Dump bytecode's POLY-class opcode state_ids.
        std::ostringstream poly_opcodes_os;
        const std::size_t n_inst_b = cr_b.bytecode.size() / sizeof(cedar::Instruction);
        for (std::size_t k = 0; k < n_inst_b; ++k) {
            cedar::Instruction ins{};
            std::memcpy(&ins, cr_b.bytecode.data() + k * sizeof(cedar::Instruction),
                        sizeof(ins));
            if (ins.opcode == cedar::Opcode::POLY_BEGIN ||
                ins.opcode == cedar::Opcode::FOREACH_EVENT) {
                poly_opcodes_os << "[" << k << "]op="
                                << static_cast<int>(ins.opcode)
                                << " state_id=" << ins.state_id << " ";
            }
        }
        INFO("post-B cr_b POLY/FOREACH opcodes: " << poly_opcodes_os.str());
        auto* seq_post = vm.states().get_if<cedar::SequenceState>(seq_state_id);
        auto* poly_post = vm.states().get_if<cedar::PolyAllocState>(poly_state_id);
        INFO("post-B cycle_index: "
             << (seq_post ? static_cast<int>(seq_post->cycle_index) : -1)
             << " last_queried_cycle: "
             << (seq_post ? seq_post->last_queried_cycle : -1.0f)
             << " output.num_events: "
             << (seq_post ? static_cast<int>(seq_post->output.num_events) : -1));
        std::ostringstream voice_os;
        if (poly_post == nullptr) {
            voice_os << "(poly_post=null)";
        } else if (poly_post->voices == nullptr) {
            voice_os << "(voices=null, max_voices=" << int(poly_post->max_voices)
                     << " seq_state_id=" << poly_post->seq_state_id << ")";
        } else {
            voice_os << "max_voices=" << int(poly_post->max_voices)
                     << " seq_state_id=" << poly_post->seq_state_id << " ";
            for (std::uint16_t v = 0; v < poly_post->max_voices; ++v) {
                const auto& vc = poly_post->voices[v];
                if (vc.active || vc.releasing) {
                    voice_os << "v" << v << "{a=" << vc.active << " r=" << vc.releasing
                             << " g=" << vc.gate << " freq=" << vc.freq
                             << " age=" << vc.age << "} ";
                }
            }
        }
        INFO("post-B voices: " << voice_os.str());

        // First/middle/last block samples to characterize the stuck audio.
        std::ostringstream sample_os;
        sample_os << "samples[0..4]=";
        for (std::size_t k = 0; k < 4 && k < mono_out.size(); ++k) {
            sample_os << mono_out[k] << ",";
        }
        const std::size_t mid_off = mono_out.size() / 2;
        sample_os << " samples[mid..mid+4]=";
        for (std::size_t k = 0; k < 4 && mid_off + k < mono_out.size(); ++k) {
            sample_os << mono_out[mid_off + k] << ",";
        }
        INFO("post-B " << sample_os.str());

        // Count states in the fading pool (orphaned by the swap). A nonzero
        // count past the fade window means an orphaned state is somehow still
        // driving output.
        INFO("post-B fading_count: " << vm.states().fading_count());

        // (A) Pattern keeps advancing.
        CHECK(observed_pattern_advance);
        // (B) Voices retrigger or release at least once in post-swap window.
        CHECK(observed_voice_retrigger_or_release);

        // Layer A: per-event voice-fire count over the post-swap window.
        // The "any voice retriggered" check above passes if even one voice
        // fires once; this aggregate check makes sure the count of voice
        // allocations matches the count of NoteOn onsets the pattern emits.
        INFO("post-B voice fires expected=" << fires.total_expected_fires
             << " observed=" << fires.total_observed_fires
             << " blocks_with_underfire=" << fires.blocks_with_underfire
             << " blocks_with_overfire=" << fires.blocks_with_overfire
             << " first_underfire=" << fires.first_underfire.str()
             << " first_overfire=" << fires.first_overfire.str());
        REQUIRE(fires.total_expected_fires > 0);
        CHECK(fires.blocks_with_underfire == 0u);
        CHECK(fires.total_observed_fires >= fires.total_expected_fires);

        // (C) Audio is not a stuck periodic tone. Two non-overlapping
        // 512-sample windows ≥ 0.5 s apart must differ. A frozen voice
        // playing a band-limited saw eventually settles to a periodic
        // waveform with constant per-block RMS; both checks catch that.
        const std::size_t half_sec =
            static_cast<std::size_t>(vm.context().sample_rate) / 2;
        const std::size_t win = 512;
        REQUIRE(mono_out.size() >= half_sec + win + cedar::BLOCK_SIZE);
        std::span<const float> w0(mono_out.data() + cedar::BLOCK_SIZE, win);
        std::span<const float> w1(mono_out.data() + half_sec + cedar::BLOCK_SIZE, win);
        bool windows_bit_identical = (w0.size() == w1.size()) &&
            std::memcmp(w0.data(), w1.data(), w0.size() * sizeof(float)) == 0;
        CHECK_FALSE(windows_bit_identical);

        // Block-to-block RMS variance: gather per-128-sample RMS across the
        // post-swap window, compute stddev. A live pattern with attack/release
        // envelopes produces large RMS variance; a frozen voice does not.
        std::vector<float> per_block_rms;
        per_block_rms.reserve(mono_out.size() / cedar::BLOCK_SIZE);
        for (std::size_t i = 0; i + cedar::BLOCK_SIZE <= mono_out.size();
             i += cedar::BLOCK_SIZE) {
            per_block_rms.push_back(
                block_rms({mono_out.data() + i, cedar::BLOCK_SIZE}));
        }
        REQUIRE(per_block_rms.size() > 4);
        double sum = 0.0;
        for (float r : per_block_rms) sum += r;
        const double mean = sum / per_block_rms.size();
        double var = 0.0;
        for (float r : per_block_rms) var += (r - mean) * (r - mean);
        const double stddev = std::sqrt(var / per_block_rms.size());
        INFO("post-B per-block RMS mean=" << mean << " stddev=" << stddev);
        // The user's patch envelopes a chord every ~0.55 s with attack 0.05
        // and release 0.4 — block-to-block RMS varies a lot. A frozen voice
        // gives stddev ~ 0. Use 5% of the mean as a generous floor.
        CHECK(stddev > 0.05 * std::max(mean, 1e-6));
    };

    SECTION("crossfade disabled") {
        run_scenario(0);
    }
    SECTION("default crossfade") {
        run_scenario(3);
    }
}

// ============================================================================
// Regression: hot-swap inside a unison()/poly() body with ADSR + reverb
// ============================================================================
//
// Second user report after the first fix (commit 1fd6c95) landed: editing
// ANY value in the patch below — ADSR times, reverb wet, lp cutoff, even
// just whitespace inside the closure body — causes the same drone-forever
// symptom. This patch is more nested than the first repro:
//
//   chord("...").transpose(N)  ->  EVENT_REORDER / EVENT_MAP transform state
//          |> poly(@, fat)     ->  FOREACH_EVENT(VOICE_POOL) PolyAllocState
//          |> reverb(@, ...)   ->  DattorroState (or whichever reverb opcode)
//
// fat() is a named userspace fn whose body calls unison() — a stdlib fn that
// statically fans pad_voice() into N detuned voices. So the inner saw/adsr/lp
// states live behind BLOCK_CALL with per-voice + per-call-site state_id_xor.
//
// Test contract is the same as the prior poly() drone test: pattern must
// advance, voices must retrigger/release, audio must not be a stuck periodic
// waveform.

TEST_CASE("Hot-swap inside poly+unison body (adsr edit) must not stick voices",
          "[hot_swap][regression][drone][adsr]") {
    constexpr const char* kSrcA =
        "bpm = 72\n"
        "\n"
        "fn pad_voice(freq, gate, vel, ext) ->\n"
        "    saw(freq, ext.phase) * adsr(gate, 0.5, 0.6, 0.7, 1.2) * vel\n"
        "    |> lp(@, 2800)\n"
        "\n"
        "fn fat({freq, gate, vel}) ->\n"
        "    unison(freq, gate, vel, pad_voice,\n"
        "           voices: 4, detune: 0.3, width: .7, phase: 0.2)*0.2\n"
        "\n"
        "chord(\"Cmaj9 Am11@2 Fmaj7 G9@2\").transpose(-12)\n"
        "    |> poly(@, fat)\n"
        "    |> reverb(@, 0.75, 0.6, ..{wet: 0.1})\n"
        "    |> out(@)\n";
    // Program B: same as A but with the ADSR attack time changed
    // (0.5 -> 0.6). This is the exact kind of "edit anything" the user
    // reports as triggering the drone.
    constexpr const char* kSrcB =
        "bpm = 72\n"
        "\n"
        "fn pad_voice(freq, gate, vel, ext) ->\n"
        "    saw(freq, ext.phase) * adsr(gate, 0.6, 0.6, 0.7, 1.2) * vel\n"
        "    |> lp(@, 2800)\n"
        "\n"
        "fn fat({freq, gate, vel}) ->\n"
        "    unison(freq, gate, vel, pad_voice,\n"
        "           voices: 4, detune: 0.3, width: .7, phase: 0.2)*0.2\n"
        "\n"
        "chord(\"Cmaj9 Am11@2 Fmaj7 G9@2\").transpose(-12)\n"
        "    |> poly(@, fat)\n"
        "    |> reverb(@, 0.75, 0.6, ..{wet: 0.1})\n"
        "    |> out(@)\n";

    auto run_scenario = [&](std::uint32_t crossfade_blocks) {
        auto cr_a = akkado::compile(kSrcA);
        if (!cr_a.success) {
            std::ostringstream os;
            for (const auto& d : cr_a.diagnostics) os << "  " << d.code << ": " << d.message << "\n";
            INFO("Compile A diagnostics:\n" << os.str());
        }
        REQUIRE(cr_a.success);
        auto cr_b = akkado::compile(kSrcB);
        if (!cr_b.success) {
            std::ostringstream os;
            for (const auto& d : cr_b.diagnostics) os << "  " << d.code << ": " << d.message << "\n";
            INFO("Compile B diagnostics:\n" << os.str());
        }
        REQUIRE(cr_b.success);

        const std::uint32_t seq_state_id =
            find_state_init_id(cr_a, akkado::StateInitData::Type::SequenceProgram);
        const std::uint32_t poly_state_id = find_poly_state_id(cr_a);
        REQUIRE(seq_state_id != 0);
        REQUIRE(poly_state_id != 0);

        cedar::VM vm;
        vm.set_crossfade_blocks(crossfade_blocks);
        std::vector<std::vector<cedar::Sequence>> seq_storage;
        live_load(vm, cr_a, seq_storage);

        std::array<float, cedar::BLOCK_SIZE> left{}, right{};
        const std::size_t blocks_per_sec =
            static_cast<std::size_t>(vm.context().sample_rate) / cedar::BLOCK_SIZE;

        // The chord pattern has @2 duration modifiers, total cycle length =
        // 6 beats. At 72 BPM that's 5 s per cycle. Run program A for ~6 s
        // so we cross a cycle boundary and voices have allocated and aged.
        const std::size_t blocks_a = blocks_per_sec * 6;
        for (std::size_t i = 0; i < blocks_a; ++i) {
            vm.process_block(left.data(), right.data());
        }

        auto* seq_pre = vm.states().get_if<cedar::SequenceState>(seq_state_id);
        REQUIRE(seq_pre != nullptr);
        const std::uint32_t cycle_pre = seq_pre->cycle_index;
        const float last_beat_pre = seq_pre->last_beat_pos;
        INFO("crossfade_blocks=" << crossfade_blocks
             << " cycle_index after A: " << cycle_pre
             << " last_beat_pos after A: " << last_beat_pre);
        // Pattern must have at least started — last_beat_pos > 0 even within
        // cycle 0. Use this rather than cycle_index since cycle_length is 6.
        REQUIRE(last_beat_pre > 0.0f);

        auto* poly_pre = vm.states().get_if<cedar::PolyAllocState>(poly_state_id);
        REQUIRE(poly_pre != nullptr);
        REQUIRE(poly_pre->voices != nullptr);
        std::array<std::uint32_t, cedar::PolyAllocState::MAX_VOICES> pre_age{};
        std::array<bool, cedar::PolyAllocState::MAX_VOICES> pre_active{};
        for (std::uint16_t v = 0; v < poly_pre->max_voices; ++v) {
            pre_age[v] = poly_pre->voices[v].age;
            pre_active[v] = poly_pre->voices[v].active;
        }

        // Hot-swap to program B.
        live_load(vm, cr_b, seq_storage);

        // Render another ~6 s — long enough to cross at least one cycle
        // boundary after the swap, ensuring all chord events fire.
        const std::size_t blocks_b = blocks_per_sec * 6;
        std::vector<float> mono_out;
        mono_out.reserve(blocks_b * cedar::BLOCK_SIZE);
        bool observed_voice_retrigger_or_release = false;
        bool observed_pattern_advance = false;
        VoiceFireCounter fires{&vm, poly_state_id, seq_state_id};
        for (std::size_t i = 0; i < blocks_b; ++i) {
            vm.process_block(left.data(), right.data());
            for (float s : left) mono_out.push_back(s);
            fires.step();

            auto* seq_now = vm.states().get_if<cedar::SequenceState>(seq_state_id);
            if (seq_now && (seq_now->cycle_index > cycle_pre ||
                            seq_now->last_beat_pos > last_beat_pre + 0.5f)) {
                observed_pattern_advance = true;
            }
            auto* poly_now = vm.states().get_if<cedar::PolyAllocState>(poly_state_id);
            if (poly_now && poly_now->voices) {
                for (std::uint16_t v = 0; v < poly_now->max_voices; ++v) {
                    const auto& voice = poly_now->voices[v];
                    if (voice.releasing) observed_voice_retrigger_or_release = true;
                    if (pre_active[v] && voice.active && voice.age < pre_age[v]) {
                        observed_voice_retrigger_or_release = true;
                    }
                    if (pre_active[v] && !voice.active) {
                        observed_voice_retrigger_or_release = true;
                    }
                }
            }
        }

        // Diagnostics for failure analysis.
        auto* seq_post = vm.states().get_if<cedar::SequenceState>(seq_state_id);
        auto* poly_post = vm.states().get_if<cedar::PolyAllocState>(poly_state_id);
        INFO("post-B cycle_index: "
             << (seq_post ? static_cast<int>(seq_post->cycle_index) : -1)
             << " last_queried_cycle: "
             << (seq_post ? seq_post->last_queried_cycle : -1.0f)
             << " last_beat_pos: "
             << (seq_post ? seq_post->last_beat_pos : -1.0f)
             << " output.num_events: "
             << (seq_post ? static_cast<int>(seq_post->output.num_events) : -1)
             << " pool_size: " << vm.states().size()
             << " fading: " << vm.states().fading_count());
        std::ostringstream voice_os;
        if (poly_post == nullptr) {
            voice_os << "(poly_post=null)";
        } else if (poly_post->voices == nullptr) {
            voice_os << "(voices=null)";
        } else {
            voice_os << "max_voices=" << int(poly_post->max_voices) << " ";
            for (std::uint16_t v = 0; v < poly_post->max_voices; ++v) {
                const auto& vc = poly_post->voices[v];
                if (vc.active || vc.releasing) {
                    voice_os << "v" << v << "{a=" << vc.active << " r=" << vc.releasing
                             << " g=" << vc.gate << " freq=" << vc.freq << "} ";
                }
            }
        }
        INFO("post-B voices: " << voice_os.str());
        // Dump every cr_b state_init for cross-reference if assertions fail.
        std::ostringstream all_inits_os;
        for (const auto& init : cr_b.state_inits) {
            all_inits_os << "{type=" << static_cast<int>(init.type)
                         << " id=" << init.state_id << "} ";
        }
        INFO("post-B cr_b state_inits: " << all_inits_os.str());

        CHECK(observed_pattern_advance);
        CHECK(observed_voice_retrigger_or_release);

        // Layer A: per-event voice-fire count. Every event onset that lands
        // inside the post-swap window must allocate `event.num_values` voices.
        // Catches "half the unison cluster died", "gate-off arrived early",
        // "events stopped firing mid-window" — all of which slip past the
        // RMS-variance check below.
        INFO("post-B voice fires expected=" << fires.total_expected_fires
             << " observed=" << fires.total_observed_fires
             << " blocks_with_underfire=" << fires.blocks_with_underfire
             << " blocks_with_overfire=" << fires.blocks_with_overfire
             << " first_underfire=" << fires.first_underfire.str()
             << " first_overfire=" << fires.first_overfire.str());
        REQUIRE(fires.total_expected_fires > 0);
        CHECK(fires.blocks_with_underfire == 0u);
        CHECK(fires.total_observed_fires >= fires.total_expected_fires);

        const std::size_t half_sec =
            static_cast<std::size_t>(vm.context().sample_rate) / 2;
        const std::size_t win = 512;
        REQUIRE(mono_out.size() >= half_sec + win + cedar::BLOCK_SIZE);
        std::span<const float> w0(mono_out.data() + cedar::BLOCK_SIZE, win);
        std::span<const float> w1(mono_out.data() + half_sec + cedar::BLOCK_SIZE, win);
        bool windows_bit_identical = (w0.size() == w1.size()) &&
            std::memcmp(w0.data(), w1.data(), w0.size() * sizeof(float)) == 0;
        CHECK_FALSE(windows_bit_identical);

        std::vector<float> per_block_rms;
        per_block_rms.reserve(mono_out.size() / cedar::BLOCK_SIZE);
        for (std::size_t i = 0; i + cedar::BLOCK_SIZE <= mono_out.size();
             i += cedar::BLOCK_SIZE) {
            per_block_rms.push_back(
                block_rms({mono_out.data() + i, cedar::BLOCK_SIZE}));
        }
        REQUIRE(per_block_rms.size() > 4);
        double sum = 0.0;
        for (float r : per_block_rms) sum += r;
        const double mean = sum / per_block_rms.size();
        double var = 0.0;
        for (float r : per_block_rms) var += (r - mean) * (r - mean);
        const double stddev = std::sqrt(var / per_block_rms.size());
        INFO("post-B per-block RMS mean=" << mean << " stddev=" << stddev);
        CHECK(stddev > 0.05 * std::max(mean, 1e-6));
    };

    SECTION("crossfade disabled") {
        run_scenario(0);
    }
    SECTION("default crossfade") {
        run_scenario(3);
    }
}


// ============================================================================
// Deeper repro attempt: multiple consecutive hot-swaps with varied edits
// ============================================================================
//
// User reports the drone still occurs in the browser after the FOREACH_EVENT
// touch fix and the prior regression test (which passes). User notes the bug
// becomes "reliable" after a page restart and triggers on editing ANY line.
// The single-swap test above does not reproduce — so either the bug is
// (a) a stale browser cache, (b) requires multiple swaps to accumulate,
// (c) requires a specific edit shape that the prior test misses.
//
// This test exercises (b) and (c) together: load the original patch, then
// do five consecutive hot-swaps each touching a different param (adsr times,
// lp cutoff, reverb wet, unison voices count, transpose value), rendering
// ~3 s between each. After each swap, PolyAllocState must survive and the
// audio per-block RMS variance must be > 2% of the mean.

TEST_CASE("Hot-swap chain — five varied edits must not stick voices",
          "[hot_swap][regression][drone][chain]") {
    auto make_src = [](float adsr_a, int lp_cut, float rev_wet,
                       int u_voices, int transpose) {
        std::ostringstream os;
        os << "bpm = 72\n\n"
           << "fn pad_voice(freq, gate, vel, ext) ->\n"
           << "    saw(freq, ext.phase) * adsr(gate, " << adsr_a
           << ", 0.6, 0.7, 1.2) * vel\n"
           << "    |> lp(@, " << lp_cut << ")\n\n"
           << "fn fat({freq, gate, vel}) ->\n"
           << "    unison(freq, gate, vel, pad_voice,\n"
           << "           voices: " << u_voices
           << ", detune: 0.3, width: .7, phase: 0.2)*0.2\n\n"
           << "chord(\"Cmaj9 Am11@2 Fmaj7 G9@2\").transpose(" << transpose << ")\n"
           << "    |> poly(@, fat)\n"
           << "    |> reverb(@, 0.75, 0.6, ..{wet: " << rev_wet << "})\n"
           << "    |> out(@)\n";
        return os.str();
    };

    struct Edit { const char* desc; float adsr_a; int lp_cut; float rev_wet; int u_voices; int transpose; };
    // Note: unison voices stays at 4 — voices: 6 exhausts the bytecode buffer
    // pool on this nested patch (compile error E101). The bug under
    // investigation is the runtime drone, not the codegen capacity limit.
    const Edit edits[] = {
        {"base",            0.5f, 2800, 0.1f, 4, -12},
        {"adsr attack",     0.6f, 2800, 0.1f, 4, -12},
        {"lp cutoff",       0.6f, 3200, 0.1f, 4, -12},
        {"reverb wet",      0.6f, 3200, 0.2f, 4, -12},
        {"transpose",       0.6f, 3200, 0.2f, 4,  -7},
        {"adsr release",    0.6f, 3200, 0.2f, 4,  -7},  // bumps the 1.2 implicit but actually re-emit base
    };

    auto run_scenario = [&](std::uint32_t crossfade_blocks) {
        cedar::VM vm;
        vm.set_crossfade_blocks(crossfade_blocks);
        std::vector<std::vector<cedar::Sequence>> seq_storage;

        const std::size_t blocks_per_sec =
            static_cast<std::size_t>(vm.context().sample_rate) / cedar::BLOCK_SIZE;
        std::array<float, cedar::BLOCK_SIZE> left{}, right{};

        auto cr0 = akkado::compile(make_src(edits[0].adsr_a, edits[0].lp_cut,
                                            edits[0].rev_wet, edits[0].u_voices,
                                            edits[0].transpose));
        if (!cr0.success) {
            std::ostringstream os;
            for (const auto& d : cr0.diagnostics) os << "  " << d.code << ": " << d.message << "\n";
            INFO("Compile base diagnostics:\n" << os.str());
        }
        REQUIRE(cr0.success);
        live_load(vm, cr0, seq_storage);

        for (std::size_t i = 0; i < blocks_per_sec * 3; ++i) {
            vm.process_block(left.data(), right.data());
        }

        std::uint32_t poly_id = find_poly_state_id(cr0);
        REQUIRE(poly_id != 0);
        std::uint32_t seq_id =
            find_state_init_id(cr0, akkado::StateInitData::Type::SequenceProgram);

        // Aggregate voice-fire counter spanning all edits in the chain.
        // Catches the "drone forever" symptom: if any edit kills the voice
        // pool, observed_fires falls to zero while expected_fires keeps
        // climbing — the aggregate inequality fails loudly.
        VoiceFireCounter fires{&vm, poly_id, seq_id};

        for (std::size_t e = 1; e < sizeof(edits) / sizeof(edits[0]); ++e) {
            INFO("crossfade_blocks=" << crossfade_blocks
                 << " edit #" << e << ": " << edits[e].desc);
            auto src_e = make_src(edits[e].adsr_a, edits[e].lp_cut,
                                  edits[e].rev_wet, edits[e].u_voices,
                                  edits[e].transpose);
            auto cr = akkado::compile(src_e);
            if (!cr.success) {
                std::ostringstream dbg;
                dbg << "[CHAIN] edit #" << e << " (" << edits[e].desc
                    << ") compile failed, diag count=" << cr.diagnostics.size()
                    << ", bytecode size=" << cr.bytecode.size() << "\n";
                for (std::size_t di = 0; di < cr.diagnostics.size(); ++di) {
                    const auto& d = cr.diagnostics[di];
                    dbg << "  [" << di << "] code=" << d.code
                        << " sev=" << static_cast<int>(d.severity)
                        << " msg=[" << d.message << "]\n";
                }
                dbg << "[CHAIN] source (" << src_e.size() << " chars):\n----\n"
                    << src_e << "\n----\n";
                std::fwrite(dbg.str().data(), 1, dbg.str().size(), stderr);
                std::fflush(stderr);
            }
            REQUIRE(cr.success);
            live_load(vm, cr, seq_storage);

            std::vector<float> mono;
            const std::size_t blocks = blocks_per_sec * 3;
            mono.reserve(blocks * cedar::BLOCK_SIZE);
            for (std::size_t i = 0; i < blocks; ++i) {
                vm.process_block(left.data(), right.data());
                for (float s : left) mono.push_back(s);
                fires.step();
            }

            auto* poly_after = vm.states().get_if<cedar::PolyAllocState>(poly_id);
            INFO("after edit #" << e << " poly_state present=" << (poly_after ? 1 : 0)
                 << " pool_size=" << vm.states().size()
                 << " fading=" << vm.states().fading_count());
            CHECK(poly_after != nullptr);

            auto* seq_after = vm.states().get_if<cedar::SequenceState>(seq_id);
            INFO("after edit #" << e << " seq cycle_index="
                 << (seq_after ? int(seq_after->cycle_index) : -1)
                 << " last_beat_pos=" << (seq_after ? seq_after->last_beat_pos : -1.0f));
            CHECK(seq_after != nullptr);

            std::vector<float> rms;
            for (std::size_t i = 0; i + cedar::BLOCK_SIZE <= mono.size();
                 i += cedar::BLOCK_SIZE) {
                rms.push_back(block_rms({mono.data() + i, cedar::BLOCK_SIZE}));
            }
            double sum = 0.0;
            for (float r : rms) sum += r;
            const double mean = sum / rms.size();
            double var = 0.0;
            for (float r : rms) var += (r - mean) * (r - mean);
            const double stddev = std::sqrt(var / rms.size());
            INFO("edit #" << e << " RMS mean=" << mean << " stddev=" << stddev);
            CHECK(stddev > 0.02 * std::max(mean, 1e-6));
        }

        // Layer A: aggregate fire count across all chain edits. If any edit
        // sticks the voice pool, observed will fall behind expected here.
        // Per-block strict underfire is appropriate too — each edit is
        // followed by ~250 blocks of render, plenty of time for SEQPAT_QUERY
        // to refresh and voices to allocate normally.
        INFO("chain voice fires expected=" << fires.total_expected_fires
             << " observed=" << fires.total_observed_fires
             << " blocks_with_underfire=" << fires.blocks_with_underfire
             << " blocks_with_overfire=" << fires.blocks_with_overfire
             << " first_underfire=" << fires.first_underfire.str()
             << " first_overfire=" << fires.first_overfire.str());
        REQUIRE(fires.total_expected_fires > 0);
        CHECK(fires.blocks_with_underfire == 0u);
        CHECK(fires.total_observed_fires >= fires.total_expected_fires);
    };

    SECTION("crossfade disabled — chain") {
        run_scenario(0);
    }
    SECTION("default crossfade — chain") {
        run_scenario(3);
    }
}

// ============================================================================
// Rapid-fire swap stress test
// ============================================================================
//
// Closer to what a live coder actually does: edit, save, edit-again before
// the audio thread has time to fully consume the queued program. This test
// queues a sequence of swaps with only a few blocks rendered between each
// (simulating sub-second edits) and watches for any block where the audio
// goes flat.

TEST_CASE("Hot-swap rapid-fire — small edits between blocks must not drone",
          "[hot_swap][regression][drone][rapid]") {
    auto make_src = [](float adsr_a) {
        std::ostringstream os;
        os << "bpm = 72\n\n"
           << "fn pad_voice(freq, gate, vel, ext) ->\n"
           << "    saw(freq, ext.phase) * adsr(gate, " << adsr_a
           << ", 0.6, 0.7, 1.2) * vel\n"
           << "    |> lp(@, 2800)\n\n"
           << "fn fat({freq, gate, vel}) ->\n"
           << "    unison(freq, gate, vel, pad_voice,\n"
           << "           voices: 4, detune: 0.3, width: .7, phase: 0.2)*0.2\n\n"
           << "chord(\"Cmaj9 Am11@2 Fmaj7 G9@2\").transpose(-12)\n"
           << "    |> poly(@, fat)\n"
           << "    |> reverb(@, 0.75, 0.6, ..{wet: 0.1})\n"
           << "    |> out(@)\n";
        return os.str();
    };

    auto run_scenario = [&](std::uint32_t crossfade_blocks,
                            std::size_t blocks_between_swaps) {
        cedar::VM vm;
        vm.set_crossfade_blocks(crossfade_blocks);
        std::vector<std::vector<cedar::Sequence>> seq_storage;

        const std::size_t blocks_per_sec =
            static_cast<std::size_t>(vm.context().sample_rate) / cedar::BLOCK_SIZE;
        std::array<float, cedar::BLOCK_SIZE> left{}, right{};

        auto cr0 = akkado::compile(make_src(0.50f));
        REQUIRE(cr0.success);
        live_load(vm, cr0, seq_storage);

        // Warm-up: render long enough to allocate voices and have the
        // pattern playing actively.
        for (std::size_t i = 0; i < blocks_per_sec * 4; ++i) {
            vm.process_block(left.data(), right.data());
        }

        std::uint32_t poly_id = find_poly_state_id(cr0);
        std::uint32_t seq_id =
            find_state_init_id(cr0, akkado::StateInitData::Type::SequenceProgram);

        // Aggregate voice-fire counter spanning the edit storm + the tail.
        // The user's failure mode here is "the tail goes silent" — i.e.
        // observed_fires collapses to ~0 after the rapid-fire burst. The
        // aggregate inequality catches that cleanly. We do NOT assert
        // blocks_with_underfire == 0: with only 1-4 blocks between swaps,
        // SEQPAT_QUERY may legitimately have a stale cache for one block
        // around the swap boundary, producing a transient under-fire that
        // does not indicate the bug.
        VoiceFireCounter fires{&vm, poly_id, seq_id};

        // 20 micro-edits to the ADSR attack, each separated by only a few blocks.
        // Total sim time: 20 * blocks_between_swaps blocks ≈ 20 * 0.0027 s
        // ≈ 0.05 s — emulates a user holding down a slider for 50 ms.
        std::vector<float> mono;
        for (int e = 0; e < 20; ++e) {
            const float adsr_a = 0.5f + static_cast<float>(e) * 0.02f;
            auto cr = akkado::compile(make_src(adsr_a));
            REQUIRE(cr.success);
            live_load(vm, cr, seq_storage);

            for (std::size_t i = 0; i < blocks_between_swaps; ++i) {
                vm.process_block(left.data(), right.data());
                for (float s : left) mono.push_back(s);
                fires.step();
            }
        }

        // Now render another 4 s "tail" with no edits, to see if the
        // pattern recovers (or sticks).
        for (std::size_t i = 0; i < blocks_per_sec * 4; ++i) {
            vm.process_block(left.data(), right.data());
            for (float s : left) mono.push_back(s);
            fires.step();
        }

        auto* poly_post = vm.states().get_if<cedar::PolyAllocState>(poly_id);
        auto* seq_post = vm.states().get_if<cedar::SequenceState>(seq_id);
        INFO("crossfade_blocks=" << crossfade_blocks
             << " blocks_between=" << blocks_between_swaps
             << " poly present=" << (poly_post ? 1 : 0)
             << " seq present=" << (seq_post ? 1 : 0)
             << " pool=" << vm.states().size()
             << " fading=" << vm.states().fading_count());

        // States must survive.
        CHECK(poly_post != nullptr);
        CHECK(seq_post != nullptr);

        // Audio in the tail (last ~4 s, post-rapid-fire) must not be a stuck waveform.
        const std::size_t tail_start =
            mono.size() > blocks_per_sec * cedar::BLOCK_SIZE * 3
                ? mono.size() - blocks_per_sec * cedar::BLOCK_SIZE * 3
                : 0;
        std::vector<float> rms;
        for (std::size_t i = tail_start; i + cedar::BLOCK_SIZE <= mono.size();
             i += cedar::BLOCK_SIZE) {
            rms.push_back(block_rms({mono.data() + i, cedar::BLOCK_SIZE}));
        }
        REQUIRE(rms.size() > 4);
        double sum = 0.0;
        for (float r : rms) sum += r;
        const double mean = sum / rms.size();
        double var = 0.0;
        for (float r : rms) var += (r - mean) * (r - mean);
        const double stddev = std::sqrt(var / rms.size());
        INFO("tail RMS mean=" << mean << " stddev=" << stddev);
        CHECK(stddev > 0.02 * std::max(mean, 1e-6));

        // Layer A (aggregate-only for rapid-fire): if the tail goes silent
        // observed_fires stops growing while expected_fires keeps climbing
        // with the pattern's cycle queries. Per-block strictness is dropped
        // because the rapid-fire timing legitimately straddles SEQPAT_QUERY
        // cache invalidations.
        INFO("rapid-fire voice fires expected=" << fires.total_expected_fires
             << " observed=" << fires.total_observed_fires
             << " blocks_with_underfire=" << fires.blocks_with_underfire
             << " blocks_with_overfire=" << fires.blocks_with_overfire
             << " first_underfire=" << fires.first_underfire.str()
             << " first_overfire=" << fires.first_overfire.str());
        REQUIRE(fires.total_expected_fires > 0);
        CHECK(fires.total_observed_fires >= fires.total_expected_fires);
    };

    SECTION("crossfade=0, 1 block between swaps") {
        run_scenario(0, 1);
    }
    SECTION("crossfade=0, 4 blocks between swaps") {
        run_scenario(0, 4);
    }
    SECTION("crossfade=3, 1 block between swaps") {
        run_scenario(3, 1);
    }
    SECTION("crossfade=3, 4 blocks between swaps") {
        run_scenario(3, 4);
    }
}
