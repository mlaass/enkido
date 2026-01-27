// Tests for the new simplified sequence system
// These tests verify the core sequence behavior without the complexity of
// the full pattern compilation pipeline.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cedar/opcodes/sequence.hpp>
#include <cmath>

using namespace cedar;
using Catch::Matchers::WithinAbs;

// ============================================================================
// Basic Structure Tests
// ============================================================================

TEST_CASE("Event default construction", "[sequence]") {
    Event e;
    CHECK(e.time == 0.0f);
    CHECK(e.duration == 1.0f);
    CHECK(e.chance == 1.0f);
    CHECK(e.type == EventType::DATA);
    CHECK(e.num_values == 0);
}

TEST_CASE("Sequence default construction", "[sequence]") {
    Sequence seq;
    CHECK(seq.num_events == 0);
    CHECK(seq.duration == 4.0f);
    CHECK(seq.step == 0);
    CHECK(seq.mode == SequenceMode::NORMAL);
}

TEST_CASE("SequenceState size check", "[sequence]") {
    INFO("sizeof(Event) = " << sizeof(Event));
    INFO("sizeof(Sequence) = " << sizeof(Sequence));
    INFO("sizeof(SequenceState) = " << sizeof(SequenceState));

    // SequenceState should fit in state pool
    CHECK(sizeof(SequenceState) < 16000);
}

// ============================================================================
// Basic DATA Events
// ============================================================================

TEST_CASE("Basic DATA events - single event", "[sequence]") {
    SequenceState state;

    // Create a sequence with one DATA event at time 0
    Sequence seq;
    seq.mode = SequenceMode::NORMAL;
    seq.duration = 4.0f;

    Event e;
    e.type = EventType::DATA;
    e.time = 0.0f;
    e.duration = 1.0f;
    e.chance = 1.0f;
    e.num_values = 1;
    e.values[0] = 440.0f;  // A4 frequency
    seq.add_event(e);

    state.add_sequence(seq);
    state.cycle_length = 4.0f;
    state.pattern_seed = 12345;

    query_pattern(state, 0, 4.0f);

    REQUIRE(state.output.num_events == 1);
    CHECK_THAT(state.output.events[0].time, WithinAbs(0.0f, 0.001f));
    CHECK_THAT(state.output.events[0].values[0], WithinAbs(440.0f, 0.001f));
}

TEST_CASE("Basic DATA events - two events", "[sequence]") {
    SequenceState state;

    // Create sequence: [c4 e4] -> events at t=0 and t=2
    Sequence seq;
    seq.mode = SequenceMode::NORMAL;
    seq.duration = 4.0f;

    // First event at t=0
    Event e1;
    e1.type = EventType::DATA;
    e1.time = 0.0f;
    e1.duration = 2.0f;
    e1.chance = 1.0f;
    e1.num_values = 1;
    e1.values[0] = 261.63f;  // C4
    seq.add_event(e1);

    // Second event at t=2
    Event e2;
    e2.type = EventType::DATA;
    e2.time = 2.0f;
    e2.duration = 2.0f;
    e2.chance = 1.0f;
    e2.num_values = 1;
    e2.values[0] = 329.63f;  // E4
    seq.add_event(e2);

    state.add_sequence(seq);
    state.cycle_length = 4.0f;

    query_pattern(state, 0, 4.0f);

    REQUIRE(state.output.num_events == 2);
    CHECK_THAT(state.output.events[0].values[0], WithinAbs(261.63f, 0.01f));
    CHECK_THAT(state.output.events[1].values[0], WithinAbs(329.63f, 0.01f));
}

// ============================================================================
// Degrade (Chance) Filter
// ============================================================================

TEST_CASE("Degrade chance - deterministic", "[sequence]") {
    // Same seed + time should always produce same result
    std::uint64_t seed = 0x123456789ABCDEFull;
    float time = 1.5f;

    float r1 = deterministic_random_seq(seed, time);
    float r2 = deterministic_random_seq(seed, time);

    CHECK(r1 == r2);
}

