#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cedar/vm/audio_arena.hpp>
#include <cedar/vm/buffer_pool.hpp>
#include <cedar/vm/state_pool.hpp>
#include <cedar/vm/env_map.hpp>
#include <cedar/vm/crossfade_state.hpp>
#include <cedar/dsp/constants.hpp>

#include <random>
#include <vector>
#include <string>
#include <chrono>

using namespace cedar;
using Catch::Matchers::WithinAbs;

// ============================================================================
// Cross-class Memory Stress Tests [stress]
// ============================================================================

TEST_CASE("Cross-class memory stress: VM simulation", "[stress]") {
    // Simulate a VM's memory usage pattern across multiple components

    AudioArena arena(4 * 1024 * 1024);  // 4MB arena
    BufferPool pool;
    StatePool states;
    EnvMap env;

    states.set_fade_blocks(10);
    env.set_sample_rate(48000.0f);
    env.set_default_slew_ms(5.0f);

    SECTION("1000 frames of audio processing simulation") {
        std::mt19937 rng(42);

        // Pre-create some states
        for (int i = 0; i < 100; ++i) {
            std::string name = "osc_" + std::to_string(i);
            std::uint32_t id = fnv1a_hash_runtime(name.c_str(), name.size());
            states.get_or_create<OscState>(id).phase = (440.0f + static_cast<float>(i)) * 0.001f;
        }

        // Set up environment parameters
        for (int i = 0; i < 32; ++i) {
            std::string name = "param_" + std::to_string(i);
            env.set_param(name.c_str(), static_cast<float>(i) * 0.1f);
        }

        // Simulate 1000 audio frames
        for (int frame = 0; frame < 1000; ++frame) {
            // Begin frame
            states.begin_frame();

            // Use buffers
            for (std::uint16_t b = 0; b < 32; ++b) {
                float* buf = pool.get(b);
                for (std::size_t s = 0; s < BLOCK_SIZE; ++s) {
                    buf[s] = static_cast<float>(frame + b + s) * 0.001f;
                }
            }

            // Touch active states
            for (int i = 0; i < 50; ++i) {
                std::string name = "osc_" + std::to_string((frame + i) % 100);
                std::uint32_t id = fnv1a_hash_runtime(name.c_str(), name.size());
                if (states.exists(id)) {
                    states.touch(id);
                }
            }

            // Create some new states
            if (frame % 10 == 0) {
                std::string name = "new_state_" + std::to_string(frame);
                std::uint32_t id = fnv1a_hash_runtime(name.c_str(), name.size());
                states.get_or_create<OscState>(id).phase = static_cast<float>(frame) * 0.001f;
            }

            // Update env params
            if (frame % 5 == 0) {
                int param_idx = frame % 32;
                std::string name = "param_" + std::to_string(param_idx);
                env.set_param(name.c_str(), static_cast<float>(frame) * 0.01f);
            }

            // Process interpolation
            env.update_interpolation_block();

            // GC
            states.gc_sweep();
            states.advance_fading();

            if (frame % 50 == 0) {
                states.gc_fading();
            }
        }

        // Verify system is still functional
        float* buf = pool.get(0);
        CHECK(buf != nullptr);
        CHECK(env.param_count() == 32);
    }
}

TEST_CASE("Cross-class memory stress: Arena + BufferPool interaction", "[stress]") {
    AudioArena arena(1024 * 1024);  // 1MB
    BufferPool pool;

    SECTION("interleaved usage pattern") {
        std::mt19937 rng(123);

        for (int cycle = 0; cycle < 100; ++cycle) {
            // Allocate from arena
            std::size_t arena_size = (rng() % 1000) + 100;
            float* arena_buf = arena.allocate(arena_size);

            // Use buffer pool
            std::uint16_t pool_idx = rng() % MAX_BUFFERS;
            float* pool_buf = pool.get(pool_idx);

            if (arena_buf != nullptr) {
                // Fill arena buffer
                for (std::size_t i = 0; i < arena_size; ++i) {
                    arena_buf[i] = static_cast<float>(cycle);
                }
            }

            // Fill pool buffer
            pool.fill(pool_idx, static_cast<float>(cycle));

            // Verify isolation
            if (arena_buf != nullptr) {
                CHECK_THAT(arena_buf[0], WithinAbs(static_cast<float>(cycle), 1e-6f));
            }
            CHECK_THAT(pool_buf[0], WithinAbs(static_cast<float>(cycle), 1e-6f));

            // Occasionally reset arena
            if (cycle % 20 == 0) {
                arena.reset();
            }
        }
    }
}

