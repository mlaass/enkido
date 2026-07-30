// Rapid-recompile audio-continuity fuzz harness.
//
// Companion to test_fuzz_determinism.cpp. The determinism fuzz checks that
// compile is reproducible and that a single hot-swap preserves alternation
// step. This file goes further: it drives MANY hot-swaps in sequence while
// rendering audio, and asserts that the rendered output stays clean —
// no NaN/Inf, no sample-level discontinuities, no backward beat jumps,
// no silence/burst anomalies in long-tail effects.
//
// The user-reported failure mode this is built to surface: "quickly
// repeatedly recompiling in live mode can trip up triggers, patterns and
// other systems and create audible artifacts, beat discontinuities etc."

#include <catch2/catch_test_macros.hpp>

#include "akkado/akkado.hpp"
#include "fuzz/artifact_metrics.hpp"

#include <cedar/dsp/constants.hpp>
#include <cedar/opcodes/sequence.hpp>
#include <cedar/vm/instruction.hpp>
#include <cedar/vm/state_pool.hpp>
#include <cedar/vm/vm.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Mirror of the apply-state-inits glue in test_fuzz_determinism.cpp. Each
// hot-swap call must re-apply the sequence inits so the new program's
// SequenceState lands in the pool (the state pool's snapshot/restore path
// preserves playback position when source is unchanged).
void apply_seq_state_inits(cedar::VM& vm, const akkado::CompileResult& cr,
                           std::vector<std::vector<cedar::Sequence>>& seq_storage) {
    for (const auto& init : cr.program.state_inits) {
        if (init.type != akkado::StateInitData::Type::SequenceProgram) continue;
        std::vector<cedar::Sequence> seq_copy = init.sequences;
        for (std::size_t i = 0; i < seq_copy.size() && i < init.sequence_events.size(); ++i) {
            if (!init.sequence_events[i].empty()) {
                seq_copy[i].events =
                    const_cast<cedar::Event*>(init.sequence_events[i].data());
                seq_copy[i].num_events =
                    static_cast<std::uint32_t>(init.sequence_events[i].size());
                seq_copy[i].capacity =
                    static_cast<std::uint32_t>(init.sequence_events[i].size());
            }
        }
        seq_storage.push_back(std::move(seq_copy));
        auto& stored = seq_storage.back();
        vm.init_sequence_program_state(
            init.state_id, stored.data(), stored.size(),
            init.cycle_length, init.is_sample_pattern, init.total_events);
    }
}

std::vector<cedar::Instruction> to_inst_vector(const akkado::CompileResult& cr) {
    const std::size_t n = cr.program.bytecode.size() / sizeof(cedar::Instruction);
    std::vector<cedar::Instruction> insts(n);
    std::memcpy(insts.data(), cr.program.bytecode.data(), cr.program.bytecode.size());
    return insts;
}

// Render `blocks_per_segment * (1 + swap_count)` blocks with hot-swap
// requests interspersed every `blocks_per_segment` blocks. Every hot-swap
// re-loads the SAME source — semantic IDs match, so state preservation is
// the happy path. Failures here indicate an artifact in the *machinery*,
// not in a structural mismatch.
struct StressResult {
    bool ok = false;
    std::string failure;
    fuzz::ClickDetector click_l, click_r;
    fuzz::SanityScanner sanity_l, sanity_r;
    fuzz::BeatMonotonicity beat;
    fuzz::BlockEnergyTrace energy_l;
    fuzz::EdgeCounter edges_l;       // populated for trigger-only programs
    std::uint32_t swaps_completed = 0;
    std::vector<std::uint32_t> swap_block_indices;  // for diagnostics
};

struct StressConfig {
    std::uint32_t blocks_per_segment = 50;   // ~133 ms @ 48 kHz/128
    std::uint32_t swap_count = 20;
    float click_threshold = 0.5f;            // tighten per-program if needed
    float clip_threshold = 4.0f;
    bool track_edges = false;                // true for trigger output programs
};

StressResult run_stress(const akkado::CompileResult& cr,
                        const StressConfig& cfg) {
    StressResult out;
    out.click_l.threshold = cfg.click_threshold;
    out.click_r.threshold = cfg.click_threshold;
    out.sanity_l.clip_threshold = cfg.clip_threshold;
    out.sanity_r.clip_threshold = cfg.clip_threshold;

    cedar::VM vm;
    vm.set_crossfade_blocks(3);              // mirror production default

    auto insts = to_inst_vector(cr);
    if (vm.load_program(insts) != cedar::VM::LoadResult::Success) {
        out.failure = "initial load_program failed";
        return out;
    }
    std::vector<std::vector<cedar::Sequence>> seq_storage;
    apply_seq_state_inits(vm, cr, seq_storage);

    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    std::uint32_t block_idx = 0;

    auto render_segment = [&](std::uint32_t n_blocks) {
        for (std::uint32_t b = 0; b < n_blocks; ++b) {
            vm.process_block(L.data(), R.data());
            out.click_l.observe(L.data(), cedar::BLOCK_SIZE);
            out.click_r.observe(R.data(), cedar::BLOCK_SIZE);
            out.sanity_l.observe(L.data(), cedar::BLOCK_SIZE);
            out.sanity_r.observe(R.data(), cedar::BLOCK_SIZE);
            out.beat.observe(vm.current_beat_position(), block_idx);
            out.energy_l.observe(L.data(), cedar::BLOCK_SIZE);
            if (cfg.track_edges) {
                out.edges_l.observe(L.data(), cedar::BLOCK_SIZE);
            }
            ++block_idx;
        }
    };

    // First segment establishes baseline (one block lets the initial load
    // swap in; the rest produces actual audio).
    render_segment(cfg.blocks_per_segment);

    for (std::uint32_t s = 0; s < cfg.swap_count; ++s) {
        // Mirror the worklet's retry loop: load_program may return
        // SlotBusy if the previous swap is still crossfading. Drain a few
        // blocks and retry — exactly what production audio does.
        auto insts2 = insts;             // same bytecode → same semantic IDs
        cedar::VM::LoadResult res = cedar::VM::LoadResult::SlotBusy;
        for (int retry = 0; retry < 64; ++retry) {
            res = vm.load_program(insts2);
            if (res == cedar::VM::LoadResult::Success) break;
            render_segment(1);
        }
        if (res != cedar::VM::LoadResult::Success) {
            out.failure = "load_program never succeeded at swap " +
                          std::to_string(s) + " (last result enum=" +
                          std::to_string(static_cast<int>(res)) + ")";
            return out;
        }
        out.swap_block_indices.push_back(block_idx);
        apply_seq_state_inits(vm, cr, seq_storage);
        ++out.swaps_completed;
        render_segment(cfg.blocks_per_segment);
    }

    // Validate.
    if (!out.sanity_l.ok()) {
        out.failure = "non-finite/clipping on L at sample " +
                      std::to_string(out.sanity_l.first_bad_index) +
                      " (" + out.sanity_l.first_bad_reason + ")";
        return out;
    }
    if (!out.sanity_r.ok()) {
        out.failure = "non-finite/clipping on R at sample " +
                      std::to_string(out.sanity_r.first_bad_index) +
                      " (" + out.sanity_r.first_bad_reason + ")";
        return out;
    }
    if (out.beat.backward_count > 0) {
        out.failure = "beat moved backwards " +
                      std::to_string(out.beat.backward_count) +
                      " times, worst Δ=" +
                      std::to_string(out.beat.worst_backward) +
                      " at block " + std::to_string(out.beat.worst_block);
        return out;
    }
    if (out.click_l.click_count > 0 || out.click_r.click_count > 0) {
        out.failure = "click(s) above threshold " +
                      std::to_string(cfg.click_threshold) +
                      ": L count=" + std::to_string(out.click_l.click_count) +
                      " worst=" + std::to_string(out.click_l.worst_delta) +
                      " @ sample " + std::to_string(out.click_l.worst_index) +
                      "; R count=" + std::to_string(out.click_r.click_count) +
                      " worst=" + std::to_string(out.click_r.worst_delta);
        return out;
    }
    out.ok = true;
    return out;
}

}  // namespace

