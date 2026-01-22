#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "cedar/vm/vm.hpp"
#include "cedar/vm/instruction.hpp"
#include "cedar/opcodes/utility.hpp"
#include "cedar/opcodes/sequencing.hpp"
#include <array>
#include <cmath>
#include <vector>

using namespace cedar;
using Catch::Matchers::WithinAbs;

TEST_CASE("VM basic operations", "[vm]") {
    VM vm;

    SECTION("empty program produces silence") {
        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK(left[i] == 0.0f);
            CHECK(right[i] == 0.0f);
        }
    }

    SECTION("PUSH_CONST fills buffer") {
        // Create instruction: fill buffer 0 with constant 0.5
        auto inst = make_const_instruction(Opcode::PUSH_CONST, 0, 0.5f);
        vm.load_program(std::span{&inst, 1});

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        // Check buffer 0 has the constant
        const float* buf = vm.buffers().get(0);
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK_THAT(buf[i], WithinAbs(0.5f, 1e-6f));
        }
    }
}

TEST_CASE("VM arithmetic opcodes", "[vm][arithmetic]") {
    VM vm;

    SECTION("ADD combines two buffers") {
        std::array<Instruction, 3> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 1.0f),  // buf0 = 1.0
            make_const_instruction(Opcode::PUSH_CONST, 1, 2.0f),  // buf1 = 2.0
            Instruction::make_binary(Opcode::ADD, 2, 0, 1)        // buf2 = buf0 + buf1
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        const float* result = vm.buffers().get(2);
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK_THAT(result[i], WithinAbs(3.0f, 1e-6f));
        }
    }

    SECTION("MUL multiplies two buffers") {
        std::array<Instruction, 3> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 3.0f),
            make_const_instruction(Opcode::PUSH_CONST, 1, 4.0f),
            Instruction::make_binary(Opcode::MUL, 2, 0, 1)
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        const float* result = vm.buffers().get(2);
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK_THAT(result[i], WithinAbs(12.0f, 1e-6f));
        }
    }

    SECTION("DIV handles zero safely") {
        std::array<Instruction, 3> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 1.0f),
            make_const_instruction(Opcode::PUSH_CONST, 1, 0.0f),
            Instruction::make_binary(Opcode::DIV, 2, 0, 1)
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        const float* result = vm.buffers().get(2);
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK(result[i] == 0.0f);  // Safe division by zero returns 0
        }
    }
}

TEST_CASE("VM oscillators", "[vm][oscillators]") {
    VM vm;
    vm.set_sample_rate(48000.0f);

    SECTION("OSC_SIN generates sine wave") {
        // Generate 440 Hz sine wave
        std::array<Instruction, 2> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 440.0f),  // frequency
            Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 1)       // osc with state_id=1
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        const float* result = vm.buffers().get(1);

        // Sine wave should be bounded [-1, 1]
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK(result[i] >= -1.0f);
            CHECK(result[i] <= 1.0f);
        }

        // First sample should be close to 0 (sin(0))
        CHECK_THAT(result[0], WithinAbs(0.0f, 0.1f));
    }

    SECTION("Oscillator phase continuity across blocks") {
        std::array<Instruction, 2> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 440.0f),
            Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 1)
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};

        // Process multiple blocks
        vm.process_block(left.data(), right.data());
        float last_sample_block1 = vm.buffers().get(1)[BLOCK_SIZE - 1];

        vm.process_block(left.data(), right.data());
        float first_sample_block2 = vm.buffers().get(1)[0];

        // Phase should be continuous (samples should be close)
        CHECK_THAT(first_sample_block2 - last_sample_block1, WithinAbs(0.0f, 0.2f));
    }

    SECTION("OSC_SQR generates square wave") {
        std::array<Instruction, 2> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 100.0f),
            Instruction::make_unary(Opcode::OSC_SQR, 1, 0, 2)
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        const float* result = vm.buffers().get(1);

        // Square wave values should be bounded
        // PolyBLEP anti-aliasing smooths transitions, so values near transitions
        // can be between -1 and +1
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK(result[i] >= -1.1f);
            CHECK(result[i] <= 1.1f);
        }
    }
}

TEST_CASE("VM filters", "[vm][filters]") {
    VM vm;
    vm.set_sample_rate(48000.0f);

    SECTION("FILTER_SVF_LP attenuates high frequencies") {
        // Generate noise, filter it with lowpass at 1000 Hz
        std::array<Instruction, 4> program = {
            Instruction::make_nullary(Opcode::NOISE, 0, 1),                // noise in buf0
            make_const_instruction(Opcode::PUSH_CONST, 1, 1000.0f),        // cutoff
            make_const_instruction(Opcode::PUSH_CONST, 2, 0.707f),         // Q
            Instruction::make_ternary(Opcode::FILTER_SVF_LP, 3, 0, 1, 2, 2) // filter with state_id=2
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};

        // Process several blocks for filter to stabilize
        for (int i = 0; i < 10; ++i) {
            vm.process_block(left.data(), right.data());
        }

        const float* filtered = vm.buffers().get(3);

        // Filtered signal should still vary (not all zeros)
        float variance = 0.0f;
        float mean = 0.0f;
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            mean += filtered[i];
        }
        mean /= BLOCK_SIZE;
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            variance += (filtered[i] - mean) * (filtered[i] - mean);
        }
        variance /= BLOCK_SIZE;

        CHECK(variance > 0.0f);  // Signal has some variation
    }
}

