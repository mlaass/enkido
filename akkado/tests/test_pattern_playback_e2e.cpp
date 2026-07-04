// End-to-end pattern playback tests.
//
// Closes the seam between akkado::compile() and the cedar runtime: compile
// a mini-notation source, install the resulting SequenceProgram into a
// SequenceState via StatePool, then drive query_pattern() across multiple
// cycles and assert which event(s) the runtime actually picks. The existing
// test_codegen_cycle_timing.cpp only checks sequence_events sizes; the
// existing cedar/tests/test_sequence.cpp only drives hand-built Sequence
// structs. Neither catches a compile-runtime mismatch on a top-level
// alternation that mixes simple atoms with grouped subdivisions.
//
// User-reported regression (2026-05-27): `s"bd [bd ~] sd [bd <sd hh>]"` plays
// `bd` on the third cycle instead of `sd`, and similar top-level
// alternations of notes (`n"e4 ~ ~ b3 ~ ~ g4 ~"`) misbehave.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "akkado/akkado.hpp"

#include <cedar/opcodes/sequence.hpp>
#include <cedar/vm/state_pool.hpp>
#include <cedar/vm/audio_arena.hpp>
#include <cedar/vm/vm.hpp>
#include <cedar/vm/instruction.hpp>
#include <cedar/dsp/constants.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

const akkado::StateInitData* find_seq_init(const akkado::CompileResult& result) {
    for (const auto& init : result.state_inits) {
        if (init.type == akkado::StateInitData::Type::SequenceProgram) {
            return &init;
        }
    }
    return nullptr;
}

// Map each (seq_idx, event_idx) sample event to its source sample name from
// the compile result's sample_mappings. Lets the test assert "this output
// event came from sd" without depending on a populated SampleBank.
std::unordered_map<std::uint64_t, std::string> build_sample_name_index(
    const akkado::StateInitData& init) {
    std::unordered_map<std::uint64_t, std::string> idx;
    for (const auto& m : init.sequence_sample_mappings) {
        std::uint64_t key = (static_cast<std::uint64_t>(m.seq_idx) << 32) | m.event_idx;
        idx[key] = m.sample_name;
    }
    return idx;
}

// Walk events of the seq_idx'th compiled sequence and collect the sample
// names actually carried by its DATA events, in order. Used for structural
// assertions on the ALTERNATE sub-sequence.
std::vector<std::string> sample_names_in_sequence(
    const akkado::StateInitData& init, std::uint32_t seq_idx) {
    auto sample_idx = build_sample_name_index(init);
    std::vector<std::string> names;
    if (seq_idx >= init.sequence_events.size()) return names;
    const auto& events = init.sequence_events[seq_idx];
    for (std::uint32_t ev = 0; ev < events.size(); ++ev) {
        if (events[ev].type == cedar::EventType::SUB_SEQ) {
            names.push_back("<SUB_SEQ>");
        } else {
            std::uint64_t k = (static_cast<std::uint64_t>(seq_idx) << 32) | ev;
            auto it = sample_idx.find(k);
            names.push_back(it != sample_idx.end() ? it->second : "<unmapped>");
        }
    }
    return names;
}

// Harness: install the SequenceProgram into a SequenceState via the same
// StatePool path the production VM uses, then expose query_pattern() per
// cycle. We don't run audio blocks — query_pattern is the function the
// runtime ultimately invokes once per cycle inside op_seqpat_query, so
// driving it directly tests exactly the math we care about without
// dragging in clock / cache logic.
struct PatternHarness {
    cedar::AudioArena arena{1 << 24};  // 16 MiB
    cedar::StatePool pool;
    std::uint32_t state_id = 0;
    float cycle_length = 1.0f;

    void install(const akkado::StateInitData& init, std::uint32_t sid) {
        // sequence_events stores events by value, so it owns the event
        // memory; the Sequence structs point into those vectors.
        // init_sequence_program copies the events into arena storage,
        // so we can build temporary Sequence structs here that just
        // point at init.sequence_events[i].data() and let StatePool
        // do the copy.
        std::vector<cedar::Sequence> sequences = init.sequences;
        for (std::size_t i = 0;
             i < sequences.size() && i < init.sequence_events.size(); ++i) {
            if (!init.sequence_events[i].empty()) {
                sequences[i].events = const_cast<cedar::Event*>(
                    init.sequence_events[i].data());
                sequences[i].num_events = static_cast<std::uint32_t>(
                    init.sequence_events[i].size());
                sequences[i].capacity = sequences[i].num_events;
            }
        }
        state_id = sid;
        cycle_length = init.cycle_length;
        pool.init_sequence_program(state_id, sequences.data(), sequences.size(),
                                   init.cycle_length, init.is_sample_pattern,
                                   &arena, init.total_events);
    }

