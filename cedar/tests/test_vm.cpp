#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "cedar/vm/vm.hpp"
#include "cedar/vm/instruction.hpp"
#include "cedar/opcodes/utility.hpp"
#include "cedar/opcodes/sequencing.hpp"
#include <array>
#include <cmath>

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

        // Square wave should only be +1 or -1
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK((result[i] == 1.0f || result[i] == -1.0f));
        }
    }
}

TEST_CASE("VM filters", "[vm][filters]") {
    VM vm;
    vm.set_sample_rate(48000.0f);

    SECTION("FILTER_LP attenuates high frequencies") {
        // Generate noise, filter it with lowpass at 1000 Hz
        std::array<Instruction, 4> program = {
            Instruction::make_nullary(Opcode::NOISE, 0, 1),            // noise in buf0
            make_const_instruction(Opcode::PUSH_CONST, 1, 1000.0f),    // cutoff
            make_const_instruction(Opcode::PUSH_CONST, 2, 0.707f),     // Q
            Instruction::make_ternary(Opcode::FILTER_LP, 3, 0, 1, 2, 2) // filter with state_id=2
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
            Instruction::make_ternary(Opcode::FILTER_LP, 4, 1, 2, 3, 2) // filter -> buf4
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
        program[1].reserved = static_cast<std::uint16_t>(LFOShape::SIN);
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
        program[1].reserved = static_cast<std::uint16_t>(LFOShape::TRI);
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
        program[1].reserved = static_cast<std::uint16_t>(LFOShape::SQR);
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
        program[1].reserved = static_cast<std::uint16_t>(LFOShape::SAW);
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
        // Use ceiling division to ensure we fully cover one beat
        int trigger_count = 0;
        int blocks_per_beat = (24000 + BLOCK_SIZE - 1) / BLOCK_SIZE;  // 188 blocks

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
        int blocks_per_beat = (24000 + BLOCK_SIZE - 1) / BLOCK_SIZE;  // 188 blocks

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
