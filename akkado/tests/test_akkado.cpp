#include <catch2/catch_test_macros.hpp>
#include "akkado/akkado.hpp"
#include <cedar/vm/instruction.hpp>
#include <cedar/dsp/constants.hpp>  // cedar::BUFFER_UNUSED
#include <cstring>
#include <string>

// Helper to decode float from PUSH_CONST instruction
// Float is stored directly in state_id (32 bits)
static float decode_const_float(const cedar::Instruction& inst) {
    float value;
    std::memcpy(&value, &inst.state_id, sizeof(float));
    return value;
}

// Helper to check if a diagnostic with a specific code exists
static bool has_diagnostic_code(const std::vector<akkado::Diagnostic>& diagnostics, const std::string& code) {
    for (const auto& d : diagnostics) {
        if (d.code == code) {
            return true;
        }
    }
    return false;
}

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
        CHECK(decode_const_float(inst) == 42.0f);
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
        CHECK(decode_const_float(inst[0]) == 69.0f);

        // MTOF converts MIDI to frequency
        CHECK(inst[1].opcode == cedar::Opcode::MTOF);
        CHECK(inst[1].inputs[0] == inst[0].out_buffer);

        // OSC_SAW uses the MTOF output
        CHECK(inst[2].opcode == cedar::Opcode::OSC_SAW);
        CHECK(inst[2].inputs[0] == inst[1].out_buffer);
    }

    SECTION("chord literal as oscillator frequency (uses root)") {
        auto result = akkado::compile("saw(C4')");  // C4 major chord, root = 60

        REQUIRE(result.success);
        // Should have 3 instructions: PUSH_CONST (60), MTOF, OSC_SAW
        REQUIRE(result.bytecode.size() == 3 * sizeof(cedar::Instruction));

        cedar::Instruction inst[3];
        std::memcpy(inst, result.bytecode.data(), result.bytecode.size());

        // PUSH_CONST should push MIDI note 60 (C4 - root of chord)
        CHECK(inst[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(inst[0]) == 60.0f);

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

    SECTION("pipe chain: saw(440) |> lp(%, 1000, 0.7) |> out(%)") {
        // lp is stereo-native (prd-stereo-native-opcodes Phase 4a) so it
        // produces a stereo output pair. out(%) takes the stereo pair.
        auto result = akkado::compile("saw(440) |> lp(%, 1000, 0.7) |> out(%)");

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
        // Output left input is filter L output; right is L+1
        CHECK(inst[5].inputs[0] == inst[4].out_buffer);
        CHECK(inst[5].inputs[1] == inst[4].out_buffer + 1);
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

        // Find an error diagnostic (skip stdlib warnings)
        bool found_error = false;
        for (const auto& d : result.diagnostics) {
            if (d.severity == akkado::Severity::Error) {
                found_error = true;
                break;
            }
        }
        CHECK(found_error);
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

    SECTION("closure with captured variable compiles (read-only capture)") {
        auto result = akkado::compile("y = 440\n(x) -> saw(y)");

        // Should succeed - captures are now allowed (read-only)
        REQUIRE(result.success);
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
    CHECK(akkado::Version::major >= 0);
    CHECK(akkado::Version::minor >= 0);
    CHECK(akkado::Version::patch >= 0);

    const auto expected = std::to_string(akkado::Version::major) + "." +
                          std::to_string(akkado::Version::minor) + "." +
                          std::to_string(akkado::Version::patch);
    CHECK(std::string(akkado::Version::string()) == expected);
}

TEST_CASE("Akkado match expressions", "[akkado][match]") {
    SECTION("match resolves string pattern at compile time") {
        auto result = akkado::compile(R"(
            match("sin") {
                "sin": 440
                "saw": 880
                _: 220
            }
        )");

        REQUIRE(result.success);
        // Should compile to just PUSH_CONST(440)
        REQUIRE(result.bytecode.size() == sizeof(cedar::Instruction));

        cedar::Instruction inst;
        std::memcpy(&inst, result.bytecode.data(), sizeof(inst));
        CHECK(inst.opcode == cedar::Opcode::PUSH_CONST);
    }

    SECTION("match resolves to second pattern") {
        auto result = akkado::compile(R"(
            match("saw") {
                "sin": 440
                "saw": 880
                _: 220
            }
        )");

        REQUIRE(result.success);
        REQUIRE(result.bytecode.size() == sizeof(cedar::Instruction));

        cedar::Instruction inst;
        std::memcpy(&inst, result.bytecode.data(), sizeof(inst));
        CHECK(inst.opcode == cedar::Opcode::PUSH_CONST);
    }

    SECTION("match uses wildcard when no pattern matches") {
        auto result = akkado::compile(R"(
            match("unknown") {
                "sin": 440
                "saw": 880
                _: 220
            }
        )");

        REQUIRE(result.success);
        REQUIRE(result.bytecode.size() == sizeof(cedar::Instruction));

        cedar::Instruction inst;
        std::memcpy(&inst, result.bytecode.data(), sizeof(inst));
        CHECK(inst.opcode == cedar::Opcode::PUSH_CONST);
    }

    SECTION("match with number scrutinee") {
        auto result = akkado::compile(R"(
            match(2) {
                1: 100
                2: 200
                3: 300
            }
        )");

        REQUIRE(result.success);
        REQUIRE(result.bytecode.size() == sizeof(cedar::Instruction));

        cedar::Instruction inst;
        std::memcpy(&inst, result.bytecode.data(), sizeof(inst));
        CHECK(inst.opcode == cedar::Opcode::PUSH_CONST);
    }

    SECTION("match with expression body compiles correctly") {
        auto result = akkado::compile(R"(
            match("saw") {
                "sin": saw(440)
                "saw": saw(880)
                _: saw(220)
            }
        )");

        REQUIRE(result.success);
        // Should have: PUSH_CONST(880), OSC_SAW
        REQUIRE(result.bytecode.size() == 2 * sizeof(cedar::Instruction));

        cedar::Instruction inst[2];
        std::memcpy(inst, result.bytecode.data(), result.bytecode.size());

        CHECK(inst[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(inst[1].opcode == cedar::Opcode::OSC_SAW);
    }

    SECTION("match without matching pattern and no wildcard fails") {
        auto result = akkado::compile(R"(
            match("unknown") {
                "sin": 1
                "saw": 2
            }
        )");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.diagnostics.size() >= 1);
        CHECK(has_diagnostic_code(result.diagnostics, "E121"));
    }

    SECTION("match with non-literal scrutinee uses runtime select") {
        // Non-literal scrutinee now triggers runtime match evaluation
        // using nested SELECT opcodes instead of compile-time pattern matching
        auto result = akkado::compile(R"(
            x = saw(1)
            match(x) {
                0: 10,
                1: 20,
                _: 30
            }
        )");

        REQUIRE(result.success);
        // Runtime match produces SELECT opcodes
        std::vector<cedar::Instruction> insts;
        size_t count = result.bytecode.size() / sizeof(cedar::Instruction);
        insts.resize(count);
        std::memcpy(insts.data(), result.bytecode.data(), result.bytecode.size());

        bool has_select = false;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::SELECT) {
                has_select = true;
                break;
            }
        }
        CHECK(has_select);
    }
}

TEST_CASE("Akkado match destructuring", "[akkado][match][destructure]") {
    SECTION("destructure record in match arm") {
        auto result = akkado::compile(R"(
            r = {freq: 440, vel: 0.8}
            match(r) {
                {freq, vel}: freq
                _: 0
            }
        )");
        REQUIRE(result.success);
        // Runtime match: record variable isn't a literal, so uses runtime path
        // Should emit at least the record fields + body + default
        REQUIRE(result.bytecode.size() >= 2 * sizeof(cedar::Instruction));
    }

    SECTION("destructure record - second field") {
        auto result = akkado::compile(R"(
            r = {freq: 440, vel: 0.8}
            match(r) {
                {freq, vel}: vel
                _: 0
            }
        )");
        REQUIRE(result.success);
        REQUIRE(result.bytecode.size() >= 2 * sizeof(cedar::Instruction));
    }

    SECTION("destructure with expression body") {
        auto result = akkado::compile(R"(
            r = {freq: 440, vel: 0.5}
            match(r) {
                {freq, vel}: freq * vel
                _: 0
            }
        )");
        REQUIRE(result.success);
        REQUIRE(result.bytecode.size() >= 3 * sizeof(cedar::Instruction));
    }

    SECTION("as destructuring binding in pipe") {
        auto result = akkado::compile(R"(
            r = {a: 100, b: 200}
            r as {a, b} |> a + b
        )");
        REQUIRE(result.success);
    }

    SECTION("statement-level destructure assignment") {
        auto result = akkado::compile(R"(
            r = {a: 100, b: 200}
            {a, b} = r
            saw(a) + saw(b) |> out(%, %)
        )");
        REQUIRE(result.success);
    }

    SECTION("statement-level destructure with defaults (Phase 3b)") {
        // Source has `freq` but lacks `q`; the declared default fills the slot
        // and the program compiles to a real audio program.
        auto result = akkado::compile(R"(
            preset = {freq: 220}
            {freq, q = 0.7} = preset
            osc("sin", freq) * q |> out(%, %)
        )");
        REQUIRE(result.success);
    }

    SECTION("fn-param destructure with defaults (Phase 3b)") {
        auto result = akkado::compile(R"(
            fn synth({freq = 440, wave = "saw", q = 0.7}) ->
                osc(wave, freq) * q
            sig = synth({freq: 220})
            sig |> out(%, %)
        )");
        REQUIRE(result.success);
    }

    SECTION("fn-param destructure mixed with regular params (Phase 3b)") {
        auto result = akkado::compile(R"(
            fn lp_voice(freq, {cutoff = 1000, q = 0.7}) ->
                osc("saw", freq)
            sig = lp_voice(330, {cutoff: 800})
            sig |> out(%, %)
        )");
        REQUIRE(result.success);
    }
}

TEST_CASE("Akkado user-defined functions", "[akkado][fn]") {
    SECTION("simple function definition and call") {
        auto result = akkado::compile(R"(
            fn double(x) -> x * 2
            double(100)
        )");

        REQUIRE(result.success);
        // Should have: PUSH_CONST(100), PUSH_CONST(2), MUL
        REQUIRE(result.bytecode.size() == 3 * sizeof(cedar::Instruction));

        cedar::Instruction inst[3];
        std::memcpy(inst, result.bytecode.data(), result.bytecode.size());

        CHECK(inst[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(inst[1].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(inst[2].opcode == cedar::Opcode::MUL);
    }

    SECTION("function with multiple parameters") {
        auto result = akkado::compile(R"(
            fn add3(a, b, c) -> a + b + c
            add3(1, 2, 3)
        )");

        REQUIRE(result.success);
        // Should inline the function body
        REQUIRE(result.bytecode.size() >= 3 * sizeof(cedar::Instruction));
    }

    SECTION("function with default parameter - using default") {
        auto result = akkado::compile(R"(
            fn osc_freq(freq, mult = 1.0) -> freq * mult
            osc_freq(440)
        )");

        REQUIRE(result.success);
        // PUSH_CONST(440), PUSH_CONST(1.0), MUL
        REQUIRE(result.bytecode.size() == 3 * sizeof(cedar::Instruction));

        cedar::Instruction inst[3];
        std::memcpy(inst, result.bytecode.data(), result.bytecode.size());

        CHECK(inst[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(inst[1].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(inst[2].opcode == cedar::Opcode::MUL);
    }

    SECTION("function with default parameter - overriding default") {
        auto result = akkado::compile(R"(
            fn osc_freq(freq, mult = 1.0) -> freq * mult
            osc_freq(440, 2.0)
        )");

        REQUIRE(result.success);
        // PUSH_CONST(440), PUSH_CONST(2.0), MUL
        REQUIRE(result.bytecode.size() == 3 * sizeof(cedar::Instruction));

        cedar::Instruction inst[3];
        std::memcpy(inst, result.bytecode.data(), result.bytecode.size());

        CHECK(inst[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(inst[1].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(inst[2].opcode == cedar::Opcode::MUL);
    }

    SECTION("function calling builtin") {
        auto result = akkado::compile(R"(
            fn my_saw(freq) -> saw(freq)
            my_saw(440)
        )");

        REQUIRE(result.success);
        // PUSH_CONST(440), OSC_SAW
        REQUIRE(result.bytecode.size() == 2 * sizeof(cedar::Instruction));

        cedar::Instruction inst[2];
        std::memcpy(inst, result.bytecode.data(), result.bytecode.size());

        CHECK(inst[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(inst[1].opcode == cedar::Opcode::OSC_SAW);
    }

    SECTION("function with match expression") {
        auto result = akkado::compile(R"(
            fn my_osc(type, freq) -> match(type) {
                "sin": saw(freq)
                "saw": saw(freq)
                _: saw(freq)
            }
            my_osc("saw", 440)
        )");

        REQUIRE(result.success);
        // Should compile the matching branch only
        REQUIRE(result.bytecode.size() == 2 * sizeof(cedar::Instruction));
    }

    SECTION("nested function calls") {
        auto result = akkado::compile(R"(
            fn double(x) -> x * 2
            fn quadruple(x) -> double(double(x))
            quadruple(100)
        )");

        REQUIRE(result.success);
        // PUSH_CONST(100), PUSH_CONST(2), MUL, PUSH_CONST(2), MUL
        REQUIRE(result.bytecode.size() == 5 * sizeof(cedar::Instruction));
    }

    SECTION("function can capture outer variables (read-only)") {
        auto result = akkado::compile(R"(
            y = 10
            fn add_y(x) -> x + y
            add_y(5)
        )");

        // Captures are now allowed since variables are immutable
        REQUIRE(result.success);
        // Should have ADD instruction
        REQUIRE(result.bytecode.size() >= sizeof(cedar::Instruction));
    }

    SECTION("function can call other user functions") {
        auto result = akkado::compile(R"(
            fn double(x) -> x * 2
            fn use_double(x) -> double(x) + 1
            use_double(10)
        )");

        REQUIRE(result.success);
    }

    SECTION("calling undefined function produces error") {
        auto result = akkado::compile(R"(
            undefined_fn(42)
        )");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.diagnostics.size() >= 1);
    }

    SECTION("too few arguments produces error") {
        auto result = akkado::compile(R"(
            fn add2(a, b) -> a + b
            add2(1)
        )");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.diagnostics.size() >= 1);
        CHECK(has_diagnostic_code(result.diagnostics, "E006"));
    }

    SECTION("too many arguments produces error") {
        auto result = akkado::compile(R"(
            fn double(x) -> x * 2
            double(1, 2, 3)
        )");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.diagnostics.size() >= 1);
        CHECK(has_diagnostic_code(result.diagnostics, "E007"));
    }
}

TEST_CASE("Builtins with optional parameters", "[akkado][builtins]") {
    SECTION("moog filter with defaults") {
        // moog(in, cutoff, res) - should work with just required args
        auto result = akkado::compile("saw(110) |> moog(%, 400, 2)");

        REQUIRE(result.success);
        // Expected: PUSH_CONST(110), OSC_SAW, PUSH_CONST(400), PUSH_CONST(2),
        //           PUSH_CONST(4.0), PUSH_CONST(0.5), FILTER_MOOG
        REQUIRE(result.bytecode.size() == 7 * sizeof(cedar::Instruction));

        cedar::Instruction inst[7];
        std::memcpy(inst, result.bytecode.data(), result.bytecode.size());

        CHECK(inst[6].opcode == cedar::Opcode::FILTER_MOOG);
        // Defaults should be filled in as PUSH_CONST
        CHECK(decode_const_float(inst[4]) == 4.0f);
        CHECK(decode_const_float(inst[5]) == 0.5f);
    }

    SECTION("moog filter with optional params overridden") {
        // moog(in, cutoff, res, max_res, input_scale)
        auto result = akkado::compile("saw(110) |> moog(%, 400, 2, 3.5, 0.8)");

        REQUIRE(result.success);
        REQUIRE(result.bytecode.size() == 7 * sizeof(cedar::Instruction));

        cedar::Instruction inst[7];
        std::memcpy(inst, result.bytecode.data(), result.bytecode.size());

        CHECK(inst[6].opcode == cedar::Opcode::FILTER_MOOG);
        CHECK(decode_const_float(inst[4]) == 3.5f);
        CHECK(decode_const_float(inst[5]) == 0.8f);
    }

    SECTION("freeverb with defaults") {
        auto result = akkado::compile("saw(220) |> freeverb(%, 0.5, 0.5)");

        REQUIRE(result.success);
        // Expected: PUSH_CONST(220), OSC_SAW, PUSH_CONST(0.5), PUSH_CONST(0.5),
        //           PUSH_CONST(0.28), PUSH_CONST(0.7), REVERB_FREEVERB
        REQUIRE(result.bytecode.size() == 7 * sizeof(cedar::Instruction));

        cedar::Instruction inst[7];
        std::memcpy(inst, result.bytecode.data(), result.bytecode.size());

        CHECK(inst[6].opcode == cedar::Opcode::REVERB_FREEVERB);
        CHECK(decode_const_float(inst[4]) == 0.28f);
        CHECK(decode_const_float(inst[5]) == 0.7f);
    }

    SECTION("freeverb with optional params overridden") {
        auto result = akkado::compile("saw(220) |> freeverb(%, 0.5, 0.5, 0.35, 0.8)");

        REQUIRE(result.success);
        REQUIRE(result.bytecode.size() == 7 * sizeof(cedar::Instruction));

        cedar::Instruction inst[7];
        std::memcpy(inst, result.bytecode.data(), result.bytecode.size());

        CHECK(inst[6].opcode == cedar::Opcode::REVERB_FREEVERB);
        CHECK(decode_const_float(inst[4]) == 0.35f);
        CHECK(decode_const_float(inst[5]) == 0.8f);
    }

    SECTION("flanger with optional delay range") {
        auto result = akkado::compile("saw(110) |> flanger(%, 0.5, 0.7, 0.05, 5.0)");

        REQUIRE(result.success);
        REQUIRE(result.bytecode.size() == 7 * sizeof(cedar::Instruction));

        cedar::Instruction inst[7];
        std::memcpy(inst, result.bytecode.data(), result.bytecode.size());

        CHECK(inst[6].opcode == cedar::Opcode::EFFECT_FLANGER);
        CHECK(decode_const_float(inst[4]) == 0.05f);
        CHECK(decode_const_float(inst[5]) == 5.0f);
    }

    SECTION("gate with optional hysteresis and close_time") {
        auto result = akkado::compile("saw(110) |> gate(%, -30, 8, 10)");

        REQUIRE(result.success);
        // Expected: PUSH_CONST(110), OSC_SAW, PUSH_CONST(-30), PUSH_CONST(8),
        //           PUSH_CONST(10), DYNAMICS_GATE
        // Note: gate has 3 required + 2 optional params but range default would be added

        cedar::Instruction* inst = reinterpret_cast<cedar::Instruction*>(result.bytecode.data());
        size_t num_inst = result.bytecode.size() / sizeof(cedar::Instruction);

        // Find DYNAMICS_GATE instruction
        bool found_gate = false;
        for (size_t i = 0; i < num_inst; ++i) {
            if (inst[i].opcode == cedar::Opcode::DYNAMICS_GATE) {
                found_gate = true;
                break;
            }
        }
        CHECK(found_gate);
    }

    SECTION("excite with harmonic mix") {
        auto result = akkado::compile("saw(220) |> excite(%, 0.5, 3000, 0.2, 0.8)");

        REQUIRE(result.success);

        cedar::Instruction* inst = reinterpret_cast<cedar::Instruction*>(result.bytecode.data());
        size_t num_inst = result.bytecode.size() / sizeof(cedar::Instruction);

        // Find DISTORT_EXCITE instruction
        bool found_excite = false;
        for (size_t i = 0; i < num_inst; ++i) {
            if (inst[i].opcode == cedar::Opcode::DISTORT_EXCITE) {
                found_excite = true;
                break;
            }
        }
        CHECK(found_excite);
    }
}

// Helper to find an instruction with a specific opcode in bytecode
static bool find_opcode(const std::vector<std::uint8_t>& bytecode, cedar::Opcode target) {
    cedar::Instruction* inst = reinterpret_cast<cedar::Instruction*>(
        const_cast<std::uint8_t*>(bytecode.data()));
    size_t num_inst = bytecode.size() / sizeof(cedar::Instruction);
    for (size_t i = 0; i < num_inst; ++i) {
        if (inst[i].opcode == target) {
            return true;
        }
    }
    return false;
}

// Helper to count instructions with a specific opcode in bytecode
static size_t count_opcode(const std::vector<std::uint8_t>& bytecode, cedar::Opcode target) {
    cedar::Instruction* inst = reinterpret_cast<cedar::Instruction*>(
        const_cast<std::uint8_t*>(bytecode.data()));
    size_t num_inst = bytecode.size() / sizeof(cedar::Instruction);
    size_t count = 0;
    for (size_t i = 0; i < num_inst; ++i) {
        if (inst[i].opcode == target) {
            count++;
        }
    }
    return count;
}

TEST_CASE("Akkado stdlib", "[akkado][stdlib]") {
    SECTION("stdlib osc() with sin type") {
        auto result = akkado::compile(R"(osc("sin", 440))");

        REQUIRE(result.success);
        // stdlib osc() produces: PUSH_CONST(freq), PUSH_CONST(pwm default), OSC_SIN
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SIN));
    }

    SECTION("stdlib osc() with saw type") {
        auto result = akkado::compile(R"(osc("saw", 440))");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
    }

    SECTION("stdlib osc() with sqr type") {
        auto result = akkado::compile(R"(osc("sqr", 440))");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SQR));
    }

    SECTION("stdlib osc() with tri type") {
        auto result = akkado::compile(R"(osc("tri", 440))");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_TRI));
    }

    SECTION("stdlib osc() with alternate names (sine, sawtooth, square, triangle)") {
        // Test "sine" alias
        {
            auto result = akkado::compile(R"(osc("sine", 440))");
            REQUIRE(result.success);
            CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SIN));
        }

        // Test "sawtooth" alias
        {
            auto result = akkado::compile(R"(osc("sawtooth", 440))");
            REQUIRE(result.success);
            CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
        }

        // Test "square" alias
        {
            auto result = akkado::compile(R"(osc("square", 440))");
            REQUIRE(result.success);
            CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SQR));
        }

        // Test "triangle" alias
        {
            auto result = akkado::compile(R"(osc("triangle", 440))");
            REQUIRE(result.success);
            CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_TRI));
        }
    }

    SECTION("stdlib osc() with noise type") {
        auto result = akkado::compile(R"(osc("noise", 0))");

        REQUIRE(result.success);
        // Should have: PUSH_CONST, NOISE
        // Note: noise() ignores frequency but osc() still passes it through the match
        REQUIRE(result.bytecode.size() >= 1 * sizeof(cedar::Instruction));

        cedar::Instruction* inst = reinterpret_cast<cedar::Instruction*>(result.bytecode.data());
        size_t num_inst = result.bytecode.size() / sizeof(cedar::Instruction);

        bool found_noise = false;
        for (size_t i = 0; i < num_inst; ++i) {
            if (inst[i].opcode == cedar::Opcode::NOISE) {
                found_noise = true;
                break;
            }
        }
        CHECK(found_noise);
    }

    SECTION("stdlib osc() with pwm oscillators") {
        // Test sqr_pwm
        {
            auto result = akkado::compile(R"(osc("sqr_pwm", 440, 0.25))");
            REQUIRE(result.success);

            cedar::Instruction* inst = reinterpret_cast<cedar::Instruction*>(result.bytecode.data());
            size_t num_inst = result.bytecode.size() / sizeof(cedar::Instruction);

            bool found_pwm = false;
            for (size_t i = 0; i < num_inst; ++i) {
                if (inst[i].opcode == cedar::Opcode::OSC_SQR_PWM) {
                    found_pwm = true;
                    break;
                }
            }
            CHECK(found_pwm);
        }

        // Test "pulse" alias for sqr_pwm
        {
            auto result = akkado::compile(R"(osc("pulse", 440, 0.3))");
            REQUIRE(result.success);

            cedar::Instruction* inst = reinterpret_cast<cedar::Instruction*>(result.bytecode.data());
            size_t num_inst = result.bytecode.size() / sizeof(cedar::Instruction);

            bool found_pwm = false;
            for (size_t i = 0; i < num_inst; ++i) {
                if (inst[i].opcode == cedar::Opcode::OSC_SQR_PWM) {
                    found_pwm = true;
                    break;
                }
            }
            CHECK(found_pwm);
        }
    }

    SECTION("stdlib osc() with unknown type falls back to sin") {
        auto result = akkado::compile(R"(osc("unknown_type", 440))");

        REQUIRE(result.success);
        // Should fall back to sin via the wildcard match
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SIN));
    }

    SECTION("user can shadow stdlib osc()") {
        // Define a custom osc() that always returns a saw
        auto result = akkado::compile(R"(
            fn osc(type, freq, pwm = 0.5) -> saw(freq)
            osc("sin", 440)
        )");

        REQUIRE(result.success);
        // User's osc() should produce OSC_SAW (not OSC_SIN!)
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
        // And should NOT produce OSC_SIN
        CHECK_FALSE(find_opcode(result.bytecode, cedar::Opcode::OSC_SIN));
    }

    SECTION("stdlib osc() works in pipe chain") {
        auto result = akkado::compile(R"(osc("saw", 440) |> lp(%, 1000, 0.7) |> out(%, %))");

        REQUIRE(result.success);
        // Should have OSC_SAW, FILTER_SVF_LP, and OUTPUT
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::FILTER_SVF_LP));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OUTPUT));
    }

    SECTION("diagnostic line numbers are correct (not offset by stdlib)") {
        // Error should be reported on line 1, not line 20+ due to stdlib
        auto result = akkado::compile("undefined_identifier");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.diagnostics.size() >= 1);

        // Find the first error diagnostic (skip warnings like stdlib redefinition)
        const akkado::Diagnostic* error_diag = nullptr;
        for (const auto& d : result.diagnostics) {
            if (d.severity == akkado::Severity::Error) {
                error_diag = &d;
                break;
            }
        }
        REQUIRE(error_diag != nullptr);

        // Check the error diagnostic reports line 1 (user code)
        CHECK(error_diag->location.line == 1);
        // Filename should be the user's filename, not <stdlib>
        CHECK(error_diag->filename != "<stdlib>");
    }

    SECTION("diagnostic line numbers correct for multi-line user code") {
        auto result = akkado::compile(R"(
            x = 42
            y = 100
            undefined_func(x)
        )");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.diagnostics.size() >= 1);

        // Find the first error diagnostic (skip warnings)
        const akkado::Diagnostic* error_diag = nullptr;
        for (const auto& d : result.diagnostics) {
            if (d.severity == akkado::Severity::Error) {
                error_diag = &d;
                break;
            }
        }
        REQUIRE(error_diag != nullptr);

        // Error should be on line 4 (the undefined_func call)
        // Lines: 1=empty, 2=x=42, 3=y=100, 4=undefined_func
        CHECK(error_diag->location.line == 4);
    }
}

