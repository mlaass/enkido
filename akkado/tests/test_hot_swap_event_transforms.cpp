// Regression test: hot-swap of patches using EVENT_MAP / EVENT_FILTER /
// EVENT_REORDER / EVENT_FANOUT / EVENT_RATE_SCALE must produce audio
// bit-identical to a continuous (no-swap) baseline.
//
// Why this exists:
//   `cedar_apply_state_inits` (web/wasm/nkido_wasm.cpp:1161) runs on every
//   recompile in the browser. It re-initializes every state type — not just
//   SequenceProgram. The existing fuzz harness (test_fuzz_recompile_audio.cpp
//   line 40 `apply_seq_state_inits`) only handles SequenceProgram and skips
//   the rest, leaving a coverage gap for `init_event_transform`,
//   `init_event_rate_scale`, `init_poly_state`, `init_foreach_state`,
//   `init_extended_params`, and the inline Timeline init.
//
//   This test closes that gap. It uses `live_apply_state_inits`
//   (wasm_state_inits_helper.hpp) which mirrors WASM exactly, then asserts
//   sample-level audio equivalence between two VMs:
//     A) one that recompiles N times during playback
//     B) one that never recompiles
//   If recompile is transparent (as it must be for identical source), the
//   two streams are bit-identical post-warmup. Any divergence — even a
//   single block of drift — fails the test.
//
//   Threshold: RMSE / reference_RMS < 1e-3 over the post-swap window. The
//   margin allows tiny floating-point round-off from instruction reordering
//   but rejects any audible change.

#include <catch2/catch_test_macros.hpp>

#include "akkado/akkado.hpp"
#include "wasm_state_inits_helper.hpp"

#include <cedar/dsp/constants.hpp>
#include <cedar/vm/instruction.hpp>
#include <cedar/vm/vm.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::vector<cedar::Instruction> to_inst(const akkado::CompileResult& cr) {
    const std::size_t n = cr.bytecode.size() / sizeof(cedar::Instruction);
    std::vector<cedar::Instruction> insts(n);
    std::memcpy(insts.data(), cr.bytecode.data(), cr.bytecode.size());
    return insts;
}

// Write a 32-bit float WAV (mono) for human inspection. Used by the
// hidden `[.dump]` diagnostic tests.
void write_wav_mono_f32(const std::string& path,
                        const std::vector<float>& samples,
                        std::uint32_t sample_rate) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return;
    const std::uint32_t n = static_cast<std::uint32_t>(samples.size());
    const std::uint32_t data_bytes = n * sizeof(float);
    const std::uint32_t fmt_bytes = 16;
    const std::uint16_t fmt_tag = 3;   // IEEE float
    const std::uint16_t channels = 1;
    const std::uint16_t bits = 32;
    const std::uint32_t byte_rate = sample_rate * channels * (bits / 8);
    const std::uint16_t block_align = channels * (bits / 8);
    const std::uint32_t riff_size = 36 + data_bytes;
    f.write("RIFF", 4);
    f.write(reinterpret_cast<const char*>(&riff_size), 4);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    f.write(reinterpret_cast<const char*>(&fmt_bytes), 4);
    f.write(reinterpret_cast<const char*>(&fmt_tag), 2);
    f.write(reinterpret_cast<const char*>(&channels), 2);
    f.write(reinterpret_cast<const char*>(&sample_rate), 4);
    f.write(reinterpret_cast<const char*>(&byte_rate), 4);
    f.write(reinterpret_cast<const char*>(&block_align), 2);
    f.write(reinterpret_cast<const char*>(&bits), 2);
    f.write("data", 4);
    f.write(reinterpret_cast<const char*>(&data_bytes), 4);
    f.write(reinterpret_cast<const char*>(samples.data()), data_bytes);
}

struct DiffReport {
    bool ok = true;
    std::string detail;
    double rmse = 0.0;
    double rms_ref = 0.0;
    double ratio = 0.0;
    float worst_diff = 0.0f;
    std::size_t worst_sample = 0;
};