    cedar::SequenceState& state() {
        auto* st = pool.get_if<cedar::SequenceState>(state_id);
        REQUIRE(st != nullptr);
        return *st;
    }

    // Drive one cycle of query_pattern. After this returns, state.output
    // holds the events the runtime emitted for that cycle.
    const cedar::OutputEvents& cycle(std::uint64_t cycle_index) {
        cedar::query_pattern(state(), cycle_index, cycle_length);
        return state().output;
    }
};

// type_id is assigned per distinct sample name in SequenceCompiler
// (get_or_assign_type_id, starting at 1). Walk the compiled sample
// events to recover the (sample_name -> type_id) map for assertions.
std::unordered_map<std::string, std::uint16_t> sample_type_ids(
    const akkado::StateInitData& init) {
    std::unordered_map<std::string, std::uint16_t> out;
    auto sample_idx = build_sample_name_index(init);
    for (std::size_t s = 0; s < init.sequence_events.size(); ++s) {
        for (std::uint32_t e = 0; e < init.sequence_events[s].size(); ++e) {
            const auto& ev = init.sequence_events[s][e];
            if (ev.type != cedar::EventType::DATA || ev.num_values == 0) continue;
            std::uint64_t k = (static_cast<std::uint64_t>(s) << 32) | e;
            auto it = sample_idx.find(k);
            if (it == sample_idx.end()) continue;
            out[it->second] = ev.type_id;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// VM harness: drives the real op_seqpat_query path (with its cycle cache),
// not just bare query_pattern. This is what exposes cache / double-step
// bugs that only surface during block-level playback.
// ---------------------------------------------------------------------------
struct VmRenderHost {
    cedar::VM vm;
    std::vector<std::vector<cedar::Sequence>> seq_storage;
    std::vector<cedar::Instruction> insts;
    std::uint32_t pattern_state_id = 0;
};

std::vector<cedar::Instruction> get_instructions(const akkado::CompileResult& r) {
    std::vector<cedar::Instruction> insts;
    insts.resize(r.bytecode.size() / sizeof(cedar::Instruction));
    if (!insts.empty()) {
        std::memcpy(insts.data(), r.bytecode.data(), r.bytecode.size());
    }
    return insts;
}

std::unique_ptr<VmRenderHost> vm_render_setup(const akkado::CompileResult& r) {
    auto host = std::make_unique<VmRenderHost>();
    host->vm.set_sample_rate(48000.0f);
    host->vm.set_bpm(120.0f);
    host->vm.set_block_table(r.block_table, r.main_instruction_count);
    host->insts = get_instructions(r);
    REQUIRE(host->vm.load_program_immediate(
        std::span<const cedar::Instruction>(host->insts)));

    host->seq_storage.reserve(r.state_inits.size());
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
            host->seq_storage.push_back(std::move(seq_copy));
            auto& stored = host->seq_storage.back();
            host->vm.init_sequence_program_state(init.state_id, stored.data(),
                                                  stored.size(), init.cycle_length,
                                                  init.is_sample_pattern,
                                                  init.total_events);
            if (host->pattern_state_id == 0) host->pattern_state_id = init.state_id;
        } else if (init.type == akkado::StateInitData::Type::EventTransform) {
            host->vm.init_event_transform_state(init.state_id, init.cycle_length,
                                                init.is_sample_pattern,
                                                init.total_events);
        }
    }
    return host;
}

// Run N audio blocks against the VM, returning the SequenceState's view
// of the most recently queried cycle each block. Used to assert per-cycle
// behavior under the real op_seqpat_query cache.
struct VmCycleProbe {
    std::vector<std::uint16_t> type_id_at_event0;  // type_id of state.output.events[0] per cycle
    std::vector<std::uint8_t>  num_values_at_event0;
    std::vector<std::uint32_t> num_events_per_cycle;
    std::vector<float>         midi_note_at_event0;
};

VmCycleProbe vm_probe_cycles(VmRenderHost& host, std::uint32_t cycles_to_probe) {
    // BPM=120 → 0.5s/beat → 24000 samples/beat. cycle_length is 1.0 for our
    // test patterns. Block size = 128, so ~188 blocks per cycle. We probe a
    // bit past each cycle boundary to ensure SEQPAT_QUERY has fired.
    const float spb = 24000.0f;  // samples per beat at 48kHz, 120 BPM
    const std::uint32_t blocks_per_cycle = static_cast<std::uint32_t>(
        (spb / static_cast<float>(cedar::BLOCK_SIZE))) + 1;

    VmCycleProbe out;
    out.type_id_at_event0.reserve(cycles_to_probe);
    out.num_values_at_event0.reserve(cycles_to_probe);
    out.num_events_per_cycle.reserve(cycles_to_probe);
    out.midi_note_at_event0.reserve(cycles_to_probe);

    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    std::int64_t last_recorded_cycle = -1;
    for (std::uint32_t b = 0; b < cycles_to_probe * blocks_per_cycle; ++b) {
        host.vm.process_block(L.data(), R.data());

        const auto* st = host.vm.states().get_if<cedar::SequenceState>(
            host.pattern_state_id);
        if (!st) continue;
        std::int64_t cur = static_cast<std::int64_t>(st->last_queried_cycle);
        if (cur > last_recorded_cycle && cur >= 0 &&
            static_cast<std::uint32_t>(cur) < cycles_to_probe) {
            last_recorded_cycle = cur;
            if (st->output.num_events > 0) {
                out.type_id_at_event0.push_back(st->output.events[0].type_id);
                out.num_values_at_event0.push_back(st->output.events[0].num_values);
                out.midi_note_at_event0.push_back(st->output.events[0].midi_note);
            } else {
                out.type_id_at_event0.push_back(0);
                out.num_values_at_event0.push_back(0);
                out.midi_note_at_event0.push_back(0.0f);
            }
            out.num_events_per_cycle.push_back(st->output.num_events);
        }
    }
    return out;
}

// Find the trig output buffer index used by the first SEQPAT_STEP instruction
// in the compiled bytecode. SEQPAT_STEP writes its trigger pulses to
// inputs[1]; downstream consumers like ar() / poly() read from this buffer.
// Returns 0xFFFF if no SEQPAT_STEP is present (e.g. pattern was elided).
std::uint16_t find_seqpat_step_trig_buffer(const akkado::CompileResult& r) {
    std::vector<cedar::Instruction> insts = get_instructions(r);
    for (const auto& inst : insts) {
        if (inst.opcode == cedar::Opcode::SEQPAT_STEP) {
            return inst.inputs[1];
        }
    }
    return 0xFFFF;
}

// Walk a signal, count rising edges (low→high transitions across threshold).
int count_rising_edges(const std::vector<float>& buf, float threshold = 0.5f) {
    int count = 0;
    float prev = 0.0f;
    for (float v : buf) {
        if (prev < threshold && v >= threshold) ++count;
        prev = v;
    }
    return count;
}

// Render N audio blocks and return the SEQPAT_STEP trigger buffer history.
std::vector<float> render_trig_history(VmRenderHost& host,
                                        std::uint16_t trig_buf,
                                        std::uint32_t total_blocks) {
    std::vector<float> hist;
    hist.reserve(total_blocks * cedar::BLOCK_SIZE);
    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    for (std::uint32_t b = 0; b < total_blocks; ++b) {
        host.vm.process_block(L.data(), R.data());
        const float* t = host.vm.buffers().get(trig_buf);
        hist.insert(hist.end(), t, t + cedar::BLOCK_SIZE);
    }
    return hist;
}

}  // namespace

