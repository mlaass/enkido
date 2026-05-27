// End-to-end regression test for mini-notation cycle timing.
//
// Pins the per-cycle-alternation contract at the *scheduling* layer:
// each top-level element in a mini-notation string occupies exactly one
// cycle (= one beat under the cycle=beat model). Use [...] for in-cycle
// subdivision. This is a deliberate divergence from Strudel/Tidal:
// `n"c d e f"` plays four cycles in sequence, not four sub-notes in
// one cycle.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "akkado/akkado.hpp"

#include <algorithm>
#include <limits>
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
    auto result = akkado::compile(R"(n"c4")");
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
    auto result = akkado::compile(R"(n"c4 e4")");
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
    auto result = akkado::compile(R"(n"c4 d4 e4 f4")");
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
    auto result = akkado::compile(R"(n"c d e f g a b c5")");
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
    auto result = akkado::compile(R"(n"[c4 d4 e4 f4]")");
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
    auto result_top = akkado::compile(R"(n"c4 d4 e4 f4")");
    auto result_alt = akkado::compile(R"(n"<c4 d4 e4 f4>")");
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

TEST_CASE("cycle_timing: slow(2) emits RateScale, SequenceProgram untouched",
          "[codegen][patterns][cycle_timing]") {
    // PRD prd-runtime-event-transforms Phase 3: slow/fast no longer mutate
    // SequenceProgram cycle_length. The runtime EVENT_RATE_SCALE opcode
    // adjusts SequenceState.cycle_length per block to (original / rate),
    // where slow emits rate = 1/factor. SequenceProgram init stays at 1.0.
    auto result = akkado::compile(R"(n"c4 e4".slow(2))");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(1.0f));
    REQUIRE(si->sequence_events.size() >= 2);
    REQUIRE(si->sequence_events[0].size() == 1);
    CHECK(si->sequence_events[1].size() == 2);

    bool has_rate_scale_init = false;
    for (const auto& s : result.state_inits) {
        if (s.type == akkado::StateInitData::Type::RateScale)
            has_rate_scale_init = true;
    }
    CHECK(has_rate_scale_init);
}

TEST_CASE("cycle_timing: fast(2) emits RateScale, SequenceProgram untouched",
          "[codegen][patterns][cycle_timing]") {
    auto result = akkado::compile(R"(n"c4 d4 e4 f4".fast(2))");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(1.0f));
    REQUIRE(si->sequence_events.size() >= 2);
    REQUIRE(si->sequence_events[0].size() == 1);
    CHECK(si->sequence_events[1].size() == 4);

    bool has_rate_scale_init = false;
    for (const auto& s : result.state_inits) {
        if (s.type == akkado::StateInitData::Type::RateScale)
            has_rate_scale_init = true;
    }
    CHECK(has_rate_scale_init);
}

