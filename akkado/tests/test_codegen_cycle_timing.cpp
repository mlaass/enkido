// End-to-end regression test for mini-notation cycle timing.
//
// Pins the per-cycle-alternation contract at the *scheduling* layer:
// each top-level element in a mini-notation string occupies exactly one
// cycle (= one beat under the cycle=beat model). Use [...] for in-cycle
// subdivision. This is a deliberate divergence from Strudel/Tidal:
// `pat("c d e f")` plays four cycles in sequence, not four sub-notes in
// one cycle.

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

TEST_CASE("cycle_timing: single element inlines to one event spanning the cycle",
          "[codegen][patterns][cycle_timing]") {
    auto result = akkado::compile(R"(pat("c4"))");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(1.0f));
    auto beats = beat_positions(*si);
    REQUIRE(beats.size() == 1);
    CHECK(beats[0] == Catch::Approx(0.0f).margin(0.001f));
}

TEST_CASE("cycle_timing: two top-level elements compile to per-cycle alternation",
          "[codegen][patterns][cycle_timing]") {
    auto result = akkado::compile(R"(pat("c4 e4"))");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(1.0f));
    // Root sequence holds one SUB_SEQ event pointing at the alternate
    // sub-sequence with 2 choices.
    REQUIRE(si->sequence_events.size() >= 2);
    REQUIRE(si->sequence_events[0].size() == 1);
    CHECK(si->sequence_events[1].size() == 2);
}

TEST_CASE("cycle_timing: four top-level elements alternate across four cycles",
          "[codegen][patterns][cycle_timing]") {
    auto result = akkado::compile(R"(pat("c4 d4 e4 f4"))");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(1.0f));
    REQUIRE(si->sequence_events.size() >= 2);
    REQUIRE(si->sequence_events[0].size() == 1);
    CHECK(si->sequence_events[1].size() == 4);
}

TEST_CASE("cycle_timing: eight top-level elements alternate across eight cycles",
          "[codegen][patterns][cycle_timing]") {
    auto result = akkado::compile(R"(pat("c d e f g a b c5"))");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(1.0f));
    REQUIRE(si->sequence_events.size() >= 2);
    REQUIRE(si->sequence_events[0].size() == 1);
    CHECK(si->sequence_events[1].size() == 8);
}

TEST_CASE("cycle_timing: explicit [...] subdivides one cycle",
          "[codegen][patterns][cycle_timing]") {
    auto result = akkado::compile(R"(pat("[c4 d4 e4 f4]"))");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(1.0f));
    auto beats = beat_positions(*si);
    REQUIRE(beats.size() == 4);
    CHECK(beats[0] == Catch::Approx(0.0f).margin(0.001f));
    CHECK(beats[1] == Catch::Approx(0.25f).margin(0.001f));
    CHECK(beats[2] == Catch::Approx(0.50f).margin(0.001f));
    CHECK(beats[3] == Catch::Approx(0.75f).margin(0.001f));
}

TEST_CASE("cycle_timing: <...> is a synonym of top-level alternation",
          "[codegen][patterns][cycle_timing]") {
    auto result_top = akkado::compile(R"(pat("c4 d4 e4 f4"))");
    auto result_alt = akkado::compile(R"(pat("<c4 d4 e4 f4>"))");
    REQUIRE(result_top.success);
    REQUIRE(result_alt.success);

    const auto* si_top = find_seq_init(result_top);
    const auto* si_alt = find_seq_init(result_alt);
    REQUIRE(si_top != nullptr);
    REQUIRE(si_alt != nullptr);

    CHECK(si_top->cycle_length == Catch::Approx(si_alt->cycle_length));
    REQUIRE(si_top->sequence_events.size() == si_alt->sequence_events.size());
    for (std::size_t s = 0; s < si_top->sequence_events.size(); ++s) {
        CHECK(si_top->sequence_events[s].size() == si_alt->sequence_events[s].size());
    }
}