// Render `src` two ways for `total_blocks` blocks at the same sample-rate
// alignment:
//   A) `swap_count` identical-source recompiles, evenly spaced after
//      `warmup_blocks`. Uses `live_apply_state_inits` (full WASM-style
//      apply).
//   B) no swaps, single program load.
// Then compare sample-by-sample over the post-warmup window. Threshold:
// `rmse / rms_ref < tolerance`. Tolerance default 1e-3 — far below
// audibility, well above floating-point noise from incidental reordering.
DiffReport diff_swap_vs_baseline(const char* src,
                                 std::uint32_t warmup_blocks,
                                 std::uint32_t swap_count,
                                 std::uint32_t post_blocks_per_swap,
                                 std::uint32_t crossfade,
                                 double tolerance = 1e-3,
                                 const char* wav_dump_prefix = nullptr) {
    DiffReport r;

    auto cr = akkado::compile(src);
    if (!cr.success) {
        r.ok = false;
        std::ostringstream os;
        for (const auto& d : cr.diagnostics) os << "  " << d.code << ": " << d.message << "\n";
        r.detail = "compile failed:\n" + os.str();
        return r;
    }
    auto insts = to_inst(cr);
    const std::uint32_t total_blocks =
        warmup_blocks + swap_count * post_blocks_per_swap;

    auto render_pair = [&](cedar::VM& vm,
                           bool do_swaps,
                           std::vector<float>& out_audio) {
        std::vector<std::vector<cedar::Sequence>> seq_storage;
        nkido_test::live_apply_state_inits(vm, cr, seq_storage);
        std::array<float, cedar::BLOCK_SIZE> L{}, R{};
        std::uint32_t produced = 0;
        auto render = [&](std::uint32_t n) {
            for (std::uint32_t b = 0; b < n; ++b) {
                vm.process_block(L.data(), R.data());
                out_audio.insert(out_audio.end(), L.begin(), L.end());
                ++produced;
            }
        };
        render(warmup_blocks);
        for (std::uint32_t s = 0; s < swap_count; ++s) {
            if (do_swaps) {
                cedar::VM::LoadResult res = cedar::VM::LoadResult::SlotBusy;
                for (int retry = 0; retry < 64; ++retry) {
                    res = vm.load_program(insts);
                    if (res == cedar::VM::LoadResult::Success) break;
                    render(1);
                }
                REQUIRE(res == cedar::VM::LoadResult::Success);
                nkido_test::live_apply_state_inits(vm, cr, seq_storage);
            }
            render(post_blocks_per_swap);
        }
        // Trim any extra blocks pulled in by retry-loop renders.
        const std::uint32_t target_blocks = total_blocks;
        if (produced > target_blocks) {
            const std::size_t extra_samples =
                static_cast<std::size_t>(produced - target_blocks) * cedar::BLOCK_SIZE;
            out_audio.resize(out_audio.size() - extra_samples);
        }
    };

    cedar::VM vm_swap;
    vm_swap.set_crossfade_blocks(crossfade);
    REQUIRE(vm_swap.load_program(insts) == cedar::VM::LoadResult::Success);
    std::vector<float> audio_swap;
    render_pair(vm_swap, /*do_swaps=*/true, audio_swap);

    cedar::VM vm_base;
    vm_base.set_crossfade_blocks(crossfade);
    REQUIRE(vm_base.load_program(insts) == cedar::VM::LoadResult::Success);
    std::vector<float> audio_base;
    render_pair(vm_base, /*do_swaps=*/false, audio_base);

    if (wav_dump_prefix) {
        write_wav_mono_f32(std::string(wav_dump_prefix) + "_swap.wav",
                           audio_swap, 48000);
        write_wav_mono_f32(std::string(wav_dump_prefix) + "_base.wav",
                           audio_base, 48000);
    }

    // Compare post-warmup window only — warmup is identical by construction.
    const std::size_t diff_start = warmup_blocks * cedar::BLOCK_SIZE;
    const std::size_t end =
        std::min(audio_swap.size(), audio_base.size());
    double sumsq_diff = 0.0;
    double sumsq_ref = 0.0;
    for (std::size_t i = diff_start; i < end; ++i) {
        const float d = audio_swap[i] - audio_base[i];
        const float ref = audio_base[i];
        sumsq_diff += static_cast<double>(d) * d;
        sumsq_ref += static_cast<double>(ref) * ref;
        if (std::fabs(d) > std::fabs(r.worst_diff)) {
            r.worst_diff = d;
            r.worst_sample = i;
        }
    }
    const std::size_t n = end - diff_start;
    r.rmse = std::sqrt(sumsq_diff / std::max<std::size_t>(1, n));
    r.rms_ref = std::sqrt(sumsq_ref / std::max<std::size_t>(1, n));
    if (r.rms_ref < 1e-6) {
        r.ok = false;
        r.detail = "baseline RMS too low: patch produces no audio";
        return r;
    }
    r.ratio = r.rmse / r.rms_ref;
    if (r.ratio > tolerance) {
        r.ok = false;
        std::ostringstream os;
        os << "swap audio diverged from baseline: rmse=" << r.rmse
           << " ref_rms=" << r.rms_ref
           << " ratio=" << r.ratio
           << " (tolerance " << tolerance << ")"
           << " worst_diff=" << r.worst_diff
           << " @ sample " << r.worst_sample
           << " (block " << (r.worst_sample / cedar::BLOCK_SIZE) << ")";
        r.detail = os.str();
    }
    return r;
}

}  // namespace

