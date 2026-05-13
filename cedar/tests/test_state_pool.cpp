#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cedar/vm/state_pool.hpp>
#include <cedar/vm/audio_arena.hpp>
#include <cedar/opcodes/sequence.hpp>
#include <cedar/dsp/constants.hpp>

#include <array>
#include <string>
#include <vector>

using namespace cedar;
using Catch::Matchers::WithinAbs;

// ============================================================================
// Unit Tests [state_pool]
// ============================================================================

TEST_CASE("StatePool basic operations", "[state_pool]") {
    StatePool pool;

    SECTION("get_or_create creates new state") {
        constexpr std::uint32_t id = fnv1a_hash("osc1");

        REQUIRE_FALSE(pool.exists(id));

        auto& state = pool.get_or_create<OscState>(id);
        state.phase = 0.5f;

        CHECK(pool.exists(id));
        CHECK(pool.size() == 1);
    }

    SECTION("get_or_create returns existing state") {
        constexpr std::uint32_t id = fnv1a_hash("filter1");

        auto& state1 = pool.get_or_create<SVFState>(id);
        state1.ic1eq = 1.0f;
        state1.ic2eq = 2.0f;

        auto& state2 = pool.get_or_create<SVFState>(id);
        CHECK_THAT(state2.ic1eq, WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(state2.ic2eq, WithinAbs(2.0f, 1e-6f));
        CHECK(&state1 == &state2);
    }

    SECTION("get retrieves existing state") {
        constexpr std::uint32_t id = fnv1a_hash("osc2");

        pool.get_or_create<OscState>(id).phase = 0.75f;
        REQUIRE(pool.exists(id));

        auto& state = pool.get<OscState>(id);
        CHECK_THAT(state.phase, WithinAbs(0.75f, 1e-6f));
    }

    SECTION("exists returns correct values") {
        constexpr std::uint32_t id1 = fnv1a_hash("exists1");
        constexpr std::uint32_t id2 = fnv1a_hash("exists2");

        CHECK_FALSE(pool.exists(id1));
        CHECK_FALSE(pool.exists(id2));

        pool.get_or_create<NoiseState>(id1);

        CHECK(pool.exists(id1));
        CHECK_FALSE(pool.exists(id2));
    }

    SECTION("reset clears all states") {
        constexpr std::uint32_t id1 = fnv1a_hash("reset1");
        constexpr std::uint32_t id2 = fnv1a_hash("reset2");

        pool.get_or_create<OscState>(id1);
        pool.get_or_create<SVFState>(id2);

        REQUIRE(pool.size() == 2);

        pool.reset();

        CHECK(pool.size() == 0);
        CHECK_FALSE(pool.exists(id1));
        CHECK_FALSE(pool.exists(id2));
    }
}

TEST_CASE("StatePool GC lifecycle", "[state_pool]") {
    StatePool pool;
    pool.set_fade_blocks(10);  // 10 block fade-out

    constexpr std::uint32_t id1 = fnv1a_hash("gc_test1");
    constexpr std::uint32_t id2 = fnv1a_hash("gc_test2");

    SECTION("begin_frame + touch + gc_sweep lifecycle") {
        // Create states
        pool.get_or_create<OscState>(id1).phase = 0.1f;
        pool.get_or_create<OscState>(id2).phase = 0.2f;

        REQUIRE(pool.size() == 2);

        // Frame 1: Touch only id1
        pool.begin_frame();
        pool.touch(id1);
        pool.gc_sweep();

        // id1 still active, id2 moved to fading
        CHECK(pool.exists(id1));
        CHECK_FALSE(pool.exists(id2));  // No longer in active pool
        CHECK(pool.fading_count() == 1);

        // Can still access fading state
        const OscState* fading = pool.get_fading<OscState>(id2);
        REQUIRE(fading != nullptr);
        CHECK_THAT(fading->phase, WithinAbs(0.2f, 1e-6f));
    }

    SECTION("fade gain decreases over time") {
        pool.get_or_create<OscState>(id1).phase = 0.5f;

        // Move to fading
        pool.begin_frame();
        // Don't touch id1
        pool.gc_sweep();

        REQUIRE(pool.fading_count() == 1);

        // Initial fade gain should be 1.0
        float gain1 = pool.get_fade_gain(id1);
        CHECK_THAT(gain1, WithinAbs(1.0f, 1e-6f));

        // Advance fade
        for (int i = 0; i < 5; ++i) {
            pool.advance_fading();
        }

        // Gain should have decreased
        float gain2 = pool.get_fade_gain(id1);
        CHECK(gain2 < gain1);
        CHECK(gain2 > 0.0f);

        // Advance to completion
        for (int i = 0; i < 10; ++i) {
            pool.advance_fading();
        }

        float gain3 = pool.get_fade_gain(id1);
        CHECK_THAT(gain3, WithinAbs(0.0f, 0.01f));
    }

    SECTION("gc_fading removes finished fading states") {
        pool.get_or_create<OscState>(id1);

        pool.begin_frame();
        pool.gc_sweep();

        REQUIRE(pool.fading_count() == 1);

        // Advance past fade duration
        for (int i = 0; i < 20; ++i) {
            pool.advance_fading();
        }

        pool.gc_fading();

        CHECK(pool.fading_count() == 0);
        CHECK(pool.get_fading<OscState>(id1) == nullptr);
    }
}

TEST_CASE("StatePool type replacement", "[state_pool]") {
    StatePool pool;

    constexpr std::uint32_t id = fnv1a_hash("type_test");

    SECTION("same ID can hold different types after reset") {
        pool.get_or_create<OscState>(id).phase = 0.5f;
        REQUIRE(pool.exists(id));

        pool.reset();

        pool.get_or_create<SVFState>(id).ic1eq = 99.0f;
        auto& filter = pool.get<SVFState>(id);
        CHECK_THAT(filter.ic1eq, WithinAbs(99.0f, 1e-6f));
    }

    SECTION("type change with get_or_create replaces state") {
        pool.get_or_create<OscState>(id).phase = 0.5f;
        REQUIRE(pool.exists(id));

        // Request different type - should replace
        auto& filter = pool.get_or_create<SVFState>(id);
        filter.ic1eq = 42.0f;

        CHECK(pool.exists(id));
        CHECK_THAT(pool.get<SVFState>(id).ic1eq, WithinAbs(42.0f, 1e-6f));
    }
}

// ============================================================================
// Edge Cases [state_pool][edge]
// ============================================================================

TEST_CASE("StatePool edge cases", "[state_pool][edge]") {
    StatePool pool;

    SECTION("hash collisions - known FNV-1a collision pairs") {
        // FNV-1a is generally collision-resistant but we test with many names
        std::vector<std::string> names = {
            "a", "b", "c", "osc1", "osc2", "filter1", "filter2",
            "delay1", "reverb1", "chorus1"
        };

        std::vector<std::uint32_t> hashes;
        for (const auto& name : names) {
            hashes.push_back(fnv1a_hash_runtime(name.c_str(), name.size()));
        }

        // All hashes should be unique for these test names
        for (std::size_t i = 0; i < hashes.size(); ++i) {
            for (std::size_t j = i + 1; j < hashes.size(); ++j) {
                CHECK(hashes[i] != hashes[j]);
            }
        }

        // Create states for all
        for (std::size_t i = 0; i < names.size(); ++i) {
            pool.get_or_create<OscState>(hashes[i]).phase = static_cast<float>(i) * 0.1f;
        }

        // Verify all are distinct
        for (std::size_t i = 0; i < names.size(); ++i) {
            CHECK_THAT(pool.get<OscState>(hashes[i]).phase,
                       WithinAbs(static_cast<float>(i) * 0.1f, 1e-6f));
        }
    }

    SECTION("create many states") {
        const std::uint32_t num_states = 200;  // Within MAX_STATES limit
        for (std::uint32_t i = 0; i < num_states; ++i) {
            pool.get_or_create<OscState>(i).phase = static_cast<float>(i) * 0.001f;
        }

        CHECK(pool.size() == num_states);

        // Verify first and last
        CHECK_THAT(pool.get<OscState>(0).phase, WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(pool.get<OscState>(num_states - 1).phase,
                   WithinAbs(static_cast<float>(num_states - 1) * 0.001f, 1e-6f));
    }

    SECTION("GC operations on empty pool") {
        REQUIRE(pool.size() == 0);

        // Should not crash
        pool.begin_frame();
        pool.gc_sweep();
        pool.advance_fading();
        pool.gc_fading();

        CHECK(pool.size() == 0);
        CHECK(pool.fading_count() == 0);
    }

    SECTION("fade-out with 0 blocks") {
        pool.set_fade_blocks(0);
        constexpr std::uint32_t id = fnv1a_hash("zero_fade");

        pool.get_or_create<OscState>(id);

        pool.begin_frame();
        pool.gc_sweep();

        // With 0 blocks, state should not go to fading pool
        CHECK(pool.fading_count() == 0);
    }

    SECTION("fade-out with very large blocks") {
        pool.set_fade_blocks(10000);
        constexpr std::uint32_t id = fnv1a_hash("long_fade");

        pool.get_or_create<OscState>(id);

        pool.begin_frame();
        pool.gc_sweep();

        // Fade gain should start at 1.0
        CHECK_THAT(pool.get_fade_gain(id), WithinAbs(1.0f, 1e-6f));

        // After 100 blocks, should still be mostly fading
        for (int i = 0; i < 100; ++i) {
            pool.advance_fading();
        }

        float gain = pool.get_fade_gain(id);
        CHECK(gain > 0.9f);
    }

    SECTION("touch non-existent state") {
        constexpr std::uint32_t id = fnv1a_hash("nonexistent");

        // Should not crash
        pool.begin_frame();
        pool.touch(id);
        pool.gc_sweep();

        CHECK(pool.size() == 0);
    }

    SECTION("multiple gc_sweep calls per frame") {
        constexpr std::uint32_t id = fnv1a_hash("multi_gc");
        pool.get_or_create<OscState>(id);

        pool.begin_frame();
        pool.touch(id);
        pool.gc_sweep();
        pool.gc_sweep();  // Second sweep
        pool.gc_sweep();  // Third sweep

        // State should still be active
        CHECK(pool.exists(id));
    }
}

// ============================================================================
// FNV-1a Hash Tests
// ============================================================================

TEST_CASE("FNV-1a hash function", "[state_pool]") {
    SECTION("compile-time hash works") {
        constexpr std::uint32_t h1 = fnv1a_hash("test");
        constexpr std::uint32_t h2 = fnv1a_hash("test");
        CHECK(h1 == h2);
    }

    SECTION("runtime and compile-time match") {
        constexpr std::uint32_t compile_time = fnv1a_hash("hello");
        std::uint32_t runtime = fnv1a_hash_runtime("hello", 5);
        CHECK(compile_time == runtime);
    }

    SECTION("different strings produce different hashes") {
        constexpr std::uint32_t h1 = fnv1a_hash("osc");
        constexpr std::uint32_t h2 = fnv1a_hash("filter");
        constexpr std::uint32_t h3 = fnv1a_hash("delay");
        CHECK(h1 != h2);
        CHECK(h2 != h3);
        CHECK(h1 != h3);
    }

    SECTION("empty string has consistent hash") {
        constexpr std::uint32_t h1 = fnv1a_hash("");
        constexpr std::uint32_t h2 = fnv1a_hash("");
        CHECK(h1 == h2);
    }

    SECTION("hash is stable across calls") {
        std::string test = "stability_test";
        std::uint32_t h1 = fnv1a_hash_runtime(test.c_str(), test.size());
        std::uint32_t h2 = fnv1a_hash_runtime(test.c_str(), test.size());
        std::uint32_t h3 = fnv1a_hash_runtime(test.c_str(), test.size());
        CHECK(h1 == h2);
        CHECK(h2 == h3);
    }
}

// ============================================================================
// Hot-Swap Preservation Tests [state_pool][hot_swap]
// ============================================================================

namespace {

// Build a small test Sequence with N DATA events in ALTERNATE mode. Events
// are stack-owned by the caller; the resulting Sequence struct only holds a
// pointer.
struct TestSeq {
    std::vector<Event> events;
    Sequence seq{};

    explicit TestSeq(std::size_t n, SequenceMode mode = SequenceMode::ALTERNATE) {
        events.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            events[i].type = EventType::DATA;
            events[i].time = 0.0f;
            events[i].duration = 1.0f;
            events[i].num_values = 1;
            events[i].values[0] = static_cast<float>(i + 1) * 100.0f;
        }
        seq.events = events.data();
        seq.num_events = static_cast<std::uint32_t>(n);
        seq.capacity = static_cast<std::uint32_t>(n);
        seq.duration = static_cast<float>(n);
        seq.mode = mode;
        seq.step = 0;
    }
};

}  // namespace

TEST_CASE("init_sequence_program preserves playback counters across hot-swap",
          "[state_pool][hot_swap][determinism]") {
    // Regression for live-coding bug: recompiling the same program reset the
    // alternation cycle counter to 0, so a `<a b c d>` pattern would jump back
    // to `a` whenever the user re-evaluated the buffer. We need the playback
    // counters preserved so live recompiles are audibly silent for unchanged
    // source.
    StatePool pool;
    AudioArena arena;
    const std::uint32_t state_id = fnv1a_hash("test/alt_pattern");

    // First load: 4 events in ALTERNATE mode (simulates `<a b c d>`).
    TestSeq first(4);
    pool.init_sequence_program(state_id, &first.seq, 1, /*cycle_length=*/4.0f,
                               /*is_sample_pattern=*/false, &arena,
                               /*total_events=*/4);

    // Audio thread advances playback: alternation has run 7 times, cycle
    // counter is at 5, beat tracking has caught up.
    {
        auto& s = pool.get<SequenceState>(state_id);
        REQUIRE(s.num_sequences == 1);
        s.sequences[0].step = 7;
        s.cycle_index = 5;
        s.current_index = 3;
        s.last_beat_pos = 12.5f;
        s.last_queried_cycle = 5.0f;
    }

    // Live-coding recompile: same source, same state_id, fresh Sequence with
    // step=0 in the compile result.
    TestSeq second(4);
    pool.init_sequence_program(state_id, &second.seq, 1, /*cycle_length=*/4.0f,
                               /*is_sample_pattern=*/false, &arena,
                               /*total_events=*/4);

    auto& s = pool.get<SequenceState>(state_id);

    SECTION("alternation step persists") {
        CHECK(s.sequences[0].step == 7);
    }
    SECTION("cycle index persists") {
        CHECK(s.cycle_index == 5);
    }
    SECTION("current index persists") {
        CHECK(s.current_index == 3);
    }
    SECTION("last beat position persists") {
        CHECK_THAT(s.last_beat_pos, WithinAbs(12.5f, 1e-6f));
    }
    SECTION("last queried cycle persists") {
        CHECK_THAT(s.last_queried_cycle, WithinAbs(5.0f, 1e-6f));
    }
    SECTION("events are still applied (refreshed from new program)") {
        REQUIRE(s.sequences[0].num_events == 4);
        // Events should reflect the *new* program's content, even though
        // they happen to match. Verify by checking the value.
        CHECK_THAT(s.sequences[0].events[0].values[0], WithinAbs(100.0f, 1e-6f));
        CHECK_THAT(s.sequences[0].events[3].values[0], WithinAbs(400.0f, 1e-6f));
    }
}

TEST_CASE("init_sequence_program initializes counters on first load",
          "[state_pool][hot_swap]") {
    // First-time init must NOT preserve uninitialized counters — they should
    // start at the documented defaults. Only an existing state_id should
    // trigger the hot-swap preservation path.
    StatePool pool;
    AudioArena arena;
    const std::uint32_t state_id = fnv1a_hash("test/fresh");

    TestSeq first(3);
    pool.init_sequence_program(state_id, &first.seq, 1, 4.0f, false, &arena, 3);

    auto& s = pool.get<SequenceState>(state_id);
    CHECK(s.sequences[0].step == 0);
    CHECK(s.cycle_index == 0);
    CHECK(s.current_index == 0);
    CHECK_THAT(s.last_beat_pos, WithinAbs(-1.0f, 1e-6f));
    CHECK_THAT(s.last_queried_cycle, WithinAbs(-1.0f, 1e-6f));
}

TEST_CASE("init_sequence_program resets counters when sequence count differs",
          "[state_pool][hot_swap]") {
    // Structural change (e.g., user changed `<a b c d>` to `<a b>`) should
    // not blindly preserve counters that index into a different-shaped state.
    // The contract: if the new program has a different number of sequences,
    // start playback fresh.
    StatePool pool;
    AudioArena arena;
    const std::uint32_t state_id = fnv1a_hash("test/restructured");

    TestSeq first(4);
    pool.init_sequence_program(state_id, &first.seq, 1, 4.0f, false, &arena, 4);
    {
        auto& s = pool.get<SequenceState>(state_id);
        s.sequences[0].step = 7;
        s.cycle_index = 5;
        s.current_index = 3;
    }

    // Reload as a TWO-sequence program: structural difference.
    TestSeq a(4), b(4);
    Sequence sequences_arr[2] = {a.seq, b.seq};
    pool.init_sequence_program(state_id, sequences_arr, 2, 4.0f, false, &arena, 8);

    auto& s = pool.get<SequenceState>(state_id);
    CHECK(s.num_sequences == 2);
    CHECK(s.cycle_index == 0);
    CHECK(s.current_index == 0);
}

// ============================================================================
// Stress Tests [state_pool][stress]
// ============================================================================

TEST_CASE("StatePool stress test", "[state_pool][stress]") {
    StatePool pool;
    pool.set_fade_blocks(5);

    SECTION("create 200 states, gc 100, create 100 new - repeat 100x") {
        for (int cycle = 0; cycle < 100; ++cycle) {
            // Create 200 states (within MAX_STATES limit)
            for (int i = 0; i < 200; ++i) {
                std::uint32_t id = static_cast<std::uint32_t>(cycle * 10000 + i);
                pool.get_or_create<OscState>(id).phase = static_cast<float>(i) * 0.001f;
            }

            // Begin frame and only touch half
            pool.begin_frame();
            for (int i = 0; i < 100; ++i) {
                std::uint32_t id = static_cast<std::uint32_t>(cycle * 10000 + i);
                pool.touch(id);
            }
            pool.gc_sweep();

            // Create 100 new states
            for (int i = 200; i < 300; ++i) {
                std::uint32_t id = static_cast<std::uint32_t>(cycle * 10000 + i);
                pool.get_or_create<OscState>(id).phase = static_cast<float>(i) * 0.001f;
            }

            // Process fading
            for (int b = 0; b < 10; ++b) {
                pool.advance_fading();
            }
            pool.gc_fading();

            // Verify some states
            std::uint32_t check_id = static_cast<std::uint32_t>(cycle * 10000);
            if (pool.exists(check_id)) {
                CHECK_THAT(pool.get<OscState>(check_id).phase, WithinAbs(0.0f, 1e-6f));
            }
        }
    }

    SECTION("rapid state churn") {
        std::vector<std::uint32_t> ids;
        for (int i = 0; i < 100; ++i) {
            ids.push_back(fnv1a_hash_runtime(("state" + std::to_string(i)).c_str(),
                                              ("state" + std::to_string(i)).size()));
        }

        for (int frame = 0; frame < 1000; ++frame) {
            pool.begin_frame();

            // Create/touch random subset
            for (int i = 0; i < 50; ++i) {
                std::uint32_t id = ids[(frame + i) % ids.size()];
                pool.get_or_create<OscState>(id).phase = static_cast<float>(frame) * 0.001f;
                pool.touch(id);
            }

            pool.gc_sweep();
            pool.advance_fading();

            if (frame % 10 == 0) {
                pool.gc_fading();
            }
        }
    }
}
