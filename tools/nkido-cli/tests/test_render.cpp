// Integration tests for nkido-cli render mode. We invoke the built CLI as a
// subprocess so we exercise the same end-to-end path real users hit. Render
// mode shares its program-loading code with play/serve/ui (see
// program_loader.cpp), so any regression here implies a regression in those
// modes too — hence these tests double as the regression net for the play /
// serve patches.

#include <catch2/catch_test_macros.hpp>

#include "test_paths.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Run the CLI with the given arguments and return its exit status. Output
// goes to stderr/stdout of the test binary, which Catch2 captures on failure.
int run_cli(const std::string& args) {
    std::string cmd = std::string(nkido::test::CLI_BINARY) + " " + args;
    return std::system(cmd.c_str());
}

int run_cli_env(const std::string& env_prefix, const std::string& args) {
    std::string cmd = env_prefix + " " + std::string(nkido::test::CLI_BINARY) +
                      " " + args;
    return std::system(cmd.c_str());
}

struct Wav {
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits_per_sample = 0;
    std::vector<float> samples;  // interleaved
};

// Minimal WAV reader. Handles 16-bit PCM with a single fmt chunk and a
// single data chunk — exactly what nkido-cli's render mode emits.
Wav read_wav(const std::string& path) {
    Wav out;
    std::ifstream f(path, std::ios::binary);
    REQUIRE(f.is_open());

    char riff[4];
    f.read(riff, 4);
    REQUIRE(std::memcmp(riff, "RIFF", 4) == 0);
    f.ignore(4);  // file size
    char wave[4];
    f.read(wave, 4);
    REQUIRE(std::memcmp(wave, "WAVE", 4) == 0);

    char chunk_id[4];
    std::uint32_t chunk_size = 0;

    while (f.read(chunk_id, 4)) {
        f.read(reinterpret_cast<char*>(&chunk_size), 4);
        if (std::memcmp(chunk_id, "fmt ", 4) == 0) {
            std::uint16_t format = 0;
            f.read(reinterpret_cast<char*>(&format), 2);
            REQUIRE(format == 1);  // PCM
            f.read(reinterpret_cast<char*>(&out.channels), 2);
            f.read(reinterpret_cast<char*>(&out.sample_rate), 4);
            f.ignore(4);  // byte rate
            f.ignore(2);  // block align
            f.read(reinterpret_cast<char*>(&out.bits_per_sample), 2);
            f.ignore(static_cast<std::streamsize>(chunk_size) - 16);
        } else if (std::memcmp(chunk_id, "data", 4) == 0) {
            REQUIRE(out.bits_per_sample == 16);
            const std::size_t n = chunk_size / 2;
            out.samples.resize(n);
            std::vector<std::int16_t> raw(n);
            f.read(reinterpret_cast<char*>(raw.data()),
                   static_cast<std::streamsize>(chunk_size));
            for (std::size_t i = 0; i < n; ++i) {
                out.samples[i] = static_cast<float>(raw[i]) / 32768.0f;
            }
            break;
        } else {
            f.ignore(chunk_size);
        }
    }
    return out;
}

// Take every Nth sample (channel selection on interleaved data).
std::vector<float> channel(const Wav& w, std::uint16_t ch) {
    std::vector<float> out;
    out.reserve(w.samples.size() / w.channels);
    for (std::size_t i = ch; i < w.samples.size(); i += w.channels) {
        out.push_back(w.samples[i]);
    }
    return out;
}

float peak(const std::vector<float>& xs) {
    float p = 0.0f;
    for (float x : xs) p = std::max(p, std::fabs(x));
    return p;
}

float rms(const std::vector<float>& xs) {
    if (xs.empty()) return 0.0f;
    double s = 0.0;
    for (float x : xs) {
        const double xd = static_cast<double>(x);
        s += xd * xd;
    }
    return static_cast<float>(std::sqrt(s / static_cast<double>(xs.size())));
}

// Count attack transients. We track a smoothed envelope and look for points
// where the rising slope crosses a threshold after a quiet window. Used as a
// rough "how many notes were triggered" metric for the polyphony test.
int count_attacks(const std::vector<float>& xs, std::uint32_t sample_rate) {
    if (xs.empty()) return 0;
    const float sr_f = static_cast<float>(sample_rate);
    const float attack_factor = 1.0f - std::exp(-1.0f / (0.001f * sr_f));
    const float decay_factor  = 1.0f - std::exp(-1.0f / (0.040f * sr_f));
    float env = 0.0f;
    int attacks = 0;
    bool above = false;
    const float threshold = 0.05f;
    for (float x : xs) {
        const float ax = std::fabs(x);
        if (ax > env) env += (ax - env) * attack_factor;
        else          env += (ax - env) * decay_factor;
        if (!above && env > threshold) {
            ++attacks;
            above = true;
        } else if (above && env < threshold * 0.5f) {
            above = false;
        }
    }
    return attacks;
}

std::string out_path(const char* tag) {
    auto p = std::filesystem::temp_directory_path()
             / (std::string("nkido_cli_test_") + tag + ".wav");
    return p.string();
}

}  // namespace