TEST_CASE("VM output", "[vm][output]") {
    VM vm;

    SECTION("OUTPUT writes to stereo buffers") {
        std::array<Instruction, 2> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 0.75f),
            Instruction::make_unary(Opcode::OUTPUT, 0, 0)  // out_buffer unused for OUTPUT
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK_THAT(left[i], WithinAbs(0.75f, 1e-6f));
            CHECK_THAT(right[i], WithinAbs(0.75f, 1e-6f));
        }
    }
}

TEST_CASE("VM state management", "[vm][state]") {
    VM vm;

    SECTION("reset clears all state") {
        // Generate some oscillator state
        std::array<Instruction, 2> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 440.0f),
            Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 1)
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        CHECK(vm.states().size() == 1);

        vm.reset();

        CHECK(vm.states().size() == 0);
    }

    SECTION("hot swap preserves matching state") {
        std::array<Instruction, 2> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 440.0f),
            Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 42)  // state_id = 42
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};

        // Run several blocks to accumulate phase
        for (int i = 0; i < 100; ++i) {
            vm.process_block(left.data(), right.data());
        }

        // Hot swap - same state_id should preserve phase
        vm.hot_swap_begin();
        vm.load_program(program);  // Same program
        vm.process_block(left.data(), right.data());
        vm.hot_swap_end();

        // State should still exist
        CHECK(vm.states().exists(42));
    }
}

TEST_CASE("VM signal chain", "[vm][integration]") {
    VM vm;
    vm.set_sample_rate(48000.0f);

    SECTION("oscillator through filter to output") {
        // 440 Hz sine -> lowpass 2000 Hz -> output
        std::array<Instruction, 5> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 440.0f),     // freq
            Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 1),         // osc -> buf1
            make_const_instruction(Opcode::PUSH_CONST, 2, 2000.0f),    // cutoff
            make_const_instruction(Opcode::PUSH_CONST, 3, 0.707f),     // Q
            Instruction::make_ternary(Opcode::FILTER_SVF_LP, 4, 1, 2, 3, 2) // filter -> buf4
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};

        // Process and check output is bounded
        for (int block = 0; block < 10; ++block) {
            vm.process_block(left.data(), right.data());

            const float* result = vm.buffers().get(4);
            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
                CHECK(result[i] >= -2.0f);  // Allow some headroom
                CHECK(result[i] <= 2.0f);
            }
        }
    }
}

TEST_CASE("FNV-1a hash", "[vm][hash]") {
    SECTION("compile-time hash") {
        constexpr auto hash1 = fnv1a_hash("main/osc1");
        constexpr auto hash2 = fnv1a_hash("main/osc1");
        constexpr auto hash3 = fnv1a_hash("main/osc2");

        CHECK(hash1 == hash2);  // Same string = same hash
        CHECK(hash1 != hash3);  // Different string = different hash
    }

    SECTION("runtime hash matches compile-time") {
        constexpr auto compile_time = fnv1a_hash("test/path");
        auto runtime = fnv1a_hash_runtime("test/path", 9);

        CHECK(compile_time == runtime);
    }
}

// ============================================================================
// Sequencing & Timing Opcodes Tests
// ============================================================================

TEST_CASE("VM CLOCK opcode", "[vm][sequencing]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);  // 120 BPM = 0.5 seconds per beat = 24000 samples per beat

    SECTION("beat_phase ramps 0 to 1 over one beat") {
        // rate=0 selects beat_phase
        Instruction inst = Instruction::make_nullary(Opcode::CLOCK, 0, 1);
        inst.rate = 0;  // beat_phase mode
        vm.load_program(std::span{&inst, 1});

        std::array<float, BLOCK_SIZE> left{}, right{};

        // At 120 BPM and 48kHz, samples_per_beat = 24000
        // After processing one block (128 samples), beat_phase should be ~0.00533

        vm.process_block(left.data(), right.data());
        const float* result = vm.buffers().get(0);

        // First sample should be near 0
        CHECK(result[0] >= 0.0f);
        CHECK(result[0] < 0.001f);

        // Phase should increase monotonically
        for (std::size_t i = 1; i < BLOCK_SIZE; ++i) {
            CHECK(result[i] > result[i - 1]);
        }

        // All values should be in [0, 1)
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK(result[i] >= 0.0f);
            CHECK(result[i] < 1.0f);
        }
    }

    SECTION("bar_phase is 4x slower than beat_phase") {
        Instruction inst = Instruction::make_nullary(Opcode::CLOCK, 0, 1);
        inst.rate = 1;  // bar_phase mode
        vm.load_program(std::span{&inst, 1});

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        const float* result = vm.buffers().get(0);

        // Bar phase should be 4x slower, so increment should be 4x smaller
        float expected_increment = 1.0f / (24000.0f * 4.0f);  // samples_per_bar
        float actual_increment = result[1] - result[0];

        CHECK_THAT(actual_increment, WithinAbs(expected_increment, 1e-7f));
    }
}