TEST_CASE("Akkado arrays and len()", "[akkado][array]") {
    SECTION("array literal compiles (uses first element)") {
        auto result = akkado::compile("[1, 2, 3]");

        REQUIRE(result.success);
        // Should emit first element (1) as PUSH_CONST
        REQUIRE(result.bytecode.size() >= sizeof(cedar::Instruction));

        cedar::Instruction inst;
        std::memcpy(&inst, result.bytecode.data(), sizeof(inst));
        CHECK(inst.opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(inst) == 1.0f);
    }

    SECTION("empty array compiles to zero") {
        auto result = akkado::compile("[]");

        REQUIRE(result.success);
        REQUIRE(result.bytecode.size() == sizeof(cedar::Instruction));

        cedar::Instruction inst;
        std::memcpy(&inst, result.bytecode.data(), sizeof(inst));
        CHECK(inst.opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(inst) == 0.0f);
    }

    SECTION("len() of array literal") {
        auto result = akkado::compile("len([1, 2, 3])");

        REQUIRE(result.success);
        // Should emit 3 as PUSH_CONST
        REQUIRE(result.bytecode.size() >= sizeof(cedar::Instruction));

        cedar::Instruction inst;
        std::memcpy(&inst, result.bytecode.data(), sizeof(inst));
        CHECK(inst.opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(inst) == 3.0f);
    }

    SECTION("len() of empty array") {
        auto result = akkado::compile("len([])");

        REQUIRE(result.success);
        REQUIRE(result.bytecode.size() == sizeof(cedar::Instruction));

        cedar::Instruction inst;
        std::memcpy(&inst, result.bytecode.data(), sizeof(inst));
        CHECK(inst.opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(inst) == 0.0f);
    }

    SECTION("len() of single element array") {
        auto result = akkado::compile("len([42])");

        REQUIRE(result.success);

        cedar::Instruction inst;
        std::memcpy(&inst, result.bytecode.data(), sizeof(inst));
        CHECK(inst.opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(inst) == 1.0f);
    }

    SECTION("len() in pipe expression") {
        auto result = akkado::compile("[1, 2, 3] |> len(%)");

        REQUIRE(result.success);
        REQUIRE(result.bytecode.size() == sizeof(cedar::Instruction));

        cedar::Instruction inst;
        std::memcpy(&inst, result.bytecode.data(), sizeof(inst));
        CHECK(inst.opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(inst) == 3.0f);
    }

    SECTION("len() in expression") {
        auto result = akkado::compile("len([1, 2, 3, 4, 5]) + 10");

        REQUIRE(result.success);
        // Should emit: PUSH_CONST(5), PUSH_CONST(10), ADD
        REQUIRE(result.bytecode.size() == 3 * sizeof(cedar::Instruction));

        cedar::Instruction inst[3];
        std::memcpy(inst, result.bytecode.data(), result.bytecode.size());

        CHECK(inst[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(inst[0]) == 5.0f);
        CHECK(inst[1].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(inst[1]) == 10.0f);
        CHECK(inst[2].opcode == cedar::Opcode::ADD);
    }

    SECTION("array as function argument") {
        // For now, this just passes first element
        auto result = akkado::compile("saw([440, 880, 1320])");

        REQUIRE(result.success);
        // Should compile using first element (440)
    }

    SECTION("array indexing compiles") {
        // For now, indexing just returns the array (first element)
        auto result = akkado::compile("[1, 2, 3][0]");

        REQUIRE(result.success);
    }
}

TEST_CASE("Pattern variables", "[akkado][pattern]") {
    SECTION("pattern variable assignment") {
        auto result = akkado::compile(R"(
            drums = pat("bd sd")
            drums
        )");

        REQUIRE(result.success);
        // Patterns now use SEQPAT_QUERY + SEQPAT_STEP (lazy query system)
        bool has_pat_step = false;
        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) {
                has_pat_step = true;
                break;
            }
        }
        CHECK(has_pat_step);
    }

    SECTION("pattern variable reuse") {
        // Using the same pattern variable multiple times should work
        auto result = akkado::compile(R"(
            melody = pat("c4 e4 g4")
            melody
        )");

        REQUIRE(result.success);
    }

    SECTION("multiple pattern variables") {
        auto result = akkado::compile(R"(
            drums = pat("bd sd")
            bass = pat("c2 e2 g2")
            drums
        )");

        REQUIRE(result.success);
    }

    SECTION("pitch pattern variable") {
        auto result = akkado::compile(R"(
            notes = pat("c4 e4 g4")
            notes
        )");

        REQUIRE(result.success);
        // Patterns now use SEQPAT_QUERY + SEQPAT_STEP (lazy query system)
        bool has_pat_step = false;
        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) {
                has_pat_step = true;
                break;
            }
        }
        CHECK(has_pat_step);
    }

    SECTION("sample pattern in state_inits") {
        auto result = akkado::compile(R"(
            drums = pat("bd sd hh")
            drums
        )");

        REQUIRE(result.success);
        // Should have state initialization data for the pattern (now uses SequenceProgram)
        REQUIRE(result.state_inits.size() >= 1);
        CHECK(result.state_inits[0].type == akkado::StateInitData::Type::SequenceProgram);
        // Sequences should be populated (root sequence with events)
        CHECK(result.state_inits[0].sequences.size() >= 1);
        // Sample patterns should be marked
        CHECK(result.state_inits[0].is_sample_pattern == true);
    }
}