// ============================================================================
// Sample-pattern alternation: top-level atoms mixed with groups
// ============================================================================

TEST_CASE("e2e: s\"bd [bd ~] sd [bd <sd hh>]\" alternates one element per cycle",
          "[codegen][patterns][e2e][alternation]") {
    auto result = akkado::compile(R"(s"bd [bd ~] sd [bd <sd hh>]")");
    REQUIRE(result.success);
    const auto* init = find_seq_init(result);
    REQUIRE(init != nullptr);

    // Structural: cycle_length=1, root holds one SUB_SEQ event into the
    // 4-choice ALTERNATE sub-sequence (sub_seq_1).
    REQUIRE(init->cycle_length == 1.0f);
    REQUIRE(init->sequence_events.size() >= 2);
    REQUIRE(init->sequence_events[0].size() == 1);
    REQUIRE(init->sequences[1].mode == cedar::SequenceMode::ALTERNATE);
    REQUIRE(init->sequence_events[1].size() == 4);

    // sub_seq_1 events must be [bd DATA, SUB_SEQ([bd ~]), sd DATA,
    // SUB_SEQ([bd <sd hh>])] — the off-by-one bug shape would have sd in
    // event_idx 0 or 1 instead of 2.
    auto names = sample_names_in_sequence(*init, 1);
    REQUIRE(names.size() == 4);
    CHECK(names[0] == "bd");
    CHECK(names[1] == "<SUB_SEQ>");
    CHECK(names[2] == "sd");
    CHECK(names[3] == "<SUB_SEQ>");

    // Runtime: drive 4 cycles, assert which sample plays each cycle. type_id
    // is stable across the (event -> output) copy in process_event.
    auto type_ids = sample_type_ids(*init);
    REQUIRE(type_ids.count("bd") == 1);
    REQUIRE(type_ids.count("sd") == 1);
    REQUIRE(type_ids.count("hh") == 1);
    const std::uint16_t bd_id = type_ids["bd"];
    const std::uint16_t sd_id = type_ids["sd"];

    PatternHarness h;
    h.install(*init, /*state_id=*/1);

    // Cycle 0: bd (one event)
    {
        const auto& out = h.cycle(0);
        REQUIRE(out.num_events == 1);
        CHECK(out.events[0].num_values == 1);
        CHECK(out.events[0].type_id == bd_id);
    }
    // Cycle 1: [bd ~] → one bd at t=0, one rest at t=0.5 (rest = num_values 0)
    {
        const auto& out = h.cycle(1);
        REQUIRE(out.num_events == 2);
        CHECK(out.events[0].type_id == bd_id);
        CHECK(out.events[0].num_values == 1);
        CHECK(out.events[1].num_values == 0);  // rest
    }
    // Cycle 2: the failing one — should be sd
    {
        const auto& out = h.cycle(2);
        REQUIRE(out.num_events == 1);
        CHECK(out.events[0].num_values == 1);
        CHECK(out.events[0].type_id == sd_id);  // ← regression check
    }
    // Cycle 3: [bd <sd hh>] → bd at t=0, then either sd or hh at t=0.5.
    // Inner <sd hh> is queried for the first time on this cycle (its own
    // step counter starts at 0), so it picks sd.
    {
        const auto& out = h.cycle(3);
        REQUIRE(out.num_events == 2);
        CHECK(out.events[0].type_id == bd_id);
        CHECK(out.events[1].type_id == sd_id);
    }
}