TEST_CASE("Degrade chance - 50% filter", "[sequence]") {
    SequenceState state;

    // Create sequence with 50% chance event
    Sequence seq;
    seq.mode = SequenceMode::NORMAL;
    seq.duration = 4.0f;

    Event e;
    e.type = EventType::DATA;
    e.time = 0.0f;
    e.duration = 1.0f;
    e.chance = 0.5f;  // 50% chance
    e.num_values = 1;
    e.values[0] = 440.0f;
    seq.add_event(e);

    state.add_sequence(seq);
    state.cycle_length = 4.0f;

    // Test multiple cycles with same seed - should be deterministic
    state.pattern_seed = 12345;
    query_pattern(state, 0, 4.0f);
    std::uint32_t result1 = state.output.num_events;

    query_pattern(state, 0, 4.0f);
    std::uint32_t result2 = state.output.num_events;

    // Deterministic: same query should always produce same result
    CHECK(result1 == result2);

    // Result should be 0 or 1
    CHECK(result1 <= 1);
}

TEST_CASE("Degrade chance - 100% always plays", "[sequence]") {
    SequenceState state;

    Sequence seq;
    seq.mode = SequenceMode::NORMAL;

    Event e;
    e.type = EventType::DATA;
    e.time = 0.0f;
    e.chance = 1.0f;  // Always plays
    e.num_values = 1;
    e.values[0] = 440.0f;
    seq.add_event(e);

    state.add_sequence(seq);
    state.pattern_seed = 99999;

    query_pattern(state, 0, 4.0f);

    CHECK(state.output.num_events == 1);
}

TEST_CASE("Degrade chance - 0% never plays", "[sequence]") {
    SequenceState state;

    Sequence seq;
    seq.mode = SequenceMode::NORMAL;

    Event e;
    e.type = EventType::DATA;
    e.time = 0.0f;
    e.chance = 0.0f;  // Never plays
    e.num_values = 1;
    e.values[0] = 440.0f;
    seq.add_event(e);

    state.add_sequence(seq);
    state.pattern_seed = 99999;

    query_pattern(state, 0, 4.0f);

    CHECK(state.output.num_events == 0);
}

// ============================================================================
// Alternating Sequence (ALTERNATE mode)
// ============================================================================

TEST_CASE("Alternating sequence - basic", "[sequence]") {
    SequenceState state;

    // Create <a b c> pattern
    Sequence seq;
    seq.mode = SequenceMode::ALTERNATE;
    seq.duration = 4.0f;

    float freqs[] = {220.0f, 440.0f, 660.0f};  // a, b, c
    for (int i = 0; i < 3; ++i) {
        Event e;
        e.type = EventType::DATA;
        e.time = 0.0f;
        e.duration = 4.0f;
        e.num_values = 1;
        e.values[0] = freqs[i];
        seq.add_event(e);
    }

    state.add_sequence(seq);
    state.cycle_length = 4.0f;

    // Query 5 times - should cycle through a, b, c, a, b
    query_pattern(state, 0, 4.0f);
    REQUIRE(state.output.num_events == 1);
    CHECK_THAT(state.output.events[0].values[0], WithinAbs(220.0f, 0.01f));  // a

    query_pattern(state, 1, 4.0f);
    CHECK_THAT(state.output.events[0].values[0], WithinAbs(440.0f, 0.01f));  // b

    query_pattern(state, 2, 4.0f);
    CHECK_THAT(state.output.events[0].values[0], WithinAbs(660.0f, 0.01f));  // c

    query_pattern(state, 3, 4.0f);
    CHECK_THAT(state.output.events[0].values[0], WithinAbs(220.0f, 0.01f));  // a (wrap)

    query_pattern(state, 4, 4.0f);
    CHECK_THAT(state.output.events[0].values[0], WithinAbs(440.0f, 0.01f));  // b
}