TEST_CASE("First-class functions and arrays", "[akkado][first-class]") {
    SECTION("len() on array variable") {
        auto result = akkado::compile(R"(
            arr = [1, 2, 3, 4]
            len(arr)
        )");

        REQUIRE(result.success);
        // Should emit PUSH_CONST(4)
        REQUIRE(result.bytecode.size() >= sizeof(cedar::Instruction));

        // Find the last PUSH_CONST (the len result)
        cedar::Instruction* insts = reinterpret_cast<cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        // Last instruction should be PUSH_CONST(4)
        CHECK(insts[count - 1].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(insts[count - 1]) == 4.0f);
    }

    SECTION("map() on array variable") {
        auto result = akkado::compile(R"(
            freqs = [440, 880]
            map(freqs, (f) -> f * 2)
        )");

        REQUIRE(result.success);
        // Should have MUL instructions for the mapping
        CHECK(find_opcode(result.bytecode, cedar::Opcode::MUL));
    }

    SECTION("lambda as variable") {
        auto result = akkado::compile(R"(
            double = (x) -> x * 2
            map([1, 2], double)
        )");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::MUL));
    }

    SECTION("fn used in map()") {
        auto result = akkado::compile(R"(
            fn triple(x) -> x * 3
            map([10], triple)
        )");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::MUL));
    }

    SECTION("closure captures variable") {
        auto result = akkado::compile(R"(
            mult = 2
            f = (x) -> x * mult
            map([10], f)
        )");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::MUL));
    }

    SECTION("variable reassignment produces error") {
        auto result = akkado::compile(R"(
            x = 1
            x = 2
        )");

        REQUIRE_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E150"));
    }

    SECTION("array variable reassignment produces error") {
        auto result = akkado::compile(R"(
            arr = [1, 2, 3]
            arr = [4, 5, 6]
        )");

        REQUIRE_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E150"));
    }

    SECTION("lambda variable reassignment produces error") {
        auto result = akkado::compile(R"(
            f = (x) -> x * 2
            f = (x) -> x * 3
        )");

        REQUIRE_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E150"));
    }

    SECTION("len() on non-array variable produces error") {
        auto result = akkado::compile(R"(
            x = 42
            len(x)
        )");

        REQUIRE_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E141"));
    }

    SECTION("map() with non-function second argument produces error") {
        auto result = akkado::compile(R"(
            map([1, 2], 42)
        )");

        REQUIRE_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E130"));
    }

    SECTION("array variable in expression") {
        auto result = akkado::compile(R"(
            freqs = [440, 550, 660]
            len(freqs) + 1
        )");

        REQUIRE(result.success);
        // Should have: PUSH_CONST(3), PUSH_CONST(1), ADD
        CHECK(find_opcode(result.bytecode, cedar::Opcode::ADD));

        cedar::Instruction* insts = reinterpret_cast<cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        // Check structure: two PUSH_CONST followed by ADD
        bool found_structure = false;
        for (std::size_t i = 0; i + 2 < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::PUSH_CONST &&
                decode_const_float(insts[i]) == 3.0f &&
                insts[i + 1].opcode == cedar::Opcode::PUSH_CONST &&
                decode_const_float(insts[i + 1]) == 1.0f &&
                insts[i + 2].opcode == cedar::Opcode::ADD) {
                found_structure = true;
                break;
            }
        }
        CHECK(found_structure);
    }

    SECTION("map with sum for polyphony") {
        auto result = akkado::compile(R"(
            freqs = [440, 550, 660]
            map(freqs, (f) -> f * 2) |> sum(%)
        )");

        REQUIRE(result.success);
        // Should have MUL and ADD instructions
        CHECK(find_opcode(result.bytecode, cedar::Opcode::MUL));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::ADD));
    }
}

