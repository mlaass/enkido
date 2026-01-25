#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cedar/vm/env_map.hpp>
#include <cedar/vm/state_pool.hpp>
#include <cedar/dsp/constants.hpp>

#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <string>
#include <mutex>

using namespace cedar;
using Catch::Matchers::WithinAbs;

// ============================================================================
// Thread Safety Tests [thread]
// ============================================================================

TEST_CASE("EnvMap SPSC thread safety", "[thread]") {
    EnvMap env;
    env.set_sample_rate(48000.0f);
    env.set_default_slew_ms(5.0f);

    SECTION("producer-consumer with high contention") {
        std::atomic<bool> running{true};
        std::atomic<int> consumer_reads{0};
        std::atomic<int> producer_writes{0};

        // Pre-create parameters
        for (int i = 0; i < 16; ++i) {
            std::string name = "contention_" + std::to_string(i);
            env.set_param(name.c_str(), 0.0f);
        }

        // Consumer thread (simulates audio thread)
        std::thread consumer([&]() {
            std::vector<std::uint32_t> hashes;
            for (int i = 0; i < 16; ++i) {
                std::string name = "contention_" + std::to_string(i);
                hashes.push_back(fnv1a_hash_runtime(name.c_str(), name.size()));
            }

            while (running.load(std::memory_order_acquire)) {
                for (auto hash : hashes) {
                    float val = env.get(hash);
                    (void)val;
                    ++consumer_reads;
                }
                env.update_interpolation_sample();

                // Simulate audio thread timing (~21us per sample at 48kHz)
                // But we speed up for testing
                std::this_thread::yield();
            }
        });

        // Producer thread (simulates host thread)
        std::thread producer([&]() {
            for (int iter = 0; iter < 10000; ++iter) {
                for (int i = 0; i < 16; ++i) {
                    std::string name = "contention_" + std::to_string(i);
                    env.set_param(name.c_str(), static_cast<float>(iter % 100) * 0.01f);
                    ++producer_writes;
                }
                // Small delay between batches
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
            running.store(false, std::memory_order_release);
        });

        producer.join();
        consumer.join();

        CHECK(consumer_reads.load() > 0);
        CHECK(producer_writes.load() == 160000);
    }

    SECTION("rapid parameter updates during read") {
        std::atomic<bool> done{false};
        std::atomic<int> anomalies{0};

        env.set_param("rapid", 0.0f);
        std::uint32_t hash = fnv1a_hash("rapid");

        // Reader thread
        std::thread reader([&]() {
            float prev = 0.0f;
            while (!done.load(std::memory_order_acquire)) {
                float curr = env.get(hash);
                // Values should be between 0 and 100
                if (curr < -1.0f || curr > 101.0f) {
                    ++anomalies;
                }
                prev = curr;
                env.update_interpolation_sample();
            }
        });

        // Writer thread
        std::thread writer([&]() {
            for (int i = 0; i < 100000; ++i) {
                env.set_param("rapid", static_cast<float>(i % 100));
            }
            done.store(true, std::memory_order_release);
        });

        writer.join();
        reader.join();

        CHECK(anomalies.load() == 0);
    }
}

TEST_CASE("EnvMap multiple writers", "[thread]") {
    EnvMap env;
    env.set_sample_rate(48000.0f);
    env.set_default_slew_ms(0.0f);  // Instant for testing

    SECTION("4 threads writing distinct parameters") {
        std::vector<std::thread> writers;
        std::atomic<int> total_writes{0};

        for (int t = 0; t < 4; ++t) {
            writers.emplace_back([&, t]() {
                for (int i = 0; i < 1000; ++i) {
                    std::string name = "thread_" + std::to_string(t) + "_param";
                    env.set_param(name.c_str(), static_cast<float>(i));
                    ++total_writes;
                }
            });
        }

        for (auto& w : writers) {
            w.join();
        }

        CHECK(total_writes.load() == 4000);

        // All 4 params should exist
        for (int t = 0; t < 4; ++t) {
            std::string name = "thread_" + std::to_string(t) + "_param";
            CHECK(env.has_param(name.c_str()));
        }
    }

    SECTION("multiple threads writing same parameter") {
        std::vector<std::thread> writers;
        std::atomic<int> successful_writes{0};

        for (int t = 0; t < 4; ++t) {
            writers.emplace_back([&]() {
                for (int i = 0; i < 1000; ++i) {
                    if (env.set_param("shared", static_cast<float>(i))) {
                        ++successful_writes;
                    }
                }
            });
        }

        for (auto& w : writers) {
            w.join();
        }

        // All writes should succeed (updating same param)
        CHECK(successful_writes.load() == 4000);
        CHECK(env.has_param("shared"));
    }
}

TEST_CASE("EnvMap reader during capacity fill", "[thread]") {
    EnvMap env;
    env.set_sample_rate(48000.0f);
    env.set_default_slew_ms(0.0f);

    SECTION("continuous reads while filling to capacity") {
        std::atomic<bool> done{false};
        std::atomic<int> read_count{0};

        // Pre-create one param
        env.set_param("baseline", 42.0f);
        std::uint32_t baseline_hash = fnv1a_hash("baseline");

        // Reader thread
        std::thread reader([&]() {
            while (!done.load(std::memory_order_acquire)) {
                float val = env.get(baseline_hash);
                CHECK_THAT(val, WithinAbs(42.0f, 1e-3f));
                ++read_count;
                std::this_thread::yield();
            }
        });

        // Writer thread - fill capacity
        std::thread writer([&]() {
            for (std::size_t i = 0; i < MAX_ENV_PARAMS - 1; ++i) {
                std::string name = "fill_" + std::to_string(i);
                env.set_param(name.c_str(), static_cast<float>(i));
            }
            done.store(true, std::memory_order_release);
        });

        writer.join();
        reader.join();

        CHECK(read_count.load() > 0);
        CHECK(env.param_count() == MAX_ENV_PARAMS);
    }
}

// ============================================================================
// StatePool Thread Safety (Note: StatePool is NOT thread-safe by design)
// These tests verify single-threaded access patterns
// ============================================================================

TEST_CASE("StatePool single-thread safety patterns", "[thread]") {
    StatePool pool;
    pool.set_fade_blocks(5);

    SECTION("rapid state creation and GC") {
        for (int cycle = 0; cycle < 100; ++cycle) {
            pool.begin_frame();

            // Create states
            for (int i = 0; i < 100; ++i) {
                std::uint32_t id = static_cast<std::uint32_t>(cycle * 1000 + i);
                pool.get_or_create<OscState>(id).phase = static_cast<float>(i) * 0.001f;
            }

            // Touch only some
            for (int i = 0; i < 50; ++i) {
                std::uint32_t id = static_cast<std::uint32_t>(cycle * 1000 + i);
                pool.touch(id);
            }

            pool.gc_sweep();
            pool.advance_fading();
            pool.gc_fading();
        }
    }
}

// ============================================================================
// Integration: Simulated Audio Thread Pattern [thread]
// ============================================================================

TEST_CASE("Simulated audio/host thread pattern", "[thread]") {
    EnvMap env;
    env.set_sample_rate(48000.0f);
    env.set_default_slew_ms(10.0f);

    SECTION("realistic audio processing pattern") {
        std::atomic<bool> running{true};
        std::atomic<int> blocks_processed{0};

        // Pre-create parameters
        for (int i = 0; i < 8; ++i) {
            std::string name = "audio_param_" + std::to_string(i);
            env.set_param(name.c_str(), 0.5f);
        }

        std::vector<std::uint32_t> param_hashes;
        for (int i = 0; i < 8; ++i) {
            std::string name = "audio_param_" + std::to_string(i);
            param_hashes.push_back(fnv1a_hash_runtime(name.c_str(), name.size()));
        }

        // Audio thread - processes blocks
        std::thread audio_thread([&]() {
            while (running.load(std::memory_order_acquire)) {
                // Simulate block processing
                for (int sample = 0; sample < 128; ++sample) {
                    // Read all params
                    for (auto hash : param_hashes) {
                        float val = env.get(hash);
                        (void)val;
                    }
                    env.update_interpolation_sample();
                }

                ++blocks_processed;

                // Simulate ~2.67ms block time (speed up for test)
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });

        // Host thread - sends parameter updates
        std::thread host_thread([&]() {
            for (int update = 0; update < 500; ++update) {
                // Update random parameter
                int idx = update % 8;
                std::string name = "audio_param_" + std::to_string(idx);
                float value = static_cast<float>(update % 100) * 0.01f;
                env.set_param(name.c_str(), value);

                // Simulate UI update rate (~60fps)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            running.store(false, std::memory_order_release);
        });

        host_thread.join();
        audio_thread.join();

        CHECK(blocks_processed.load() > 0);
    }

    SECTION("burst parameter updates") {
        std::atomic<bool> done{false};
        std::atomic<float> last_read{0.0f};

        env.set_param("burst", 0.0f);
        std::uint32_t hash = fnv1a_hash("burst");

        // Reader
        std::thread reader([&]() {
            while (!done.load(std::memory_order_acquire)) {
                float val = env.get(hash);
                last_read.store(val, std::memory_order_release);
                env.update_interpolation_sample();
            }
        });

        // Burst writer
        std::thread writer([&]() {
            for (int burst = 0; burst < 10; ++burst) {
                // Burst of 100 rapid updates
                for (int i = 0; i < 100; ++i) {
                    env.set_param("burst", static_cast<float>(burst * 100 + i));
                }
                // Pause between bursts
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            done.store(true, std::memory_order_release);
        });

        writer.join();
        reader.join();

        // Final value should be near 999
        float final_target = env.get_target(hash);
        CHECK_THAT(final_target, WithinAbs(999.0f, 1.0f));
    }
}

// ============================================================================
// Stress: Many Threads [thread][stress]
// ============================================================================

TEST_CASE("High thread count stress", "[thread][stress]") {
    EnvMap env;
    env.set_sample_rate(48000.0f);
    env.set_default_slew_ms(1.0f);

    SECTION("10 writer threads") {
        std::vector<std::thread> threads;
        std::atomic<int> total_writes{0};

        for (int t = 0; t < 10; ++t) {
            threads.emplace_back([&, t]() {
                for (int i = 0; i < 100; ++i) {
                    std::string name = "thread" + std::to_string(t) + "_p" + std::to_string(i % 10);
                    env.set_param(name.c_str(), static_cast<float>(i));
                    ++total_writes;
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        CHECK(total_writes.load() == 1000);
    }

    SECTION("mixed readers and writers") {
        std::atomic<bool> done{false};
        std::atomic<int> reads{0};
        std::atomic<int> writes{0};

        // Pre-create params
        for (int i = 0; i < 20; ++i) {
            std::string name = "mixed_" + std::to_string(i);
            env.set_param(name.c_str(), 0.0f);
        }

        std::vector<std::thread> threads;

        // 3 reader threads
        for (int r = 0; r < 3; ++r) {
            threads.emplace_back([&]() {
                while (!done.load(std::memory_order_acquire)) {
                    for (int i = 0; i < 20; ++i) {
                        std::string name = "mixed_" + std::to_string(i);
                        std::uint32_t hash = fnv1a_hash_runtime(name.c_str(), name.size());
                        float val = env.get(hash);
                        (void)val;
                        ++reads;
                    }
                    std::this_thread::yield();
                }
            });
        }

        // 2 writer threads
        for (int w = 0; w < 2; ++w) {
            threads.emplace_back([&, w]() {
                for (int iter = 0; iter < 500; ++iter) {
                    for (int i = 0; i < 20; ++i) {
                        std::string name = "mixed_" + std::to_string(i);
                        env.set_param(name.c_str(), static_cast<float>(iter + w));
                        ++writes;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
                done.store(true, std::memory_order_release);
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        CHECK(reads.load() > 0);
        CHECK(writes.load() == 20000);
    }
}