TEST_CASE("render mode emits non-silent audio for a pure-oscillator patch", "[render][oscillator]") {
    const std::string wav = out_path("oscillator");
    std::filesystem::remove(wav);

    std::ostringstream args;
    args << std::string(nkido::test::FIXTURES_DIR) + "/oscillator.akk"
         << " render --seconds 0.5 -o " << wav;

    REQUIRE(run_cli(args.str()) == 0);
    REQUIRE(std::filesystem::exists(wav));

    Wav w = read_wav(wav);
    REQUIRE(w.sample_rate == 48000);
    REQUIRE(w.channels == 2);

    auto left = channel(w, 0);
    CHECK(peak(left) > 0.1f);
    CHECK(rms(left) > 0.05f);
}

TEST_CASE("render mode triggers polyphonic note pattern (state inits)", "[render][poly]") {
    // The note() pattern + poly() composition writes SequenceProgram and
    // PolyAlloc state inits. If apply_state_inits() doesn't run (the bug we
    // just fixed), the program produces no audio — this test catches that.
    const std::string wav = out_path("poly_seq");
    std::filesystem::remove(wav);

    std::ostringstream args;
    args << std::string(nkido::test::FIXTURES_DIR) + "/poly_seq.akk"
         << " render --seconds 2 --bpm 240 -o " << wav;

    REQUIRE(run_cli(args.str()) == 0);
    REQUIRE(std::filesystem::exists(wav));

    Wav w = read_wav(wav);
    auto left = channel(w, 0);
    CHECK(peak(left) > 0.1f);
    CHECK(rms(left) > 0.02f);
    // bpm=240, cycle=4 beats → 1 cycle/sec, 4 notes/cycle, 2 seconds = 8
    // distinct note attacks. Allow some slack for the envelope detector.
    CHECK(count_attacks(left, w.sample_rate) >= 4);
}

TEST_CASE("render mode loads samples from a local bank manifest", "[render][sample]") {
    const std::string wav = out_path("sample_play");
    std::filesystem::remove(wav);

    std::ostringstream args;
    args << std::string(nkido::test::FIXTURES_DIR) + "/sample_play.akk"
         << " render --seconds 2 --bpm 120"
         << " --bank file://" << nkido::test::LOCAL_BANK_MANIFEST
         << " -o " << wav;

    REQUIRE(run_cli(args.str()) == 0);
    REQUIRE(std::filesystem::exists(wav));

    Wav w = read_wav(wav);
    auto left = channel(w, 0);
    CHECK(peak(left) > 0.1f);
    CHECK(rms(left) > 0.005f);
}

TEST_CASE("render mode auto-loads patch-declared SoundFonts via alias",
          "[render][soundfont][default_soundfont]") {
    // `soundfont(@, "gm", 0)` must produce sound without --soundfont on the
    // CLI. asset_loader::resolve_soundfont_alias() resolves "gm" to the
    // bundled TimGM6mb.sf3 (NKIDO_SOUNDFONT_PATH points at
    // web/static/soundfonts in dev builds). Regression net for the CLI
    // integration gap fixed in prepare_program_assets().
    const std::string wav = out_path("soundfont_gm_auto");
    std::filesystem::remove(wav);

    std::ostringstream args;
    args << std::string(nkido::test::FIXTURES_DIR) + "/soundfont_gm.akk"
         << " render --seconds 2 -o " << wav;

    REQUIRE(run_cli(args.str()) == 0);
    REQUIRE(std::filesystem::exists(wav));

    Wav w = read_wav(wav);
    auto left = channel(w, 0);
    CHECK(peak(left) > 0.05f);
    CHECK(rms(left) > 0.005f);
}

TEST_CASE("--no-default-soundfont suppresses auto-load and produces silence",
          "[render][soundfont][default_soundfont]") {
    // Opt-out flag must be load-bearing: same patch as the previous test, but
    // with the flag the SoundFont never registers and the opcode short-circuits
    // to zero buffers. Verifies the flag wires through Options correctly.
    const std::string wav = out_path("soundfont_gm_off");
    std::filesystem::remove(wav);

    std::ostringstream args;
    args << std::string(nkido::test::FIXTURES_DIR) + "/soundfont_gm.akk"
         << " render --seconds 1 --no-default-soundfont -o " << wav;

    REQUIRE(run_cli(args.str()) == 0);
    REQUIRE(std::filesystem::exists(wav));

    Wav w = read_wav(wav);
    auto left = channel(w, 0);
    CHECK(peak(left) < 0.001f);
}

TEST_CASE("render mode resolves bare sample names via built-in default kit",
          "[render][sample][default_kit]") {
    // Parity check: `s"bd"` must produce non-silent audio with neither a
    // `--bank` flag nor a `samples()` call in the source. The default kit
    // is auto-loaded from NKIDO_DEFAULT_KIT (set here to a one-entry
    // fixture so the test is self-contained).
    const std::string wav = out_path("default_kit_render");
    std::filesystem::remove(wav);

    std::ostringstream args;
    args << "render --seconds 2 --bpm 120"
         << " --source 's\"bd ~ ~ ~\".out()'"
         << " -o " << wav;

    const std::string env_prefix = std::string("NKIDO_DEFAULT_KIT=") +
                                   nkido::test::DEFAULT_KIT_FIXTURE;
    REQUIRE(run_cli_env(env_prefix, args.str()) == 0);
    REQUIRE(std::filesystem::exists(wav));

    Wav w = read_wav(wav);
    auto left = channel(w, 0);
    CHECK(peak(left) > 0.05f);
}