TEST_CASE("Cross-class memory stress: StatePool + EnvMap coordination", "[stress]") {
    StatePool states;
    EnvMap env;

    states.set_fade_blocks(5);
    env.set_sample_rate(48000.0f);
    env.set_default_slew_ms(10.0f);

    SECTION("coordinated state and parameter management") {
        // Create paired states and params
        for (int i = 0; i < 50; ++i) {
            std::string state_name = "synth_" + std::to_string(i);
            std::string param_name = "synth_" + std::to_string(i) + "_freq";

            std::uint32_t state_id = fnv1a_hash_runtime(state_name.c_str(), state_name.size());
            states.get_or_create<OscState>(state_id).phase = static_cast<float>(i) * 0.1f;

            env.set_param(param_name.c_str(), 440.0f * (1.0f + static_cast<float>(i) * 0.1f));
        }

        // Simulate program changes
        for (int change = 0; change < 100; ++change) {
            states.begin_frame();

            // Keep some, remove others
            for (int i = 0; i < 50; ++i) {
                if ((i + change) % 3 != 0) {  // Keep 2/3
                    std::string state_name = "synth_" + std::to_string(i);
                    std::uint32_t state_id = fnv1a_hash_runtime(state_name.c_str(), state_name.size());
                    if (states.exists(state_id)) {
                        states.touch(state_id);
                    }
                }
            }

            states.gc_sweep();

            // Update corresponding params
            for (int i = 0; i < 50; ++i) {
                std::string param_name = "synth_" + std::to_string(i) + "_freq";
                env.set_param(param_name.c_str(), 440.0f + static_cast<float>(change));
            }

            // Process
            for (int s = 0; s < 128; ++s) {
                env.update_interpolation_sample();
            }

            states.advance_fading();

            if (change % 10 == 0) {
                states.gc_fading();
            }

            // Recreate removed states
            for (int i = 0; i < 50; ++i) {
                std::string state_name = "synth_" + std::to_string(i);
                std::uint32_t state_id = fnv1a_hash_runtime(state_name.c_str(), state_name.size());
                if (!states.exists(state_id)) {
                    states.get_or_create<OscState>(state_id).phase = 0.0f;
                }
            }
        }

        CHECK(env.param_count() == 50);
    }
}

TEST_CASE("Cross-class memory stress: Crossfade during state transitions", "[stress]") {
    BufferPool pool;
    StatePool states;
    CrossfadeState xfade;
    CrossfadeBuffers xfade_bufs;

    states.set_fade_blocks(8);

    SECTION("crossfade with state pool GC") {
        // Create initial states
        for (int i = 0; i < 20; ++i) {
            std::uint32_t id = static_cast<std::uint32_t>(i);
            states.get_or_create<OscState>(id).phase = static_cast<float>(i) * 0.01f;
        }

        // Simulate 50 program switches
        for (int switch_num = 0; switch_num < 50; ++switch_num) {
            // Start crossfade
            xfade.begin(8);

            // Fill old buffers
            for (std::size_t s = 0; s < BLOCK_SIZE; ++s) {
                xfade_bufs.old_left[s] = std::sin(static_cast<float>(s) * 0.1f);
                xfade_bufs.old_right[s] = std::sin(static_cast<float>(s) * 0.1f);
            }

            // Begin state transition
            states.begin_frame();

            // Touch only half the states
            for (int i = 0; i < 10; ++i) {
                std::uint32_t id = static_cast<std::uint32_t>((i + switch_num) % 20);
                if (states.exists(id)) {
                    states.touch(id);
                }
            }

            states.gc_sweep();

            // Process crossfade blocks
            while (!xfade.is_completing()) {
                xfade.advance();

                // Fill new buffers
                for (std::size_t s = 0; s < BLOCK_SIZE; ++s) {
                    xfade_bufs.new_left[s] = std::cos(static_cast<float>(s) * 0.1f);
                    xfade_bufs.new_right[s] = std::cos(static_cast<float>(s) * 0.1f);
                }

                // Mix
                float* out_left = pool.get(0);
                float* out_right = pool.get(1);
                xfade_bufs.mix_equal_power(out_left, out_right, xfade.position());

                // Process fading states
                states.advance_fading();
            }

            xfade.complete();
            states.gc_fading();

            // Recreate missing states
            for (int i = 0; i < 20; ++i) {
                std::uint32_t id = static_cast<std::uint32_t>(i);
                if (!states.exists(id)) {
                    states.get_or_create<OscState>(id).phase = static_cast<float>(switch_num) * 0.01f;
                }
            }
        }
    }
}