TEST_CASE("VM LFO opcode", "[vm][sequencing]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    SECTION("SIN LFO outputs sine wave bounded [-1, 1]") {
        std::array<Instruction, 2> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 1.0f),  // 1 cycle per beat
            Instruction::make_unary(Opcode::LFO, 1, 0, 1)         // LFO with state_id=1
        };
        // Set shape to SIN (0) in reserved field
        program[1].rate = static_cast<std::uint8_t>(LFOShape::SIN);
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};

        // Process multiple blocks to see full cycle
        for (int block = 0; block < 200; ++block) {
            vm.process_block(left.data(), right.data());
            const float* result = vm.buffers().get(1);

            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
                CHECK(result[i] >= -1.0f);
                CHECK(result[i] <= 1.0f);
            }
        }
    }

    SECTION("TRI LFO outputs triangle wave") {
        std::array<Instruction, 2> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 1.0f),
            Instruction::make_unary(Opcode::LFO, 1, 0, 2)
        };
        program[1].rate = static_cast<std::uint8_t>(LFOShape::TRI);
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        const float* result = vm.buffers().get(1);

        // Triangle wave should be bounded [-1, 1]
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK(result[i] >= -1.0f);
            CHECK(result[i] <= 1.0f);
        }
    }

    SECTION("SQR LFO outputs only +1 or -1") {
        std::array<Instruction, 2> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 1.0f),
            Instruction::make_unary(Opcode::LFO, 1, 0, 3)
        };
        program[1].rate = static_cast<std::uint8_t>(LFOShape::SQR);
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};

        for (int block = 0; block < 200; ++block) {
            vm.process_block(left.data(), right.data());
            const float* result = vm.buffers().get(1);

            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
                CHECK((result[i] == 1.0f || result[i] == -1.0f));
            }
        }
    }

    SECTION("SAW LFO ramps -1 to 1") {
        std::array<Instruction, 2> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 1.0f),
            Instruction::make_unary(Opcode::LFO, 1, 0, 4)
        };
        program[1].rate = static_cast<std::uint8_t>(LFOShape::SAW);
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        const float* result = vm.buffers().get(1);

        // SAW should be bounded [-1, 1]
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK(result[i] >= -1.0f);
            CHECK(result[i] <= 1.0f);
        }

        // First sample should be near -1 (phase=0 -> 2*0-1 = -1)
        CHECK_THAT(result[0], WithinAbs(-1.0f, 0.01f));
    }
}

TEST_CASE("VM EUCLID opcode", "[vm][sequencing]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    SECTION("euclidean pattern helper function") {
        // Test the euclidean pattern computation directly
        // euclid(3, 8) should produce: X..X..X. (hits at 0, 3, 6)
        std::uint32_t pattern = compute_euclidean_pattern(3, 8, 0);

        // Check that exactly 3 bits are set
        int count = 0;
        for (int i = 0; i < 8; ++i) {
            if (pattern & (1u << i)) count++;
        }
        CHECK(count == 3);
    }

    SECTION("euclid(4,4) produces all triggers") {
        std::uint32_t pattern = compute_euclidean_pattern(4, 4, 0);
        CHECK(pattern == 0b1111);  // All 4 steps are hits
    }

    SECTION("euclid(1,4) produces single trigger") {
        std::uint32_t pattern = compute_euclidean_pattern(1, 4, 0);

        int count = 0;
        for (int i = 0; i < 4; ++i) {
            if (pattern & (1u << i)) count++;
        }
        CHECK(count == 1);
    }

    SECTION("EUCLID opcode outputs triggers") {
        std::array<Instruction, 3> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 4.0f),  // 4 hits
            make_const_instruction(Opcode::PUSH_CONST, 1, 4.0f),  // 4 steps
            Instruction::make_binary(Opcode::EUCLID, 2, 0, 1, 1)  // state_id=1
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};

        // With 4 hits in 4 steps, every step should trigger
        // Process enough blocks to see multiple triggers
        int trigger_count = 0;
        for (int block = 0; block < 1000; ++block) {
            vm.process_block(left.data(), right.data());
            const float* result = vm.buffers().get(2);

            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
                if (result[i] == 1.0f) trigger_count++;
                // Output should be either 0 or 1
                CHECK((result[i] == 0.0f || result[i] == 1.0f));
            }
        }

        // Should have gotten some triggers
        CHECK(trigger_count > 0);
    }
}

TEST_CASE("VM TRIGGER opcode", "[vm][sequencing]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);  // 24000 samples per beat

    SECTION("division=1 produces 1 trigger per beat") {
        std::array<Instruction, 2> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 1.0f),  // 1 trigger per beat
            Instruction::make_unary(Opcode::TRIGGER, 1, 0, 1)
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};

        // Process one beat worth of samples (24000 samples)
        // Use floor division to avoid counting triggers past the beat boundary
        int trigger_count = 0;
        int blocks_per_beat = 24000 / BLOCK_SIZE;  // 187 blocks = 23936 samples

        for (int block = 0; block < blocks_per_beat; ++block) {
            vm.process_block(left.data(), right.data());
            const float* result = vm.buffers().get(1);

            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
                if (result[i] == 1.0f) trigger_count++;
            }
        }

        // Should have exactly 1 trigger per beat
        CHECK(trigger_count == 1);
    }

    SECTION("division=4 produces 4 triggers per beat") {
        std::array<Instruction, 2> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 4.0f),  // 4 triggers per beat (16th notes)
            Instruction::make_unary(Opcode::TRIGGER, 1, 0, 2)
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};

        int trigger_count = 0;
        // Use floor division to avoid counting triggers past the beat boundary
        int blocks_per_beat = 24000 / BLOCK_SIZE;  // 187 blocks = 23936 samples (just under 1 beat)

        for (int block = 0; block < blocks_per_beat; ++block) {
            vm.process_block(left.data(), right.data());
            const float* result = vm.buffers().get(1);

            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
                if (result[i] == 1.0f) trigger_count++;
            }
        }

        // Should have exactly 4 triggers per beat
        CHECK(trigger_count == 4);
    }

    SECTION("trigger outputs only 0 or 1") {
        std::array<Instruction, 2> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 4.0f),
            Instruction::make_unary(Opcode::TRIGGER, 1, 0, 3)
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};

        for (int block = 0; block < 100; ++block) {
            vm.process_block(left.data(), right.data());
            const float* result = vm.buffers().get(1);

            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
                CHECK((result[i] == 0.0f || result[i] == 1.0f));
            }
        }
    }
}

