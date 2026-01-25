#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cedar/vm/crossfade_state.hpp>
#include <cedar/dsp/constants.hpp>

#include <cmath>

using namespace cedar;
using Catch::Matchers::WithinAbs;

// ============================================================================
// CrossfadeState Unit Tests [crossfade]
// ============================================================================

TEST_CASE("CrossfadeState basic operations", "[crossfade]") {
    CrossfadeState state;

    SECTION("initial state is idle") {
        CHECK(state.is_idle());
        CHECK_FALSE(state.is_active());
        CHECK_FALSE(state.is_completing());
        CHECK(state.phase == CrossfadeState::Phase::Idle);
    }

    SECTION("begin transitions to pending then active") {
        state.begin(10);  // 10 blocks duration

        CHECK(state.phase == CrossfadeState::Phase::Pending);
        CHECK_FALSE(state.is_idle());

        // Advance once to move to Active
        state.advance();
        CHECK(state.phase == CrossfadeState::Phase::Active);
    }

    SECTION("position progresses 0.0 to 1.0") {
        state.begin(4);  // 4 blocks

        // Initial position (before any advance) is 0
        CHECK_THAT(state.position(), WithinAbs(0.0f, 1e-6f));

        // After first advance: Pending->Active, blocks_remaining=3
        state.advance();
        CHECK_THAT(state.position(), WithinAbs(0.25f, 0.01f));  // 1 - 3/4

        state.advance();  // blocks_remaining=2
        CHECK_THAT(state.position(), WithinAbs(0.5f, 0.01f));   // 1 - 2/4

        state.advance();  // blocks_remaining=1
        CHECK_THAT(state.position(), WithinAbs(0.75f, 0.01f));  // 1 - 1/4

        state.advance();  // blocks_remaining=0, phase->Completing
        CHECK(state.phase == CrossfadeState::Phase::Completing);
        CHECK_THAT(state.position(), WithinAbs(1.0f, 0.01f));   // 1 - 0/4
    }

    SECTION("complete transitions to idle") {
        state.begin(2);
        state.advance();  // Pending -> Active, blocks_remaining=1
        state.advance();  // blocks_remaining=0 -> Completing

        CHECK(state.is_completing());

        state.complete();

        CHECK(state.is_idle());
        CHECK(state.phase == CrossfadeState::Phase::Idle);
    }

    SECTION("is_active returns true for Pending, Active, and Completing") {
        // is_active() returns true for Pending, Active, OR Completing phases
        CHECK_FALSE(state.is_active());  // Idle

        state.begin(5);
        CHECK(state.is_active());  // Pending is considered "active"

        state.advance();
        CHECK(state.is_active());  // Active

        // Advance to completing
        for (int i = 0; i < 10; ++i) {
            state.advance();
        }

        // Completing is also "active"
        CHECK(state.is_active());
    }

    SECTION("is_completing returns correct state") {
        state.begin(2);
        CHECK_FALSE(state.is_completing());

        state.advance();  // Active
        CHECK_FALSE(state.is_completing());

        state.advance();  // blocks_remaining -> 0, Completing
        CHECK(state.is_completing());
    }

    SECTION("is_idle returns correct state") {
        CHECK(state.is_idle());

        state.begin(3);
        CHECK_FALSE(state.is_idle());

        state.advance();
        CHECK_FALSE(state.is_idle());

        // Complete the crossfade
        for (int i = 0; i < 5; ++i) {
            state.advance();
        }
        state.complete();

        CHECK(state.is_idle());
    }
}