TEST_CASE("Cross-class memory stress: Maximum capacity test", "[stress]") {
    BufferPool pool;
    EnvMap env;
    StatePool states;

    env.set_sample_rate(48000.0f);
    env.set_default_slew_ms(1.0f);

    SECTION("fill all available slots") {
        // Fill EnvMap to capacity
        std::size_t env_count = 0;
        for (std::size_t i = 0; i < MAX_ENV_PARAMS + 10; ++i) {
            std::string name = "p" + std::to_string(i);
            if (env.set_param(name.c_str(), static_cast<float>(i))) {
                ++env_count;
            } else {
                break;
            }
        }
        CHECK(env_count == MAX_ENV_PARAMS);

        // Fill StatePool with many states
        for (std::uint32_t i = 0; i < 2000; ++i) {
            states.get_or_create<OscState>(i).phase = static_cast<float>(i) * 0.001f;
        }
        CHECK(states.size() == 2000);

        // Use all buffer pool buffers
        for (std::uint16_t i = 0; i < MAX_BUFFERS; ++i) {
            pool.fill(i, static_cast<float>(i));
        }

        // Verify everything still works
        for (std::uint16_t i = 0; i < MAX_BUFFERS; ++i) {
            float* buf = pool.get(i);
            CHECK_THAT(buf[0], WithinAbs(static_cast<float>(i), 1e-6f));
        }
    }
}

TEST_CASE("Cross-class memory stress: Long-running simulation", "[stress]") {
    BufferPool pool;
    StatePool states;
    EnvMap env;

    states.set_fade_blocks(5);
    env.set_sample_rate(48000.0f);
    env.set_default_slew_ms(5.0f);

    SECTION("simulate 1 minute of real-time audio") {
        // 1 minute at 48kHz with 128 sample blocks = ~22500 blocks
        const int total_blocks = 22500;

        // Initial setup
        for (int i = 0; i < 16; ++i) {
            std::string name = "voice_" + std::to_string(i);
            std::uint32_t id = fnv1a_hash_runtime(name.c_str(), name.size());
            states.get_or_create<OscState>(id).phase = static_cast<float>(i) / 12.0f;
        }

        for (int i = 0; i < 8; ++i) {
            std::string name = "ctrl_" + std::to_string(i);
            env.set_param(name.c_str(), 0.5f);
        }

        // Process blocks
        for (int block = 0; block < total_blocks; ++block) {
            states.begin_frame();

            // Simulate note on/off events
            if (block % 100 == 0) {
                int voice = (block / 100) % 16;
                std::string name = "voice_" + std::to_string(voice);
                std::uint32_t id = fnv1a_hash_runtime(name.c_str(), name.size());
                states.touch(id);
            }

            // Occasional parameter changes
            if (block % 50 == 0) {
                int ctrl = block % 8;
                std::string name = "ctrl_" + std::to_string(ctrl);
                env.set_param(name.c_str(), static_cast<float>(block % 100) * 0.01f);
            }

            // Audio processing simulation
            for (std::uint16_t b = 0; b < 16; ++b) {
                float* buf = pool.get(b);
                for (std::size_t s = 0; s < BLOCK_SIZE; ++s) {
                    buf[s] = std::sin(static_cast<float>(block * BLOCK_SIZE + s) * 0.01f);
                }
            }

            env.update_interpolation_block();
            states.gc_sweep();
            states.advance_fading();

            if (block % 100 == 0) {
                states.gc_fading();
            }
        }

        // Verify system health after long run
        CHECK(env.param_count() <= 8);
        float* buf = pool.get(0);
        CHECK(buf != nullptr);
    }
}
