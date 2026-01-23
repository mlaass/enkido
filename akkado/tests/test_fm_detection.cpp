#include <catch2/catch_test_macros.hpp>
#include "akkado/akkado.hpp"
#include <cedar/vm/instruction.hpp>
#include <cstring>

using namespace akkado;

// Helper to compile and get instructions
static std::vector<cedar::Instruction> compile_to_instructions(std::string_view source) {
    auto result = akkado::compile(source, "<test>");
    REQUIRE(result.success);

    // Convert bytecode back to instructions
    std::size_t num_instructions = result.bytecode.size() / sizeof(cedar::Instruction);
    std::vector<cedar::Instruction> instructions(num_instructions);
    std::memcpy(instructions.data(), result.bytecode.data(), result.bytecode.size());
    return instructions;
}

// Helper to check if opcode is 4x oversampled
static bool is_4x_opcode(cedar::Opcode op) {
    return op == cedar::Opcode::OSC_SIN_4X ||
           op == cedar::Opcode::OSC_TRI_4X ||
           op == cedar::Opcode::OSC_SAW_4X ||
           op == cedar::Opcode::OSC_SQR_4X ||
           op == cedar::Opcode::OSC_SQR_PWM_4X ||
           op == cedar::Opcode::OSC_SAW_PWM_4X;
}

// Helper to check if opcode is basic (non-oversampled) oscillator
static bool is_basic_osc(cedar::Opcode op) {
    return op == cedar::Opcode::OSC_SIN ||
           op == cedar::Opcode::OSC_TRI ||
           op == cedar::Opcode::OSC_SAW ||
           op == cedar::Opcode::OSC_SQR;
}

TEST_CASE("FM Detection: constant frequency uses basic oscillator", "[codegen][fm]") {
    // NOTE: sin(x) is now a math function. Use osc("sin", freq) for oscillators.
    auto instructions = compile_to_instructions(R"(osc("sin", 440))");

    bool found_basic = false;
    for (const auto& inst : instructions) {
        if (inst.opcode == cedar::Opcode::OSC_SIN) {
            found_basic = true;
        }
        REQUIRE(!is_4x_opcode(inst.opcode));  // Should NOT be 4x
    }
    REQUIRE(found_basic);
}

TEST_CASE("FM Detection: oscillator-modulated frequency uses 4x", "[codegen][fm]") {
    // osc("sin", osc("sin", 100) * 1000 + 440) - classic FM
    auto instructions = compile_to_instructions(R"(osc("sin", osc("sin", 100) * 1000 + 440))");

    bool found_4x = false;
    int osc_count = 0;
    for (const auto& inst : instructions) {
        if (is_4x_opcode(inst.opcode)) {
            found_4x = true;
        }
        if (is_basic_osc(inst.opcode) || is_4x_opcode(inst.opcode)) {
            osc_count++;
        }
    }

    // Should have at least one 4x oscillator (the carrier)
    // The modulator (inner osc) should also be upgraded since it produces FM
    REQUIRE(found_4x);
    REQUIRE(osc_count == 2);  // Two oscillators total
}

TEST_CASE("FM Detection: nested FM upgrades outer oscillators", "[codegen][fm]") {
    // Deeply nested FM with osc() syntax
    auto instructions = compile_to_instructions(R"(osc("sin", osc("sin", osc("sin", 50) * 200 + 100) * 1000 + 440))");

    int basic_count = 0;
    int upgraded_count = 0;

    for (const auto& inst : instructions) {
        if (is_basic_osc(inst.opcode)) {
            basic_count++;
        }
        if (is_4x_opcode(inst.opcode)) {
            upgraded_count++;
        }
    }

    // Innermost osc("sin", 50) has constant freq -> basic
    // Middle osc uses inner osc output -> 4x
    // Outer osc uses middle osc output -> 4x
    REQUIRE(basic_count == 1);   // Only innermost
    REQUIRE(upgraded_count == 2); // Middle and outer
}

TEST_CASE("FM Detection: arithmetic preserves FM status", "[codegen][fm]") {
    // Addition preserves FM status
    auto instructions = compile_to_instructions(R"(osc("sin", osc("sin", 100) + 440))");

    bool found_4x_sin = false;
    for (const auto& inst : instructions) {
        if (inst.opcode == cedar::Opcode::OSC_SIN_4X) {
            found_4x_sin = true;
        }
    }
    REQUIRE(found_4x_sin);
}

TEST_CASE("FM Detection: saw and sqr also upgrade", "[codegen][fm]") {
    // saw with FM modulated frequency
    auto instructions = compile_to_instructions(R"(saw(osc("sin", 100) * 500 + 200))");

    bool found_4x_saw = false;
    for (const auto& inst : instructions) {
        if (inst.opcode == cedar::Opcode::OSC_SAW_4X) {
            found_4x_saw = true;
        }
    }
    REQUIRE(found_4x_saw);
}

TEST_CASE("FM Detection: noise also triggers FM upgrade", "[codegen][fm]") {
    // Noise-modulated frequency
    auto instructions = compile_to_instructions(R"(osc("sin", noise() * 100 + 440))");

    bool found_4x_sin = false;
    bool found_noise = false;
    for (const auto& inst : instructions) {
        if (inst.opcode == cedar::Opcode::OSC_SIN_4X) {
            found_4x_sin = true;
        }
        if (inst.opcode == cedar::Opcode::NOISE) {
            found_noise = true;
        }
    }
    REQUIRE(found_noise);
    REQUIRE(found_4x_sin);
}

// ============================================================================
// PWM Oscillator FM Detection Tests
// ============================================================================

TEST_CASE("FM Detection: sqr_pwm with constant frequency uses basic opcode", "[codegen][fm][pwm]") {
    auto instructions = compile_to_instructions("sqr_pwm(440, 0.3)");

    bool found_basic = false;
    for (const auto& inst : instructions) {
        if (inst.opcode == cedar::Opcode::OSC_SQR_PWM) {
            found_basic = true;
        }
        // Should NOT be upgraded
        REQUIRE(inst.opcode != cedar::Opcode::OSC_SQR_PWM_4X);
    }
    REQUIRE(found_basic);
}

TEST_CASE("FM Detection: sqr_pwm with FM frequency upgrades to 4x", "[codegen][fm][pwm]") {
    // sqr_pwm with FM on frequency input
    auto instructions = compile_to_instructions(R"(sqr_pwm(osc("sin", 100) * 500 + 200, 0.3))");

    bool found_4x = false;
    for (const auto& inst : instructions) {
        if (inst.opcode == cedar::Opcode::OSC_SQR_PWM_4X) {
            found_4x = true;
        }
    }
    REQUIRE(found_4x);
}

TEST_CASE("FM Detection: saw_pwm with FM frequency upgrades to 4x", "[codegen][fm][pwm]") {
    // saw_pwm with FM on frequency input
    auto instructions = compile_to_instructions(R"(saw_pwm(osc("sin", 100) * 500 + 200, 0.5))");

    bool found_4x = false;
    for (const auto& inst : instructions) {
        if (inst.opcode == cedar::Opcode::OSC_SAW_PWM_4X) {
            found_4x = true;
        }
    }
    REQUIRE(found_4x);
}