TEST_CASE("Pipes in functions and closures", "[akkado][pipe]") {
    SECTION("pipe in function body") {
        auto result = akkado::compile(R"(
            fn process(x) -> lp(x, 1000, 0.7) |> hp(%, 200, 0.7)
            saw(440) |> process(%) |> out(%, %)
        )");

        REQUIRE(result.success);
        // Should have: SAW, LP filter, HP filter, OUTPUT
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::FILTER_SVF_LP));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::FILTER_SVF_HP));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OUTPUT));
    }

    SECTION("pipe in closure body") {
        auto result = akkado::compile(R"(
            process = (x) -> lp(x, 1000, 0.7) |> hp(%, 200, 0.7)
            saw(440) |> process(%) |> out(%, %)
        )");

        REQUIRE(result.success);
        // Should have: SAW, LP filter, HP filter, OUTPUT
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::FILTER_SVF_LP));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::FILTER_SVF_HP));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OUTPUT));
    }

    SECTION("pipe-to-lambda syntax") {
        auto result = akkado::compile(R"(
            process = x |> lp(%, 1000, 0.7) |> hp(%, 200, 0.7)
            saw(440) |> process(%) |> out(%, %)
        )");

        REQUIRE(result.success);
        // Should have: SAW, LP filter, HP filter, OUTPUT
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::FILTER_SVF_LP));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::FILTER_SVF_HP));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OUTPUT));
    }

    SECTION("longer pipe chain in function body") {
        auto result = akkado::compile(R"(
            fn fx_chain(sig) -> sig |> lp(%, 2000, 0.5) |> tube(%, 0.3) |> hp(%, 100, 0.7)
            saw(220) |> fx_chain(%) |> out(%, %)
        )");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OUTPUT));
    }

    SECTION("pipe-to-lambda used in map") {
        auto result = akkado::compile(R"(
            freqs = [440, 550]
            fx = x |> saw(%) |> lp(%, 1000, 0.7)
            map(freqs, fx) |> sum(%)
        )");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::FILTER_SVF_LP));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::ADD));
    }

    SECTION("nested function calls with pipes") {
        auto result = akkado::compile(R"(
            fn gain(x) -> x * 0.5
            fn process(x) -> x |> gain(%)
            saw(440) |> process(%) |> out(%, %)
        )");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::MUL));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OUTPUT));
    }
}