// ----------------------------------------------------------------------------
// Click localization diagnostic.
// ----------------------------------------------------------------------------
// Tagged [.localize] so it stays out of the default run. Renders one
// failing scenario keeping the full audio in memory, then walks samples
// to dump every click: surrounding sample values, block index, distance
// to the nearest swap event, and the crossfade phase at that block.
// Run with:
//   akkado_tests "[.localize]"

namespace {

struct LocalizedClick {
    std::uint64_t sample_idx;
    std::uint32_t block_idx;
    float prev_sample;
    float cur_sample;
    float delta;
    std::uint32_t nearest_swap_block;
    std::int64_t blocks_from_swap;       // negative = before, positive = after
};

std::vector<LocalizedClick> render_and_localize(
    const akkado::CompileResult& cr,
    std::uint32_t blocks_per_segment,
    std::uint32_t swap_count,
    std::uint32_t crossfade_blocks,
    float click_threshold,
    std::vector<float>& out_audio,
    std::vector<std::uint32_t>& out_swap_blocks) {

    cedar::VM vm;
    vm.set_crossfade_blocks(crossfade_blocks);

    auto insts = to_inst_vector(cr);
    REQUIRE(vm.load_program(insts) == cedar::VM::LoadResult::Success);
    std::vector<std::vector<cedar::Sequence>> seq_storage;
    apply_seq_state_inits(vm, cr, seq_storage);

    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    std::uint32_t block_idx = 0;

    auto render_segment = [&](std::uint32_t n) {
        for (std::uint32_t b = 0; b < n; ++b) {
            vm.process_block(L.data(), R.data());
            out_audio.insert(out_audio.end(), L.begin(), L.end());
            ++block_idx;
        }
    };

    render_segment(blocks_per_segment);
    for (std::uint32_t s = 0; s < swap_count; ++s) {
        auto insts2 = insts;
        cedar::VM::LoadResult res = cedar::VM::LoadResult::SlotBusy;
        for (int retry = 0; retry < 64; ++retry) {
            res = vm.load_program(insts2);
            if (res == cedar::VM::LoadResult::Success) break;
            render_segment(1);
        }
        REQUIRE(res == cedar::VM::LoadResult::Success);
        out_swap_blocks.push_back(block_idx);
        apply_seq_state_inits(vm, cr, seq_storage);
        render_segment(blocks_per_segment);
    }

    // Walk audio, locate clicks, attach context.
    std::vector<LocalizedClick> clicks;
    for (std::size_t i = 1; i < out_audio.size(); ++i) {
        const float d = std::fabs(out_audio[i] - out_audio[i - 1]);
        if (d <= click_threshold) continue;
        LocalizedClick c{};
        c.sample_idx = i;
        c.block_idx = static_cast<std::uint32_t>(i / cedar::BLOCK_SIZE);
        c.prev_sample = out_audio[i - 1];
        c.cur_sample = out_audio[i];
        c.delta = d;
        // Find nearest swap block.
        std::int64_t best = INT64_MAX;
        for (std::uint32_t sb : out_swap_blocks) {
            std::int64_t df = static_cast<std::int64_t>(c.block_idx) - sb;
            if (std::llabs(df) < std::llabs(best)) {
                best = df;
                c.nearest_swap_block = sb;
            }
        }
        c.blocks_from_swap = best;
        clicks.push_back(c);
    }
    return clicks;
}

}  // namespace

TEST_CASE("recompile-audio: localize click — tonal program",
          "[.localize][diag]") {
    const char* src =
        R"(n"c4 e4 g4 b4" |> sine(%.freq) |> out(% * 0.3, % * 0.3))";
    auto cr = akkado::compile(src);
    REQUIRE(cr.success);

    std::vector<float> audio;
    std::vector<std::uint32_t> swaps;
    auto clicks = render_and_localize(
        cr, /*bps=*/60, /*swaps=*/25, /*crossfade=*/3, /*thr=*/0.5f, audio, swaps);

    // Dump up to first 10 clicks.
    std::ostringstream report;
    report << "\n  total clicks: " << clicks.size()
           << "\n  total samples: " << audio.size()
           << "\n  crossfade_blocks=3\n";
    for (std::size_t k = 0; k < clicks.size() && k < 10; ++k) {
        const auto& c = clicks[k];
        // Print sample window x[n-2..n+2] for context.
        report << "    [" << k << "] sample=" << c.sample_idx
               << " block=" << c.block_idx
               << " (blocks_from_nearest_swap=" << c.blocks_from_swap
               << ", swap_block=" << c.nearest_swap_block << ")"
               << " δ=" << c.delta
               << "\n      window:";
        for (std::int64_t off = -2; off <= 2; ++off) {
            std::int64_t pos = static_cast<std::int64_t>(c.sample_idx) + off;
            if (pos >= 0 && pos < static_cast<std::int64_t>(audio.size())) {
                report << " x[" << off << "]=" << audio[pos];
            }
        }
        report << "\n";
    }
    UNSCOPED_INFO(report.str());
    // Make the test "fail" so Catch2 prints the INFO. Cleaner than CAPTURE
    // gymnastics — this case only runs when explicitly requested.
    CHECK(clicks.empty());
}

TEST_CASE("recompile-audio: localize click — fixed-freq osc (no pattern)",
          "[.localize][diag]") {
    // Strip the pattern out — bare oscillator with constant frequency.
    // If clicks still appear, the bug is in the OSC opcode's reaction to
    // hot-swap (state lost / phase reset). If clean, the pattern->osc
    // freq-buffer path is somehow involved.
    const char* src = R"(sine(220) |> out(% * 0.3, % * 0.3))";
    auto cr = akkado::compile(src);
    REQUIRE(cr.success);

    std::vector<float> audio;
    std::vector<std::uint32_t> swaps;
    auto clicks = render_and_localize(
        cr, /*bps=*/60, /*swaps=*/25, /*crossfade=*/0, /*thr=*/0.05f,
        audio, swaps);

    std::ostringstream report;
    report << "\n  fixed-freq osc (crossfade=0): clicks=" << clicks.size()
           << " over " << audio.size() << " samples (threshold=0.05)\n";
    for (std::size_t k = 0; k < clicks.size() && k < 5; ++k) {
        const auto& c = clicks[k];
        report << "    sample=" << c.sample_idx
               << " block=" << c.block_idx
               << " (blocks_from_swap=" << c.blocks_from_swap << ")"
               << " δ=" << c.delta << " "
               << c.prev_sample << " → " << c.cur_sample << "\n";
    }
    UNSCOPED_INFO(report.str());
    CHECK(clicks.empty());
}