TEST_CASE("cycle_timing: subdivision inside [...] supports weights",
          "[codegen][patterns][cycle_timing]") {
    // [c4@2 e4] — total weight 3, "c4" holds for 2/3 of the cycle.
    // cycle_length=1; event "c4" at t=0 (duration ~0.667),
    // event "e4" at t=~0.667 (duration ~0.333).
    auto result = akkado::compile(R"(n"[c4@2 e4]")");
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

TEST_CASE("cycle_timing: n\"a\", n\"<a>\", n\"[a]\" are byte-equivalent",
          "[codegen][patterns][cycle_timing][single_child]") {
    // The b5f2768 single-child inline guard means all three forms collapse
    // to a direct atom emission with identical bytecode. Preserving this
    // invariant also keeps late()/early() from double-shifting through a
    // needless sub-sequence wrapper.
    auto bare = akkado::compile(R"(n"a")");
    auto seq  = akkado::compile(R"(n"<a>")");
    auto grp  = akkado::compile(R"(n"[a]")");
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

// ============================================================================
// Per-element rate modifiers (`/N`, `@N`) in mini-notation.
// Regression for: `c"<E5 Cm5 Am4 Dm5>/4"` previously dropped the /4 — each
// chord advanced every beat instead of holding for 4 beats. The fix routes
// Slow/Weight through compile_top_level_pattern: any non-unit weight at the
// top level switches MiniPattern from per-cycle alternation to a weighted
// subdivision whose cycle_length = sum-of-weights.
// ============================================================================

TEST_CASE("cycle_timing: /N on a top-level <...> stretches each note across N cycles",
          "[codegen][patterns][cycle_timing][per_element_rate]") {
    // The user's reported bug, expressed with notes so we don't have to drag
    // poly() in just to compile. `<c4 d4 e4 f4>/4` should be equivalent to
    // `<c4 d4 e4 f4>.slow(4)`: each note holds for 4 beats, total 16 beats.
    auto result = akkado::compile(R"(n"<c4 d4 e4 f4>/4")");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    // Single top-level child with weight 4 → subdivision path, cycle_length 4.
    CHECK(si->cycle_length == Catch::Approx(4.0f));
    // Single SUB_SEQ event in the root sequence pointing at the inner
    // ALTERNATE that holds the 4 chord events.
    REQUIRE(si->sequence_events.size() >= 2);
    REQUIRE(si->sequence_events[0].size() == 1);
    CHECK(si->sequence_events[1].size() == 4);
}

TEST_CASE("cycle_timing: /N on a top-level [...] stretches the group across N cycles",
          "[codegen][patterns][cycle_timing][per_element_rate]") {
    // `[a b c d]/2` — group's 4 events stretched across 2 cycles.
    // Events stay at relative t = 0, 0.25, 0.5, 0.75 in [0,1) but
    // cycle_length=2 means they fire at beats 0, 0.5, 1.0, 1.5.
    auto result = akkado::compile(R"(n"[c4 d4 e4 f4]/2")");
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

TEST_CASE("cycle_timing: /N on one element of a multi-element pattern stretches just that element",
          "[codegen][patterns][cycle_timing][per_element_rate]") {
    // `a [b c]/2 d` — three top-level elements with weights [1, 2, 1].
    // cycle_length = 4; middle group's children fall at the inner
    // subdivisions of its 2-beat slot.
    //   a       → beat 0
    //   [b c]/2 → b at beat 1, c at beat 2 (slot spans beats 1-3)
    //   d       → beat 3
    auto result = akkado::compile(R"(n"c4 [d4 e4]/2 f4")");
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

TEST_CASE("cycle_timing: top-level @N (weight) stretches that element",
          "[codegen][patterns][cycle_timing][per_element_rate]") {
    // `c4@2 e4` — two top-level elements with weights [2, 1].
    // cycle_length = 3; events at beats 0, 2.
    auto result = akkado::compile(R"(n"c4@2 e4")");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(3.0f));
    auto beats = beat_positions(*si);
    REQUIRE(beats.size() == 2);
    CHECK(beats[0] == Catch::Approx(0.0f).margin(0.001f));
    CHECK(beats[1] == Catch::Approx(2.0f).margin(0.001f));
}

TEST_CASE("cycle_timing: Weight + Slow on the same atom compose multiplicatively",
          "[codegen][patterns][cycle_timing][per_element_rate]") {
    // `a@2/3` walks the Modified chain: weight × Weight × Slow = 1 × 2 × 3 = 6.
    // Pattern is a single top-level element with weight 6 → cycle_length 6.
    auto result = akkado::compile(R"(n"c4@2/3")");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(6.0f));
}

TEST_CASE("cycle_timing: chord pattern with top-level /N (user-reported bug)",
          "[codegen][patterns][cycle_timing][per_element_rate]") {
    // Exact form from the user's bug report, wrapped in poly() so the
    // polyphonic chord output has a consumer. chord() / c"…" both go
    // through handle_chord_call which also picks up cycle_length() now.
    auto result = akkado::compile(
        R"(c"<E5 Cm5 Am4 Dm5>/4" |> poly(@, ({freq}) -> osc("sin", freq)) |> out(@, @))");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(4.0f));
}

// ============================================================================
// Top-level rest regression: `n"c4 ~ ~ ~"`, `s"bd ~ ~ ~"`, …
// User report: rests in top-level patterns produce spurious trigger pulses
// and/or the wrong pitch rotates between cycles, instead of one-event-per-
// cycle alternation with silent rest cycles. These tests pin the *compiled
// sequence shape* — `num_values == 0` for rest entries, correct freq for
// non-rest entries — so a regression in the audio path is provably not a
// regression in codegen (or vice versa).
// ============================================================================

namespace {

// Per-event num_values across an alternate sub-sequence (sequence_events[1]),
// in alternate-step order. ALTERNATE-mode events are written in the same
// order they were added to the sub-sequence (no time-sort applies because
// every alternate child shares time=0).
std::vector<int> alternate_num_values(const akkado::StateInitData& si) {
    std::vector<int> out;
    if (si.sequence_events.size() < 2) return out;
    for (const auto& e : si.sequence_events[1]) {
        out.push_back(static_cast<int>(e.num_values));
    }
    return out;
}

// Per-event freq for events with num_values >= 1, in the same ordering.
// For rest entries (num_values == 0) we push NaN so a stray match would
// stand out.
std::vector<float> alternate_voice0_freqs(const akkado::StateInitData& si) {
    std::vector<float> out;
    if (si.sequence_events.size() < 2) return out;
    for (const auto& e : si.sequence_events[1]) {
        out.push_back(e.num_values >= 1 ? e.values[0]
                                        : std::numeric_limits<float>::quiet_NaN());
    }
    return out;
}

constexpr float kFreqC4 = 261.6256f;
constexpr float kFreqE5 = 659.2551f;
constexpr float kFreqD3 = 146.8324f;
constexpr float kFreqE4 = 329.6276f;
constexpr float kFreqB3 = 246.9417f;
constexpr float kFreqG4 = 391.9954f;

}  // namespace

TEST_CASE("cycle_timing: top-level rest `n\"c4 ~ ~ ~\"` → 4 alternate events with [1,0,0,0]",
          "[codegen][patterns][cycle_timing][rest_regression]") {
    auto result = akkado::compile(R"(n"c4 ~ ~ ~")");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(1.0f));
    REQUIRE(si->sequence_events.size() >= 2);
    REQUIRE(si->sequence_events[0].size() == 1);  // one SUB_SEQ in root
    REQUIRE(si->sequence_events[1].size() == 4);  // 4 alternate choices

    auto nv = alternate_num_values(*si);
    REQUIRE(nv == std::vector<int>{1, 0, 0, 0});

    auto freqs = alternate_voice0_freqs(*si);
    CHECK(freqs[0] == Catch::Approx(kFreqC4).margin(0.5f));
}

