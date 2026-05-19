// Compile-level tests for the transport() builtin.
//
// transport() decouples a pattern from the global clock: each trigger edge
// advances playback. The SEQPAT_TRANSPORT opcode, VM dispatch, and the
// `handle_transport_call` codegen handler shipped in 5cb4eda, but no
// BUILTIN_FUNCTIONS entry was registered until prd-pattern-transport's
// follow-up — without it the analyzer rejected `transport(...)` with E004
// before codegen ever ran. These tests guard the source -> bytecode path.
//
// Behavioural verification of trigger-driven advancement is out of scope here;
// the cedar ExtendedParams audit guard (test_audit_inst_rate.cpp) covers the
// opcode body.

#include <catch2/catch_test_macros.hpp>
#include "akkado/akkado.hpp"
#include <cedar/vm/instruction.hpp>
#include <cstring>
#include <vector>

namespace {

// Decode the flat bytecode blob into a vector of instructions.
std::vector<cedar::Instruction> get_instructions(const akkado::CompileResult& result) {
    std::vector<cedar::Instruction> instructions;
    const std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);
    instructions.resize(count);
    std::memcpy(instructions.data(), result.bytecode.data(), result.bytecode.size());
    return instructions;
}

// Find the first instruction with the given opcode, or nullptr.
const cedar::Instruction* find_instruction(const std::vector<cedar::Instruction>& insts,
                                           cedar::Opcode op) {
    for (const auto& inst : insts) {
        if (inst.opcode == op) return &inst;
    }
    return nullptr;
}

// True if the result carries an Error-severity diagnostic with the given code.
bool has_error(const akkado::CompileResult& result, std::string_view code) {
    for (const auto& d : result.diagnostics) {
        if (d.severity == akkado::Severity::Error && d.code == code) return true;
    }
    return false;
}

}  // namespace

// =============================================================================
// All six PRD §Syntax examples must compile (docs/prd-pattern-transport.md).
// =============================================================================

TEST_CASE("transport(): PRD syntax examples compile", "[transport][codegen]") {
    SECTION("basic — step on each trigger") {
        auto result = akkado::compile(
            R"(transport(n"c4 e4 g4", trigger(2)) as e |> osc("sin", e.freq) |> out(@))");
        CHECK(result.success);
    }

    SECTION("custom step size") {
        auto result = akkado::compile(
            R"(transport(n"c4 e4 g4", trigger(4), 0.5) as e |> osc("sin", e.freq) |> out(@))");
        CHECK(result.success);
    }

    SECTION("cross-pattern triggers") {
        auto result = akkado::compile(
            R"(transport(n"c4 e4 g4", n"x x x x".trig) as e |> osc("sin", e.freq) |> out(@))");
        CHECK(result.success);
    }

    SECTION("pipe style — @ is the pattern") {
        auto result = akkado::compile(
            R"(n"c4 e4 g4" |> transport(@, trigger(2)) as e |> osc("sin", e.freq) |> out(@))");
        CHECK(result.success);
    }

    SECTION("negative step — reverse playback") {
        auto result = akkado::compile(
            R"(transport(n"c4 e4 g4", trigger(2), -1.0) as e |> osc("sin", e.freq) |> out(@))");
        CHECK(result.success);
    }

    SECTION("with reset trigger (4th arg)") {
        auto result = akkado::compile(
            R"(transport(n"c4 e4 g4", trigger(2), 1.0, trigger(0.5)) as e |> osc("sin", e.freq) |> out(@))");
        CHECK(result.success);
    }
}

// =============================================================================
// Codegen emits the SEQPAT_TRANSPORT opcode (and the clock-overridden query).
// =============================================================================

TEST_CASE("transport(): emits SEQPAT_TRANSPORT", "[transport][codegen]") {
    auto result = akkado::compile(
        R"(transport(n"c4 e4 g4", trigger(2)) as e |> osc("sin", e.freq) |> out(@))");
    REQUIRE(result.success);

    auto insts = get_instructions(result);
    CHECK(find_instruction(insts, cedar::Opcode::SEQPAT_TRANSPORT) != nullptr);
    // handle_transport_call also emits a SEQPAT_QUERY whose clock is overridden
    // by the transport's beat_pos buffer.
    CHECK(find_instruction(insts, cedar::Opcode::SEQPAT_QUERY) != nullptr);
}

// =============================================================================
// Error paths.
// =============================================================================

TEST_CASE("transport(): rejects bad arg counts", "[transport][diagnostics]") {
    SECTION("too few arguments (1)") {
        auto result = akkado::compile(R"(transport(n"c4") |> out(@))");
        CHECK_FALSE(result.success);
    }

    SECTION("too many arguments (5)") {
        auto result = akkado::compile(
            R"(transport(n"c4 e4 g4", trigger(2), 1.0, trigger(0.5), 99) |> out(@))");
        CHECK_FALSE(result.success);
    }
}

TEST_CASE("transport(): rejects non-pattern first argument", "[transport][diagnostics]") {
    auto result = akkado::compile(R"(transport(440, trigger(2)) |> out(@))");
    CHECK_FALSE(result.success);
    // handle_transport_call's is_pattern_node check.
    CHECK(has_error(result, "E133"));
}