TEST_CASE("recompile-audio: localize click — sustained reverb",
          "[.localize][diag]") {
    // Sustained sine into freeverb. With state pool + arena deep copy,
    // reverb output should be perfectly continuous across swap. Any
    // clicks here point at something my deep copy still misses.
    const char* src = R"(sine(330) * 0.3 |> reverb(@) |> out(@, @))";
    auto cr = akkado::compile(src);
    REQUIRE(cr.success);

    for (std::uint32_t cf : {0u, 3u}) {
        std::vector<float> audio;
        std::vector<std::uint32_t> swaps;
        auto clicks = render_and_localize(
            cr, /*bps=*/50, /*swaps=*/10, /*crossfade=*/cf,
            /*thr=*/0.3f, audio, swaps);

        std::ostringstream report;
        report << "\n  sustained reverb (crossfade=" << cf << "): clicks="
               << clicks.size() << " over " << audio.size() << " samples\n";
        for (std::size_t k = 0; k < clicks.size() && k < 8; ++k) {
            const auto& c = clicks[k];
            report << "    sample=" << c.sample_idx
                   << " block=" << c.block_idx
                   << " (blocks_from_swap=" << c.blocks_from_swap << ")"
                   << " δ=" << c.delta << " "
                   << c.prev_sample << " → " << c.cur_sample << "\n";
        }
        UNSCOPED_INFO(report.str());
    }
    CHECK(true);
}

TEST_CASE("recompile-audio: localize click — crossfade=0 (isolation)",
          "[.localize][diag]") {
    // Same program, but disable the crossfade. If clicks disappear, the
    // crossfade mixing path is the source. If they remain, the click is
    // in the swap/rebind itself or in the program-execution boundary.
    const char* src =
        R"(n"c4 e4 g4 b4" |> sine(%.freq) |> out(% * 0.3, % * 0.3))";
    auto cr = akkado::compile(src);
    REQUIRE(cr.success);

    std::vector<float> audio;
    std::vector<std::uint32_t> swaps;
    auto clicks = render_and_localize(
        cr, /*bps=*/60, /*swaps=*/25, /*crossfade=*/0, /*thr=*/0.5f, audio, swaps);

    std::ostringstream report;
    report << "\n  crossfade=0 (no mixing): total clicks=" << clicks.size()
           << " over " << audio.size() << " samples\n";
    for (std::size_t k = 0; k < clicks.size() && k < 5; ++k) {
        const auto& c = clicks[k];
        report << "    sample=" << c.sample_idx
               << " block=" << c.block_idx
               << " (blocks_from_swap=" << c.blocks_from_swap << ")"
               << " δ=" << c.delta << " "
               << c.prev_sample << " → " << c.cur_sample << "\n";
    }
    UNSCOPED_INFO(report.str());
    CHECK(clicks.empty());
}

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("recompile-audio: continuity over rapid same-source swaps",
          "[fuzz][recompile][audio]") {
    // Continuous tonal program. Same-source recompile should preserve all
    // state; the audio should look like a single uninterrupted render.
    static const char* sources[] = {
        // single sine through a pattern of pitches
        R"(n"c4 e4 g4 b4" |> sine(%.freq) |> out(% * 0.3, % * 0.3))",
        // alternation — exercises the sequence step-counter we just fixed
        R"(n"<c4 e4> g4 b4" |> sine(%.freq) |> out(% * 0.3, % * 0.3))",
        // group + repeat
        R"(n"[c4 e4] g4 c4*2" |> sine(%.freq) |> out(% * 0.3, % * 0.3))",
        // chord via soundfont — exercises poly + sample paths
        R"(c"<[Am Am7] [Dm7 Dm] G CM>" |> soundfont(@, "gm", 0) |> out(@ * 0.4))",
    };

    StressConfig cfg;
    cfg.blocks_per_segment = 60;     // ~160 ms per segment
    cfg.swap_count = 25;             // 25 swaps × 60 blocks ≈ 4 s total
    cfg.click_threshold = 0.5f;

    for (const char* src : sources) {
        UNSCOPED_INFO("src=" << src);
        auto cr = akkado::compile(src);
        REQUIRE(cr.success);
        auto r = run_stress(cr, cfg);
        if (!r.ok) {
            UNSCOPED_INFO("failure: " << r.failure);
            UNSCOPED_INFO("swaps_completed=" << r.swaps_completed);
        }
        CHECK(r.ok);
    }
}

TEST_CASE("recompile-audio: trigger pulse count survives rapid swaps",
          "[fuzz][recompile][trigger]") {
    // Pattern emits N pulses/cycle; output bus carries the raw trigger.
    // The total rising-edge count over the rendered window must equal
    // expected pulses regardless of how many times we hot-swap.
    struct Case {
        const char* src;
        std::uint32_t pulses_per_cycle;
    };
    static const Case cases[] = {
        {R"(n"c4 c4 c4 c4" |> %.trig |> out(%, %))", 4},
        {R"(n"c4 c4 c4" |> %.trig |> out(%, %))", 3},
        {R"(n"[c4 c4] c4 c4" |> %.trig |> out(%, %))", 4},
    };

    StressConfig cfg;
    cfg.blocks_per_segment = 80;
    cfg.swap_count = 15;
    cfg.track_edges = true;
    // Trigger output naturally jumps 0→1 in one sample, so click threshold
    // must sit above the legitimate pulse height.
    cfg.click_threshold = 1.5f;
    cfg.clip_threshold = 2.0f;

    for (const Case& c : cases) {
        UNSCOPED_INFO("src=" << c.src);
        auto cr = akkado::compile(c.src);
        REQUIRE(cr.success);
        auto r = run_stress(cr, cfg);
        if (!r.ok) {
            UNSCOPED_INFO("failure: " << r.failure);
            UNSCOPED_INFO("swaps_completed=" << r.swaps_completed);
        }
        CHECK(r.ok);

        // Trigger-count check: how many cycles did we actually render?
        // Sample rate is the cedar default; default BPM is 120 → 0.5 s/beat
        // → 4 beats/cycle → 2 s/cycle → 750 blocks/cycle @ 128-sample blocks
        // and 48 kHz. We rendered (1 + swap_count) * blocks_per_segment
        // blocks plus retry blocks (small). Convert to cycles, multiply by
        // pulses_per_cycle, allow ±1 pulse slack for partial cycles.
        const std::uint64_t total_samples =
            static_cast<std::uint64_t>(cedar::BLOCK_SIZE) * cfg.blocks_per_segment *
            (1 + cfg.swap_count);
        const float seconds = static_cast<float>(total_samples) / 48000.0f;
        const float cycles = seconds / 2.0f;  // 120 BPM, 4 beats/cycle
        const std::uint64_t expected_min =
            static_cast<std::uint64_t>(std::floor(cycles)) * c.pulses_per_cycle;
        const std::uint64_t expected_max =
            static_cast<std::uint64_t>(std::ceil(cycles + 1.0f)) * c.pulses_per_cycle;

        UNSCOPED_INFO("edges observed=" << r.edges_l.rising_edges
                      << " expected range=[" << expected_min << ", "
                      << expected_max << "] over " << cycles << " cycles");
        CHECK(r.edges_l.rising_edges >= expected_min);
        CHECK(r.edges_l.rising_edges <= expected_max);
    }
}