// ----------------------------------------------------------------------------
// Single-transform smoke tests. Each EVENT_* path on its own must be
// transparent to a same-source recompile.
// ----------------------------------------------------------------------------

TEST_CASE("hot-swap event-transform: .transpose is transparent",
          "[hot_swap][event_transforms][regression]") {
    const char* src =
        R"(n"c4 e4 g4 b4".transpose(-12) |> sine(@.freq) |> out(@*.3, @*.3))";
    for (std::uint32_t cf : {0u, 3u}) {
        UNSCOPED_INFO("crossfade=" << cf);
        auto r = diff_swap_vs_baseline(src, 120, 8, 80, cf);
        UNSCOPED_INFO(r.detail);
        CHECK(r.ok);
    }
}

TEST_CASE("hot-swap event-transform: .velocity is transparent",
          "[hot_swap][event_transforms][regression]") {
    const char* src =
        R"(n"c4 e4 g4 b4".velocity(0.8) as e |> sine(e.freq) * e.vel |> out(@*.3, @*.3))";
    for (std::uint32_t cf : {0u, 3u}) {
        UNSCOPED_INFO("crossfade=" << cf);
        auto r = diff_swap_vs_baseline(src, 120, 8, 80, cf);
        UNSCOPED_INFO(r.detail);
        CHECK(r.ok);
    }
}

TEST_CASE("hot-swap event-transform: .rev (EVENT_REORDER) is transparent",
          "[hot_swap][event_transforms][regression]") {
    const char* src =
        R"(n"c4 e4 g4 b4".rev() |> sine(@.freq) |> out(@*.3, @*.3))";
    for (std::uint32_t cf : {0u, 3u}) {
        UNSCOPED_INFO("crossfade=" << cf);
        auto r = diff_swap_vs_baseline(src, 120, 8, 80, cf);
        UNSCOPED_INFO(r.detail);
        CHECK(r.ok);
    }
}

TEST_CASE("hot-swap event-transform: .slow (EVENT_RATE_SCALE) is transparent",
          "[hot_swap][event_transforms][regression]") {
    const char* src =
        R"(n"c4 e4 g4 b4".slow(2) |> sine(@.freq) |> out(@*.3, @*.3))";
    for (std::uint32_t cf : {0u, 3u}) {
        UNSCOPED_INFO("crossfade=" << cf);
        auto r = diff_swap_vs_baseline(src, 120, 8, 80, cf);
        UNSCOPED_INFO(r.detail);
        CHECK(r.ok);
    }
}