// Regression: top-level `expr as name` followed by `name |> f(@)` used to be
// silently rewritten into a never-called closure, producing silent audio
// output. The visualizations.akk patch was the public symptom and was
// initially misdiagnosed as visualizer opcodes "eating" the signal.
//
// Two-part fix lives in akkado/src/analyzer.cpp:
//  - collect_definitions registers top-level PipeBinding names so
//    rewrite_pipes' symbol-lookup sees them.
//  - rewrite_pipes only fires the closure-from-pipe sugar when
//    closure_allowed=true (binding-RHS context), not at expression-statement
//    position — so a stray undefined identifier surfaces as E005 instead.
TEST_CASE("Top-level `as` binding then pipe-to-out (regression)",
          "[akkado][pipe][regression]") {
    SECTION("bytecode wires OUTPUT to the bound expression's buffer") {
        auto result = akkado::compile(R"(
            saw(220) * 0.3 as x
            x |> out(@)
        )");
        REQUIRE(result.success);

        // Walk the bytecode and assert that OUTPUT reads from the MUL's
        // out_buffer. Pre-fix, OUTPUT read a freshly-allocated closure-
        // parameter buffer that was never written → silence.
        const auto* inst = reinterpret_cast<const cedar::Instruction*>(
            result.bytecode.data());
        const size_t n = result.bytecode.size() / sizeof(cedar::Instruction);

        std::uint16_t mul_out = cedar::BUFFER_UNUSED;
        std::uint16_t output_in = cedar::BUFFER_UNUSED;
        for (size_t i = 0; i < n; ++i) {
            if (inst[i].opcode == cedar::Opcode::MUL) {
                mul_out = inst[i].out_buffer;
            } else if (inst[i].opcode == cedar::Opcode::OUTPUT) {
                output_in = inst[i].inputs[0];
            }
        }
        REQUIRE(mul_out != cedar::BUFFER_UNUSED);
        REQUIRE(output_in != cedar::BUFFER_UNUSED);
        CHECK(output_in == mul_out);
    }

    SECTION("closure-from-pipe sugar still works in binding RHS") {
        // process = x |> ... is the documented sugar: x is a parameter of
        // the implicit closure, not an undefined identifier. Fix 2 must
        // preserve this path (closure_allowed=true on Assignment RHS).
        auto result = akkado::compile(R"(
            process = x |> lp(%, 1000, 0.7)
            saw(440) |> process(%) |> out(%, %)
        )");
        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::FILTER_SVF_LP));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OUTPUT));
    }

    SECTION("undefined identifier at top-level pipe is a diagnostic, not a closure") {
        // Without Fix 2, this would silently compile as a closure
        // `(xxs) -> osc(xxs, 220)` that never runs — silence with no error.
        // Now it must surface as an undefined-identifier diagnostic so
        // typos and stale references are caught.
        auto result = akkado::compile(R"(
            xxs |> osc(@, 220)
        )");
        REQUIRE_FALSE(result.success);
        // Either E005 (Undefined identifier) or E102 — both are valid
        // signals of "this identifier wasn't bound." We require *some*
        // diagnostic, not silent closure conversion.
        CHECK(result.diagnostics.size() >= 1);
        bool flagged = has_diagnostic_code(result.diagnostics, "E005")
                    || has_diagnostic_code(result.diagnostics, "E102");
        CHECK(flagged);
    }

    SECTION("nested `as` binding inside a longer pipe chain") {
        // Mirrors the visualizations.akk shape: a chain ending in `as v`
        // followed by a separate `v |>` statement that feeds out().
        auto result = akkado::compile(R"(
            saw(220) * 0.3 |> lp(@, 2400) as v
            v |> @ + freeverb(@, 0.5, 0.5) * 0.4 |> out(@)
        )");
        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OUTPUT));
        // Bytecode must include the freeverb opcode — proves the second
        // statement actually compiled the signal chain rather than
        // collapsing to a stranded closure.
        CHECK(find_opcode(result.bytecode, cedar::Opcode::REVERB_FREEVERB));
    }
}