// ----------------------------------------------------------------------------
// Trigger-train continuity across recompile.
// ----------------------------------------------------------------------------
// The core question this answers: does the act of hot-swapping disturb the
// trigger train? Pulses produced by a `%.trig` route should land at the
// same absolute sample positions whether or not the program was recompiled
// during the render.
//
// Comparison strategy:
//   1. Render BASELINE (swap_count = 0) — collect every rising edge.
//   2. Render STRESS  (swap_count = K) — collect every rising edge.
//   3. Same source, same duration → arrays must match.
//
// Two thresholds:
//   * default (epsilon = 2 samples) — tolerates sub-sample drift from the
//     crossfade window where both old + new programs mix.
//   * `[.strict]`-tagged (epsilon = 0) — strict byte-identical equality,
//     opt-in via the hidden Catch2 tag.
//
// Independent of `pat()`'s cycle-length semantics: we don't care how many
// pulses there are per second, only that the swap-laden render produces
// the SAME pulses as the no-swap render.

namespace {

struct ContinuityDiff {
    bool ok = true;
    std::string detail;
};

ContinuityDiff diff_pulse_positions(const std::vector<std::uint64_t>& baseline,
                                    const std::vector<std::uint64_t>& stress,
                                    const std::vector<std::uint32_t>& swap_blocks,
                                    std::int64_t epsilon_samples) {
    ContinuityDiff d;
    if (baseline.size() != stress.size()) {
        std::ostringstream os;
        os << "pulse count differs: baseline=" << baseline.size()
           << " stress=" << stress.size();
        // Locate the first index where the streams diverge, to help diagnose
        // whether a pulse was missed (stress shorter) or doubled (longer).
        const std::size_t common = std::min(baseline.size(), stress.size());
        for (std::size_t i = 0; i < common; ++i) {
            const std::int64_t delta =
                static_cast<std::int64_t>(stress[i]) -
                static_cast<std::int64_t>(baseline[i]);
            if (std::abs(delta) > epsilon_samples) {
                os << "; first divergence at pulse " << i
                   << " baseline=" << baseline[i]
                   << " stress=" << stress[i]
                   << " (block " << (stress[i] / cedar::BLOCK_SIZE) << ")";
                break;
            }
        }
        d.ok = false;
        d.detail = os.str();
        return d;
    }
    for (std::size_t i = 0; i < baseline.size(); ++i) {
        const std::int64_t delta =
            static_cast<std::int64_t>(stress[i]) -
            static_cast<std::int64_t>(baseline[i]);
        if (std::abs(delta) > epsilon_samples) {
            std::ostringstream os;
            os << "pulse " << i << " drifted: baseline=" << baseline[i]
               << " stress=" << stress[i] << " delta=" << delta
               << " samples (eps=" << epsilon_samples
               << "); block " << (stress[i] / cedar::BLOCK_SIZE);
            if (!swap_blocks.empty()) {
                os << "; swap blocks=[";
                for (std::uint32_t b : swap_blocks) os << b << " ";
                os << "]";
            }
            d.ok = false;
            d.detail = os.str();
            return d;
        }
    }
    return d;
}

void check_continuity_against_baseline(const char* src,
                                       std::uint32_t blocks_per_segment,
                                       std::uint32_t swap_count,
                                       std::int64_t epsilon_samples) {
    auto cr = akkado::compile(src);
    REQUIRE(cr.success);

    // Both renders use the same blocks_per_segment so total simulated
    // audio duration is identical: blocks_per_segment * (1 + swap_count).
    StressConfig base_cfg;
    base_cfg.blocks_per_segment = blocks_per_segment;
    base_cfg.swap_count = 0;
    base_cfg.track_edges = true;
    base_cfg.click_threshold = 1.5f;
    base_cfg.clip_threshold = 2.0f;
    // Zero swaps render only the first segment by construction; bump it
    // to match the stress total so the duration aligns.
    base_cfg.blocks_per_segment = blocks_per_segment * (1 + swap_count);

    StressConfig stress_cfg;
    stress_cfg.blocks_per_segment = blocks_per_segment;
    stress_cfg.swap_count = swap_count;
    stress_cfg.track_edges = true;
    stress_cfg.click_threshold = 1.5f;
    stress_cfg.clip_threshold = 2.0f;

    auto baseline = run_stress(cr, base_cfg);
    auto stress = run_stress(cr, stress_cfg);

    // Sanity must hold for both — otherwise the pulse data is suspect.
    REQUIRE(baseline.sanity_l.ok());
    REQUIRE(stress.sanity_l.ok());

    auto diff = diff_pulse_positions(baseline.edges_l.positions,
                                     stress.edges_l.positions,
                                     stress.swap_block_indices,
                                     epsilon_samples);
    if (!diff.ok) {
        UNSCOPED_INFO("src=" << src);
        UNSCOPED_INFO("epsilon=" << epsilon_samples << " samples");
        UNSCOPED_INFO("baseline pulses=" << baseline.edges_l.positions.size()
                      << " stress pulses=" << stress.edges_l.positions.size());
        UNSCOPED_INFO("divergence: " << diff.detail);
    }
    CHECK(diff.ok);
}

// Programs covering distinct pattern shapes — single density, alternation,
// and a longer run. The continuity assertion is the same for all three;
// shape variety is to catch any failure mode that's specific to a payload
// type.
constexpr const char* CONTINUITY_PROGRAMS[] = {
    R"(n"c4 c4 c4 c4" |> %.trig |> out(%, %))",
    R"(n"<c4 e4 g4 b4>" |> %.trig |> out(%, %))",
    R"(n"c4 c4 c4 c4 c4 c4 c4 c4" |> %.trig |> out(%, %))",
};

}  // namespace

TEST_CASE("recompile-audio: trigger train near-identical to no-swap baseline",
          "[fuzz][recompile][trigger][continuity]") {
    // Default epsilon: 2 samples (~0.04 ms) — well below any audible
    // threshold and below the crossfade window (3 blocks = 384 samples).
    // A failure here means recompile shifted the trigger train.
    for (const char* src : CONTINUITY_PROGRAMS) {
        check_continuity_against_baseline(src,
                                          /*blocks_per_segment=*/80,
                                          /*swap_count=*/25,
                                          /*epsilon_samples=*/2);
    }
}

TEST_CASE("recompile-audio: trigger train byte-identical to no-swap baseline",
          "[.strict][fuzz][recompile][trigger][continuity]") {
    // Strict variant — opt-in via the `.strict` tag (Catch2 convention for
    // tests skipped by default). Asserts ZERO drift. A pass here means the
    // hot-swap path is provably transparent to the trigger train.
    for (const char* src : CONTINUITY_PROGRAMS) {
        check_continuity_against_baseline(src,
                                          /*blocks_per_segment=*/80,
                                          /*swap_count=*/25,
                                          /*epsilon_samples=*/0);
    }
}

// ----------------------------------------------------------------------------
// Multi-source variant: trigger continuity while the program is being
// *edited* between recompiles, not just reloaded.
// ----------------------------------------------------------------------------
// Live-coding doesn't load the same bytecode K times; users tweak things
// between hits of the recompile shortcut. If state preservation only works
// for byte-identical source, the trigger train will drift the moment the
// downstream signal flow changes.
//
// Each STRESS swap cycles through variants A → B → C → A → ... All variants
// share the same trigger-bearing prefix `n"…" |> %.trig`, differing
// only in the output side, so the trigger SEMANTIC is unchanged across
// every swap. The baseline uses variant A.

