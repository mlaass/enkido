#include <catch2/catch_test_macros.hpp>
#include "akkado/akkado.hpp"
#include <cedar/vm/instruction.hpp>
#include <cstring>

TEST_CASE("Akkado compilation", "[akkado]") {
    SECTION("empty source produces error") {
        auto result = akkado::compile("");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.diagnostics.size() >= 1);
        CHECK(result.diagnostics[0].severity == akkado::Severity::Error);
        CHECK(result.diagnostics[0].code == "E001");
    }

    SECTION("comment-only source succeeds") {
        auto result = akkado::compile("// test");

        REQUIRE(result.success);
        CHECK(result.bytecode.empty());  // No instructions for comment-only
    }

    SECTION("simple number literal") {
        auto result = akkado::compile("42");

        REQUIRE(result.success);
        REQUIRE(result.bytecode.size() == sizeof(cedar::Instruction));

        cedar::Instruction inst;
        std::memcpy(&inst, result.bytecode.data(), sizeof(inst));
        CHECK(inst.opcode == cedar::Opcode::PUSH_CONST);

        float value;
        std::memcpy(&value, &inst.state_id, sizeof(float));
        CHECK(value == 42.0f);
    }

    SECTION("simple oscillator") {
        auto result = akkado::compile("saw(440)");

        REQUIRE(result.success);
        // Should have 2 instructions: PUSH_CONST for 440, OSC_SAW
        REQUIRE(result.bytecode.size() == 2 * sizeof(cedar::Instruction));

        cedar::Instruction inst[2];
        std::memcpy(inst, result.bytecode.data(), result.bytecode.size());

        CHECK(inst[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(inst[1].opcode == cedar::Opcode::OSC_SAW);
        CHECK(inst[1].inputs[0] == inst[0].out_buffer);  // OSC reads CONST output
    }

    SECTION("pitch literal as oscillator frequency") {
        auto result = akkado::compile("saw('a4')");  // A4 = 440 Hz

        REQUIRE(result.success);
        // Should have 3 instructions: PUSH_CONST (69), MTOF, OSC_SAW
        REQUIRE(result.bytecode.size() == 3 * sizeof(cedar::Instruction));

        cedar::Instruction inst[3];
        std::memcpy(inst, result.bytecode.data(), result.bytecode.size());

        // PUSH_CONST should push MIDI note 69 (A4)
        CHECK(inst[0].opcode == cedar::Opcode::PUSH_CONST);
        float midi_note;
        std::memcpy(&midi_note, &inst[0].state_id, sizeof(float));
        CHECK(midi_note == 69.0f);

        // MTOF converts MIDI to frequency
        CHECK(inst[1].opcode == cedar::Opcode::MTOF);
        CHECK(inst[1].inputs[0] == inst[0].out_buffer);

        // OSC_SAW uses the MTOF output
        CHECK(inst[2].opcode == cedar::Opcode::OSC_SAW);
        CHECK(inst[2].inputs[0] == inst[1].out_buffer);
    }

    SECTION("chord literal as oscillator frequency (uses root)") {
        auto result = akkado::compile("saw('c4:maj')");  // C4 major chord, root = 60

        REQUIRE(result.success);
        // Should have 3 instructions: PUSH_CONST (60), MTOF, OSC_SAW
        REQUIRE(result.bytecode.size() == 3 * sizeof(cedar::Instruction));

        cedar::Instruction inst[3];
        std::memcpy(inst, result.bytecode.data(), result.bytecode.size());

        // PUSH_CONST should push MIDI note 60 (C4 - root of chord)
        CHECK(inst[0].opcode == cedar::Opcode::PUSH_CONST);
        float midi_note;
        std::memcpy(&midi_note, &inst[0].state_id, sizeof(float));
        CHECK(midi_note == 60.0f);

        CHECK(inst[1].opcode == cedar::Opcode::MTOF);
        CHECK(inst[2].opcode == cedar::Opcode::OSC_SAW);
    }

    SECTION("pipe expression: saw(440) |> out(%, %)") {
        auto result = akkado::compile("saw(440) |> out(%, %)");

        REQUIRE(result.success);
        // Should have: PUSH_CONST, OSC_SAW, OUTPUT
        REQUIRE(result.bytecode.size() == 3 * sizeof(cedar::Instruction));

        cedar::Instruction inst[3];
        std::memcpy(inst, result.bytecode.data(), result.bytecode.size());

        CHECK(inst[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(inst[1].opcode == cedar::Opcode::OSC_SAW);
        CHECK(inst[2].opcode == cedar::Opcode::OUTPUT);

        // OUTPUT should take the SAW output for both L and R
        CHECK(inst[2].inputs[0] == inst[1].out_buffer);
        CHECK(inst[2].inputs[1] == inst[1].out_buffer);
    }

    SECTION("pipe chain: saw(440) |> lp(%, 1000, 0.7) |> out(%, %)") {
        auto result = akkado::compile("saw(440) |> lp(%, 1000, 0.7) |> out(%, %)");

        REQUIRE(result.success);
        // PUSH_CONST(440), OSC_SAW, PUSH_CONST(1000), PUSH_CONST(0.7), FILTER_SVF_LP, OUTPUT
        REQUIRE(result.bytecode.size() == 6 * sizeof(cedar::Instruction));

        cedar::Instruction inst[6];
        std::memcpy(inst, result.bytecode.data(), result.bytecode.size());

        // Check the chain
        CHECK(inst[0].opcode == cedar::Opcode::PUSH_CONST);  // 440
        CHECK(inst[1].opcode == cedar::Opcode::OSC_SAW);
        CHECK(inst[2].opcode == cedar::Opcode::PUSH_CONST);  // 1000
        CHECK(inst[3].opcode == cedar::Opcode::PUSH_CONST);  // 0.7
        CHECK(inst[4].opcode == cedar::Opcode::FILTER_SVF_LP);  // SVF is default
        CHECK(inst[5].opcode == cedar::Opcode::OUTPUT);

        // Filter input is saw output
        CHECK(inst[4].inputs[0] == inst[1].out_buffer);
        // Output input is filter output
        CHECK(inst[5].inputs[0] == inst[4].out_buffer);
    }

    SECTION("variable assignment") {
        auto result = akkado::compile("x = 440\nsaw(x)");

        REQUIRE(result.success);
        // PUSH_CONST, OSC_SAW
        REQUIRE(result.bytecode.size() >= 2 * sizeof(cedar::Instruction));
    }

    SECTION("arithmetic operators") {
        auto result = akkado::compile("440 + 220");

        REQUIRE(result.success);
        // PUSH_CONST(440), PUSH_CONST(220), ADD
        REQUIRE(result.bytecode.size() == 3 * sizeof(cedar::Instruction));

        cedar::Instruction inst[3];
        std::memcpy(inst, result.bytecode.data(), result.bytecode.size());

        CHECK(inst[2].opcode == cedar::Opcode::ADD);
    }

    SECTION("unknown function produces error") {
        auto result = akkado::compile("unknown_function(42)");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.diagnostics.size() >= 1);
        CHECK(result.diagnostics[0].severity == akkado::Severity::Error);
    }

    SECTION("hole outside pipe produces error") {
        auto result = akkado::compile("%");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.diagnostics.size() >= 1);
    }

    SECTION("simple closure compiles (single param)") {
        auto result = akkado::compile("(x) -> saw(x)");

        // Should compile - no captures
        REQUIRE(result.success);
    }

    SECTION("closure with captured variable produces error") {
        auto result = akkado::compile("y = 440\n(x) -> saw(y)");

        // Should fail - captures 'y'
        REQUIRE_FALSE(result.success);
        REQUIRE(result.diagnostics.size() >= 1);
        CHECK(result.diagnostics[0].code == "E008");
    }

    SECTION("closure with multiple params") {
        auto result = akkado::compile("(x, y) -> add(x, y)");

        // Should compile - no captures
        REQUIRE(result.success);
    }

    SECTION("env_follower builtin with defaults") {
        auto result = akkado::compile("saw(100) |> env_follower(%)");

        REQUIRE(result.success);
        // PUSH_CONST(100), OSC_SAW, PUSH_CONST(0.01), PUSH_CONST(0.1), ENV_FOLLOWER
        REQUIRE(result.bytecode.size() == 5 * sizeof(cedar::Instruction));

        cedar::Instruction inst[5];
        std::memcpy(inst, result.bytecode.data(), result.bytecode.size());

        CHECK(inst[0].opcode == cedar::Opcode::PUSH_CONST);  // 100
        CHECK(inst[1].opcode == cedar::Opcode::OSC_SAW);
        CHECK(inst[2].opcode == cedar::Opcode::PUSH_CONST);  // 0.01 (default attack)
        CHECK(inst[3].opcode == cedar::Opcode::PUSH_CONST);  // 0.1 (default release)
        CHECK(inst[4].opcode == cedar::Opcode::ENV_FOLLOWER);
        CHECK(inst[4].inputs[0] == inst[1].out_buffer);  // Follower reads saw output
        CHECK(inst[4].inputs[1] == inst[2].out_buffer);  // Default attack
        CHECK(inst[4].inputs[2] == inst[3].out_buffer);  // Default release
    }

    SECTION("env_follower with explicit attack/release") {
        auto result = akkado::compile("saw(100) |> env_follower(%, 0.001, 0.5)");

        REQUIRE(result.success);
        // PUSH_CONST(100), OSC_SAW, PUSH_CONST(0.001), PUSH_CONST(0.5), ENV_FOLLOWER
        REQUIRE(result.bytecode.size() == 5 * sizeof(cedar::Instruction));

        cedar::Instruction inst[5];
        std::memcpy(inst, result.bytecode.data(), result.bytecode.size());

        CHECK(inst[0].opcode == cedar::Opcode::PUSH_CONST);  // 100
        CHECK(inst[1].opcode == cedar::Opcode::OSC_SAW);
        CHECK(inst[2].opcode == cedar::Opcode::PUSH_CONST);  // 0.001 (attack)
        CHECK(inst[3].opcode == cedar::Opcode::PUSH_CONST);  // 0.5 (release)
        CHECK(inst[4].opcode == cedar::Opcode::ENV_FOLLOWER);
        CHECK(inst[4].inputs[0] == inst[1].out_buffer);  // Input signal
        CHECK(inst[4].inputs[1] == inst[2].out_buffer);  // Attack time
        CHECK(inst[4].inputs[2] == inst[3].out_buffer);  // Release time
    }

    SECTION("env_follower alias 'follower' works") {
        auto result = akkado::compile("saw(100) |> follower(%)");

        REQUIRE(result.success);
        // PUSH_CONST(100), OSC_SAW, PUSH_CONST(0.01), PUSH_CONST(0.1), ENV_FOLLOWER
        REQUIRE(result.bytecode.size() == 5 * sizeof(cedar::Instruction));
        
        cedar::Instruction inst[5];
        std::memcpy(inst, result.bytecode.data(), result.bytecode.size());
        CHECK(inst[4].opcode == cedar::Opcode::ENV_FOLLOWER);
    }
}

TEST_CASE("Akkado version", "[akkado]") {
    CHECK(akkado::Version::major == 0);
    CHECK(akkado::Version::minor == 1);
    CHECK(akkado::Version::patch == 0);
    CHECK(akkado::Version::string() == "0.1.0");
}