// ============================================================================
// Nested Sub-Sequence
// ============================================================================

TEST_CASE("Nested sub-sequence - [a b] c", "[sequence]") {
    SequenceState state;

    // Create inner sequence [a b] (ID 1)
    // Note: SequenceCompiler produces normalized sequences (duration=1.0)
    // Events a and b each take half the inner sequence span
    Sequence inner;
    inner.mode = SequenceMode::NORMAL;
    inner.duration = 1.0f;  // Normalized coordinates

    Event ea;
    ea.type = EventType::DATA;
    ea.time = 0.0f;
    ea.duration = 0.5f;  // Takes half of inner sequence
    ea.num_values = 1;
    ea.values[0] = 220.0f;  // a
    inner.add_event(ea);

    Event eb;
    eb.type = EventType::DATA;
    eb.time = 0.5f;  // Starts at middle of inner sequence
    eb.duration = 0.5f;  // Takes second half
    eb.num_values = 1;
    eb.values[0] = 330.0f;  // b
    inner.add_event(eb);

    // Create outer sequence [[a b] c] (ID 0)
    // SUB_SEQ takes first half, c takes second half
    Sequence outer;
    outer.mode = SequenceMode::NORMAL;
    outer.duration = 1.0f;  // Normalized coordinates

    // First element: SUB_SEQ to inner sequence (takes first half = 2 beats)
    Event e_inner;
    e_inner.type = EventType::SUB_SEQ;
    e_inner.time = 0.0f;
    e_inner.duration = 0.5f;  // Takes half the outer sequence (0-2 beats)
    e_inner.seq_id = 1;  // Points to inner sequence
    outer.add_event(e_inner);

    // Second element: DATA event c (takes second half = 2 beats)
    Event ec;
    ec.type = EventType::DATA;
    ec.time = 0.5f;  // Starts at middle
    ec.duration = 0.5f;  // Takes second half
    ec.num_values = 1;
    ec.values[0] = 440.0f;  // c
    outer.add_event(ec);

    // Add sequences (outer first as ID 0, inner as ID 1)
    state.add_sequence(outer);
    state.add_sequence(inner);
    state.cycle_length = 4.0f;

    query_pattern(state, 0, 4.0f);

    // Should have 3 events: a, b, c
    REQUIRE(state.output.num_events == 3);

    // Events should be sorted by time
    // Inner sequence takes 0-2 beats, a at 0, b at 1
    // c takes 2-4 beats, starts at beat 2
    CHECK_THAT(state.output.events[0].values[0], WithinAbs(220.0f, 0.01f));  // a at t=0
    CHECK_THAT(state.output.events[1].values[0], WithinAbs(330.0f, 0.01f));  // b at t=1
    CHECK_THAT(state.output.events[2].values[0], WithinAbs(440.0f, 0.01f));  // c at t=2
}