namespace {

// Variants share the same pattern prefix; the trailing `* k` changes
// bytecode without changing the trigger sequence. State ID for the
// pattern + trigger should match across all three (same source text in
// the pattern prefix → same semantic ID).
constexpr const char* MULTI_VARIANTS[] = {
    R"(n"c4 c4 c4 c4" |> %.trig |> out(% * 1.0, % * 1.0))",
    R"(n"c4 c4 c4 c4" |> %.trig |> out(% * 0.9, % * 0.9))",
    R"(n"c4 c4 c4 c4" |> %.trig |> out(% * 0.8, % * 0.8))",
};

StressResult run_multi_source_stress(
    const std::vector<const char*>& srcs,
    std::uint32_t blocks_per_segment,
    std::uint32_t swap_count) {
    StressResult out;
    out.click_l.threshold = 1.5f;
    out.click_r.threshold = 1.5f;
    out.sanity_l.clip_threshold = 2.0f;
    out.sanity_r.clip_threshold = 2.0f;

    cedar::VM vm;
    vm.set_crossfade_blocks(3);

    // Pre-compile every variant once.
    std::vector<akkado::CompileResult> compiled;
    for (const char* s : srcs) {
        auto cr = akkado::compile(s);
        REQUIRE(cr.success);
        compiled.push_back(std::move(cr));
    }

    // Start with variant 0.
    auto insts0 = to_inst_vector(compiled[0]);
    if (vm.load_program(insts0) != cedar::VM::LoadResult::Success) {
        out.failure = "initial load_program failed";
        return out;
    }
    std::vector<std::vector<cedar::Sequence>> seq_storage;
    apply_seq_state_inits(vm, compiled[0], seq_storage);

    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    std::uint32_t block_idx = 0;

    auto render_segment = [&](std::uint32_t n_blocks) {
        for (std::uint32_t b = 0; b < n_blocks; ++b) {
            vm.process_block(L.data(), R.data());
            out.click_l.observe(L.data(), cedar::BLOCK_SIZE);
            out.click_r.observe(R.data(), cedar::BLOCK_SIZE);
            out.sanity_l.observe(L.data(), cedar::BLOCK_SIZE);
            out.sanity_r.observe(R.data(), cedar::BLOCK_SIZE);
            out.beat.observe(vm.current_beat_position(), block_idx);
            out.edges_l.observe(L.data(), cedar::BLOCK_SIZE);
            ++block_idx;
        }
    };

    render_segment(blocks_per_segment);

    for (std::uint32_t s = 0; s < swap_count; ++s) {
        const std::size_t variant_idx = (s + 1) % compiled.size();
        auto insts2 = to_inst_vector(compiled[variant_idx]);
        cedar::VM::LoadResult res = cedar::VM::LoadResult::SlotBusy;
        for (int retry = 0; retry < 64; ++retry) {
            res = vm.load_program(insts2);
            if (res == cedar::VM::LoadResult::Success) break;
            render_segment(1);
        }
        if (res != cedar::VM::LoadResult::Success) {
            out.failure = "load_program never succeeded at swap " +
                          std::to_string(s);
            return out;
        }
        out.swap_block_indices.push_back(block_idx);
        apply_seq_state_inits(vm, compiled[variant_idx], seq_storage);
        ++out.swaps_completed;
        render_segment(blocks_per_segment);
    }

    out.ok = out.sanity_l.ok() && out.sanity_r.ok();
    return out;
}

}  // namespace

TEST_CASE("recompile-audio: trigger train continuous across edits "
          "(multi-source)",
          "[fuzz][recompile][trigger][continuity]") {
    // Baseline: variant 0, no swaps, long render.
    constexpr std::uint32_t blocks_per_segment = 80;
    constexpr std::uint32_t swap_count = 25;
    constexpr std::int64_t EPSILON = 2;

    auto cr0 = akkado::compile(MULTI_VARIANTS[0]);
    REQUIRE(cr0.success);
    StressConfig base_cfg;
    base_cfg.blocks_per_segment = blocks_per_segment * (1 + swap_count);
    base_cfg.swap_count = 0;
    base_cfg.track_edges = true;
    base_cfg.click_threshold = 1.5f;
    base_cfg.clip_threshold = 2.0f;
    auto baseline = run_stress(cr0, base_cfg);
    REQUIRE(baseline.sanity_l.ok());

    std::vector<const char*> variants(std::begin(MULTI_VARIANTS),
                                      std::end(MULTI_VARIANTS));
    auto stress = run_multi_source_stress(variants, blocks_per_segment,
                                          swap_count);
    REQUIRE(stress.ok);

    auto diff = diff_pulse_positions(baseline.edges_l.positions,
                                     stress.edges_l.positions,
                                     stress.swap_block_indices,
                                     EPSILON);
    if (!diff.ok) {
        UNSCOPED_INFO("multi-source variants: A, B, C cycled at each swap");
        UNSCOPED_INFO("baseline pulses=" << baseline.edges_l.positions.size()
                      << " stress pulses=" << stress.edges_l.positions.size());
        UNSCOPED_INFO("divergence: " << diff.detail);
    }
    CHECK(diff.ok);
}

TEST_CASE("recompile-audio: beat position monotonic across many swaps",
          "[fuzz][recompile][clock]") {
    // current_beat_position() is derived from global_sample_counter, which
    // is incremented unconditionally per block. A backward jump indicates
    // counter corruption, a seek mid-fuzz, or block-counter mis-tracking
    // during the swap path. We assert strict monotonicity.
    auto cr = akkado::compile(
        R"(n"c4 e4 g4" |> sine(%.freq) |> out(% * 0.3, % * 0.3))");
    REQUIRE(cr.success);

    StressConfig cfg;
    cfg.blocks_per_segment = 30;
    cfg.swap_count = 50;             // really hammer the swap path
    auto r = run_stress(cr, cfg);
    if (!r.ok) {
        UNSCOPED_INFO("failure: " << r.failure);
    }
    CHECK(r.ok);
    CHECK(r.beat.backward_count == 0);
}

TEST_CASE("recompile-audio: long-tail false-positive check (baseline only)",
          "[.tail-baseline][diag]") {
    // Diagnostic: does the energy anomaly check trip in a NO-SWAP run?
    // If yes, the bursty-envelope-into-reverb pattern naturally produces
    // 50x+ block-to-block ratios and the test is too sensitive — not a
    // recompile bug. If no, the swap path IS introducing real bursts.
    static const char* sources[] = {
        R"(sine(220) * adsr(beat(2), 0.01, 0.05, 0.0, 0.05)
             |> delay(@, 0.25, 0.6) |> out(@ * 0.4, @ * 0.4))",
        R"(sine(330) * adsr(beat(2), 0.01, 0.05, 0.0, 0.05)
             |> reverb(@) |> out(@ * 0.4, @ * 0.4))",
    };
    for (const char* src : sources) {
        UNSCOPED_INFO("baseline-only src=" << src);
        auto cr = akkado::compile(src);
        if (!cr.success) continue;

        StressConfig cfg;
        cfg.blocks_per_segment = 50 * 11;  // same total duration as the swap run
        cfg.swap_count = 0;
        auto r = run_stress(cr, cfg);
        REQUIRE(r.sanity_l.ok());
        auto anomaly = r.energy_l.check_window(50, static_cast<std::uint32_t>(r.energy_l.rms.size()),
                                               /*ratio=*/50.0f, /*floor=*/1e-5f);
        UNSCOPED_INFO("baseline anomaly: kind=" << anomaly.kind
                      << " block=" << anomaly.block_idx
                      << " ratio=" << anomaly.ratio);
        // Reporting only — failure here means the test config is too
        // strict for the chosen programs; flag for relaxation.
        CHECK_FALSE(anomaly.found);
    }
}