TEST_CASE("cycle_timing: 12-element rest pattern `n\"c4 ~ ~ ~ e5 ~ ~ ~ d3 ~ ~ ~\"`",
          "[codegen][patterns][cycle_timing][rest_regression]") {
    auto result = akkado::compile(R"(n"c4 ~ ~ ~ e5 ~ ~ ~ d3 ~ ~ ~")");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(1.0f));
    REQUIRE(si->sequence_events.size() >= 2);
    REQUIRE(si->sequence_events[0].size() == 1);
    REQUIRE(si->sequence_events[1].size() == 12);

    auto nv = alternate_num_values(*si);
    REQUIRE(nv == std::vector<int>({1,0,0,0, 1,0,0,0, 1,0,0,0}));

    auto freqs = alternate_voice0_freqs(*si);
    CHECK(freqs[0] == Catch::Approx(kFreqC4).margin(0.5f));
    CHECK(freqs[4] == Catch::Approx(kFreqE5).margin(0.5f));
    CHECK(freqs[8] == Catch::Approx(kFreqD3).margin(0.5f));
}

TEST_CASE("cycle_timing: top-level rest `s\"bd ~ ~ ~\"` → 4 alternate events with [1,0,0,0]",
          "[codegen][patterns][cycle_timing][rest_regression]") {
    auto result = akkado::compile(R"(s"bd ~ ~ ~" |> out(@))");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(1.0f));
    CHECK(si->is_sample_pattern == true);
    REQUIRE(si->sequence_events.size() >= 2);
    REQUIRE(si->sequence_events[0].size() == 1);
    REQUIRE(si->sequence_events[1].size() == 4);

    auto nv = alternate_num_values(*si);
    REQUIRE(nv == std::vector<int>{1, 0, 0, 0});
}