// ============================================================================
// Note-pattern alternation: rests interleaved with notes
// ============================================================================

TEST_CASE("e2e: n\"e4 ~ ~ b3 ~ ~ g4 ~\" plays e4/rest/rest/b3/rest/rest/g4/rest",
          "[codegen][patterns][e2e][alternation]") {
    auto result = akkado::compile(R"(n"e4 ~ ~ b3 ~ ~ g4 ~")");
    REQUIRE(result.success);
    const auto* init = find_seq_init(result);
    REQUIRE(init != nullptr);

    // 8 top-level elements, all weight 1.0 → ALTERNATE sub-sequence with 8
    // events. cycle_length stays at 1.0.
    REQUIRE(init->cycle_length == 1.0f);
    REQUIRE(init->sequence_events.size() >= 2);
    REQUIRE(init->sequence_events[0].size() == 1);
    REQUIRE(init->sequences[1].mode == cedar::SequenceMode::ALTERNATE);
    REQUIRE(init->sequence_events[1].size() == 8);

    PatternHarness h;
    h.install(*init, /*state_id=*/1);

    auto is_rest = [](const cedar::OutputEvents& out) {
        return out.num_events == 1 && out.events[0].num_values == 0;
    };
    auto is_note = [](const cedar::OutputEvents& out, int midi) {
        if (out.num_events != 1) return false;
        if (out.events[0].num_values == 0) return false;
        return std::abs(out.events[0].midi_note - static_cast<float>(midi)) < 0.01f;
    };

    CHECK(is_note(h.cycle(0), 64));  // e4
    CHECK(is_rest(h.cycle(1)));
    CHECK(is_rest(h.cycle(2)));
    CHECK(is_note(h.cycle(3), 59));  // b3
    CHECK(is_rest(h.cycle(4)));
    CHECK(is_rest(h.cycle(5)));
    CHECK(is_note(h.cycle(6), 67));  // g4
    CHECK(is_rest(h.cycle(7)));
}

// ============================================================================
// Simple homogeneous case as a sanity check — distinguishes "ALTERNATE is
// broken end-to-end" from "ALTERNATE is broken specifically for mixed
// atom/group children".
// ============================================================================