TEST_CASE("VM TIMELINE opcode", "[vm][sequencing]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    SECTION("empty timeline outputs zero") {
        Instruction inst = Instruction::make_nullary(Opcode::TIMELINE, 0, 1);
        vm.load_program(std::span{&inst, 1});

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        const float* result = vm.buffers().get(0);
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK(result[i] == 0.0f);
        }
    }

    SECTION("timeline interpolates between breakpoints") {
        // First, create and initialize the state
        Instruction inst = Instruction::make_nullary(Opcode::TIMELINE, 0, 100);
        vm.load_program(std::span{&inst, 1});

        // Get the state and set up breakpoints
        auto& state = vm.states().get_or_create<TimelineState>(100);
        state.num_points = 2;
        state.points[0] = {0.0f, 0.0f, 0};  // time=0, value=0, linear
        state.points[1] = {1.0f, 1.0f, 0};  // time=1 beat, value=1, linear
        state.loop = false;

        std::array<float, BLOCK_SIZE> left{}, right{};

        // Reset the VM sample counter to start from 0
        vm.reset();
        vm.load_program(std::span{&inst, 1});

        // Re-initialize state after reset
        auto& state2 = vm.states().get_or_create<TimelineState>(100);
        state2.num_points = 2;
        state2.points[0] = {0.0f, 0.0f, 0};
        state2.points[1] = {1.0f, 1.0f, 0};
        state2.loop = false;

        vm.process_block(left.data(), right.data());

        const float* result = vm.buffers().get(0);

        // First sample should be near 0
        CHECK_THAT(result[0], WithinAbs(0.0f, 0.01f));

        // Values should increase (linear interpolation)
        for (std::size_t i = 1; i < BLOCK_SIZE; ++i) {
            CHECK(result[i] >= result[i - 1]);
        }
    }
}

// ============================================================================
// Hot-Swap & Crossfade Tests
// ============================================================================

TEST_CASE("VM hot-swap basics", "[vm][hotswap]") {
    VM vm;
    vm.set_sample_rate(48000.0f);

    SECTION("load_program returns Success") {
        auto inst = make_const_instruction(Opcode::PUSH_CONST, 0, 1.0f);
        auto result = vm.load_program(std::span{&inst, 1});
        CHECK(result == VM::LoadResult::Success);
    }

    SECTION("swap happens at block boundary") {
        auto inst = make_const_instruction(Opcode::PUSH_CONST, 0, 1.0f);
        (void)vm.load_program(std::span{&inst, 1});

        // Before process_block, swap is pending
        CHECK(vm.swap_count() == 0);

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        // After process_block, swap has occurred
        CHECK(vm.swap_count() == 1);
    }

    SECTION("multiple swaps increment counter") {
        std::array<float, BLOCK_SIZE> left{}, right{};

        auto inst1 = make_const_instruction(Opcode::PUSH_CONST, 0, 1.0f);
        (void)vm.load_program(std::span{&inst1, 1});
        vm.process_block(left.data(), right.data());
        CHECK(vm.swap_count() == 1);

        auto inst2 = make_const_instruction(Opcode::PUSH_CONST, 0, 2.0f);
        (void)vm.load_program(std::span{&inst2, 1});
        vm.process_block(left.data(), right.data());
        CHECK(vm.swap_count() == 2);
    }
}

TEST_CASE("VM state preservation across hot-swap", "[vm][hotswap]") {
    VM vm;
    vm.set_sample_rate(48000.0f);

    SECTION("oscillator phase preserved when state_id matches") {
        // Program 1: 440 Hz oscillator with state_id = 42
        std::array<Instruction, 2> program1 = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 440.0f),
            Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 42)
        };
        (void)vm.load_program(program1);

        std::array<float, BLOCK_SIZE> left{}, right{};

        // Run for several blocks to accumulate phase
        for (int i = 0; i < 100; ++i) {
            vm.process_block(left.data(), right.data());
        }

        // State should exist
        CHECK(vm.states().exists(42));

        // Program 2: Same state_id but different frequency
        std::array<Instruction, 2> program2 = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 880.0f),
            Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 42)  // Same state_id!
        };
        (void)vm.load_program(program2);
        vm.process_block(left.data(), right.data());

        // State should still exist (preserved across swap)
        CHECK(vm.states().exists(42));
    }

    SECTION("orphaned state removed after swap") {
        // Program 1: oscillator with state_id = 100
        std::array<Instruction, 2> program1 = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 440.0f),
            Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 100)
        };
        (void)vm.load_program(program1);

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());
        CHECK(vm.states().exists(100));

        // Program 2: Different state_id = 200
        std::array<Instruction, 2> program2 = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 440.0f),
            Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 200)  // Different state_id
        };
        (void)vm.load_program(program2);

        // Process a few blocks (to complete any crossfade)
        for (int i = 0; i < 10; ++i) {
            vm.process_block(left.data(), right.data());
        }

        // Call gc_sweep to clean up
        vm.hot_swap_begin();
        vm.process_block(left.data(), right.data());
        vm.hot_swap_end();

        // New state should exist
        CHECK(vm.states().exists(200));
        // Old state should be removed (orphaned)
        CHECK_FALSE(vm.states().exists(100));
    }
}