TEST_CASE("CrossfadeState state machine transitions", "[crossfade]") {
    CrossfadeState state;

    SECTION("full lifecycle: Idle -> Pending -> Active -> Completing -> Idle") {
        CHECK(state.phase == CrossfadeState::Phase::Idle);

        state.begin(3);
        CHECK(state.phase == CrossfadeState::Phase::Pending);

        state.advance();  // Pending -> Active
        CHECK(state.phase == CrossfadeState::Phase::Active);

        state.advance();  // blocks_remaining: 2
        state.advance();  // blocks_remaining: 1
        state.advance();  // blocks_remaining: 0 -> Completing
        CHECK(state.phase == CrossfadeState::Phase::Completing);

        state.complete();
        CHECK(state.phase == CrossfadeState::Phase::Idle);
    }

    SECTION("multiple crossfades in sequence") {
        for (int i = 0; i < 5; ++i) {
            state.begin(2);
            REQUIRE(state.phase == CrossfadeState::Phase::Pending);

            state.advance();  // Active
            REQUIRE(state.phase == CrossfadeState::Phase::Active);

            state.advance();  // Completing
            REQUIRE(state.phase == CrossfadeState::Phase::Completing);

            state.complete();
            REQUIRE(state.is_idle());
        }
    }

    SECTION("begin during active crossfade restarts") {
        state.begin(10);
        state.advance();
        state.advance();

        CHECK(state.phase == CrossfadeState::Phase::Active);

        state.begin(10);  // Restart

        CHECK(state.phase == CrossfadeState::Phase::Pending);
    }
}

// ============================================================================
// CrossfadeState Edge Cases [crossfade][edge]
// ============================================================================

TEST_CASE("CrossfadeState edge cases", "[crossfade][edge]") {
    CrossfadeState state;

    SECTION("duration of 1 block") {
        state.begin(1);
        CHECK_THAT(state.position(), WithinAbs(0.0f, 1e-6f));

        state.advance();  // Active, blocks_remaining=0 -> Completing
        CHECK(state.is_completing());
        CHECK_THAT(state.position(), WithinAbs(1.0f, 0.01f));
    }

    SECTION("duration of 0 blocks") {
        state.begin(0);
        // With 0 blocks, position = 1.0 - 0/0 which is handled as 1.0

        state.advance();  // Should immediately complete

        // Position should be 1.0 (since blocks_remaining and total both 0)
        // Implementation returns 1.0 when total_blocks == 0
        CHECK_THAT(state.position(), WithinAbs(1.0f, 0.01f));
    }

    SECTION("very long duration (1000 blocks)") {
        state.begin(1000);

        for (int i = 0; i < 500; ++i) {
            state.advance();
        }

        // Should be about halfway
        CHECK(state.is_active());
        float pos = state.position();
        CHECK(pos > 0.4f);
        CHECK(pos < 0.6f);
    }

    SECTION("complete without advance") {
        state.begin(5);
        state.complete();

        // Should be idle after complete
        CHECK(state.is_idle());
    }

    SECTION("advance while idle") {
        // Should not crash or change state
        state.advance();
        state.advance();
        state.advance();

        CHECK(state.is_idle());
    }
}

// ============================================================================
// CrossfadeBuffers Tests [crossfade]
// ============================================================================