// ----------------------------------------------------------------------------
// Chained-transform and full-pad tests. These exercise the combinations
// that the WASM `cedar_apply_state_inits` runs on every browser recompile.
// ----------------------------------------------------------------------------

TEST_CASE("hot-swap event-transform: chord+poly+slow+transpose is transparent",
          "[hot_swap][event_transforms][regression]") {
    const char* src = R"(
fn lead({freq, gate, vel}) -> sine(freq) * adsr(gate, 0.01, 0.1, 0.8, 0.3) * vel * 0.05
c"Cmaj7 Am7 Fmaj7 G7".slow(2).transpose(-12) |> poly(@, lead, 8) |> out(@))";
    for (std::uint32_t cf : {0u, 3u}) {
        UNSCOPED_INFO("crossfade=" << cf);
        auto r = diff_swap_vs_baseline(src, 200, 6, 120, cf);
        UNSCOPED_INFO(r.detail);
        CHECK(r.ok);
    }
}

TEST_CASE("hot-swap event-transform: user's unison-pad shape is transparent",
          "[hot_swap][event_transforms][regression][user_pad]") {
    // Reduced-voice variant of the user's unison-pad. Exercises every init
    // path the browser hits on recompile (SequenceProgram, EventTransform,
    // RateScale, PolyAlloc, ExtendedParams via `..{wet: 0.1}`).
    const char* src = R"(
bpm = 135
fn pad_voice(freq, gate, vel, ext) ->
    saw(freq, ext.phase) * adsr(gate, 0.5, 0.6, 0.7, 1.2) * vel
    |> lp(@, 2800)

fn fat({freq, gate, vel}) ->
    unison(freq, gate, vel, pad_voice,
           voices: 3, detune: 0.3, width: .7, phase: 0.2)

c"Cmaj9 Am11@2 Fmaj7 G9@2".slow(2).transpose(-12)
    |> poly(@, fat, 8, 3) * 0.2
    |> reverb(@, 0.75, 0.6, ..{wet: 0.1})
    |> out(@)
)";
    for (std::uint32_t cf : {0u, 3u}) {
        UNSCOPED_INFO("crossfade=" << cf);
        auto r = diff_swap_vs_baseline(src, 200, 6, 120, cf);
        UNSCOPED_INFO(r.detail);
        CHECK(r.ok);
    }
}

// ----------------------------------------------------------------------------
// Hidden diagnostic: dump swap + baseline WAVs for manual inspection.
// ----------------------------------------------------------------------------

TEST_CASE("hot-swap event-transform: WAV dump diagnostic",
          "[.dump][diag]") {
    const char* src = R"(
fn lead({freq, gate, vel}) -> sine(freq) * adsr(gate, 0.01, 0.1, 0.8, 0.3) * vel * 0.05
c"Cmaj7 Am7 Fmaj7 G7".slow(2).transpose(-12) |> poly(@, lead, 8) |> out(@))";
    auto r = diff_swap_vs_baseline(
        src,
        /*warmup=*/200, /*swaps=*/4, /*blocks/swap=*/120,
        /*crossfade=*/0, /*tolerance=*/1e-3,
        /*wav_dump_prefix=*/"/tmp/hot_swap_gap_diag");
    std::fprintf(stderr,
                 "WAVs: /tmp/hot_swap_gap_diag_swap.wav vs _base.wav\n"
                 "  rmse=%g  ref_rms=%g  ratio=%g  worst_diff=%g @ sample %zu (block %zu)\n",
                 r.rmse, r.rms_ref, r.ratio, r.worst_diff,
                 r.worst_sample, r.worst_sample / cedar::BLOCK_SIZE);
    CHECK(true);
}