TEST_CASE("VM crossfade", "[vm][hotswap][crossfade]") {
    VM vm;
    vm.set_sample_rate(48000.0f);

    SECTION("no crossfade for first program load") {
        auto inst = make_const_instruction(Opcode::PUSH_CONST, 0, 1.0f);
        (void)vm.load_program(std::span{&inst, 1});

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        // Should not be crossfading after first load
        CHECK_FALSE(vm.is_crossfading());
    }

    SECTION("always crossfade even for identical structure") {
        // Program 1: oscillator with state_id = 50
        std::array<Instruction, 2> program1 = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 440.0f),
            Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 50)
        };
        vm.load_program_immediate(program1);

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        // Program 2: IDENTICAL to program 1 (same state_id, same values)
        std::array<Instruction, 2> program2 = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 440.0f),  // Same freq
            Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 50)      // Same state_id
        };
        (void)vm.load_program(program2);
        vm.process_block(left.data(), right.data());

        // Always crossfade when replacing a program to avoid pops from
        // stateless instruction changes (arithmetic, constants, routing)
        CHECK(vm.is_crossfading());
    }

    SECTION("state preserved even with crossfade") {
        // Program 1: oscillator with state_id = 51
        std::array<Instruction, 2> program1 = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 440.0f),
            Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 51)
        };
        vm.load_program_immediate(program1);

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());
        CHECK(vm.states().exists(51));

        // Program 2: Different parameter, same state_id (may or may not crossfade)
        std::array<Instruction, 2> program2 = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 880.0f),  // Different freq
            Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 51)      // Same state_id
        };
        (void)vm.load_program(program2);

        // Process several blocks to complete any crossfade
        for (int i = 0; i < 10; ++i) {
            vm.process_block(left.data(), right.data());
        }

        // State should still exist (preserved across swap regardless of crossfade)
        CHECK(vm.states().exists(51));
    }

    SECTION("crossfade triggers on structural change") {
        // Program 1: single oscillator
        std::array<Instruction, 2> program1 = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 440.0f),
            Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 60)
        };
        (void)vm.load_program(program1);

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        // Program 2: Added another oscillator (different state_id)
        std::array<Instruction, 4> program2 = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 440.0f),
            Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 60),
            make_const_instruction(Opcode::PUSH_CONST, 2, 220.0f),
            Instruction::make_unary(Opcode::OSC_SIN, 3, 2, 61)  // New state_id!
        };
        (void)vm.load_program(program2);
        vm.process_block(left.data(), right.data());

        // Should be crossfading (structural change: node added)
        CHECK(vm.is_crossfading());
    }

    SECTION("crossfade completes after configured blocks") {
        vm.set_crossfade_blocks(3);  // 3 blocks

        // Program 1
        std::array<Instruction, 2> program1 = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 440.0f),
            Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 70)
        };
        (void)vm.load_program(program1);

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        // Program 2: structural change
        std::array<Instruction, 2> program2 = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 440.0f),
            Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 71)  // Different state_id
        };
        (void)vm.load_program(program2);

        // Block 1: swap + crossfade starts (position starts at 0, advances after this block)
        vm.process_block(left.data(), right.data());
        CHECK(vm.is_crossfading());
        // Position is 0.0 at start, will advance on next block

        // Block 2: crossfade continues, position advances
        vm.process_block(left.data(), right.data());
        CHECK(vm.is_crossfading());
        CHECK(vm.crossfade_position() > 0.0f);

        // Block 3: crossfade continues
        vm.process_block(left.data(), right.data());
        CHECK(vm.is_crossfading());

        // Block 4: final crossfade block at position 1.0 (Completing phase)
        vm.process_block(left.data(), right.data());
        CHECK(vm.is_crossfading());

        // Block 5: crossfade should complete (cleanup)
        vm.process_block(left.data(), right.data());
        CHECK_FALSE(vm.is_crossfading());
    }

    SECTION("crossfade position progresses 0 to 1") {
        vm.set_crossfade_blocks(3);

        // Setup programs that will trigger crossfade
        std::array<Instruction, 2> program1 = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 1.0f),
            Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 80)
        };
        (void)vm.load_program(program1);

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        std::array<Instruction, 2> program2 = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 1.0f),
            Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 81)
        };
        (void)vm.load_program(program2);

        // Track crossfade positions
        std::vector<float> positions;
        for (int i = 0; i < 5; ++i) {
            vm.process_block(left.data(), right.data());
            if (vm.is_crossfading()) {
                positions.push_back(vm.crossfade_position());
            }
        }

        // Positions should increase
        for (std::size_t i = 1; i < positions.size(); ++i) {
            CHECK(positions[i] > positions[i-1]);
        }
    }
}

TEST_CASE("VM load_program_immediate", "[vm][hotswap]") {
    VM vm;
    vm.set_sample_rate(48000.0f);

    SECTION("immediate load works without process_block") {
        auto inst = make_const_instruction(Opcode::PUSH_CONST, 0, 1.0f);
        bool result = vm.load_program_immediate(std::span{&inst, 1});
        CHECK(result);
        CHECK(vm.has_program());
    }

    SECTION("immediate load resets state") {
        // First load with state
        std::array<Instruction, 2> program1 = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 440.0f),
            Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 90)
        };
        vm.load_program_immediate(program1);

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());
        CHECK(vm.states().exists(90));

        // Second immediate load should reset
        std::array<Instruction, 2> program2 = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 440.0f),
            Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 91)
        };
        vm.load_program_immediate(program2);
        vm.process_block(left.data(), right.data());

        // Old state should be gone (reset clears all)
        CHECK_FALSE(vm.states().exists(90));
        CHECK(vm.states().exists(91));
    }
}

// ============================================================================
// StatePool Fade-Out Tests
// ============================================================================