TEST_CASE("e2e: n\"c4 d4 e4 f4\" alternates one note per cycle",
          "[codegen][patterns][e2e][alternation]") {
    auto result = akkado::compile(R"(n"c4 d4 e4 f4")");
    REQUIRE(result.success);
    const auto* init = find_seq_init(result);
    REQUIRE(init != nullptr);

    PatternHarness h;
    h.install(*init, /*state_id=*/1);

    int expected[4] = {60, 62, 64, 65};
    for (std::uint64_t c = 0; c < 4; ++c) {
        const auto& out = h.cycle(c);
        REQUIRE(out.num_events == 1);
        CHECK_THAT(out.events[0].midi_note,
                   WithinAbs(static_cast<float>(expected[c]), 0.01f));
    }
}

// ============================================================================
// Clock-derived alternation (hot-swap phase alignment, 2026-07-04).
// ALTERNATE playback position must be a pure function of the cycle number: a
// SequenceState created mid-performance (pattern added live, or a
// structurally-changed edit that skips the hot-swap snapshot restore) must
// play the same element as a state that has run since beat 0. Previously
// seq.step was a monotonic per-state counter, so a fresh pattern started at
// element 0 wherever the clock was — permanently out of phase with siblings.
// ============================================================================

TEST_CASE("e2e: fresh state joins mid-performance in phase with a veteran",
          "[codegen][patterns][e2e][alternation][hotswap]") {
    auto result = akkado::compile(R"(n"c4 d4 e4 f4")");
    REQUIRE(result.success);
    const auto* init = find_seq_init(result);
    REQUIRE(init != nullptr);

    // Veteran: queried every cycle since 0.
    PatternHarness veteran;
    veteran.install(*init, /*state_id=*/1);
    for (std::uint64_t c = 0; c <= 10; ++c) veteran.cycle(c);

    // Newcomer: fresh state whose first query is cycle 10.
    PatternHarness newcomer;
    newcomer.install(*init, /*state_id=*/2);
    const auto& nout = newcomer.cycle(10);

    // Both must play element 10 % 4 = 2 -> e4 (midi 64).
    REQUIRE(nout.num_events == 1);
    CHECK_THAT(nout.events[0].midi_note, WithinAbs(64.0f, 0.01f));
    REQUIRE(veteran.state().output.num_events == 1);
    CHECK_THAT(veteran.state().output.events[0].midi_note,
               WithinAbs(64.0f, 0.01f));
}

TEST_CASE("e2e: re-querying the same cycle is idempotent for alternation",
          "[codegen][patterns][e2e][alternation][hotswap]") {
    auto result = akkado::compile(R"(n"c4 d4 e4 f4")");
    REQUIRE(result.success);
    const auto* init = find_seq_init(result);
    REQUIRE(init != nullptr);

    PatternHarness h;
    h.install(*init, /*state_id=*/1);

    const auto& first = h.cycle(5);
    REQUIRE(first.num_events == 1);
    CHECK_THAT(first.events[0].midi_note, WithinAbs(62.0f, 0.01f));  // 5%4=1 -> d4

    // Same cycle again (as after a hot-swap mid-cycle re-query): must not
    // advance the alternation.
    const auto& second = h.cycle(5);
    REQUIRE(second.num_events == 1);
    CHECK_THAT(second.events[0].midi_note, WithinAbs(62.0f, 0.01f));
}

TEST_CASE("e2e: <c4 d4 e4>*2 fresh state derives sub-cycle steps",
          "[codegen][patterns][e2e][alternation][hotswap]") {
    auto result = akkado::compile(R"(n"<c4 d4 e4>*2")");
    REQUIRE(result.success);
    const auto* init = find_seq_init(result);
    REQUIRE(init != nullptr);

    // Two alternation steps per cycle. Fresh state at cycle 10: steps 20 and
    // 21 -> 20%3=2 (e4), 21%3=0 (c4).
    PatternHarness h;
    h.install(*init, /*state_id=*/1);
    const auto& out = h.cycle(10);
    REQUIRE(out.num_events == 2);
    CHECK_THAT(out.events[0].midi_note, WithinAbs(64.0f, 0.01f));
    CHECK_THAT(out.events[1].midi_note, WithinAbs(60.0f, 0.01f));
}