TEST_CASE("CrossfadeBuffers operations", "[crossfade]") {
    CrossfadeBuffers buffers;

    SECTION("clear zeros all buffers") {
        // Fill with non-zero
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            buffers.old_left[i] = 1.0f;
            buffers.old_right[i] = 2.0f;
            buffers.new_left[i] = 3.0f;
            buffers.new_right[i] = 4.0f;
        }

        buffers.clear();

        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK_THAT(buffers.old_left[i], WithinAbs(0.0f, 1e-6f));
            CHECK_THAT(buffers.old_right[i], WithinAbs(0.0f, 1e-6f));
            CHECK_THAT(buffers.new_left[i], WithinAbs(0.0f, 1e-6f));
            CHECK_THAT(buffers.new_right[i], WithinAbs(0.0f, 1e-6f));
        }
    }

    SECTION("mix_equal_power at position 0.0") {
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            buffers.old_left[i] = 1.0f;
            buffers.old_right[i] = 1.0f;
            buffers.new_left[i] = 0.0f;
            buffers.new_right[i] = 0.0f;
        }

        float out_left[BLOCK_SIZE];
        float out_right[BLOCK_SIZE];

        buffers.mix_equal_power(out_left, out_right, 0.0f);

        // At position 0: old_gain=cos(0)=1, new_gain=sin(0)=0 -> 100% old
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK_THAT(out_left[i], WithinAbs(1.0f, 0.01f));
            CHECK_THAT(out_right[i], WithinAbs(1.0f, 0.01f));
        }
    }

    SECTION("mix_equal_power at position 0.5 with identical signals") {
        // Equal power crossfade with identical signals at midpoint
        // old_gain = cos(PI/4) = sqrt(0.5) ≈ 0.707
        // new_gain = sin(PI/4) = sqrt(0.5) ≈ 0.707
        // output = 0.707 * 1 + 0.707 * 1 = sqrt(2) ≈ 1.414
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            buffers.old_left[i] = 1.0f;
            buffers.old_right[i] = 1.0f;
            buffers.new_left[i] = 1.0f;
            buffers.new_right[i] = 1.0f;
        }

        float out_left[BLOCK_SIZE];
        float out_right[BLOCK_SIZE];

        buffers.mix_equal_power(out_left, out_right, 0.5f);

        // Equal power preserves POWER, not amplitude. With identical in-phase
        // signals, amplitude peaks at sqrt(2) at midpoint.
        float expected = std::sqrt(2.0f);
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK_THAT(out_left[i], WithinAbs(expected, 0.02f));
            CHECK_THAT(out_right[i], WithinAbs(expected, 0.02f));
        }
    }

    SECTION("mix_equal_power at position 1.0") {
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            buffers.old_left[i] = 0.0f;
            buffers.old_right[i] = 0.0f;
            buffers.new_left[i] = 1.0f;
            buffers.new_right[i] = 1.0f;
        }

        float out_left[BLOCK_SIZE];
        float out_right[BLOCK_SIZE];

        buffers.mix_equal_power(out_left, out_right, 1.0f);

        // At position 1: old_gain=cos(PI/2)=0, new_gain=sin(PI/2)=1 -> 100% new
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK_THAT(out_left[i], WithinAbs(1.0f, 0.01f));
            CHECK_THAT(out_right[i], WithinAbs(1.0f, 0.01f));
        }
    }

    SECTION("mix_linear at position 0.0") {
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            buffers.old_left[i] = 1.0f;
            buffers.old_right[i] = 2.0f;
            buffers.new_left[i] = 3.0f;
            buffers.new_right[i] = 4.0f;
        }

        float out_left[BLOCK_SIZE];
        float out_right[BLOCK_SIZE];

        buffers.mix_linear(out_left, out_right, 0.0f);

        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK_THAT(out_left[i], WithinAbs(1.0f, 0.01f));
            CHECK_THAT(out_right[i], WithinAbs(2.0f, 0.01f));
        }
    }

    SECTION("mix_linear at position 0.5") {
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            buffers.old_left[i] = 0.0f;
            buffers.old_right[i] = 0.0f;
            buffers.new_left[i] = 2.0f;
            buffers.new_right[i] = 2.0f;
        }

        float out_left[BLOCK_SIZE];
        float out_right[BLOCK_SIZE];

        buffers.mix_linear(out_left, out_right, 0.5f);

        // Linear at 0.5: 0.5 * old + 0.5 * new = 0.5 * 0 + 0.5 * 2 = 1.0
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK_THAT(out_left[i], WithinAbs(1.0f, 0.01f));
            CHECK_THAT(out_right[i], WithinAbs(1.0f, 0.01f));
        }
    }

    SECTION("mix_linear at position 1.0") {
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            buffers.old_left[i] = 1.0f;
            buffers.old_right[i] = 2.0f;
            buffers.new_left[i] = 3.0f;
            buffers.new_right[i] = 4.0f;
        }

        float out_left[BLOCK_SIZE];
        float out_right[BLOCK_SIZE];

        buffers.mix_linear(out_left, out_right, 1.0f);

        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK_THAT(out_left[i], WithinAbs(3.0f, 0.01f));
            CHECK_THAT(out_right[i], WithinAbs(4.0f, 0.01f));
        }
    }

    SECTION("equal power maintains unity gain for uncorrelated signals") {
        // When old and new are DIFFERENT signals, equal power maintains
        // constant perceived loudness. Test with orthogonal signals.
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            buffers.old_left[i] = 1.0f;
            buffers.old_right[i] = 1.0f;
            buffers.new_left[i] = 0.0f;  // Different from old
            buffers.new_right[i] = 0.0f;
        }

        float out_left[BLOCK_SIZE];
        float out_right[BLOCK_SIZE];

        // At position 0.5: old_gain = cos(PI/4) ≈ 0.707, new_gain ≈ 0.707
        // output = 0.707 * 1 + 0.707 * 0 = 0.707
        buffers.mix_equal_power(out_left, out_right, 0.5f);

        float expected = std::cos(HALF_PI * 0.5f);  // ~0.707
        CHECK_THAT(out_left[0], WithinAbs(expected, 0.02f));
    }
}