TEST_CASE("recompile-audio: long-tail effect doesn't burst or go silent on swap",
          "[fuzz][recompile][tail]") {
    // Delay + reverb tails are state that *must* survive recompile. We
    // assert per-block energy doesn't suddenly spike (feedback burst) or
    // crash to zero (buffer wiped) anywhere outside the first segment.
    //
    // Source programs use sustained (un-enveloped) input on purpose: a
    // plucky envelope into reverb naturally produces 100×+ block-to-block
    // RMS ratios between the tail decay and the next attack, which would
    // false-positive the energy check independent of any recompile bug
    // (verified via the [.tail-baseline] diagnostic above).
    static const char* sources[] = {
        R"(sine(220) * 0.3 |> delay(@, 0.25, 0.6) |> out(@, @))",
        R"(sine(330) * 0.3 |> reverb(@) |> out(@, @))",
    };

    for (const char* src : sources) {
        UNSCOPED_INFO("src=" << src);
        auto cr = akkado::compile(src);
        if (!cr.success) continue;  // builtin name may vary; skip if compile fails

        StressConfig cfg;
        cfg.blocks_per_segment = 50;
        cfg.swap_count = 10;
        auto r = run_stress(cr, cfg);
        if (!r.ok) {
            UNSCOPED_INFO("failure: " << r.failure);
        }
        CHECK(r.ok);

        // Energy continuity check: scan blocks after the first swap and
        // make sure no adjacent pair exhibits a 50x burst or full silence
        // collapse.
        if (!r.swap_block_indices.empty()) {
            const std::uint32_t first = r.swap_block_indices.front();
            const std::uint32_t last =
                static_cast<std::uint32_t>(r.energy_l.rms.size());
            auto anomaly = r.energy_l.check_window(first, last,
                                                   /*ratio=*/50.0f,
                                                   /*floor=*/1e-5f);
            if (anomaly.found) {
                UNSCOPED_INFO("energy anomaly: kind=" << anomaly.kind
                              << " block=" << anomaly.block_idx
                              << " ratio=" << anomaly.ratio);
            }
            CHECK_FALSE(anomaly.found);
        }
    }
}

// ============================================================================
// Random-program stress: take seeds from the existing generator and run
// each through the same many-swap continuity check.
// ============================================================================

TEST_CASE("recompile-audio: random-program stress over many seeds",
          "[fuzz][recompile][random]") {
    // We can't include test_fuzz_determinism.cpp's Generator directly
    // without restructuring, so inline a tiny grammar here. Kept smaller
    // than the determinism generator on purpose — render-bounded stress
    // is much more expensive than compile-determinism.
    auto note_atom = [](std::mt19937_64& rng) {
        static const char* atoms[] = {
            "c3", "d3", "e3", "g3", "a3", "c4", "d4", "e4", "g4", "b4"};
        std::uniform_int_distribution<int> d(0, 9);
        return std::string(atoms[d(rng)]);
    };

    auto gen_pattern = [&](std::mt19937_64& rng) {
        std::uniform_int_distribution<int> n_dist(2, 5);
        const int n = n_dist(rng);
        std::string body = note_atom(rng);
        for (int i = 1; i < n; ++i) body += " " + note_atom(rng);
        std::ostringstream os;
        os << "n\"" << body
           << R"(" |> sine(%.freq) |> out(% * 0.3, % * 0.3))";
        return os.str();
    };

    constexpr int N = 12;
    StressConfig cfg;
    cfg.blocks_per_segment = 40;
    cfg.swap_count = 10;
    cfg.click_threshold = 0.5f;

    for (int s = 0; s < N; ++s) {
        const std::uint64_t seed = 0xA1B2C3D4ull + s * 1031ull;
        std::mt19937_64 rng(seed);
        const std::string src = gen_pattern(rng);

        auto cr = akkado::compile(src);
        if (!cr.success) continue;

        auto r = run_stress(cr, cfg);
        if (!r.ok) {
            UNSCOPED_INFO("seed=" << seed << " src=" << src);
            UNSCOPED_INFO("failure: " << r.failure);
        }
        CHECK(r.ok);
    }
}

// ============================================================================
// Regression corpus — recompile-audio specific. Seed with the user's
// stated concern (trigger/pattern artifacts on rapid recompile) so any
// future regression in that area lands here.
// ============================================================================

TEST_CASE("recompile-audio corpus: known regressions",
          "[fuzz][recompile][corpus]") {
    struct Case {
        const char* name;
        const char* src;
        StressConfig cfg;
    };

    StressConfig trig_cfg;
    trig_cfg.click_threshold = 1.5f;
    trig_cfg.clip_threshold = 2.0f;
    trig_cfg.track_edges = true;
    trig_cfg.blocks_per_segment = 60;
    trig_cfg.swap_count = 12;

    StressConfig audio_cfg;
    audio_cfg.click_threshold = 0.5f;
    audio_cfg.blocks_per_segment = 60;
    audio_cfg.swap_count = 12;

    static const Case cases[] = {
        // User-stated concern (2026-05-18): rapid recompile in live mode
        // tripping trigger/pattern systems. Seeded as a baseline so that
        // any future regression in pulse-count continuity surfaces here.
        {"user-trigger-pattern",
         R"(n"c4 c4 c4 c4" |> %.trig |> out(%, %))",
         trig_cfg},
        {"user-alternating-osc",
         R"(n"<c4 e4> g4" |> sine(%.freq) |> out(% * 0.3, % * 0.3))",
         audio_cfg},
    };

    for (const auto& c : cases) {
        UNSCOPED_INFO("corpus case: " << c.name);
        auto cr = akkado::compile(c.src);
        REQUIRE(cr.success);
        auto r = run_stress(cr, c.cfg);
        if (!r.ok) {
            UNSCOPED_INFO("failure: " << r.failure);
        }
        CHECK(r.ok);
    }
}

// ============================================================================
// Direct SequenceState preservation across recompile (Cedar invariant).
// ============================================================================
// The trigger-train tests above verify the OBSERVABLE outcome (no drift in
// pulse positions). The tests below verify the CAUSE — every per-pattern
// playback field in SequenceState is byte-equal across a byte-identical
// recompile. User report (2026-05-19): "recompiling messes with pattern
// playback position; some index gets reset, even for identical source."
// The trigger-train check passes with eps=0, so any field this catches
// must be one the trigger-train observable does NOT depend on (e.g. the
// gate-retrigger sentinel, or fields read by visualisers).
//
// Covers the surviving pattern literal surfaces post-`pat()` retirement:
//   n"…"  note pattern        s"…"  sample pattern
//   c"…"  chord pattern       v"…"  value pattern
//   t"…"  timeline pattern
// `pat(…)` is intentionally NOT included — it is deprecated per the
// `prd-remove-pat-builtin.md` plan and must not seed new tests.