TEST_CASE("e2e: nested <c4 <d4 e4>> keeps c-d-c-e order and joins in phase",
          "[codegen][patterns][e2e][alternation][hotswap]") {
    auto result = akkado::compile(R"(n"<c4 <d4 e4>>")");
    REQUIRE(result.success);
    const auto* init = find_seq_init(result);
    REQUIRE(init != nullptr);

    // From cycle 0 the Tidal order: c4 d4 c4 e4.
    PatternHarness a;
    a.install(*init, /*state_id=*/1);
    int expected[4] = {60, 62, 60, 64};
    for (std::uint64_t c = 0; c < 4; ++c) {
        const auto& out = a.cycle(c);
        REQUIRE(out.num_events == 1);
        CHECK_THAT(out.events[0].midi_note,
                   WithinAbs(static_cast<float>(expected[c]), 0.01f));
    }

    // Fresh state at cycle 3: inner step floor(3 * 0.5) = 1 -> e4. The old
    // monotonic counter would have played d4 here.
    PatternHarness b;
    b.install(*init, /*state_id=*/2);
    const auto& out = b.cycle(3);
    REQUIRE(out.num_events == 1);
    CHECK_THAT(out.events[0].midi_note, WithinAbs(64.0f, 0.01f));
}

// ============================================================================
// VM-driven tests: exercise op_seqpat_query (with its per-cycle cache) by
// actually running audio blocks, not by calling query_pattern directly.
// This is the closest in-process analog of the WASM playback path.
// ============================================================================

TEST_CASE("e2e VM: n\"c4 d4 e4 f4\" through process_block emits one note per cycle",
          "[codegen][patterns][e2e][alternation][vm]") {
    auto result = akkado::compile(R"(n"c4 d4 e4 f4" |> sine(@) |> out(@))");
    REQUIRE(result.success);
    auto host = vm_render_setup(result);
    REQUIRE(host->pattern_state_id != 0);

    auto probe = vm_probe_cycles(*host, 4);
    REQUIRE(probe.midi_note_at_event0.size() == 4);

    float expected[4] = {60.0f, 62.0f, 64.0f, 65.0f};
    for (std::size_t c = 0; c < 4; ++c) {
        INFO("cycle " << c);
        CHECK(probe.num_events_per_cycle[c] == 1);
        CHECK_THAT(probe.midi_note_at_event0[c], WithinAbs(expected[c], 0.01f));
    }
}

TEST_CASE("e2e VM: n\"e4 ~ ~ b3 ~ ~ g4 ~\" through process_block",
          "[codegen][patterns][e2e][alternation][vm]") {
    auto result = akkado::compile(R"(n"e4 ~ ~ b3 ~ ~ g4 ~" |> sine(@) |> out(@))");
    REQUIRE(result.success);
    auto host = vm_render_setup(result);
    REQUIRE(host->pattern_state_id != 0);

    auto probe = vm_probe_cycles(*host, 8);
    REQUIRE(probe.midi_note_at_event0.size() == 8);

    // num_values == 0 means rest event; otherwise check midi_note.
    auto check_rest = [&](std::size_t c) {
        INFO("cycle " << c << " expected rest");
        CHECK(probe.num_events_per_cycle[c] == 1);
        CHECK(probe.num_values_at_event0[c] == 0);
    };
    auto check_note = [&](std::size_t c, float midi) {
        INFO("cycle " << c << " expected midi " << midi);
        CHECK(probe.num_events_per_cycle[c] == 1);
        CHECK(probe.num_values_at_event0[c] == 1);
        CHECK_THAT(probe.midi_note_at_event0[c], WithinAbs(midi, 0.01f));
    };

    check_note(0, 64.0f);  // e4
    check_rest(1);
    check_rest(2);
    check_note(3, 59.0f);  // b3
    check_rest(4);
    check_rest(5);
    check_note(6, 67.0f);  // g4
    check_rest(7);
}