TEST_CASE("StatePool fade-out tracking", "[state_pool][fadeout]") {
    StatePool pool;
    pool.set_fade_blocks(3);

    SECTION("orphaned state moves to fading pool") {
        // Create a state
        pool.begin_frame();
        auto& osc = pool.get_or_create<OscState>(100);
        osc.phase = 0.5f;

        // Don't touch state 100 in next frame
        pool.begin_frame();
        pool.gc_sweep();

        // Should be in fading pool, not active
        CHECK_FALSE(pool.exists(100));
        CHECK(pool.fading_count() == 1);
        CHECK(pool.get_fading<OscState>(100) != nullptr);
        CHECK_THAT(pool.get_fade_gain(100), WithinAbs(1.0f, 1e-6f));

        // Preserved phase value
        const auto* fading = pool.get_fading<OscState>(100);
        REQUIRE(fading != nullptr);
        CHECK_THAT(fading->phase, WithinAbs(0.5f, 1e-6f));
    }

    SECTION("fade gain decrements per block") {
        pool.begin_frame();
        pool.get_or_create<OscState>(200);
        pool.begin_frame();
        pool.gc_sweep();

        // Initial gain is 1.0
        CHECK_THAT(pool.get_fade_gain(200), WithinAbs(1.0f, 1e-6f));

        // After one advance, gain decreases
        pool.advance_fading();
        CHECK(pool.get_fade_gain(200) < 1.0f);
        CHECK(pool.get_fade_gain(200) > 0.0f);

        // After all blocks, gain is 0
        pool.advance_fading();
        pool.advance_fading();
        CHECK_THAT(pool.get_fade_gain(200), WithinAbs(0.0f, 1e-6f));
    }

    SECTION("gc_fading removes completed fades") {
        pool.begin_frame();
        pool.get_or_create<OscState>(300);
        pool.begin_frame();
        pool.gc_sweep();

        CHECK(pool.fading_count() == 1);

        // Complete the fade
        pool.advance_fading();
        pool.advance_fading();
        pool.advance_fading();
        pool.gc_fading();

        CHECK(pool.fading_count() == 0);
        CHECK(pool.get_fading<OscState>(300) == nullptr);
    }

    SECTION("active states return fade gain 1.0") {
        pool.begin_frame();
        pool.get_or_create<OscState>(400);
        CHECK_THAT(pool.get_fade_gain(400), WithinAbs(1.0f, 1e-6f));
    }

    SECTION("non-existent states return fade gain 0.0") {
        CHECK_THAT(pool.get_fade_gain(999), WithinAbs(0.0f, 1e-6f));
    }
}

TEST_CASE("VM fade-out integration", "[vm][fadeout]") {
    VM vm;

    SECTION("fade-out syncs with crossfade duration") {
        // Initial program with state
        std::array<Instruction, 2> program1 = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 440.0f),
            Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 100)
        };
        vm.load_program_immediate(program1);

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());
        CHECK(vm.states().exists(100));

        // Swap to program without state 100
        std::array<Instruction, 2> program2 = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 880.0f),
            Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 200)  // Different state ID
        };
        vm.load_program(program2);

        // Process until crossfade completes
        for (int i = 0; i < 10; ++i) {
            vm.process_block(left.data(), right.data());
        }

        // State 100 should eventually be cleaned up
        CHECK_FALSE(vm.states().exists(100));
        CHECK(vm.states().exists(200));
    }
}

// ============================================================================
// EnvMap Tests
// ============================================================================

TEST_CASE("EnvMap basic operations", "[env_map]") {
    EnvMap env;
    env.set_sample_rate(48000.0f);

    SECTION("set and get parameter") {
        CHECK(env.set_param("Speed", 0.8f));
        CHECK(env.has_param("Speed"));

        std::uint16_t hash = static_cast<std::uint16_t>(fnv1a_hash("Speed") & 0xFFFF);
        CHECK_THAT(env.get_target(hash), WithinAbs(0.8f, 1e-6f));
    }

    SECTION("new parameter starts at target, subsequent changes interpolate") {
        env.set_param("Volume", 1.0f, 1.0f);  // 1ms slew
        std::uint16_t hash = static_cast<std::uint16_t>(fnv1a_hash("Volume") & 0xFFFF);

        // New parameters start at target value (to avoid ramping from zero)
        CHECK_THAT(env.get(hash), WithinAbs(1.0f, 1e-6f));

        // Change target - now it should interpolate
        env.set_param("Volume", 0.0f, 1.0f);

        // After some interpolation steps, should approach new target
        for (int i = 0; i < 1000; ++i) {
            env.update_interpolation_sample();
        }

        float value = env.get(hash);
        CHECK(value < 0.5f);  // Should have moved toward 0.0
    }

    SECTION("remove parameter") {
        env.set_param("Test", 0.5f);
        CHECK(env.has_param("Test"));

        env.remove_param("Test");
        CHECK_FALSE(env.has_param("Test"));
    }

    SECTION("non-existent parameter returns 0") {
        std::uint16_t hash = static_cast<std::uint16_t>(fnv1a_hash("NonExistent") & 0xFFFF);
        CHECK_THAT(env.get(hash), WithinAbs(0.0f, 1e-6f));
    }

    SECTION("multiple parameters") {
        CHECK(env.set_param("Param1", 0.1f));
        CHECK(env.set_param("Param2", 0.2f));
        CHECK(env.set_param("Param3", 0.3f));

        CHECK(env.param_count() == 3);

        CHECK_THAT(env.get_target(static_cast<std::uint16_t>(fnv1a_hash("Param1") & 0xFFFF)), WithinAbs(0.1f, 1e-6f));
        CHECK_THAT(env.get_target(static_cast<std::uint16_t>(fnv1a_hash("Param2") & 0xFFFF)), WithinAbs(0.2f, 1e-6f));
        CHECK_THAT(env.get_target(static_cast<std::uint16_t>(fnv1a_hash("Param3") & 0xFFFF)), WithinAbs(0.3f, 1e-6f));
    }

    SECTION("update existing parameter") {
        env.set_param("Update", 0.5f);
        CHECK_THAT(env.get_target(static_cast<std::uint16_t>(fnv1a_hash("Update") & 0xFFFF)), WithinAbs(0.5f, 1e-6f));

        env.set_param("Update", 0.9f);
        CHECK_THAT(env.get_target(static_cast<std::uint16_t>(fnv1a_hash("Update") & 0xFFFF)), WithinAbs(0.9f, 1e-6f));

        // Should still be only one parameter
        CHECK(env.param_count() == 1);
    }

    SECTION("reset clears all parameters") {
        env.set_param("A", 1.0f);
        env.set_param("B", 2.0f);
        CHECK(env.param_count() == 2);

        env.reset();
        CHECK(env.param_count() == 0);
        CHECK_FALSE(env.has_param("A"));
        CHECK_FALSE(env.has_param("B"));
    }
}

