#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cedar/vm/env_map.hpp>
#include <cedar/vm/state_pool.hpp>  // For fnv1a_hash

#include <cmath>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>

using namespace cedar;
using Catch::Matchers::WithinAbs;

// ============================================================================
// Unit Tests [env_map]
// ============================================================================

TEST_CASE("EnvMap unit tests: basic operations", "[env_map]") {
    EnvMap env;
    env.set_sample_rate(48000.0f);
    env.set_default_slew_ms(0.0f);  // No smoothing for basic tests

    SECTION("set_param and get round-trip") {
        REQUIRE(env.set_param("volume", 0.5f));

        std::uint32_t hash = fnv1a_hash("volume");
        float value = env.get(hash);
        CHECK_THAT(value, WithinAbs(0.5f, 1e-6f));
    }

    SECTION("set_param and get_target") {
        env.set_param("freq", 440.0f);

        std::uint32_t hash = fnv1a_hash("freq");
        float target = env.get_target(hash);
        CHECK_THAT(target, WithinAbs(440.0f, 1e-6f));
    }

    SECTION("has_param accuracy") {
        CHECK_FALSE(env.has_param("nonexistent"));

        env.set_param("exists", 1.0f);
        CHECK(env.has_param("exists"));
        CHECK_FALSE(env.has_param("still_nonexistent"));
    }

    SECTION("has_param_hash accuracy") {
        std::uint32_t hash = fnv1a_hash("test_hash");
        CHECK_FALSE(env.has_param_hash(hash));

        env.set_param("test_hash", 1.0f);
        CHECK(env.has_param_hash(hash));
    }

    SECTION("remove_param cleanup") {
        env.set_param("removable", 123.0f);
        REQUIRE(env.has_param("removable"));

        env.remove_param("removable");
        CHECK_FALSE(env.has_param("removable"));
    }

    SECTION("param_count tracks correctly") {
        CHECK(env.param_count() == 0);

        env.set_param("p1", 1.0f);
        CHECK(env.param_count() == 1);

        env.set_param("p2", 2.0f);
        CHECK(env.param_count() == 2);

        env.set_param("p1", 1.5f);  // Update, not add
        CHECK(env.param_count() == 2);

        // Note: remove_param marks slot inactive but doesn't decrement count
        // (param_count tracks allocated slots, not active params)
        env.remove_param("p1");
        CHECK(env.param_count() == 2);  // Still 2 allocated slots
        CHECK_FALSE(env.has_param("p1"));  // But p1 is no longer active
    }

    SECTION("reset clears all parameters") {
        env.set_param("a", 1.0f);
        env.set_param("b", 2.0f);
        env.set_param("c", 3.0f);

        REQUIRE(env.param_count() == 3);

        env.reset();

        CHECK(env.param_count() == 0);
        CHECK_FALSE(env.has_param("a"));
        CHECK_FALSE(env.has_param("b"));
        CHECK_FALSE(env.has_param("c"));
    }
}

TEST_CASE("EnvMap unit tests: interpolation", "[env_map]") {
    EnvMap env;
    env.set_sample_rate(48000.0f);
    env.set_default_slew_ms(100.0f);  // 100ms smoothing

    SECTION("interpolation converges to target") {
        env.set_param("smooth", 1.0f);
        std::uint32_t hash = fnv1a_hash("smooth");

        // Initial value should be at target (first set)
        CHECK_THAT(env.get(hash), WithinAbs(1.0f, 1e-6f));

        // Change target
        env.set_param("smooth", 0.0f);

        // Immediately after change, should still be near 1.0
        CHECK_THAT(env.get(hash), WithinAbs(1.0f, 0.1f));

        // Update interpolation many times
        for (int i = 0; i < 48000; ++i) {  // 1 second worth
            env.update_interpolation_sample();
        }

        // Should have converged to 0.0
        CHECK_THAT(env.get(hash), WithinAbs(0.0f, 0.01f));
    }

    SECTION("block-based interpolation") {
        env.set_param("block_smooth", 0.5f);
        std::uint32_t hash = fnv1a_hash("block_smooth");

        env.set_param("block_smooth", 1.0f);

        // Update per block (128 samples assumed)
        for (int block = 0; block < 1000; ++block) {
            env.update_interpolation_block();
        }

        CHECK_THAT(env.get(hash), WithinAbs(1.0f, 0.01f));
    }

    SECTION("custom slew time per parameter") {
        env.set_param("fast", 0.0f, 10.0f);   // 10ms slew
        env.set_param("slow", 0.0f, 500.0f);  // 500ms slew

        std::uint32_t fast_hash = fnv1a_hash("fast");
        std::uint32_t slow_hash = fnv1a_hash("slow");

        env.set_param("fast", 1.0f, 10.0f);
        env.set_param("slow", 1.0f, 500.0f);

        // After some updates, fast should converge faster
        for (int i = 0; i < 4800; ++i) {  // 100ms worth
            env.update_interpolation_sample();
        }

        float fast_val = env.get(fast_hash);
        float slow_val = env.get(slow_hash);

        // Fast should be closer to target
        CHECK(std::abs(1.0f - fast_val) < std::abs(1.0f - slow_val));
    }

    SECTION("zero slew time gives instant change") {
        env.set_default_slew_ms(0.0f);
        env.set_param("instant", 0.0f);
        std::uint32_t hash = fnv1a_hash("instant");

        env.set_param("instant", 1.0f);

        // For existing params, current isn't set immediately - need one update
        // With slew coeff = 1.0, one update sets current = target
        env.update_interpolation_sample();

        CHECK_THAT(env.get(hash), WithinAbs(1.0f, 1e-6f));
    }
}