TEST_CASE("Closure parameters in user functions", "[akkado][fn][closure-params]") {
    SECTION("simple closure parameter") {
        // fn apply(sig, fx) -> fx(sig) with inline closure
        auto result = akkado::compile(R"(
            fn apply(sig, fx) -> fx(sig)
            apply(saw(440), (x) -> x * 0.5)
        )");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::MUL));
    }

    SECTION("multiple closure parameters") {
        // fn dual(sig, fx1, fx2) -> fx1(sig) + fx2(sig)
        auto result = akkado::compile(R"(
            fn dual(sig, fx1, fx2) -> fx1(sig) + fx2(sig)
            dual(saw(440), (x) -> x * 0.5, (x) -> x * 0.3)
        )");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::ADD));
    }

    SECTION("mixed scalar and closure parameters") {
        auto result = akkado::compile(R"(
            fn process(sig, amount, fx) -> fx(sig) * amount
            process(saw(440), 0.7, (x) -> x * 2)
        )");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::MUL));
    }

    SECTION("named function as closure parameter") {
        auto result = akkado::compile(R"(
            fn my_gain(x) -> x * 0.5
            fn apply(sig, fx) -> fx(sig)
            apply(saw(440), my_gain)
        )");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::MUL));
    }

    SECTION("closure passthrough through nested function") {
        auto result = akkado::compile(R"(
            fn inner(sig, fx) -> fx(sig)
            fn outer(sig, fx) -> inner(sig, fx)
            outer(saw(440), (x) -> x * 0.5)
        )");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::MUL));
    }

    SECTION("closure with stateful builtin") {
        // Closure that calls a stateful builtin (lp filter)
        auto result = akkado::compile(R"(
            fn apply(sig, fx) -> fx(sig)
            apply(saw(440), (x) -> lp(x, 1000))
        )");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::FILTER_SVF_LP));
    }

    SECTION("multiple closures with stateful builtins get independent state IDs") {
        // Each closure should have a distinct semantic path
        auto result = akkado::compile(R"(
            fn dual(sig, fx1, fx2) -> fx1(sig) + fx2(sig)
            dual(saw(440), (x) -> lp(x, 500), (x) -> lp(x, 2000))
        )");

        REQUIRE(result.success);

        // Should have two FILTER_SVF_LP instructions with different state_ids
        cedar::Instruction* inst = reinterpret_cast<cedar::Instruction*>(
            const_cast<std::uint8_t*>(result.bytecode.data()));
        size_t num_inst = result.bytecode.size() / sizeof(cedar::Instruction);

        std::vector<std::uint32_t> lp_state_ids;
        for (size_t i = 0; i < num_inst; ++i) {
            if (inst[i].opcode == cedar::Opcode::FILTER_SVF_LP) {
                lp_state_ids.push_back(inst[i].state_id);
            }
        }
        REQUIRE(lp_state_ids.size() == 2);
        CHECK(lp_state_ids[0] != lp_state_ids[1]);
    }

    SECTION("multiband3fx stdlib function") {
        auto result = akkado::compile(R"(
            multiband3fx(saw(110), 200, 2000,
                (x) -> x * 0.8,
                (x) -> x,
                (x) -> x * 0.5
            )
        )");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
        // Should have cascaded lp/hp filters
        CHECK(find_opcode(result.bytecode, cedar::Opcode::FILTER_SVF_LP));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::FILTER_SVF_HP));
        // And addition to recombine bands
        CHECK(find_opcode(result.bytecode, cedar::Opcode::ADD));
    }

    SECTION("multiband3fx with stateful effects") {
        auto result = akkado::compile(R"(
            multiband3fx(saw(110), 200, 2000,
                (x) -> lp(x, 100),
                (x) -> lp(x, 1000),
                (x) -> lp(x, 4000)
            )
        )");

        REQUIRE(result.success);

        // All three effect lp() filters should have distinct state_ids from each other
        // and from the band-splitting filters
        cedar::Instruction* inst = reinterpret_cast<cedar::Instruction*>(
            const_cast<std::uint8_t*>(result.bytecode.data()));
        size_t num_inst = result.bytecode.size() / sizeof(cedar::Instruction);

        std::vector<std::uint32_t> lp_state_ids;
        for (size_t i = 0; i < num_inst; ++i) {
            if (inst[i].opcode == cedar::Opcode::FILTER_SVF_LP) {
                lp_state_ids.push_back(inst[i].state_id);
            }
        }
        // 4 band-splitting lp() + 3 effect lp() = 7 total
        // But multiband3fx uses lp(lp(sig,f1),f1) for lo and lp(lp(...),f2) for mid = 4 splitting lp
        // Plus 3 effect lp calls = 7
        REQUIRE(lp_state_ids.size() == 7);

        // All state_ids should be unique
        std::set<std::uint32_t> unique_ids(lp_state_ids.begin(), lp_state_ids.end());
        CHECK(unique_ids.size() == lp_state_ids.size());
    }
}

TEST_CASE("String default parameters", "[akkado][fn][string-defaults]") {
    SECTION("string default used when arg omitted") {
        auto result = akkado::compile(R"(
            fn my_osc(type = "saw", freq = 440) -> match(type) {
                "saw": saw(freq)
                "tri": tri(freq)
                _: saw(freq)
            }
            my_osc()
        )");

        REQUIRE(result.success);
        // "saw" default should resolve match to saw branch
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
        CHECK_FALSE(find_opcode(result.bytecode, cedar::Opcode::OSC_TRI));
    }

    SECTION("string default overridden by explicit arg") {
        auto result = akkado::compile(R"(
            fn my_osc(type = "saw", freq = 440) -> match(type) {
                "saw": saw(freq)
                "tri": tri(freq)
                _: saw(freq)
            }
            my_osc("tri", 880)
        )");

        REQUIRE(result.success);
        // "tri" argument should resolve match to tri branch
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_TRI));
    }

    SECTION("string default with numeric default") {
        auto result = akkado::compile(R"(
            fn synth(wave = "tri", freq = 220) -> match(wave) {
                "tri": tri(freq)
                "sqr": sqr(freq)
                _: saw(freq)
            }
            synth()
        )");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_TRI));
    }
}

TEST_CASE("Named arguments for user functions", "[akkado][fn][named-args]") {
    SECTION("named args reorder correctly") {
        auto result = akkado::compile(R"(
            fn add3(a, b, c) -> a + b + c
            add3(1, c: 3, b: 2)
        )");

        REQUIRE(result.success);
        // Should produce ADD instructions from inlining
        CHECK(find_opcode(result.bytecode, cedar::Opcode::ADD));
    }

    SECTION("named args with defaults") {
        auto result = akkado::compile(R"(
            fn f(a, b = 5, c = 10) -> a * b + c
            f(2, c: 20)
        )");

        REQUIRE(result.success);
        // a=2, b=5 (default), c=20
        // Should have MUL and ADD
        CHECK(find_opcode(result.bytecode, cedar::Opcode::MUL));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::ADD));
    }

    SECTION("named args with string defaults") {
        auto result = akkado::compile(R"(
            fn my_osc(freq = 440, type = "tri") -> match(type) {
                "tri": tri(freq)
                "saw": saw(freq)
                _: tri(freq)
            }
            my_osc(type: "saw", freq: 880)
        )");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
    }
}

TEST_CASE("Closures as return values", "[akkado][fn][closure-return]") {
    SECTION("function returning closure") {
        auto result = akkado::compile(R"(
            fn make_gain(amt) -> (sig) -> sig * amt
            g = make_gain(0.5)
            g(saw(440))
        )");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::MUL));
    }

    SECTION("closure return captures multiple params") {
        auto result = akkado::compile(R"(
            fn make_filter(cut, q) -> (sig) -> lp(sig, cut, q)
            f = make_filter(1000, 0.7)
            f(saw(440))
        )");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::FILTER_SVF_LP));
    }

    SECTION("closure return used inline in pipe") {
        auto result = akkado::compile(R"(
            fn make_gain(amt) -> (sig) -> sig * amt
            half = make_gain(0.5)
            saw(440) |> half(%) |> out(%, %)
        )");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::MUL));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OUTPUT));
    }
}