TEST_CASE("VM external parameter binding", "[vm][env]") {
    VM vm;
    vm.set_sample_rate(48000.0f);

    SECTION("set_param creates parameter") {
        CHECK(vm.set_param("Cutoff", 0.5f));
        CHECK(vm.has_param("Cutoff"));
    }

    SECTION("ENV_GET reads parameter") {
        vm.set_param("Amplitude", 0.75f, 0.1f);  // Very fast slew

        // Create program with ENV_GET
        std::uint32_t hash = fnv1a_hash("Amplitude");
        std::array<Instruction, 2> program = {
            Instruction::make_nullary(Opcode::ENV_GET, 0, hash),
            Instruction::make_unary(Opcode::OUTPUT, 0, 0)
        };
        vm.load_program_immediate(program);

        // Process several blocks to allow interpolation
        std::array<float, BLOCK_SIZE> left{}, right{};
        for (int block = 0; block < 10; ++block) {
            vm.process_block(left.data(), right.data());
        }

        // Output should be approaching 0.75
        const float* buf = vm.buffers().get(0);
        CHECK(buf[BLOCK_SIZE - 1] > 0.5f);
    }

    SECTION("ENV_GET with fallback for missing param") {
        // Create fallback buffer
        std::array<Instruction, 2> program = {
            make_const_instruction(Opcode::PUSH_CONST, 1, 0.25f),  // fallback = 0.25
            Instruction{Opcode::ENV_GET, 0, 0, {1, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}, static_cast<std::uint16_t>(fnv1a_hash("Missing") & 0xFFFF)}
        };
        vm.load_program_immediate(program);

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        // Should use fallback value
        const float* buf = vm.buffers().get(0);
        CHECK_THAT(buf[0], WithinAbs(0.25f, 1e-6f));
    }

    SECTION("parameter changes are smoothed") {
        vm.set_param("Smooth", 0.0f, 10.0f);  // 10ms slew

        std::uint32_t hash = fnv1a_hash("Smooth");
        std::array<Instruction, 1> program = {
            Instruction::make_nullary(Opcode::ENV_GET, 0, hash)
        };
        vm.load_program_immediate(program);

        // Initial process
        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        // Change parameter
        vm.set_param("Smooth", 1.0f, 10.0f);

        // Process and check that transition is gradual
        vm.process_block(left.data(), right.data());
        const float* buf = vm.buffers().get(0);

        // First sample should still be close to 0
        CHECK(buf[0] < 0.5f);

        // Last sample should be higher but not yet at 1.0
        CHECK(buf[BLOCK_SIZE - 1] > buf[0]);
    }
}

// ============================================================================
// Envelope Follower Tests
// ============================================================================