// ============================================================================
// Edge Cases [env_map][edge]
// ============================================================================

TEST_CASE("EnvMap unit tests: edge cases", "[env_map][edge]") {
    EnvMap env;
    env.set_sample_rate(48000.0f);
    env.set_default_slew_ms(10.0f);

    SECTION("fill all MAX_ENV_PARAMS slots") {
        bool all_ok = true;
        for (std::size_t i = 0; i < MAX_ENV_PARAMS; ++i) {
            std::string name = "param_" + std::to_string(i);
            if (!env.set_param(name.c_str(), static_cast<float>(i))) {
                all_ok = false;
                break;
            }
        }

        CHECK(all_ok);
        CHECK(env.param_count() == MAX_ENV_PARAMS);

        // Next should fail
        CHECK_FALSE(env.set_param("overflow", 999.0f));
    }

    SECTION("hash table collision handling via linear probing") {
        // Create params that might hash to similar slots
        std::vector<std::string> params;
        for (int i = 0; i < 100; ++i) {
            params.push_back("collision_test_" + std::to_string(i));
        }

        for (const auto& p : params) {
            REQUIRE(env.set_param(p.c_str(), 1.0f));
        }

        // All should be retrievable
        for (const auto& p : params) {
            CHECK(env.has_param(p.c_str()));
        }
    }

    SECTION("slew extremes - 0ms") {
        env.set_param("zero_slew", 0.0f, 0.0f);
        std::uint32_t hash = fnv1a_hash("zero_slew");

        env.set_param("zero_slew", 1.0f, 0.0f);

        // For existing params, need one update cycle even with instant slew
        env.update_interpolation_sample();

        CHECK_THAT(env.get(hash), WithinAbs(1.0f, 1e-6f));
    }

    SECTION("slew extremes - long slew") {
        // Note: EnvMap clamps slew_coeff to minimum 0.0001, so very long slew
        // times (>~208ms at 48kHz) all converge at the same rate.
        // Test with 500ms slew to verify interpolation works over time.
        env.set_param("long_slew", 0.0f, 500.0f);
        std::uint32_t hash = fnv1a_hash("long_slew");

        env.set_param("long_slew", 1.0f, 500.0f);

        // After ~100ms worth of samples, should be partially interpolated
        for (int i = 0; i < 4800; ++i) {  // 100ms at 48kHz
            env.update_interpolation_sample();
        }

        float val = env.get(hash);
        // Should have moved from 0 toward 1, but not reached target
        CHECK(val > 0.1f);
        CHECK(val < 0.9f);
    }

    SECTION("negative values") {
        env.set_param("neg", -100.0f);
        std::uint32_t hash = fnv1a_hash("neg");
        CHECK_THAT(env.get(hash), WithinAbs(-100.0f, 1e-4f));
    }

    SECTION("very small values") {
        env.set_param("tiny", 1e-10f);
        std::uint32_t hash = fnv1a_hash("tiny");
        CHECK_THAT(env.get(hash), WithinAbs(1e-10f, 1e-12f));
    }

    SECTION("very large values") {
        env.set_param("huge", 1e10f);
        std::uint32_t hash = fnv1a_hash("huge");
        CHECK_THAT(env.get(hash), WithinAbs(1e10f, 1e8f));
    }

    SECTION("get non-existent hash returns 0") {
        std::uint32_t hash = fnv1a_hash("does_not_exist");
        float val = env.get(hash);
        CHECK_THAT(val, WithinAbs(0.0f, 1e-6f));
    }

    SECTION("update existing parameter multiple times") {
        env.set_param("update_test", 1.0f);
        env.set_param("update_test", 2.0f);
        env.set_param("update_test", 3.0f);
        env.set_param("update_test", 4.0f);

        std::uint32_t hash = fnv1a_hash("update_test");
        CHECK_THAT(env.get_target(hash), WithinAbs(4.0f, 1e-6f));
    }

    SECTION("remove and re-add parameter") {
        env.set_param("readdable", 1.0f);
        REQUIRE(env.has_param("readdable"));

        env.remove_param("readdable");
        REQUIRE_FALSE(env.has_param("readdable"));

        env.set_param("readdable", 2.0f);
        CHECK(env.has_param("readdable"));
        CHECK_THAT(env.get_target(fnv1a_hash("readdable")), WithinAbs(2.0f, 1e-6f));
    }

    SECTION("remove non-existent parameter") {
        // Should not crash
        env.remove_param("never_existed");
        CHECK(env.param_count() == 0);
    }
}