TEST_CASE("cycle_timing: 8-element interleaved rest pattern `n\"e4 ~ ~ b3 ~ ~ g4 ~\"`",
          "[codegen][patterns][cycle_timing][rest_regression]") {
    auto result = akkado::compile(R"(n"e4 ~ ~ b3 ~ ~ g4 ~")");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(1.0f));
    REQUIRE(si->sequence_events.size() >= 2);
    REQUIRE(si->sequence_events[0].size() == 1);
    REQUIRE(si->sequence_events[1].size() == 8);

    auto nv = alternate_num_values(*si);
    REQUIRE(nv == std::vector<int>({1,0,0, 1,0,0, 1,0}));

    auto freqs = alternate_voice0_freqs(*si);
    CHECK(freqs[0] == Catch::Approx(kFreqE4).margin(0.5f));
    CHECK(freqs[3] == Catch::Approx(kFreqB3).margin(0.5f));
    CHECK(freqs[6] == Catch::Approx(kFreqG4).margin(0.5f));
}

TEST_CASE("cycle_timing: rest inside [...] subdivision `n\"[c4 ~ ~ ~]\"`",
          "[codegen][patterns][cycle_timing][rest_regression]") {
    // [c4 ~ ~ ~] is a single-child top-level wrap → inlines via the
    // single-child alternate guard → compile_group_events fills the root
    // directly with 4 DATA events at 0, 0.25, 0.5, 0.75.
    auto result = akkado::compile(R"(n"[c4 ~ ~ ~]")");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(1.0f));
    REQUIRE(si->sequence_events.size() >= 1);
    REQUIRE(si->sequence_events[0].size() == 4);

    // Events are kept in insertion order (compile_group_events appends
    // children left-to-right at increasing time_offset).
    const auto& evs = si->sequence_events[0];
    CHECK(evs[0].time == Catch::Approx(0.0f).margin(0.001f));
    CHECK(evs[1].time == Catch::Approx(0.25f).margin(0.001f));
    CHECK(evs[2].time == Catch::Approx(0.5f).margin(0.001f));
    CHECK(evs[3].time == Catch::Approx(0.75f).margin(0.001f));

    CHECK(static_cast<int>(evs[0].num_values) == 1);
    CHECK(static_cast<int>(evs[1].num_values) == 0);
    CHECK(static_cast<int>(evs[2].num_values) == 0);
    CHECK(static_cast<int>(evs[3].num_values) == 0);

    CHECK(evs[0].values[0] == Catch::Approx(kFreqC4).margin(0.5f));
}

TEST_CASE("cycle_timing: explicit alternation `n\"<c4 e5 d3>\"` → 3 alternate events",
          "[codegen][patterns][cycle_timing][rest_regression]") {
    auto result = akkado::compile(R"(n"<c4 e5 d3>")");
    REQUIRE(result.success);
    const auto* si = find_seq_init(result);
    REQUIRE(si != nullptr);

    CHECK(si->cycle_length == Catch::Approx(1.0f));
    REQUIRE(si->sequence_events.size() >= 2);
    REQUIRE(si->sequence_events[0].size() == 1);
    REQUIRE(si->sequence_events[1].size() == 3);

    auto nv = alternate_num_values(*si);
    REQUIRE(nv == std::vector<int>{1, 1, 1});

    auto freqs = alternate_voice0_freqs(*si);
    CHECK(freqs[0] == Catch::Approx(kFreqC4).margin(0.5f));
    CHECK(freqs[1] == Catch::Approx(kFreqE5).margin(0.5f));
    CHECK(freqs[2] == Catch::Approx(kFreqD3).margin(0.5f));
}