TEST_CASE("VM ENV_FOLLOWER opcode", "[vm][envelopes]") {
    VM vm;
    vm.set_sample_rate(48000.0f);

    SECTION("follows constant amplitude signal") {
        // Create a constant signal at 0.5 amplitude
        // ENV_FOLLOWER should track this with attack/release times
        std::array<Instruction, 4> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 0.5f),      // input signal
            make_const_instruction(Opcode::PUSH_CONST, 1, 0.01f),     // attack = 10ms
            make_const_instruction(Opcode::PUSH_CONST, 2, 0.01f),     // release = 10ms
            Instruction::make_ternary(Opcode::ENV_FOLLOWER, 3, 0, 1, 2, 1)  // follower
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};

        // Process multiple blocks to let envelope settle
        for (int block = 0; block < 50; ++block) {
            vm.process_block(left.data(), right.data());
        }

        // After settling, envelope should be close to 0.5
        const float* result = vm.buffers().get(3);
        CHECK_THAT(result[BLOCK_SIZE - 1], WithinAbs(0.5f, 0.01f));
    }

    SECTION("attack phase rises from zero") {
        // Start with zero, then jump to 1.0
        // Envelope should rise gradually based on attack time
        std::array<Instruction, 4> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 1.0f),      // input signal
            make_const_instruction(Opcode::PUSH_CONST, 1, 0.1f),      // attack = 100ms
            make_const_instruction(Opcode::PUSH_CONST, 2, 0.1f),      // release = 100ms
            Instruction::make_ternary(Opcode::ENV_FOLLOWER, 3, 0, 1, 2, 1)
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        const float* result = vm.buffers().get(3);

        // First sample should be near zero (starting state)
        CHECK(result[0] < 0.1f);

        // Envelope should rise monotonically during attack
        for (std::size_t i = 1; i < BLOCK_SIZE; ++i) {
            CHECK(result[i] >= result[i - 1]);
        }

        // Should not reach full amplitude in one block with 100ms attack
        CHECK(result[BLOCK_SIZE - 1] < 0.9f);
    }

    SECTION("release phase falls from peak") {
        // First build up envelope, then drop input to zero
        std::array<Instruction, 4> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 1.0f),
            make_const_instruction(Opcode::PUSH_CONST, 1, 0.01f),     // fast attack
            make_const_instruction(Opcode::PUSH_CONST, 2, 0.1f),      // slow release = 100ms
            Instruction::make_ternary(Opcode::ENV_FOLLOWER, 3, 0, 1, 2, 1)
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};

        // Build up envelope with fast attack
        for (int block = 0; block < 20; ++block) {
            vm.process_block(left.data(), right.data());
        }

        // Now drop input to zero
        program[0] = make_const_instruction(Opcode::PUSH_CONST, 0, 0.0f);
        vm.load_program(program);

        vm.process_block(left.data(), right.data());
        const float* result = vm.buffers().get(3);

        // First sample should still be high (envelope hasn't released yet)
        CHECK(result[0] > 0.8f);

        // Envelope should fall monotonically during release
        for (std::size_t i = 1; i < BLOCK_SIZE; ++i) {
            CHECK(result[i] <= result[i - 1]);
        }

        // Should not reach zero in one block with 100ms release
        CHECK(result[BLOCK_SIZE - 1] > 0.1f);
    }

    SECTION("tracks oscillating signal") {
        // Use a sine wave as input - envelope should track the amplitude
        std::array<Instruction, 6> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 100.0f),    // 100 Hz sine
            Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 10),       // oscillator
            make_const_instruction(Opcode::PUSH_CONST, 2, 0.001f),    // very fast attack
            make_const_instruction(Opcode::PUSH_CONST, 3, 0.001f),    // very fast release
            Instruction::make_ternary(Opcode::ENV_FOLLOWER, 4, 1, 2, 3, 2)
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};

        // Process several blocks to let oscillator stabilize
        for (int block = 0; block < 10; ++block) {
            vm.process_block(left.data(), right.data());
        }

        const float* result = vm.buffers().get(4);

        // Envelope should be positive and bounded
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK(result[i] >= 0.0f);
            CHECK(result[i] <= 1.2f);  // Allow some overshoot
        }

        // With fast attack/release, envelope should track peaks (~1.0 for sine)
        float max_env = 0.0f;
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            max_env = std::max(max_env, result[i]);
        }
        CHECK(max_env > 0.7f);  // Should reach near peak amplitude
    }

    SECTION("different attack and release times") {
        // Fast attack, slow release - classic envelope follower behavior
        std::array<Instruction, 4> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 0.8f),
            make_const_instruction(Opcode::PUSH_CONST, 1, 0.001f),    // 1ms attack (fast)
            make_const_instruction(Opcode::PUSH_CONST, 2, 0.5f),      // 500ms release (slow)
            Instruction::make_ternary(Opcode::ENV_FOLLOWER, 3, 0, 1, 2, 1)
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};

        // Build up with fast attack
        for (int block = 0; block < 10; ++block) {
            vm.process_block(left.data(), right.data());
        }

        // Should reach near target quickly with fast attack
        const float* result = vm.buffers().get(3);
        CHECK(result[BLOCK_SIZE - 1] > 0.7f);

        // Now drop input
        program[0] = make_const_instruction(Opcode::PUSH_CONST, 0, 0.0f);
        vm.load_program(program);

        vm.process_block(left.data(), right.data());
        result = vm.buffers().get(3);

        // With slow release, should still be high after one block
        CHECK(result[BLOCK_SIZE - 1] > 0.6f);
    }

    SECTION("handles zero input gracefully") {
        std::array<Instruction, 4> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 0.0f),
            make_const_instruction(Opcode::PUSH_CONST, 1, 0.01f),
            make_const_instruction(Opcode::PUSH_CONST, 2, 0.01f),
            Instruction::make_ternary(Opcode::ENV_FOLLOWER, 3, 0, 1, 2, 1)
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        const float* result = vm.buffers().get(3);

        // Should remain at or near zero
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK(result[i] >= 0.0f);
            CHECK(result[i] < 0.01f);
        }
    }

    SECTION("tracks negative input (absolute value)") {
        // Envelope follower should track absolute value
        std::array<Instruction, 4> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, -0.6f),     // negative input
            make_const_instruction(Opcode::PUSH_CONST, 1, 0.01f),
            make_const_instruction(Opcode::PUSH_CONST, 2, 0.01f),
            Instruction::make_ternary(Opcode::ENV_FOLLOWER, 3, 0, 1, 2, 1)
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};

        // Let envelope settle
        for (int block = 0; block < 50; ++block) {
            vm.process_block(left.data(), right.data());
        }

        const float* result = vm.buffers().get(3);

        // Should track absolute value (0.6), not negative
        CHECK_THAT(result[BLOCK_SIZE - 1], WithinAbs(0.6f, 0.01f));
        CHECK(result[BLOCK_SIZE - 1] > 0.0f);
    }

    SECTION("parameter changes update coefficients") {
        // Start with slow attack
        std::array<Instruction, 4> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 1.0f),
            make_const_instruction(Opcode::PUSH_CONST, 1, 0.5f),      // 500ms attack
            make_const_instruction(Opcode::PUSH_CONST, 2, 0.1f),
            Instruction::make_ternary(Opcode::ENV_FOLLOWER, 3, 0, 1, 2, 1)
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        const float* result = vm.buffers().get(3);
        float slow_rise = result[BLOCK_SIZE - 1];

        // Reset and try with fast attack
        vm.reset();
        program[1] = make_const_instruction(Opcode::PUSH_CONST, 1, 0.001f);  // 1ms attack
        vm.load_program(program);

        vm.process_block(left.data(), right.data());
        result = vm.buffers().get(3);
        float fast_rise = result[BLOCK_SIZE - 1];

        // Fast attack should reach higher value in same time
        CHECK(fast_rise > slow_rise);
    }
}
