// Mutating recompile drift fuzz — Leg 4 of the memory-integrity harness
// (docs/prd-memory-integrity-tests.md §3.5).
//
// Seeds from stdlib + a curated corpus + a grammar synthesizer, applies token-
// and structural-level mutations (a large fraction of which produce INVALID
// code by design, exercising compiler error recovery), recompiles, hot-swaps
// each successful program into a single live VM, and asserts bounded process
// RSS via BOTH a peak ceiling and a linear-slope check on sampled RSS — the
// latter catches slow steady leaks (e.g. per-compile state never reset, the
// 2026-06-08 symptom in §1.1) even when peak stays under the ceiling.
//
// Iteration count is set via the custom `--iters N` arg (akkado_test_main.cpp):
// local 100, nightly 10k, pre-release 100k.
//
// Negative test (§8.4): a per-iteration leak in the compiler (e.g. a
// std::vector allocated and abandoned per compile) makes the slope check fail
// with a quoted bytes/iteration figure. Documented walkthrough only — shipping
// a deliberately-leaky compiler would be hostile to CI.

#include <catch2/catch_test_macros.hpp>

#include "akkado/akkado.hpp"
#include "fuzz/grammar_synth.hpp"
#include "fuzz/mutator.hpp"

#include <cedar/dsp/constants.hpp>
#include <cedar/opcodes/sequence.hpp>
#include <cedar/vm/instruction.hpp>
#include <cedar/vm/vm.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// Iteration count, parsed from `--iters N` in akkado_test_main.cpp.
namespace akkado_test {
extern int g_drift_iters;
}

namespace {

namespace fs = std::filesystem;

// --- process RSS (Linux) ----------------------------------------------------
std::size_t current_rss_kb() {
#if defined(__linux__)
    std::ifstream st("/proc/self/status");
    std::string line;
    while (std::getline(st, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::size_t kb = 0;
            for (char c : line) {
                if (c >= '0' && c <= '9') kb = kb * 10 + static_cast<std::size_t>(c - '0');
            }
            return kb;
        }
    }
#endif
    return 0;  // non-Linux: RSS sampling unavailable; checks become no-ops.
}

// --- seed corpus ------------------------------------------------------------
std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void add_dir_seeds(const char* dir, std::vector<std::string>& out) {
    if (dir == nullptr) return;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (entry.is_regular_file() && entry.path().extension() == ".ak") {
            std::string s = read_file(entry.path());
            if (!s.empty()) out.push_back(std::move(s));
        }
    }
}

std::vector<std::string> load_seeds() {
    std::vector<std::string> seeds;
#ifdef AKKADO_STDLIB_DIR
    add_dir_seeds(AKKADO_STDLIB_DIR, seeds);
#endif
#ifdef AKKADO_TEST_CORPUS_DIR
    add_dir_seeds(AKKADO_TEST_CORPUS_DIR, seeds);
#endif
    if (seeds.empty()) {
        // Fallback so the test is self-contained even without the dirs.
        seeds = {
            R"(n"c4 e4 g4 b4" |> sine(%.freq) |> out(% * 0.3, % * 0.3))",
            R"(saw(220) |> lp(@, 1200) |> out(@ * 0.3, @ * 0.3))",
            R"(sine(330) * 0.3 |> reverb(@) |> out(@, @))",
        };
    }
    return seeds;
}

// --- VM glue (mirrors the helpers in test_fuzz_recompile_audio.cpp) ----------
std::vector<cedar::Instruction> to_inst_vector(const akkado::CompileResult& cr) {
    const std::size_t n = cr.program.bytecode.size() / sizeof(cedar::Instruction);
    std::vector<cedar::Instruction> insts(n);
    if (n != 0) std::memcpy(insts.data(), cr.program.bytecode.data(), cr.program.bytecode.size());
    return insts;
}

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
        vm.init_sequence_program_state(init.state_id, stored.data(), stored.size(),
                                       init.cycle_length, init.is_sample_pattern,
                                       init.total_events);
    }
}