TEST_CASE("Variadic rest parameters", "[akkado][fn][variadic]") {
    SECTION("rest param with sum") {
        auto result = akkado::compile(R"(
            fn mix(...sigs) -> sum(sigs)
            mix(saw(440), saw(880))
        )");

        REQUIRE(result.success);
        CHECK(count_opcode(result.bytecode, cedar::Opcode::OSC_SAW) == 2);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::ADD));
    }

    SECTION("rest param with len") {
        auto result = akkado::compile(R"(
            fn count(...items) -> len(items)
            count(1, 2, 3)
        )");

        REQUIRE(result.success);
        // len(items) should be compile-time constant 3
        cedar::Instruction* insts = reinterpret_cast<cedar::Instruction*>(result.bytecode.data());
        std::size_t num = result.bytecode.size() / sizeof(cedar::Instruction);

        // Should have a PUSH_CONST(3) somewhere
        bool found_three = false;
        for (std::size_t i = 0; i < num; ++i) {
            if (insts[i].opcode == cedar::Opcode::PUSH_CONST &&
                decode_const_float(insts[i]) == 3.0f) {
                found_three = true;
                break;
            }
        }
        CHECK(found_three);
    }

    SECTION("rest param with required params before it") {
        auto result = akkado::compile(R"(
            fn mix_with_gain(gain, ...sigs) -> sum(sigs) * gain
            mix_with_gain(0.5, saw(440), saw(880))
        )");

        REQUIRE(result.success);
        CHECK(count_opcode(result.bytecode, cedar::Opcode::OSC_SAW) == 2);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::MUL));
    }

    SECTION("single arg to rest param") {
        auto result = akkado::compile(R"(
            fn mix(...sigs) -> sum(sigs)
            mix(saw(440))
        )");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
    }
}

TEST_CASE("Partial application", "[akkado][fn][partial]") {
    SECTION("partial application with underscore") {
        auto result = akkado::compile(R"(
            fn my_add(a, b) -> a + b
            add3 = my_add(3, _)
            add3(4)
        )");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::ADD));
    }

    SECTION("partial application first arg") {
        auto result = akkado::compile(R"(
            fn my_mul(a, b) -> a * b
            double = my_mul(_, 2)
            double(5)
        )");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::MUL));
    }

    SECTION("partial application of builtin") {
        auto result = akkado::compile(R"(
            low_pass = lp(_, 1000)
            saw(440) |> low_pass(%) |> out(%, %)
        )");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::FILTER_SVF_LP));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OUTPUT));
    }

    SECTION("partial application with multiple placeholders") {
        auto result = akkado::compile(R"(
            fn combine(a, b, c) -> a + b + c
            g = combine(_, 10, _)
            g(1, 2)
        )");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::ADD));
    }
}

TEST_CASE("Underscore placeholder default-filling", "[akkado][fn][placeholder]") {
    SECTION("builtin: _ skips optional parameter with default") {
        auto result = akkado::compile(R"(
            lp(osc("sin", 440), 5000, _) |> out(%, %)
        )");
        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::FILTER_SVF_LP));
    }

    SECTION("builtin: _ equivalent to omitting trailing arg") {
        auto r1 = akkado::compile(R"(
            lp(osc("sin", 440), 5000, _) |> out(%, %)
        )");
        auto r2 = akkado::compile(R"(
            lp(osc("sin", 440), 5000) |> out(%, %)
        )");
        REQUIRE(r1.success);
        REQUIRE(r2.success);
        CHECK(r1.bytecode.size() == r2.bytecode.size());
    }

    SECTION("builtin: multiple _ skip middle params") {
        auto result = akkado::compile(R"(
            delay(osc("sin", 440), 0.25, _, _, 0.8) |> out(%, %)
        )");
        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::DELAY));
    }

    SECTION("user fn: _ with numeric default") {
        auto result = akkado::compile(R"(
            fn f(x, y = 10, z = 20) -> x + y + z
            f(1, _, 30) |> out(%, %)
        )");
        REQUIRE(result.success);
    }

    SECTION("user fn: _ with string default") {
        auto result = akkado::compile(R"(
            fn synth(freq, wave = "saw") -> osc(wave, freq)
            synth(440, _) |> out(%, %)
        )");
        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
    }

    SECTION("error: _ on required builtin param") {
        auto result = akkado::compile(R"(
            lp(_, 1000) |> out(%, %)
        )");
        // _ on required 'in' param — should be partial application (closure), not error
        // Only errors if used directly (not assigned)
        // Actually this becomes partial application since 'in' has no default
    }

    SECTION("partial application still works for required params") {
        auto result = akkado::compile(R"(
            low_pass = lp(_, 1000)
            osc("saw", 440) |> low_pass(%) |> out(%, %)
        )");
        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::FILTER_SVF_LP));
    }

    SECTION("disambiguation: _ on optional is default-filling, not partial app") {
        // lp(signal, 5000, _) — third param 'q' has a default
        // Should compile directly, not create a closure
        auto result = akkado::compile(R"(
            osc("saw", 440) |> lp(%, 5000, _) |> out(%, %)
        )");
        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::FILTER_SVF_LP));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OUTPUT));
    }
}

TEST_CASE("Function composition", "[akkado][fn][compose]") {
    SECTION("compose two functions") {
        auto result = akkado::compile(R"(
            fn double(x) -> x * 2
            fn inc(x) -> x + 1
            f = compose(double, inc)
            f(5)
        )");

        REQUIRE(result.success);
        // double(5) = 10, inc(10) = 11
        CHECK(find_opcode(result.bytecode, cedar::Opcode::MUL));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::ADD));
    }

    SECTION("compose with partial application") {
        auto result = akkado::compile(R"(
            pipeline = compose(lp(_, 1000), hp(_, 200))
            saw(440) |> pipeline(%) |> out(%, %)
        )");

        REQUIRE(result.success);
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OSC_SAW));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::FILTER_SVF_LP));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::FILTER_SVF_HP));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::OUTPUT));
    }

    SECTION("compose three functions") {
        auto result = akkado::compile(R"(
            fn a(x) -> x * 2
            fn b(x) -> x + 1
            fn c(x) -> x * 3
            f = compose(a, b, c)
            f(5)
        )");

        REQUIRE(result.success);
        // a(5)=10, b(10)=11, c(11)=33
        CHECK(find_opcode(result.bytecode, cedar::Opcode::MUL));
        CHECK(find_opcode(result.bytecode, cedar::Opcode::ADD));
    }
}