// Patch welcome/16-snare-roll.akk: top-level pattern that mixes simple
// atoms, rests, groups, AND a MiniSequence as the last element. The
// `<bd cp>` case is the one my earlier tests didn't cover — is_compound_node
// returns false for MiniSequence (see codegen_patterns.cpp:303), so it
// hits a different compile path than `[...]` groups.
TEST_CASE("e2e VM: snare-roll patch (16 elements ending in <bd cp>)",
          "[codegen][patterns][e2e][alternation][vm][samples]") {
    auto result = akkado::compile(
        R"(s"sd ~ sd ~ sd sd ~ sd [sd sd] sd [sd sd] sd [sd sd sd sd] sd ~ <bd cp>" |> out(@))");
    REQUIRE(result.success);
    const auto* init = find_seq_init(result);
    REQUIRE(init != nullptr);

    // 16 top-level elements → ALTERNATE with 16 events.
    REQUIRE(init->sequence_events.size() >= 2);
    REQUIRE(init->sequences[1].mode == cedar::SequenceMode::ALTERNATE);
    REQUIRE(init->sequence_events[1].size() == 16);

    auto type_ids = sample_type_ids(*init);
    REQUIRE(type_ids.count("sd") == 1);
    REQUIRE(type_ids.count("bd") == 1);
    REQUIRE(type_ids.count("cp") == 1);
    const std::uint16_t sd_id = type_ids["sd"];
    const std::uint16_t bd_id = type_ids["bd"];
    const std::uint16_t cp_id = type_ids["cp"];

    auto host = vm_render_setup(result);
    REQUIRE(host->pattern_state_id != 0);

    // Drive 16 cycles, then 16 more (to verify <bd cp> wraps to cp on the
    // second time the last slot is selected — i.e. cycle 31).
    auto probe = vm_probe_cycles(*host, 32);
    REQUIRE(probe.num_events_per_cycle.size() == 32);

    // Expected per-cycle first-event identity:
    //   0  sd      1  rest   2  sd      3  rest
    //   4  sd      5  sd     6  rest    7  sd
    //   8  [sd sd] → first event = sd (sub_seq_2)
    //   9  sd     10  [sd sd] → sd
    //  11  sd     12  [sd sd sd sd] → sd
    //  13  sd     14  rest
    //  15  <bd cp> → first cycle picks bd
    //  31  <bd cp> → second time the slot is selected → cp
    auto rest_at = [&](std::size_t c) {
        INFO("cycle " << c << " expected rest");
        CHECK(probe.num_values_at_event0[c] == 0);
    };
    auto sample_at = [&](std::size_t c, std::uint16_t id, const char* name) {
        INFO("cycle " << c << " expected " << name);
        CHECK(probe.num_values_at_event0[c] != 0);
        CHECK(probe.type_id_at_event0[c] == id);
    };
    sample_at(0,  sd_id, "sd");
    rest_at(1);
    sample_at(2,  sd_id, "sd");
    rest_at(3);
    sample_at(4,  sd_id, "sd");
    sample_at(5,  sd_id, "sd");
    rest_at(6);
    sample_at(7,  sd_id, "sd");
    sample_at(8,  sd_id, "sd");  // [sd sd] → first event = sd
    sample_at(9,  sd_id, "sd");
    sample_at(10, sd_id, "sd");
    sample_at(11, sd_id, "sd");
    sample_at(12, sd_id, "sd");
    sample_at(13, sd_id, "sd");
    rest_at(14);
    sample_at(15, bd_id, "bd (first <bd cp> pick)");
    // Cycle 31 = the same top-level slot 15 picked again → inner ALTERNATE
    // step advances → expect cp.
    sample_at(31, cp_id, "cp (second <bd cp> pick)");
}

TEST_CASE("e2e VM: s\"bd [bd ~] sd [bd <sd hh>]\" through process_block",
          "[codegen][patterns][e2e][alternation][vm][samples]") {
    auto result = akkado::compile(R"(s"bd [bd ~] sd [bd <sd hh>]" |> out(@))");
    REQUIRE(result.success);
    const auto* init = find_seq_init(result);
    REQUIRE(init != nullptr);

    auto type_ids = sample_type_ids(*init);
    REQUIRE(type_ids.count("bd") == 1);
    REQUIRE(type_ids.count("sd") == 1);

    auto host = vm_render_setup(result);
    REQUIRE(host->pattern_state_id != 0);

    auto probe = vm_probe_cycles(*host, 4);
    REQUIRE(probe.type_id_at_event0.size() == 4);

    const std::uint16_t bd_id = type_ids["bd"];
    const std::uint16_t sd_id = type_ids["sd"];

    {
        INFO("cycle 0 → bd");
        CHECK(probe.num_events_per_cycle[0] == 1);
        CHECK(probe.type_id_at_event0[0] == bd_id);
    }
    {
        INFO("cycle 1 → [bd ~] → bd, rest");
        CHECK(probe.num_events_per_cycle[1] == 2);
        CHECK(probe.type_id_at_event0[1] == bd_id);
    }
    {
        INFO("cycle 2 → sd (regression target)");
        CHECK(probe.num_events_per_cycle[2] == 1);
        CHECK(probe.type_id_at_event0[2] == sd_id);  // bug: comes back as bd_id
    }
    {
        INFO("cycle 3 → [bd <sd hh>] → bd, sd");
        CHECK(probe.num_events_per_cycle[3] == 2);
        CHECK(probe.type_id_at_event0[3] == bd_id);
    }
}

// ============================================================================
// Rest-beat trigger output: tests the actual SEQPAT_STEP `trig` signal, not
// just the OutputEvent num_values. The user reports clicks on every beat in
// `n"e4 ~ ~ b3 ~ ~ g4 ~" |> tri(@freq) * ar(@trig, ...)` even though state
// .output[0].num_values is 0 on the rest cycles. Either the trigger buffer
// itself goes high spuriously, or something downstream re-derives a trigger.
// These tests pin the trigger-buffer signal directly.
// ============================================================================