// Least-squares slope (bytes per iteration) over (iter, rss_bytes) samples.
double slope_bytes_per_iter(const std::vector<std::pair<int, std::size_t>>& s) {
    if (s.size() < 2) return 0.0;
    double n = static_cast<double>(s.size());
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (const auto& [it, rss_kb] : s) {
        double x = static_cast<double>(it);
        double y = static_cast<double>(rss_kb) * 1024.0;  // KB → bytes
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    double denom = n * sxx - sx * sx;
    if (denom == 0.0) return 0.0;
    return (n * sxy - sx * sy) / denom;
}

}  // namespace

TEST_CASE("drift fuzz: bounded RSS over mutating recompiles", "[drift_fuzz]") {
    const int iters = akkado_test::g_drift_iters;
    REQUIRE(iters > 0);

    // Budgets (mirror scripts/memory/budgets.sh §6.3).
    constexpr std::size_t PEAK_CEILING_BYTES = std::size_t(1024) * 1024 * 1024;  // 1 GB
    constexpr double SLOPE_THRESHOLD_BYTES_PER_ITER = 1024.0;                    // 1 KB/iter

    const std::vector<std::string> seeds = load_seeds();
    REQUIRE_FALSE(seeds.empty());

    fuzz::Mutator mut(0xD817'F022ull);
    fuzz::GrammarSynth synth(0x6A5EED'1234ull);  // distinct seed
    std::mt19937_64 strat(0xC0FFEEull);

    auto make_vm = [] {
        auto v = std::make_unique<cedar::VM>();
        v->set_crossfade_blocks(0);
        return v;
    };
    auto vm = make_vm();

    // Programs are hot-swapped into ONE persistent VM for the whole run — no
    // periodic recreation. This is the regression guard for
    // prd-audio-arena-reclamation: before that fix a single VM hot-swapped
    // through ~150+ structurally distinct FX programs exhausted the audio arena
    // (per-program reverb/delay buffers were never reclaimed on state GC) and
    // crashed on a null FX buffer. With reclamation the arena's bump high-water
    // (arena_bytes_used) plateaus instead of climbing to exhaustion; we assert
    // that plateau below.

    // Retain the CompileResult + its sequence backing for the last few loaded
    // programs: set_crossfade_blocks(0) still micro-crossfades structural
    // changes, during which the VM keeps processing the PREVIOUS program and
    // reading its sequence events (which point into the CompileResult). Freeing
    // them at the iteration boundary is a use-after-free. A small ring keeps
    // them alive across the crossfade without unbounded growth.
    std::deque<std::pair<akkado::CompileResult,
                         std::vector<std::vector<cedar::Sequence>>>> live;

    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    std::vector<std::pair<int, std::size_t>> rss_samples;  // (iter, rss_kb)
    std::size_t peak_kb = 0;

    // Audio-arena bump high-water across the run. With reclamation this
    // plateaus; without it (the bug) it climbs monotonically to the 32 MB
    // arena ceiling and then degrades FX to passthrough. Sampled every loaded
    // program (after its blocks run) so the peak reflects steady-state churn.
    std::size_t peak_arena_bytes = 0;
    std::size_t arena_bytes_first_half_peak = 0;  // peak over the first half of loads
    std::size_t arena_bytes_second_half_peak = 0; // peak over the second half

    // Sample ~100 points regardless of iteration count.
    const int sample_every = std::max(1, iters / 100);

    std::size_t compiled_ok = 0, compile_err = 0, loaded_ok = 0;

    for (int i = 0; i < iters; ++i) {
        // Strategy selection: 50% token-mutate a seed, 30% structural-mutate a
        // seed, 20% synthesize a fresh valid program (§7.4).
        const int roll = static_cast<int>(strat() % 100);
        const std::string& seed = seeds[static_cast<std::size_t>(i) % seeds.size()];
        std::string src;
        if (roll < 50) {
            src = mut.mutate_token(seed, static_cast<int>(1 + strat() % 4));
        } else if (roll < 80) {
            src = mut.mutate_structural(seed);
        } else {
            src = synth.program();
        }

        akkado::CompileResult cr;
        try {
            cr = akkado::compile(src);
        } catch (const std::exception& e) {
            // Compile must never throw — it reports errors via diagnostics.
            // A throw is a robustness bug; surface it with the reproducing input.
            UNSCOPED_INFO("iter=" << i << " threw: " << e.what()
                          << "\n  src=" << src);
            FAIL("akkado::compile threw on a mutated input");
        }

        if (cr.success) {
            ++compiled_ok;
            // Hot-swap into the live VM and run a few blocks so swap/state-GC
            // paths execute.
            auto insts = to_inst_vector(cr);
            const std::uint32_t req_buffers = cr.program.required_buffers;
            if (!insts.empty()) {
                vm->buffers().ensure_capacity(req_buffers);
                cedar::VM::LoadResult res = cedar::VM::LoadResult::SlotBusy;
                for (int retry = 0; retry < 16; ++retry) {
                    res = vm->load_program(insts);
                    if (res == cedar::VM::LoadResult::Success) break;
                    vm->process_block(L.data(), R.data());
                }
                if (res == cedar::VM::LoadResult::Success) {
                    ++loaded_ok;
                    // Move the CompileResult into the retention ring BEFORE
                    // applying — the registered sequences point into its data.
                    live.push_back({std::move(cr), {}});
                    apply_seq_state_inits(*vm, live.back().first, live.back().second);
                    for (int b = 0; b < 16; ++b) {
                        vm->process_block(L.data(), R.data());
                    }
                    while (live.size() > 3) live.pop_front();

                    // Sample the arena high-water after this program has run.
                    const std::size_t ab = vm->arena_bytes_used();
                    if (ab > peak_arena_bytes) peak_arena_bytes = ab;
                    if (i < iters / 2) {
                        if (ab > arena_bytes_first_half_peak) arena_bytes_first_half_peak = ab;
                    } else {
                        if (ab > arena_bytes_second_half_peak) arena_bytes_second_half_peak = ab;
                    }
                }
            }
        } else {
            // Error path (~the majority of token-mutated iterations): the
            // compiler returned diagnostics, did not crash, and the VM is
            // still alive. This is the load-bearing error-recovery guarantee
            // (§9.1). Nothing to assert beyond "we got here".
            ++compile_err;
        }

        if (i % sample_every == 0) {
            const std::size_t rss = current_rss_kb();
            if (rss > peak_kb) peak_kb = rss;
            rss_samples.push_back({i, rss});
        }
    }
    // Final sample.
    {
        const std::size_t rss = current_rss_kb();
        if (rss > peak_kb) peak_kb = rss;
        rss_samples.push_back({iters, rss});
    }

    const bool have_rss = peak_kb > 0;  // false on non-Linux
    const std::size_t peak_bytes = peak_kb * 1024;

    // Post-warmup slope. Discard the first HALF of samples as warmup: RSS rises
    // during allocator/interner/pool warm-up and then plateaus (measured: peak
    // is flat ~210 MB from 1k to 100k iters, while a fixed-10%-warmup slope
    // falls 34k→8k→3k→0.1k bytes/iter as the window clears the warm-up tail).
    // A real per-compile leak would instead hold a roughly CONSTANT slope at any
    // scale and is caught here once the warm-up is a small fraction of the run.
    const std::size_t warmup = rss_samples.size() / 2;
    std::vector<std::pair<int, std::size_t>> post_warmup(
        rss_samples.begin() + static_cast<long>(warmup), rss_samples.end());
    const double slope = slope_bytes_per_iter(post_warmup);

    // The slope/steady-state checks are only statistically valid once warm-up
    // is a small fraction of the run — i.e. at the nightly/pre-release scales
    // (10k / 100k). At the local smoke scale (100) warm-up dominates and the
    // slope is meaningless, so only the peak ceiling is enforced there.
    const bool slope_meaningful =
        post_warmup.size() >= 20 &&
        (post_warmup.back().first - post_warmup.front().first) >= 4000;

    UNSCOPED_INFO("iters=" << iters
                  << " compiled_ok=" << compiled_ok
                  << " compile_err=" << compile_err
                  << " loaded_ok=" << loaded_ok
                  << " peak_rss_kb=" << peak_kb
                  << " slope_bytes_per_iter=" << slope
                  << " slope_enforced=" << slope_meaningful);

    // Both error and success paths must be exercised for the fuzz to be
    // meaningful (the mutators are tuned to produce a healthy mix).
    CHECK(compiled_ok > 0);
    CHECK(compile_err > 0);

    if (have_rss) {
        // Peak ceiling — catches sudden explosions; valid at any scale.
        CHECK(peak_bytes < PEAK_CEILING_BYTES);

        if (slope_meaningful) {
            // Linear slope — catches slow steady leaks of any size.
            CHECK(slope < SLOPE_THRESHOLD_BYTES_PER_ITER);

            // Rough steady-state sanity (§3.5): RSS over the plateau (second
            // half) should not be drifting up. glibc rarely returns freed
            // pages to the OS, so allow generous headroom — this catches gross
            // drift; the slope check above catches subtle drift.
            const std::size_t mid = post_warmup.front().second;
            const std::size_t end = post_warmup.back().second;
            if (mid > 0) {
                const double growth = static_cast<double>(end) / static_cast<double>(mid);
                UNSCOPED_INFO("steady-state plateau_start_kb=" << mid
                              << " end_kb=" << end << " growth=" << growth);
                CHECK(growth < 1.25);  // < 25% growth across the plateau
            }
        } else {
            UNSCOPED_INFO("slope/steady-state checks skipped: run too short for a "
                          "valid measurement (need >= ~8000 iters; this is the "
                          "nightly/pre-release rigor, see PRD §6.2)");
        }
    }

    // --- bounded audio arena (prd-audio-arena-reclamation regression) --------
    // The headline guard. A single persistent VM hot-swapped through many
    // structurally distinct FX programs must NOT exhaust the audio arena.
    // Before reclamation, per-program reverb/delay buffers were never reclaimed
    // on state GC, so the bump high-water climbed monotonically to the 32 MB
    // ceiling; the next allocation returned null and an FX opcode dereferenced
    // it (SIGSEGV at ~155 swaps). With reclamation each evicted program returns
    // its buffers to the free list and the high-water plateaus (~8.6 MB at 1.5k
    // iters, ~11.4 MB at 10k — a slow running-max creep as larger programs
    // appear, NOT cumulative drift), so allocations never fail.
    const std::size_t exhaustions = vm->arena_exhaustion_count();
    UNSCOPED_INFO("arena exhaustions=" << exhaustions
                  << " peak_bytes=" << peak_arena_bytes
                  << " first_half_peak=" << arena_bytes_first_half_peak
                  << " second_half_peak=" << arena_bytes_second_half_peak
                  << " capacity=" << cedar::AudioArena::DEFAULT_SIZE
                  << " loaded_ok=" << loaded_ok);

    // Primary, scale-insensitive check: zero failed allocations across the run.
    // Directly asserts the bug is fixed without a magic byte threshold — the
    // running-max footprint creeps with iters but never reaches exhaustion.
    CHECK(exhaustions == 0);

    if (loaded_ok > 0) {
        // Secondary defense-in-depth: even short of outright exhaustion, the
        // high-water must stay well under the ceiling. 3/4 of the arena (24 MB)
        // sits far above the observed plateau yet below the ~32 MB the bug
        // reached, catching a partial regression that climbs without (yet)
        // failing an allocation.
        CHECK(peak_arena_bytes < cedar::AudioArena::DEFAULT_SIZE * 3 / 4);
    }
}