// Test for a <b c> d pattern - ALTERNATE embedded in NORMAL sequence
// This tests that SUB_SEQ time_scale propagation is correct
TEST_CASE("Embedded alternate - a <b c> d", "[sequence]") {
    SequenceState state;

    // Create ALTERNATE sequence (seq_id=1): <b c>
    // Each choice has duration=1.0 (full sequence span)
    Sequence alt_seq;
    alt_seq.mode = SequenceMode::ALTERNATE;
    alt_seq.duration = 1.0f;

    Event eb;
    eb.type = EventType::DATA;
    eb.time = 0.0f;
    eb.duration = 1.0f;  // Takes full span of wherever this alternate is placed
    eb.num_values = 1;
    eb.values[0] = 330.0f;  // b
    alt_seq.add_event(eb);

    Event ec;
    ec.type = EventType::DATA;
    ec.time = 0.0f;
    ec.duration = 1.0f;
    ec.num_values = 1;
    ec.values[0] = 440.0f;  // c
    alt_seq.add_event(ec);

    // Create root sequence (seq_id=0): a <b c> d
    // Each element takes 1/3 of the cycle
    Sequence root_seq;
    root_seq.mode = SequenceMode::NORMAL;
    root_seq.duration = 1.0f;

    // a at 0-1/3
    Event ea;
    ea.type = EventType::DATA;
    ea.time = 0.0f;
    ea.duration = 1.0f / 3.0f;
    ea.num_values = 1;
    ea.values[0] = 220.0f;  // a
    root_seq.add_event(ea);

    // SUB_SEQ pointing to alternate at 1/3-2/3
    Event e_alt;
    e_alt.type = EventType::SUB_SEQ;
    e_alt.time = 1.0f / 3.0f;
    e_alt.duration = 1.0f / 3.0f;  // Takes 1/3 of cycle
    e_alt.seq_id = 1;
    root_seq.add_event(e_alt);

    // d at 2/3-1
    Event ed;
    ed.type = EventType::DATA;
    ed.time = 2.0f / 3.0f;
    ed.duration = 1.0f / 3.0f;
    ed.num_values = 1;
    ed.values[0] = 550.0f;  // d
    root_seq.add_event(ed);

    // Add sequences: root first (ID 0), then alternate (ID 1)
    state.add_sequence(root_seq);
    state.add_sequence(alt_seq);
    state.cycle_length = 4.0f;

    query_pattern(state, 0, 4.0f);

    // Should have 3 events
    REQUIRE(state.output.num_events == 3);

    // Check times: a at 0, b at 1.33, d at 2.66
    CHECK_THAT(state.output.events[0].time, WithinAbs(0.0f, 0.01f));
    CHECK_THAT(state.output.events[1].time, WithinAbs(4.0f / 3.0f, 0.01f));  // 1.33
    CHECK_THAT(state.output.events[2].time, WithinAbs(8.0f / 3.0f, 0.01f));  // 2.66

    // Check values
    CHECK_THAT(state.output.events[0].values[0], WithinAbs(220.0f, 0.01f));  // a
    CHECK_THAT(state.output.events[1].values[0], WithinAbs(330.0f, 0.01f));  // b (first alternate choice)
    CHECK_THAT(state.output.events[2].values[0], WithinAbs(550.0f, 0.01f));  // d

    // CRITICAL: Check durations - each should be 1/3 of cycle = 1.33 beats
    // This is the bug we're testing for!
    float expected_duration = 4.0f / 3.0f;  // 1.33 beats
    CHECK_THAT(state.output.events[0].duration, WithinAbs(expected_duration, 0.01f));  // a
    CHECK_THAT(state.output.events[1].duration, WithinAbs(expected_duration, 0.01f));  // b from alternate
    CHECK_THAT(state.output.events[2].duration, WithinAbs(expected_duration, 0.01f));  // d
}

// ============================================================================
// Random Choice (RANDOM mode)
// ============================================================================

TEST_CASE("Random choice - deterministic selection", "[sequence]") {
    SequenceState state;

    // Create a | b | c pattern
    Sequence seq;
    seq.mode = SequenceMode::RANDOM;
    seq.duration = 4.0f;

    float freqs[] = {220.0f, 440.0f, 660.0f};
    for (int i = 0; i < 3; ++i) {
        Event e;
        e.type = EventType::DATA;
        e.time = 0.0f;
        e.num_values = 1;
        e.values[0] = freqs[i];
        seq.add_event(e);
    }

    state.add_sequence(seq);
    state.pattern_seed = 0x12345678;

    // Same seed + cycle should always pick same result
    query_pattern(state, 0, 4.0f);
    REQUIRE(state.output.num_events == 1);
    float result1 = state.output.events[0].values[0];

    // Reset step counter for deterministic behavior
    state.sequences[0].step = 0;
    query_pattern(state, 0, 4.0f);
    float result2 = state.output.events[0].values[0];

    CHECK_THAT(result1, WithinAbs(result2, 0.001f));

    // Result should be one of our frequencies
    bool valid = (std::abs(result1 - 220.0f) < 0.01f ||
                  std::abs(result1 - 440.0f) < 0.01f ||
                  std::abs(result1 - 660.0f) < 0.01f);
    CHECK(valid);
}