TEST_CASE("e2e VM trig signal: n\"c4 ~ ~ ~\" fires trig exactly once per 4 cycles",
          "[codegen][patterns][e2e][rest_trig][vm]") {
    auto result = akkado::compile(
        R"(n"c4 ~ ~ ~" |> sine(@freq) * ar(@trig, 0.001, 0.1) |> out(@))");
    REQUIRE(result.success);
    auto host = vm_render_setup(result);
    REQUIRE(host->pattern_state_id != 0);

    std::uint16_t trig_buf = find_seqpat_step_trig_buffer(result);
    REQUIRE(trig_buf != 0xFFFF);

    // BPM=120, 1 beat = 24000 samples, BLOCK_SIZE=128 → ~188 blocks/cycle.
    // 24000 samples/beat / 128 samples/block = 187.5 → use 187 (just under
    // a full cycle) so 8*blocks_per_cycle stays *under* the trigger at the
    // 8-cycle boundary instead of grazing it.
    const std::uint32_t blocks_per_cycle =
        static_cast<std::uint32_t>(24000 / cedar::BLOCK_SIZE);
    // 4 cycles = 1 full pattern period. Render 8 to verify the second pass too.
    auto trig_history = render_trig_history(*host, trig_buf, 8 * blocks_per_cycle);

    // Expectation: 1 rising edge per 4 cycles (only on cycle 0 and 4).
    // Pre-fix bug fired an extra trigger on the rest-cycle immediately
    // after every note (cycles 1, 5) because the wrap mid-block crossed
    // the just-finished cycle's stale events[0] (c4) instead of the new
    // cycle's events[0] (rest).
    CHECK(count_rising_edges(trig_history) == 2);
}

TEST_CASE("e2e VM trig signal: n\"e4 ~ ~ b3 ~ ~ g4 ~\" fires trig exactly 3 times per cycle",
          "[codegen][patterns][e2e][rest_trig][vm]") {
    auto result = akkado::compile(
        R"(n"e4 ~ ~ b3 ~ ~ g4 ~" |> tri(@freq) * ar(@trig, 0.001, 0.05) |> out(@))");
    REQUIRE(result.success);
    auto host = vm_render_setup(result);
    REQUIRE(host->pattern_state_id != 0);

    std::uint16_t trig_buf = find_seqpat_step_trig_buffer(result);
    REQUIRE(trig_buf != 0xFFFF);

    // 24000 samples/beat / 128 samples/block = 187.5 → use 187 (just under
    // a full cycle) so 8*blocks_per_cycle stays *under* the trigger at the
    // 8-cycle boundary instead of grazing it.
    const std::uint32_t blocks_per_cycle =
        static_cast<std::uint32_t>(24000 / cedar::BLOCK_SIZE);
    // 8 cycles = 1 full pattern period.
    auto trig_history = render_trig_history(*host, trig_buf, 8 * blocks_per_cycle);

    // Expectation: 3 rising edges per 8-cycle period (cycles 0, 3, 6).
    // Pre-fix bug fired triggers on every cycle that immediately followed
    // a note (cycles 1, 4, 7 in addition to the legitimate 0/3/6).
    CHECK(count_rising_edges(trig_history) == 3);
}

TEST_CASE("e2e VM trig signal: s\"bd ~ ~ ~\" fires trig exactly once per 4 cycles",
          "[codegen][patterns][e2e][rest_trig][vm]") {
    auto result = akkado::compile(R"(s"bd ~ ~ ~" |> out(@))");
    REQUIRE(result.success);
    auto host = vm_render_setup(result);
    REQUIRE(host->pattern_state_id != 0);

    std::uint16_t trig_buf = find_seqpat_step_trig_buffer(result);
    REQUIRE(trig_buf != 0xFFFF);

    // 24000 samples/beat / 128 samples/block = 187.5 → use 187 (just under
    // a full cycle) so 8*blocks_per_cycle stays *under* the trigger at the
    // 8-cycle boundary instead of grazing it.
    const std::uint32_t blocks_per_cycle =
        static_cast<std::uint32_t>(24000 / cedar::BLOCK_SIZE);
    auto trig_history = render_trig_history(*host, trig_buf, 8 * blocks_per_cycle);

    // bd ~ ~ ~ across 8 cycles → 2 trigger rises (cycles 0 and 4).
    CHECK(count_rising_edges(trig_history) == 2);
}
