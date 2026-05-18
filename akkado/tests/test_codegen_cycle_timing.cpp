// End-to-end regression test for mini-notation cycle timing.
//
// Pins the Strudel/Tidal cycle-fitting contract at the *scheduling* layer:
// the entire mini-notation string is exactly one cycle (4 beats) regardless
// of element count. More notes -> shorter notes; cycle length stays fixed.
//
// Background: prior to the 2026-05-18 audit fix, codegen rescaled the
// pattern by `num_top_level_elements` beats, so `pat("c d e f g a b c5")`
// ran for 2 cycles instead of 1. The bug was invisible to eval-only tests
// because pattern_eval correctly produces normalized [0, 1) event times;
// the rescale happened one stage later in codegen_patterns.cpp. See
// docs/audits/mini-notation-cycle-timing_audit_2026-05-18.md.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "akkado/akkado.hpp"

#include <algorithm>
#include <vector>

namespace {

const akkado::StateInitData* find_seq_init(const akkado::CompileResult& result) {
    for (const auto& init : result.state_inits) {
        if (init.type == akkado::StateInitData::Type::SequenceProgram) {
            return &init;
        }
    }
    return nullptr;
}

// Map normalized [0, 1) event times to absolute beat positions and return
// them sorted. Event ordering inside `sequence_events[0]` is not always
// monotonic (e.g. rev reorders), so sort for stable assertions.
std::vector<float> beat_positions(const akkado::StateInitData& si) {
    std::vector<float> beats;
    if (si.sequence_events.empty()) return beats;
    for (const auto& e : si.sequence_events[0]) {
        beats.push_back(e.time * si.cycle_length);
    }
    std::sort(beats.begin(), beats.end());
    return beats;
}

}  // namespace

TEST_CASE("cycle_timing: single element fits in one canonical cycle",
          "[codegen][patterns][cycle_timing]") {
    auto result = akkado::compile(R"(pat("c4"))");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(4.0f));
    auto beats = beat_positions(*si);
    REQUIRE(beats.size() == 1);
    CHECK(beats[0] == Catch::Approx(0.0f).margin(0.001f));
}

TEST_CASE("cycle_timing: two elements fit in one canonical cycle",
          "[codegen][patterns][cycle_timing]") {
    auto result = akkado::compile(R"(pat("c4 e4"))");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(4.0f));
    auto beats = beat_positions(*si);
    REQUIRE(beats.size() == 2);
    CHECK(beats[0] == Catch::Approx(0.0f).margin(0.001f));
    CHECK(beats[1] == Catch::Approx(2.0f).margin(0.001f));
}

TEST_CASE("cycle_timing: four elements land on integer beats",
          "[codegen][patterns][cycle_timing]") {
    auto result = akkado::compile(R"(pat("c4 d4 e4 f4"))");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(4.0f));
    auto beats = beat_positions(*si);
    REQUIRE(beats.size() == 4);
    CHECK(beats[0] == Catch::Approx(0.0f).margin(0.001f));
    CHECK(beats[1] == Catch::Approx(1.0f).margin(0.001f));
    CHECK(beats[2] == Catch::Approx(2.0f).margin(0.001f));
    CHECK(beats[3] == Catch::Approx(3.0f).margin(0.001f));
}

TEST_CASE("cycle_timing: eight elements fit in one cycle as half-beats",
          "[codegen][patterns][cycle_timing]") {
    auto result = akkado::compile(R"(pat("c d e f g a b c5"))");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(4.0f));
    auto beats = beat_positions(*si);
    REQUIRE(beats.size() == 8);
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(beats[i] == Catch::Approx(static_cast<float>(i) * 0.5f).margin(0.001f));
    }
}

TEST_CASE("cycle_timing: slow(2) doubles cycle length",
          "[codegen][patterns][cycle_timing]") {
    auto result = akkado::compile(R"(pat("c4 e4").slow(2))");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(8.0f));
    auto beats = beat_positions(*si);
    REQUIRE(beats.size() == 2);
    CHECK(beats[0] == Catch::Approx(0.0f).margin(0.001f));
    CHECK(beats[1] == Catch::Approx(4.0f).margin(0.001f));
}

TEST_CASE("cycle_timing: fast(2) halves cycle length",
          "[codegen][patterns][cycle_timing]") {
    auto result = akkado::compile(R"(pat("c4 d4 e4 f4").fast(2))");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(2.0f));
    auto beats = beat_positions(*si);
    REQUIRE(beats.size() == 4);
    CHECK(beats[0] == Catch::Approx(0.0f).margin(0.001f));
    CHECK(beats[1] == Catch::Approx(0.5f).margin(0.001f));
    CHECK(beats[2] == Catch::Approx(1.0f).margin(0.001f));
    CHECK(beats[3] == Catch::Approx(1.5f).margin(0.001f));
}

TEST_CASE("cycle_timing: alternation occupies one slot per cycle",
          "[codegen][patterns][cycle_timing]") {
    // <a b c d> is a single top-level element that alternates across cycles.
    // Within a single cycle query (cycle 0), it emits one event at beat 0
    // spanning the full cycle.
    auto result = akkado::compile(R"(pat("<c4 d4 e4 f4>"))");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(4.0f));
    // Static analysis of the compiled sequence may surface the alternation
    // sub-sequence (>= 1 SUB_SEQ event or the inner choices) — the contract
    // we pin is that the cycle length is unaffected by the alternation
    // arity. Inner structure is the eval-stage concern covered by
    // test_mini_notation.cpp.
    REQUIRE(!si->sequence_events.empty());
}

TEST_CASE("cycle_timing: weighted element holds its proportional slot",
          "[codegen][patterns][cycle_timing]") {
    // pat("a@2 b") — total weight 3, "a" holds for 2/3 of the cycle.
    // In beats: cycle_length=4; event "a" at beat 0 (duration ~2.667),
    // event "b" at beat ~2.667 (duration ~1.333).
    auto result = akkado::compile(R"(pat("c4@2 e4"))");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(4.0f));
    auto beats = beat_positions(*si);
    REQUIRE(beats.size() == 2);
    CHECK(beats[0] == Catch::Approx(0.0f).margin(0.01f));
    CHECK(beats[1] == Catch::Approx(8.0f / 3.0f).margin(0.01f));  // ~2.667
}