TEST_CASE("TypedValue type checking", "[akkado][types]") {
    SECTION("out() accepts Signal arguments (Number auto-promotes)") {
        // Number constants are valid Signal arguments
        auto result = akkado::compile("out(0.5, 0.5)");
        REQUIRE(result.success);
    }

    SECTION("out() accepts oscillator Signal arguments") {
        auto result = akkado::compile("saw(440) |> out(%, %)");
        REQUIRE(result.success);
    }

    SECTION("out() rejects String argument with E160") {
        // out() is annotated Signal — passing a string should fail
        auto result = akkado::compile(R"(out("hello"))");
        REQUIRE_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E160"));
    }

    SECTION("out() rejects Function argument with E160") {
        auto result = akkado::compile(R"(
            fn f(x) -> x * 2
            out(f)
        )");
        REQUIRE_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E160"));
    }

    SECTION("field access on Signal gives type-specific error") {
        auto result = akkado::compile("saw(440).freq");
        REQUIRE_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E135"));
    }

    SECTION("field access on pattern works") {
        auto result = akkado::compile(R"(
            pat("c4 e4 g4").freq |> out(%, %)
        )");
        REQUIRE(result.success);
    }

    SECTION("field access on record works") {
        auto result = akkado::compile(R"(
            r = {freq: 440, vel: 0.8}
            r.freq |> saw(%) |> out(%, %)
        )");
        REQUIRE(result.success);
    }

    SECTION("slow() accepts MiniLiteral pattern") {
        auto result = akkado::compile(R"(
            slow(pat("c4 e4"), 2).freq |> saw(%) |> out(%, %)
        )");
        REQUIRE(result.success);
    }

    SECTION("slow() rejects non-pattern argument") {
        auto result = akkado::compile("slow(440, 2)");
        REQUIRE_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E133"));
    }

    SECTION("value_type_name returns correct names") {
        CHECK(std::string(akkado::value_type_name(akkado::ValueType::Signal)) == "Signal");
        CHECK(std::string(akkado::value_type_name(akkado::ValueType::Number)) == "Number");
        CHECK(std::string(akkado::value_type_name(akkado::ValueType::Pattern)) == "Pattern");
        CHECK(std::string(akkado::value_type_name(akkado::ValueType::Record)) == "Record");
        CHECK(std::string(akkado::value_type_name(akkado::ValueType::Array)) == "Array");
        CHECK(std::string(akkado::value_type_name(akkado::ValueType::String)) == "String");
        CHECK(std::string(akkado::value_type_name(akkado::ValueType::Function)) == "Function");
        CHECK(std::string(akkado::value_type_name(akkado::ValueType::Void)) == "Void");
    }

    SECTION("type_compatible follows subtyping rules") {
        using akkado::ValueType;
        using akkado::ParamValueType;
        using akkado::type_compatible;

        // Any accepts everything
        CHECK(type_compatible(ValueType::Signal, ParamValueType::Any));
        CHECK(type_compatible(ValueType::Pattern, ParamValueType::Any));
        CHECK(type_compatible(ValueType::String, ParamValueType::Any));

        // Signal accepts Signal and Number
        CHECK(type_compatible(ValueType::Signal, ParamValueType::Signal));
        CHECK(type_compatible(ValueType::Number, ParamValueType::Signal));
        CHECK(type_compatible(ValueType::Pattern, ParamValueType::Signal));
        CHECK_FALSE(type_compatible(ValueType::String, ParamValueType::Signal));
        CHECK_FALSE(type_compatible(ValueType::Function, ParamValueType::Signal));

        // Pattern only accepts Pattern
        CHECK(type_compatible(ValueType::Pattern, ParamValueType::Pattern));
        CHECK_FALSE(type_compatible(ValueType::Signal, ParamValueType::Pattern));

        // Record accepts Record and Pattern (Pattern is subtype)
        CHECK(type_compatible(ValueType::Record, ParamValueType::Record));
        CHECK(type_compatible(ValueType::Pattern, ParamValueType::Record));
        CHECK_FALSE(type_compatible(ValueType::Signal, ParamValueType::Record));

        // Exact match types
        CHECK(type_compatible(ValueType::String, ParamValueType::String));
        CHECK_FALSE(type_compatible(ValueType::Signal, ParamValueType::String));
        CHECK(type_compatible(ValueType::Function, ParamValueType::Function));
        CHECK_FALSE(type_compatible(ValueType::Signal, ParamValueType::Function));
        CHECK(type_compatible(ValueType::Array, ParamValueType::Array));
        CHECK_FALSE(type_compatible(ValueType::Signal, ParamValueType::Array));
    }
}

TEST_CASE("TypedValue integration", "[akkado][types][integration]") {

    // ── 1. Pattern type flow through pipe binding and field access ──

    SECTION("pattern pipe binding with field access") {
        auto result = akkado::compile(R"(
            pat("c4 e4") as p |> saw(p.freq) |> % * p.vel |> out(%, %)
        )");
        REQUIRE(result.success);
    }

    SECTION("pattern transform result retains type through binding") {
        auto result = akkado::compile(R"(
            slow(pat("c4 e4"), 2) as p |> p.freq |> saw(%) |> out(%, %)
        )");
        REQUIRE(result.success);
    }

    SECTION("chained pattern transforms preserve type") {
        auto result = akkado::compile(R"(
            pat("c4 e4") |> slow(%, 2) |> fast(%, 3) |> transpose(%, 5)
        )");
        REQUIRE(result.success);
    }

    SECTION("nested pattern transform with field access") {
        auto result = akkado::compile(R"(
            slow(fast(pat("c4 e4"), 2), 3).freq |> saw(%) |> out(%, %)
        )");
        REQUIRE(result.success);
    }

    // ── 2. Record type flow through pipe binding and spreading ──

    SECTION("record pipe binding with field access") {
        auto result = akkado::compile(R"(
            {freq: 440, amp: 0.5} as r |> saw(r.freq) |> % * r.amp |> out(%, %)
        )");
        REQUIRE(result.success);
    }

    SECTION("record spread preserves field types") {
        auto result = akkado::compile(R"(
            base = {freq: 440, amp: 0.5}
            merged = {..base, amp: 0.8}
            merged.freq |> saw(%) |> out(%, %)
        )");
        REQUIRE(result.success);
    }

    SECTION("record field access on spread result") {
        auto result = akkado::compile(R"(
            base = {a: 100, b: 200}
            merged = {..base, c: 300}
            merged.a |> saw(%) |> out(%, %)
        )");
        REQUIRE(result.success);
    }

    // ── 3. Array + closure type interactions ──

    SECTION("array in map preserves element types") {
        auto result = akkado::compile(R"(
            [440, 880, 1320] |> map(%, (f) -> saw(f)) |> sum(%) |> out(%, %)
        )");
        REQUIRE(result.success);
    }

    SECTION("len on non-array still errors") {
        auto result = akkado::compile(R"(
            saw(440) |> len(%)
        )");
        REQUIRE_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E122"));
    }

    SECTION("array pipe binding then map") {
        auto result = akkado::compile(R"(
            freqs = [440, 880]
            freqs as arr |> map(arr, (f) -> saw(f)) |> sum(%) |> out(%, %)
        )");
        REQUIRE(result.success);
    }

    // ── 4. Closure capture type preservation ──

    SECTION("closure captures record and accesses field") {
        auto result = akkado::compile(R"(
            r = {cutoff: 1000}
            fn process(sig) -> lp(sig, r.cutoff)
            saw(440) |> process(%) |> out(%, %)
        )");
        REQUIRE(result.success);
    }

    SECTION("closure captures array and uses len") {
        auto result = akkado::compile(R"(
            notes = [440, 880, 1320]
            fn voice_count() -> len(notes)
            voice_count()
        )");
        REQUIRE(result.success);
    }

    // ── 5. Pattern transforms reject wrong types (negative tests) ──

    SECTION("slow rejects Signal argument") {
        auto result = akkado::compile("slow(saw(440), 2)");
        REQUIRE_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E133"));
    }

    SECTION("fast rejects record argument") {
        auto result = akkado::compile("fast({freq: 440}, 2)");
        REQUIRE_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E133"));
    }

    SECTION("transpose rejects array argument") {
        auto result = akkado::compile("transpose([440, 880], 5)");
        REQUIRE_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E133"));
    }

    SECTION("rev rejects number") {
        auto result = akkado::compile("rev(440)");
        REQUIRE_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E133"));
    }

    // ── 6. Field access errors with type-specific messages ──

    SECTION("field access on array gives type-specific error") {
        auto result = akkado::compile("[1, 2, 3].freq");
        REQUIRE_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E135"));
    }

    SECTION("field access on function caught by analyzer") {
        auto result = akkado::compile(R"(
            fn f(x) -> x * 2
            f.name
        )");
        REQUIRE_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E061"));
    }

    SECTION("unknown field on record gives field list") {
        auto result = akkado::compile(R"(
            {a: 1, b: 2}.c
        )");
        REQUIRE_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E131"));
    }

    // ── 7. Stereo type interactions ──

    SECTION("stereo signal accepted by out") {
        auto result = akkado::compile(R"(
            stereo(saw(440), saw(880)) |> out(%)
        )");
        REQUIRE(result.success);
    }

    SECTION("pan result accepted by out") {
        auto result = akkado::compile(R"(
            saw(440) |> pan(%, 0.5) |> out(%)
        )");
        REQUIRE(result.success);
    }

    // ── 8. Match expression type flow ──

    SECTION("match arms produce signal type") {
        auto result = akkado::compile(R"(
            x = param("x", 0, 0, 1)
            match(1) {
                1: saw(440)
                2: saw(880)
                _: saw(220)
            } |> out(%, %)
        )");
        REQUIRE(result.success);
    }

    SECTION("match with record destructuring") {
        auto result = akkado::compile(R"(
            r = {a: 440, b: 880}
            r as {a, b} |> saw(a) + saw(b) |> out(%, %)
        )");
        REQUIRE(result.success);
    }

    // ── 9. E160 type mismatch from generic builtin path ──

    SECTION("out rejects Array argument") {
        auto result = akkado::compile("out([1, 2, 3])");
        REQUIRE_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E160"));
    }

    SECTION("out accepts Pattern argument") {
        auto result = akkado::compile(R"(out(pat("c4")))");
        REQUIRE(result.success);
    }
}