// ============================================================================
// Bug Case: <e5 b4 d5 c5 a4 c5>*8
// ============================================================================

TEST_CASE("Bug case: <a b c d e f>*8 - 8 events cycling through 6", "[sequence]") {
    SequenceState state;

    // The pattern <e5 b4 d5 c5 a4 c5>*8 should produce 8 events per cycle,
    // cycling through the 6 alternates: 0,1,2,3,4,5,0,1

    // For this test, we model it as:
    // - Root sequence (ID 0): 8 SUB_SEQ events pointing to an ALTERNATE sequence
    // - Alternate sequence (ID 1): 6 DATA events

    // Create the ALTERNATE sequence with 6 notes
    Sequence alt_seq;
    alt_seq.mode = SequenceMode::ALTERNATE;
    alt_seq.duration = 4.0f;

    float freqs[] = {659.26f, 493.88f, 587.33f, 523.25f, 440.0f, 523.25f};  // e5 b4 d5 c5 a4 c5
    for (int i = 0; i < 6; ++i) {
        Event e;
        e.type = EventType::DATA;
        e.time = 0.0f;
        e.duration = 0.5f;
        e.num_values = 1;
        e.values[0] = freqs[i];
        alt_seq.add_event(e);
    }

    // Create the root sequence with 8 SUB_SEQ events
    Sequence root_seq;
    root_seq.mode = SequenceMode::NORMAL;
    root_seq.duration = 4.0f;

    for (int i = 0; i < 8; ++i) {
        Event e;
        e.type = EventType::SUB_SEQ;
        e.time = static_cast<float>(i) * 0.5f;  // Events at 0, 0.5, 1, 1.5, 2, 2.5, 3, 3.5
        e.duration = 0.5f;
        e.seq_id = 1;  // Points to alternate sequence
        root_seq.add_event(e);
    }

    // Add sequences (root first as ID 0)
    state.add_sequence(root_seq);
    state.add_sequence(alt_seq);
    state.cycle_length = 4.0f;
    state.pattern_seed = 12345;

    query_pattern(state, 0, 4.0f);

    // Should have exactly 8 events
    REQUIRE(state.output.num_events == 8);

    // Events should cycle through 0,1,2,3,4,5,0,1
    CHECK_THAT(state.output.events[0].values[0], WithinAbs(freqs[0], 0.01f));  // e5
    CHECK_THAT(state.output.events[1].values[0], WithinAbs(freqs[1], 0.01f));  // b4
    CHECK_THAT(state.output.events[2].values[0], WithinAbs(freqs[2], 0.01f));  // d5
    CHECK_THAT(state.output.events[3].values[0], WithinAbs(freqs[3], 0.01f));  // c5
    CHECK_THAT(state.output.events[4].values[0], WithinAbs(freqs[4], 0.01f));  // a4
    CHECK_THAT(state.output.events[5].values[0], WithinAbs(freqs[5], 0.01f));  // c5
    CHECK_THAT(state.output.events[6].values[0], WithinAbs(freqs[0], 0.01f));  // e5 (wrap)
    CHECK_THAT(state.output.events[7].values[0], WithinAbs(freqs[1], 0.01f));  // b4
}

// ============================================================================
// Speed Modifier (*N) - Handled by compiler, not runtime
// ============================================================================