TEST_CASE("cycle_timing: slow(2) doubles cycle length",
          "[codegen][patterns][cycle_timing]") {
    auto result = akkado::compile(R"(pat("c4 e4").slow(2))");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(2.0f));
    REQUIRE(si->sequence_events.size() >= 2);
    REQUIRE(si->sequence_events[0].size() == 1);
    CHECK(si->sequence_events[1].size() == 2);
}

TEST_CASE("cycle_timing: fast(2) halves cycle length",
          "[codegen][patterns][cycle_timing]") {
    auto result = akkado::compile(R"(pat("c4 d4 e4 f4").fast(2))");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(0.5f));
    REQUIRE(si->sequence_events.size() >= 2);
    REQUIRE(si->sequence_events[0].size() == 1);
    CHECK(si->sequence_events[1].size() == 4);
}

TEST_CASE("cycle_timing: subdivision inside [...] supports weights",
          "[codegen][patterns][cycle_timing]") {
    // [c4@2 e4] — total weight 3, "c4" holds for 2/3 of the cycle.
    // cycle_length=1; event "c4" at t=0 (duration ~0.667),
    // event "e4" at t=~0.667 (duration ~0.333).
    auto result = akkado::compile(R"(pat("[c4@2 e4]"))");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(1.0f));
    auto beats = beat_positions(*si);
    REQUIRE(beats.size() == 2);
    CHECK(beats[0] == Catch::Approx(0.0f).margin(0.001f));
    CHECK(beats[1] == Catch::Approx(2.0f / 3.0f).margin(0.001f));
}

TEST_CASE("cycle_timing: timeline(...) curves keep subdivision semantics",
          "[codegen][patterns][cycle_timing][timeline]") {
    // The codegen revert only flips MiniPattern dispatch in compile_into_sequence.
    // timeline() compiles via PatternEvaluator::evaluate which still subdivides
    // the [0,1) span across breakpoints. Pin that we didn't accidentally route
    // timeline curves through alternation.
    auto result = akkado::compile(R"(timeline("__/''") |> out(@, @))");
    REQUIRE(result.success);

    bool timeline_seen = false;
    for (const auto& init : result.state_inits) {
        if (init.type == akkado::StateInitData::Type::Timeline) {
            timeline_seen = true;
            break;
        }
    }
    CHECK(timeline_seen);
}

TEST_CASE("cycle_timing: pat(\"a\"), pat(\"<a>\"), pat(\"[a]\") are byte-equivalent",
          "[codegen][patterns][cycle_timing][single_child]") {
    // The b5f2768 single-child inline guard means all three forms collapse
    // to a direct atom emission with identical bytecode. Preserving this
    // invariant also keeps late()/early() from double-shifting through a
    // needless sub-sequence wrapper.
    auto bare = akkado::compile(R"(pat("a"))");
    auto seq  = akkado::compile(R"(pat("<a>"))");
    auto grp  = akkado::compile(R"(pat("[a]"))");
    REQUIRE(bare.success);
    REQUIRE(seq.success);
    REQUIRE(grp.success);

    const auto* si_bare = find_seq_init(bare);
    const auto* si_seq  = find_seq_init(seq);
    const auto* si_grp  = find_seq_init(grp);
    REQUIRE(si_bare != nullptr);
    REQUIRE(si_seq  != nullptr);
    REQUIRE(si_grp  != nullptr);

    CHECK(si_bare->cycle_length == Catch::Approx(si_seq->cycle_length));
    CHECK(si_bare->cycle_length == Catch::Approx(si_grp->cycle_length));
    REQUIRE(si_bare->sequence_events.size() == si_seq->sequence_events.size());
    REQUIRE(si_bare->sequence_events.size() == si_grp->sequence_events.size());
    for (std::size_t s = 0; s < si_bare->sequence_events.size(); ++s) {
        CHECK(si_bare->sequence_events[s].size() == si_seq->sequence_events[s].size());
        CHECK(si_bare->sequence_events[s].size() == si_grp->sequence_events[s].size());
    }
}