namespace {

struct SeqStateSnapshot {
    bool present = false;
    std::uint32_t num_sequences = 0;
    std::uint32_t current_index = 0;
    float last_beat_pos = 0.0f;
    float last_queried_cycle = 0.0f;
    std::uint32_t cycle_index = 0;
    float last_beat_pos_gate = 0.0f;
    std::vector<std::uint32_t> seq_steps;
};

SeqStateSnapshot snapshot_seq_state(cedar::VM& vm, std::uint32_t state_id) {
    SeqStateSnapshot s;
    auto* st = vm.states().get_if<cedar::SequenceState>(state_id);
    if (!st) return s;
    s.present = true;
    s.num_sequences = st->num_sequences;
    s.current_index = st->current_index;
    s.last_beat_pos = st->last_beat_pos;
    s.last_queried_cycle = st->last_queried_cycle;
    s.cycle_index = st->cycle_index;
    s.last_beat_pos_gate = st->last_beat_pos_gate;
    s.seq_steps.resize(st->num_sequences);
    for (std::uint32_t i = 0; i < st->num_sequences; ++i) {
        s.seq_steps[i] = st->sequences[i].step;
    }
    return s;
}

// Field-by-field diff. Returns first divergence (or empty if equal).
// Caller asserts the string is empty.
std::string diff_seq_snapshots(const SeqStateSnapshot& a,
                               const SeqStateSnapshot& b) {
    auto fmt = [](const char* field, auto x, auto y) {
        std::ostringstream os;
        os << field << " differs: before=" << x << " after=" << y;
        return os.str();
    };
    if (a.present != b.present) {
        return fmt("present", a.present, b.present);
    }
    if (a.num_sequences != b.num_sequences) {
        return fmt("num_sequences", a.num_sequences, b.num_sequences);
    }
    if (a.current_index != b.current_index) {
        return fmt("current_index", a.current_index, b.current_index);
    }
    if (a.last_beat_pos != b.last_beat_pos) {
        return fmt("last_beat_pos", a.last_beat_pos, b.last_beat_pos);
    }
    if (a.last_queried_cycle != b.last_queried_cycle) {
        return fmt("last_queried_cycle",
                   a.last_queried_cycle, b.last_queried_cycle);
    }
    if (a.cycle_index != b.cycle_index) {
        return fmt("cycle_index", a.cycle_index, b.cycle_index);
    }
    if (a.last_beat_pos_gate != b.last_beat_pos_gate) {
        return fmt("last_beat_pos_gate",
                   a.last_beat_pos_gate, b.last_beat_pos_gate);
    }
    for (std::size_t i = 0; i < a.seq_steps.size(); ++i) {
        if (a.seq_steps[i] != b.seq_steps[i]) {
            std::ostringstream os;
            os << "sequences[" << i << "].step differs: before="
               << a.seq_steps[i] << " after=" << b.seq_steps[i];
            return os.str();
        }
    }
    return {};
}

// Locate the first SequenceProgram state_id emitted by the compile result.
std::uint32_t first_seq_state_id(const akkado::CompileResult& cr) {
    for (const auto& init : cr.program.state_inits) {
        if (init.type == akkado::StateInitData::Type::SequenceProgram) {
            return init.state_id;
        }
    }
    return 0;
}

// Drive the VM for `warmup_blocks` blocks so SequenceState has non-zero
// playback fields, snapshot the state, do a byte-identical recompile
// (load_program + apply_state_inits), and snapshot again. Returns the
// diff string (empty = pass).
std::string check_state_preservation(const char* src,
                                     std::uint32_t warmup_blocks) {
    auto cr = akkado::compile(src);
    if (!cr.success) return std::string("compile failed: ") + src;
    const std::uint32_t state_id = first_seq_state_id(cr);
    if (state_id == 0) return "no SequenceProgram state_init emitted";

    cedar::VM vm;
    vm.set_crossfade_blocks(3);
    auto insts = to_inst_vector(cr);
    if (vm.load_program(insts) != cedar::VM::LoadResult::Success) {
        return "initial load_program failed";
    }
    std::vector<std::vector<cedar::Sequence>> seq_storage;
    apply_seq_state_inits(vm, cr, seq_storage);

    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    for (std::uint32_t b = 0; b < warmup_blocks; ++b) {
        vm.process_block(L.data(), R.data());
    }

    const SeqStateSnapshot before = snapshot_seq_state(vm, state_id);
    if (!before.present) return "SequenceState missing after warmup";
    // Sanity: warmup should have moved SOMETHING.
    bool any_progress =
        before.current_index > 0 ||
        before.cycle_index > 0 ||
        before.last_beat_pos > 0.0f;
    for (auto s : before.seq_steps) any_progress = any_progress || s > 0;
    if (!any_progress) {
        return "warmup did not advance SequenceState; bump warmup_blocks";
    }

    // Byte-identical recompile.
    auto insts2 = insts;
    cedar::VM::LoadResult res = cedar::VM::LoadResult::SlotBusy;
    for (int retry = 0; retry < 64; ++retry) {
        res = vm.load_program(insts2);
        if (res == cedar::VM::LoadResult::Success) break;
        vm.process_block(L.data(), R.data());
    }
    if (res != cedar::VM::LoadResult::Success) {
        return "load_program never succeeded";
    }
    apply_seq_state_inits(vm, cr, seq_storage);

    const SeqStateSnapshot after = snapshot_seq_state(vm, state_id);
    return diff_seq_snapshots(before, after);
}

// Same as above but with `swap_count` consecutive byte-identical
// recompiles, each preceded by `inter_blocks` of audio so the playback
// fields keep advancing. Returns the diff between the snapshot taken
// just before the FIRST recompile and the snapshot taken just after the
// LAST recompile, after advancing the same total number of blocks the
// no-swap baseline would have advanced. Catches drift that accumulates
// over many recompiles even when each individual recompile looks clean.
std::string check_state_preservation_repeated(const char* src,
                                              std::uint32_t warmup_blocks,
                                              std::uint32_t swap_count,
                                              std::uint32_t inter_blocks) {
    // Baseline: render warmup + swap_count * inter_blocks blocks with NO
    // swaps and capture the final state.
    auto cr = akkado::compile(src);
    if (!cr.success) return "compile failed";
    const std::uint32_t state_id = first_seq_state_id(cr);
    if (state_id == 0) return "no SequenceProgram state_init";

    SeqStateSnapshot baseline_final;
    {
        cedar::VM vm;
        vm.set_crossfade_blocks(3);
        auto insts = to_inst_vector(cr);
        if (vm.load_program(insts) != cedar::VM::LoadResult::Success) {
            return "baseline: initial load failed";
        }
        std::vector<std::vector<cedar::Sequence>> seq_storage;
        apply_seq_state_inits(vm, cr, seq_storage);
        std::array<float, cedar::BLOCK_SIZE> L{}, R{};
        const std::uint32_t total_blocks =
            warmup_blocks + swap_count * inter_blocks;
        for (std::uint32_t b = 0; b < total_blocks; ++b) {
            vm.process_block(L.data(), R.data());
        }
        baseline_final = snapshot_seq_state(vm, state_id);
        if (!baseline_final.present) return "baseline: state missing";
    }

    // Stress: same total render time, but with swap_count byte-identical
    // recompiles interspersed.
    SeqStateSnapshot stress_final;
    {
        cedar::VM vm;
        vm.set_crossfade_blocks(3);
        auto insts = to_inst_vector(cr);
        if (vm.load_program(insts) != cedar::VM::LoadResult::Success) {
            return "stress: initial load failed";
        }
        std::vector<std::vector<cedar::Sequence>> seq_storage;
        apply_seq_state_inits(vm, cr, seq_storage);
        std::array<float, cedar::BLOCK_SIZE> L{}, R{};
        for (std::uint32_t b = 0; b < warmup_blocks; ++b) {
            vm.process_block(L.data(), R.data());
        }
        for (std::uint32_t s = 0; s < swap_count; ++s) {
            auto insts2 = insts;
            cedar::VM::LoadResult res = cedar::VM::LoadResult::SlotBusy;
            for (int retry = 0; retry < 64; ++retry) {
                res = vm.load_program(insts2);
                if (res == cedar::VM::LoadResult::Success) break;
                vm.process_block(L.data(), R.data());
            }
            if (res != cedar::VM::LoadResult::Success) {
                return "stress: load_program never succeeded";
            }
            apply_seq_state_inits(vm, cr, seq_storage);
            for (std::uint32_t b = 0; b < inter_blocks; ++b) {
                vm.process_block(L.data(), R.data());
            }
        }
        stress_final = snapshot_seq_state(vm, state_id);
        if (!stress_final.present) return "stress: state missing";
    }

    return diff_seq_snapshots(baseline_final, stress_final);
}

// Sources spanning every surviving pattern literal surface. Picked so
// each surface produces non-trivial state under warmup (at least one
// alternation step advance + one cycle boundary). Out-of-band cycle
// length: cycle = beat by default (commit 9d99490), so 200 blocks at
// 48 kHz / 128 BLOCK_SIZE = 0.533s = ~1.06 cycles at 120 BPM.
constexpr const char* STATE_PRESERVE_SOURCES[] = {
    // Top-level alternation note pattern (4 cycles to cycle the alt).
    R"(n"c4 d4 e4 f4" |> sine(@.freq) |> out(@ * 0.3, @ * 0.3))",
    // In-cycle subdivision via brackets (single cycle covers all events).
    R"(n"[c4 d4 e4 f4]" |> sine(@.freq) |> out(@ * 0.3, @ * 0.3))",
    // Chord pattern via poly (E410 requires poly() wrapping for multi-voice
    // chords into a mono synth — see reference/mini-notation/chords.md).
    R"(fn lead({freq, gate, vel}) -> sine(freq) * vel * 0.05
       c"<Am Em F G>" |> poly(@, lead, 4) |> out(@))",
    // Value pattern modulating amplitude (no osc-on-pattern needed).
    R"(v"<0.2 0.5 0.8 0.5>" * sine(220) |> out(@, @))",
    // Nested alternation — stresses sub-sequence step counters.
    R"(n"<[c4 e4] [g4 b4]>" |> sine(@.freq) |> out(@ * 0.3, @ * 0.3))",
};

}  // namespace