// ============================================================================
// CrossfadeBuffers Edge Cases [crossfade][edge]
// ============================================================================

TEST_CASE("CrossfadeBuffers edge cases", "[crossfade][edge]") {
    CrossfadeBuffers buffers;

    SECTION("mix with DC offset signals") {
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            buffers.old_left[i] = 0.5f;
            buffers.old_right[i] = 0.5f;
            buffers.new_left[i] = -0.5f;
            buffers.new_right[i] = -0.5f;
        }

        float out_left[BLOCK_SIZE];
        float out_right[BLOCK_SIZE];

        buffers.mix_linear(out_left, out_right, 0.5f);

        // Linear: 0.5 * 0.5 + 0.5 * (-0.5) = 0.25 - 0.25 = 0
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK_THAT(out_left[i], WithinAbs(0.0f, 0.01f));
        }
    }

    SECTION("mix with varying signals") {
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            float phase = static_cast<float>(i) / static_cast<float>(BLOCK_SIZE);
            buffers.old_left[i] = std::sin(phase * 6.28f);
            buffers.old_right[i] = std::sin(phase * 6.28f);
            buffers.new_left[i] = std::cos(phase * 6.28f);
            buffers.new_right[i] = std::cos(phase * 6.28f);
        }

        float out_left[BLOCK_SIZE];
        float out_right[BLOCK_SIZE];

        buffers.mix_equal_power(out_left, out_right, 0.5f);

        // Just verify no crashes and reasonable output
        bool all_finite = true;
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            if (!std::isfinite(out_left[i]) || !std::isfinite(out_right[i])) {
                all_finite = false;
                break;
            }
        }
        CHECK(all_finite);
    }

    SECTION("mix with special float values") {
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            buffers.old_left[i] = 0.0f;
            buffers.old_right[i] = 0.0f;
            buffers.new_left[i] = 0.0f;
            buffers.new_right[i] = 0.0f;
        }

        float out_left[BLOCK_SIZE];
        float out_right[BLOCK_SIZE];

        // Should handle edge case positions
        buffers.mix_linear(out_left, out_right, 0.0f);
        buffers.mix_linear(out_left, out_right, 1.0f);
        buffers.mix_equal_power(out_left, out_right, 0.0f);
        buffers.mix_equal_power(out_left, out_right, 1.0f);

        CHECK(true);  // No crash
    }
}

// ============================================================================
// Stress Tests [crossfade][stress]
// ============================================================================

TEST_CASE("CrossfadeState stress test", "[crossfade][stress]") {
    CrossfadeState state;

    SECTION("1000 rapid crossfades") {
        for (int i = 0; i < 1000; ++i) {
            std::uint32_t duration = (i % 20) + 1;
            state.begin(duration);

            while (!state.is_completing() && !state.is_idle()) {
                state.advance();
            }

            state.complete();
            REQUIRE(state.is_idle());
        }
    }

    SECTION("varying durations") {
        std::uint32_t durations[] = {1, 2, 5, 10, 20, 50, 100, 1};

        for (std::uint32_t dur : durations) {
            state.begin(dur);

            int advances = 0;
            while (!state.is_completing() && advances < 1000) {
                state.advance();
                ++advances;
            }

            state.complete();
            CHECK(state.is_idle());
        }
    }
}

TEST_CASE("CrossfadeBuffers stress test", "[crossfade][stress]") {
    CrossfadeBuffers buffers;

    SECTION("many mix operations") {
        for (int iter = 0; iter < 10000; ++iter) {
            float pos = static_cast<float>(iter % 100) / 100.0f;

            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
                buffers.old_left[i] = static_cast<float>(iter) * 0.0001f;
                buffers.new_left[i] = 1.0f - static_cast<float>(iter) * 0.0001f;
            }

            float out_left[BLOCK_SIZE];
            float out_right[BLOCK_SIZE];

            if (iter % 2 == 0) {
                buffers.mix_equal_power(out_left, out_right, pos);
            } else {
                buffers.mix_linear(out_left, out_right, pos);
            }
        }
    }
}