TEST_CASE("Speed modifier - c4*2 produces shorter event", "[sequence]") {
    // *2 means the event plays 2x faster (half duration)
    // This is a compile-time transformation - the compiler would emit
    // an event with duration 0.5 (normalized) which becomes 2.0 beats
    // when scaled by cycle_length (4.0 beats)

    SequenceState state;

    Sequence seq;
    seq.mode = SequenceMode::NORMAL;
    seq.duration = 4.0f;

    Event e;
    e.type = EventType::DATA;
    e.time = 0.0f;
    e.duration = 0.5f;  // Half duration (representing 2x speed)
    e.num_values = 1;
    e.values[0] = 261.63f;
    seq.add_event(e);

    state.add_sequence(seq);

    query_pattern(state, 0, 4.0f);

    REQUIRE(state.output.num_events == 1);
    // Duration 0.5 * 4.0 cycle_length = 2.0 beats
    CHECK_THAT(state.output.events[0].duration, WithinAbs(2.0f, 0.01f));
}

// ============================================================================
// Repeat Modifier (!N) - Handled by compiler, not runtime
// ============================================================================

TEST_CASE("Repeat modifier - c4!2 produces two events", "[sequence]") {
    // !2 means the event is repeated twice
    // This is a compile-time transformation, so we test the result

    SequenceState state;

    Sequence seq;
    seq.mode = SequenceMode::NORMAL;
    seq.duration = 4.0f;

    // Two events representing c4!2
    Event e1;
    e1.type = EventType::DATA;
    e1.time = 0.0f;
    e1.duration = 2.0f;
    e1.num_values = 1;
    e1.values[0] = 261.63f;
    seq.add_event(e1);

    Event e2;
    e2.type = EventType::DATA;
    e2.time = 2.0f;
    e2.duration = 2.0f;
    e2.num_values = 1;
    e2.values[0] = 261.63f;
    seq.add_event(e2);

    state.add_sequence(seq);

    query_pattern(state, 0, 4.0f);

    REQUIRE(state.output.num_events == 2);
    CHECK_THAT(state.output.events[0].values[0], WithinAbs(261.63f, 0.01f));
    CHECK_THAT(state.output.events[1].values[0], WithinAbs(261.63f, 0.01f));
}

// ============================================================================
// Multi-Value Events (Chords)
// ============================================================================

TEST_CASE("Multi-value event - chord", "[sequence]") {
    SequenceState state;

    Sequence seq;
    seq.mode = SequenceMode::NORMAL;

    // C major chord
    Event e;
    e.type = EventType::DATA;
    e.time = 0.0f;
    e.duration = 4.0f;
    e.num_values = 3;
    e.values[0] = 261.63f;  // C4
    e.values[1] = 329.63f;  // E4
    e.values[2] = 392.00f;  // G4
    seq.add_event(e);

    state.add_sequence(seq);

    query_pattern(state, 0, 4.0f);

    REQUIRE(state.output.num_events == 1);
    CHECK(state.output.events[0].num_values == 3);
    CHECK_THAT(state.output.events[0].values[0], WithinAbs(261.63f, 0.01f));
    CHECK_THAT(state.output.events[0].values[1], WithinAbs(329.63f, 0.01f));
    CHECK_THAT(state.output.events[0].values[2], WithinAbs(392.00f, 0.01f));
}

// ============================================================================
// Source Location Tracking (UI Highlighting)
// ============================================================================

TEST_CASE("Source location tracking", "[sequence]") {
    SequenceState state;

    Sequence seq;
    seq.mode = SequenceMode::NORMAL;

    Event e;
    e.type = EventType::DATA;
    e.time = 0.0f;
    e.num_values = 1;
    e.values[0] = 440.0f;
    e.source_offset = 5;
    e.source_length = 3;
    seq.add_event(e);

    state.add_sequence(seq);

    query_pattern(state, 0, 4.0f);

    REQUIRE(state.output.num_events == 1);
    CHECK(state.output.events[0].source_offset == 5);
    CHECK(state.output.events[0].source_length == 3);
}