TEST_CASE("recompile state: SequenceState preserved across one byte-identical "
          "recompile (all surfaces)",
          "[fuzz][recompile][state]") {
    // Block budget: ~0.5s at 120 BPM ≈ 1 cycle → alternation step counter
    // has advanced past the first element for every source.
    constexpr std::uint32_t warmup_blocks = 250;

    for (const char* src : STATE_PRESERVE_SOURCES) {
        UNSCOPED_INFO("src=" << src);
        const std::string diff = check_state_preservation(src, warmup_blocks);
        if (!diff.empty()) {
            UNSCOPED_INFO("preservation failure: " << diff);
        }
        CHECK(diff.empty());
    }
}

TEST_CASE("recompile state: SequenceState preserved across N byte-identical "
          "recompiles (all surfaces)",
          "[fuzz][recompile][state][repeated]") {
    // Cumulative version: 20 swaps with 100-block gaps. Each individual
    // recompile may look clean but small drift can accumulate; final
    // state must match the no-swap baseline state byte-for-byte.
    constexpr std::uint32_t warmup_blocks = 100;
    constexpr std::uint32_t swap_count = 20;
    constexpr std::uint32_t inter_blocks = 100;

    for (const char* src : STATE_PRESERVE_SOURCES) {
        UNSCOPED_INFO("src=" << src);
        const std::string diff = check_state_preservation_repeated(
            src, warmup_blocks, swap_count, inter_blocks);
        if (!diff.empty()) {
            UNSCOPED_INFO("preservation failure: " << diff);
        }
        CHECK(diff.empty());
    }
}

TEST_CASE("recompile state: SequenceState preserved across same-structure "
          "edit (n\"…\" surface)",
          "[fuzz][recompile][state][edit]") {
    // Edit changes a NOTE VALUE but keeps the AST shape identical (4 events
    // in an alternation sub-sequence). num_sequences must match → snapshot
    // restoration kicks in → playback fields preserved. If this fails, the
    // restoration condition at state_pool.hpp:947 is more fragile than
    // num_sequences-equality.
    constexpr const char* src_a =
        R"(n"c4 d4 e4 f4" |> sine(@.freq) |> out(@ * 0.3, @ * 0.3))";
    constexpr const char* src_b =
        R"(n"c4 d4 e4 g4" |> sine(@.freq) |> out(@ * 0.3, @ * 0.3))";

    auto cr_a = akkado::compile(src_a);
    REQUIRE(cr_a.success);
    auto cr_b = akkado::compile(src_b);
    REQUIRE(cr_b.success);

    const std::uint32_t state_id_a = first_seq_state_id(cr_a);
    const std::uint32_t state_id_b = first_seq_state_id(cr_b);
    REQUIRE(state_id_a != 0);
    REQUIRE(state_id_b != 0);
    // If the state_ids differ between A and B, the hot-swap path has no
    // chance to preserve state across the edit: rebind_states keys by ID.
    // Two same-structure programs that disagree on the pattern's state_id
    // would be a separate root-cause bug; surface it loudly here.
    UNSCOPED_INFO("state_id A=" << state_id_a << " B=" << state_id_b);
    CHECK(state_id_a == state_id_b);
    if (state_id_a != state_id_b) return;

    cedar::VM vm;
    vm.set_crossfade_blocks(3);
    auto insts_a = to_inst_vector(cr_a);
    REQUIRE(vm.load_program(insts_a) == cedar::VM::LoadResult::Success);
    std::vector<std::vector<cedar::Sequence>> seq_storage;
    apply_seq_state_inits(vm, cr_a, seq_storage);

    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    for (std::uint32_t b = 0; b < 250; ++b) {
        vm.process_block(L.data(), R.data());
    }
    const SeqStateSnapshot before = snapshot_seq_state(vm, state_id_a);
    REQUIRE(before.present);

    auto insts_b = to_inst_vector(cr_b);
    cedar::VM::LoadResult res = cedar::VM::LoadResult::SlotBusy;
    for (int retry = 0; retry < 64; ++retry) {
        res = vm.load_program(insts_b);
        if (res == cedar::VM::LoadResult::Success) break;
        vm.process_block(L.data(), R.data());
    }
    REQUIRE(res == cedar::VM::LoadResult::Success);
    apply_seq_state_inits(vm, cr_b, seq_storage);

    const SeqStateSnapshot after = snapshot_seq_state(vm, state_id_b);
    const std::string diff = diff_seq_snapshots(before, after);
    if (!diff.empty()) {
        UNSCOPED_INFO("preservation failure: " << diff);
    }
    CHECK(diff.empty());
}