// ============================================================================
// Concurrency Tests [env_map][thread]
// ============================================================================

TEST_CASE("EnvMap unit tests: thread safety", "[env_map][thread]") {
    EnvMap env;
    env.set_sample_rate(48000.0f);
    env.set_default_slew_ms(10.0f);

    SECTION("single producer single consumer") {
        std::atomic<bool> running{true};
        std::atomic<int> updates_processed{0};

        // Audio thread (consumer)
        std::thread audio_thread([&]() {
            std::uint32_t hash = fnv1a_hash("spsc_test");
            while (running.load()) {
                float val = env.get(hash);
                (void)val;  // Just read
                env.update_interpolation_sample();
                ++updates_processed;

                // Simulate block timing
                if (updates_processed.load() % 128 == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            }
        });

        // Host thread (producer)
        for (int i = 0; i < 1000; ++i) {
            env.set_param("spsc_test", static_cast<float>(i % 100) * 0.01f);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }

        running.store(false);
        audio_thread.join();

        CHECK(updates_processed.load() > 0);
    }

    SECTION("multiple host threads writing parameters") {
        std::atomic<int> total_writes{0};
        std::vector<std::thread> writers;

        for (int t = 0; t < 4; ++t) {
            writers.emplace_back([&, t]() {
                for (int i = 0; i < 250; ++i) {
                    std::string name = "writer_" + std::to_string(t) + "_param";
                    env.set_param(name.c_str(), static_cast<float>(i));
                    ++total_writes;
                }
            });
        }

        for (auto& w : writers) {
            w.join();
        }

        CHECK(total_writes.load() == 1000);
    }

    SECTION("concurrent read and write different params") {
        std::atomic<bool> running{true};
        std::atomic<int> reads{0};

        // Pre-create params
        for (int i = 0; i < 10; ++i) {
            std::string name = "concurrent_" + std::to_string(i);
            env.set_param(name.c_str(), 0.0f);
        }

        // Reader thread
        std::thread reader([&]() {
            while (running.load()) {
                for (int i = 0; i < 10; ++i) {
                    std::string name = "concurrent_" + std::to_string(i);
                    std::uint32_t hash = fnv1a_hash_runtime(name.c_str(), name.size());
                    float val = env.get(hash);
                    (void)val;
                    ++reads;
                }
            }
        });

        // Writer thread
        for (int iter = 0; iter < 100; ++iter) {
            for (int i = 0; i < 10; ++i) {
                std::string name = "concurrent_" + std::to_string(i);
                env.set_param(name.c_str(), static_cast<float>(iter));
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        running.store(false);
        reader.join();

        CHECK(reads.load() > 0);
    }
}

// ============================================================================
// Stress Tests [env_map][stress]
// ============================================================================

TEST_CASE("EnvMap unit tests: stress test", "[env_map][stress]") {
    EnvMap env;
    env.set_sample_rate(48000.0f);
    env.set_default_slew_ms(5.0f);

    SECTION("rapid parameter updates") {
        for (int i = 0; i < 100000; ++i) {
            std::string name = "stress_" + std::to_string(i % 50);
            env.set_param(name.c_str(), static_cast<float>(i % 1000) * 0.001f);
        }

        // All 50 params should exist
        for (int i = 0; i < 50; ++i) {
            std::string name = "stress_" + std::to_string(i);
            CHECK(env.has_param(name.c_str()));
        }
    }

    SECTION("interpolation under heavy load") {
        // Create params
        for (int i = 0; i < 32; ++i) {
            std::string name = "interp_" + std::to_string(i);
            env.set_param(name.c_str(), 0.0f);
        }

        // Simulate 10 seconds of audio processing
        for (int block = 0; block < 3750; ++block) {  // ~10s at 128 samples
            env.update_interpolation_block();

            // Occasionally change targets
            if (block % 100 == 0) {
                for (int i = 0; i < 32; ++i) {
                    std::string name = "interp_" + std::to_string(i);
                    env.set_param(name.c_str(), static_cast<float>((block + i) % 100) * 0.01f);
                }
            }
        }
    }
}
